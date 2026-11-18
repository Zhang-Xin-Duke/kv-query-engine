#!/usr/bin/env python3
"""Generation-pinned BatchGet client and serialized rolling-refresh coordinator."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
import sys
from pathlib import Path
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener
import uuid

if __package__:
    from .replica import snapshot_id, unsigned
else:
    from replica import snapshot_id, unsigned

if __name__ == '__main__':
    # Keep one Unavailable class when the CLI lazily imports hotkey.
    sys.modules.setdefault(f'{__package__}.client' if __package__ else 'client', sys.modules[__name__])


class Unavailable(RuntimeError):
    pass


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def rpc(endpoint, path, payload=None, timeout=5):
    data = None if payload is None else json.dumps(payload).encode()
    headers = {'Content-Type': 'application/json'}
    token = os.environ.get('INDEX_ENGINE_TOKEN')
    if token:
        headers['Authorization'] = 'Bearer ' + token
    request = Request(endpoint.rstrip('/') + path, data=data,
                      headers=headers)
    # Replica addresses are explicit; never send their token through a proxy or redirect.
    with build_opener(ProxyHandler({}), NoRedirect()).open(request, timeout=timeout) as response:
        return json.load(response)


def statuses(endpoints, timeout=2):
    result = {}
    for endpoint in endpoints:
        try:
            state = rpc(endpoint, '/status', timeout=timeout)
            if not isinstance(state, dict):
                raise ValueError('invalid replica status')
            result[endpoint] = state
        except (OSError, ValueError) as exc:
            result[endpoint] = {'state': 'UNREACHABLE', 'error': str(exc)}
    return result


def validate_response(response, keys, generation, identity, request_id, attempt_id):
    if (not isinstance(response, dict) or response.get('ok') is not True
            or type(response.get('generation')) is not int or response.get('generation') != generation
            or response.get('snapshot_id') != identity or response.get('request_id') != request_id
            or response.get('attempt_id') != attempt_id):
        raise Unavailable('response identity/version mismatch')
    rows = response.get('records')
    if not isinstance(rows, list) or len(rows) != len(keys):
        raise Unavailable('incomplete batch')
    for key, row in zip(keys, rows):
        if (not isinstance(row, dict) or type(row.get('key')) is not int or row.get('key') != key
                or type(row.get('generation')) is not int or row.get('generation') != generation
                or type(row.get('found')) is not bool
                or (row['found'] and (type(row.get('id')) is not int or row.get('id') != key
                                     or not isinstance(row.get('value'), str)))):
            raise Unavailable('record identity/version mismatch')
    return response


def audit_placement(endpoints, minimum=2):
    if type(minimum) is not int or minimum < 1 or len(set(endpoints)) != len(endpoints):
        raise ValueError('invalid replica endpoints or domain count')
    current = statuses(endpoints)
    domains = [state.get('failure_domain') for state in current.values()]
    ok = (len(endpoints) >= minimum and all(state.get('state') == 'READY' for state in current.values())
          and all(isinstance(domain, str) and domain for domain in domains)
          and len(set(domains)) == len(domains) and len(set(domains)) >= minimum)
    return dict(ok=ok, required_domains=minimum, replicas=current)


def batch_get(endpoints, keys, generation, identity=None, attempts=3, timeout=5, hold_token=None):
    unsigned(generation)
    if not 1 <= len(keys) <= 1024 or attempts < 1:
        raise ValueError('require 1..1024 keys and at least one attempt')
    for key in keys:
        unsigned(key)
    request_id, errors = uuid.uuid4().hex, []
    for _ in range(attempts):
        current = statuses(endpoints, timeout=min(2, timeout))
        candidates = [(url, state) for url, state in current.items()
                      if state.get('state') == 'READY' and state.get('generation') == generation]
        identities = {state['snapshot_id'] for _, state in candidates}
        if len(identities) > 1:
            raise Unavailable('conflicting artifacts for the same generation')
        if identity is None and identities:
            identity = next(iter(identities))
        for url, state in candidates:
            if state['snapshot_id'] != identity:
                continue
            attempt_id = uuid.uuid4().hex
            request = dict(keys=keys, generation=generation, snapshot_id=identity,
                           request_id=request_id, attempt_id=attempt_id)
            if hold_token is not None:
                request['hold_token'] = hold_token
            try:
                result = rpc(url, '/batch', request, timeout=timeout)
                return validate_response(result, keys, generation, identity, request_id, attempt_id)
            except (OSError, ValueError, Unavailable) as exc:
                # Entire response is discarded. A retry has a fresh attempt identity.
                errors.append(str(exc))
        time.sleep(0.05)
    raise Unavailable(f'generation {generation} unavailable after bounded attempts: {errors}')


@contextmanager
def controller_lock(endpoints):
    # One coordinator host, with a local advisory lock. This is not leader election.
    key = hashlib.sha256('\n'.join(sorted(endpoints)).encode()).hexdigest()
    with open(Path(tempfile.gettempdir()) / ('index-rollout-' + key + '.lock'), 'a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Unavailable('another local coordinator owns this replica group')
        yield


def rolling_refresh(endpoints, snapshot, generation, journal=None, step=False, timeout=120,
                    only=None, drain_timeout=30):
    if len(set(endpoints)) < 2 or len(set(endpoints)) != len(endpoints):
        raise ValueError('rolling refresh requires at least two distinct replicas')
    unsigned(generation)
    if only is not None and only not in endpoints:
        raise ValueError('selected replica must belong to the replica group')
    snapshot = str(Path(snapshot).resolve())
    identity = snapshot_id(snapshot)
    events = []

    def record(event, **fields):
        entry = dict(event=event, time=time.time(), **fields)
        events.append(entry)
        if journal:
            with open(journal, 'a') as out:
                out.write(json.dumps(entry) + '\n')
                out.flush()
        print(json.dumps(entry), flush=True)

    with controller_lock(endpoints):
        current = statuses(endpoints)
        for state in current.values():
            if state['state'] != 'READY':
                raise Unavailable('all replicas must be READY before rollout/resume')
            if state['generation'] > generation or (state['generation'] == generation and state['snapshot_id'] != identity):
                raise Unavailable('target conflicts with an existing generation')
        old = {(s['generation'], s['snapshot_id']) for s in current.values() if s['generation'] < generation}
        if len(old) > 1:
            raise Unavailable('old replicas disagree on snapshot identity')
        record('ROLLOUT_BEGIN', target=generation, snapshot_id=identity)
        # Keep the first replica serving while the second one is prepared.
        order = [only] if only is not None else endpoints[1:] + endpoints[:1]
        for endpoint in order:
            current = statuses(endpoints)
            state = current[endpoint]
            if state['state'] == 'READY' and state['generation'] == generation and state['snapshot_id'] == identity:
                record('ALREADY_READY', endpoint=endpoint)
                continue
            if state['state'] != 'READY' or state['generation'] >= generation:
                raise Unavailable('replica state changed; stop and reconcile')
            other_ready = [s for url, s in current.items() if url != endpoint and s['state'] == 'READY']
            if not other_ready:
                raise Unavailable('no other serving replica; rollout stopped')
            if not any(s['generation'] == state['generation'] and s['snapshot_id'] == state['snapshot_id'] for s in other_ready):
                # Last old replica: the new generation must already have full coverage.
                if not any(s['generation'] == generation and s['snapshot_id'] == identity for s in other_ready):
                    raise Unavailable('neither old nor new generation has coverage')
                record('CUTOVER', target=generation,
                       contract='new batches use target; previously admitted old batches drain')
            record('REFRESH_REPLICA', endpoint=endpoint)
            try:
                result = rpc(endpoint, '/refresh', dict(snapshot=snapshot, generation=generation,
                             snapshot_id=identity, drain_timeout=drain_timeout), timeout=timeout)
            except (OSError, ValueError) as exc:
                record('ROLLOUT_STOPPED', endpoint=endpoint, error=str(exc))
                raise Unavailable('refresh failed/timed out; inspect status before resuming') from exc
            if result['state'] != 'READY' or result['generation'] != generation or result['snapshot_id'] != identity:
                raise Unavailable('replica did not confirm target artifact')
            record('REPLICA_READY', endpoint=endpoint, generation=generation)
            if step:
                input('Replica ready. Press Enter to continue the rollout... ')
        final = statuses(endpoints)
        complete = all(s.get('state') == 'READY' and s.get('generation') == generation
                       and s.get('snapshot_id') == identity for s in final.values())
        if only is not None and not complete:
            record('ROLLOUT_PAUSED', generation=generation, replicas=final)
        elif not complete:
            raise Unavailable('final coverage check failed')
        else:
            record('ROLLOUT_COMPLETE', generation=generation)
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--replicas', nargs='+', required=True)
    commands = parser.add_subparsers(dest='command', required=True)
    status = commands.add_parser('status', parents=[common])
    status.add_argument('--brief', action='store_true', help='one readable row per replica')
    placement = commands.add_parser('placement', parents=[common], help='audit READY replicas and failure domains')
    placement.add_argument('--min-domains', type=int, default=2)
    batch = commands.add_parser('batch', parents=[common])
    batch.add_argument('--generation', type=int, required=True)
    batch.add_argument('--keys', type=int, nargs='+', required=True)
    batch.add_argument('--attempts', type=int, default=3)
    batch.add_argument('--timeout', type=float, default=5)
    batch.add_argument('--hold-token', help='explicit gate, only on replicas started with --demo-controls')
    batch.add_argument('--brief', action='store_true', help='show replica, version and hit/miss rows')
    hot = commands.add_parser('hot', parents=[common], help='reuse a cache while repeatedly querying keys')
    hot.add_argument('--generation', type=int, required=True)
    hot.add_argument('--keys', type=int, nargs='+', required=True)
    hot.add_argument('--iterations', type=int, default=10)
    hot.add_argument('--concurrency', type=int, default=1)
    hot.add_argument('--capacity', type=int, default=10000)
    hot.add_argument('--ttl', type=float, default=10.0)
    rollout = commands.add_parser('rollout', parents=[common])
    rollout.add_argument('--generation', type=int, required=True)
    rollout.add_argument('--snapshot', required=True)
    rollout.add_argument('--journal')
    rollout.add_argument('--step', action='store_true')
    refresh = commands.add_parser('refresh', parents=[common], help='manually refresh exactly one replica, with coverage checks')
    refresh.add_argument('--replica', required=True)
    refresh.add_argument('--generation', type=int, required=True)
    refresh.add_argument('--snapshot', required=True)
    refresh.add_argument('--journal')
    refresh.add_argument('--drain-timeout', type=float, default=30)
    release = commands.add_parser('release', parents=[common], help='release a manually held demo batch on one replica')
    release.add_argument('--hold-token', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'status':
            current = statuses(args.replicas)
            if args.brief:
                print('REPLICA  STATE        VERSION  ACTIVE  HELD')
                for url, state in current.items():
                    print(f"{state.get('replica', url):8} {state['state']:12} "
                          f"{str(state.get('generation', '-')):8} {str(state.get('active', '-')):7} "
                          f"{','.join(state.get('held_batches', [])) or '-'}")
            else:
                print(json.dumps(current, indent=2))
        elif args.command == 'placement':
            audit = audit_placement(args.replicas, args.min_domains)
            print(json.dumps(audit, indent=2))
            if not audit['ok']:
                parser.exit(1, 'replica placement does not meet the requested spread\n')
        elif args.command == 'hot':
            from concurrent.futures import ThreadPoolExecutor
            if __package__:
                from .hotkey import HotKeyClient
            else:
                from hotkey import HotKeyClient
            if not 1 <= args.iterations <= 10000 or not 1 <= args.concurrency <= 64:
                raise ValueError('iterations must be 1..10000 and concurrency 1..64')
            client = HotKeyClient(args.replicas, capacity=args.capacity, ttl=args.ttl)
            with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
                for result in pool.map(lambda _: client.batch_get(args.keys, args.generation),
                                       range(args.iterations)):
                    pass
            print(json.dumps(dict(last_result=result, statistics=client.statistics()), indent=2))
        elif args.command == 'batch':
            result = batch_get(args.replicas, args.keys, args.generation, attempts=args.attempts,
                               timeout=args.timeout, hold_token=args.hold_token)
            if args.brief:
                print(f"REPLICA {result['replica']}  VERSION {result['generation']}")
                print('KEY           FOUND  VERSION')
                for row in result['records']:
                    print(f"{row['key']:<13} {str(row['found']):6} {row['generation']}")
            else:
                print(json.dumps(result, indent=2))
        elif args.command == 'release':
            if len(args.replicas) != 1:
                raise ValueError('release requires exactly one replica endpoint')
            print(json.dumps(rpc(args.replicas[0], '/demo/release', {'hold_token': args.hold_token})))
        elif args.command == 'refresh':
            rolling_refresh(args.replicas, args.snapshot, args.generation, args.journal,
                            timeout=args.drain_timeout + 120, only=args.replica,
                            drain_timeout=args.drain_timeout)
        else:
            rolling_refresh(args.replicas, args.snapshot, args.generation, args.journal, args.step)
    except (Unavailable, OSError, ValueError) as exc:
        parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
