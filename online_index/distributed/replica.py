#!/usr/bin/env python3
"""HTTP replica wrapping a persistent C++ index engine (stdlib only)."""
import argparse
from contextlib import contextmanager
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time


class ProtocolError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message
        super().__init__(message)


def unsigned(value):
    if type(value) is not int or not 0 <= value < 2**64:
        raise ProtocolError(400, 'expected uint64 integer')
    return value


def snapshot_id(path):
    """Digest the complete immutable artifact, including relative filenames."""
    root = Path(path)
    if not (root / 'meta.pb').is_file() or root.is_symlink():
        raise ValueError('snapshot must contain meta.pb')
    digest = hashlib.sha256()
    for item in sorted(root.rglob('*')):
        if item.is_symlink():
            raise ValueError('snapshot symlinks are unsupported')
        if item.is_file():
            name = item.relative_to(root).as_posix().encode()
            digest.update(len(name).to_bytes(8, 'big'))
            digest.update(name)
            digest.update(item.stat().st_size.to_bytes(8, 'big'))
            with item.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
    return digest.hexdigest()


class Engine:
    def __init__(self, binary, backend, snapshot, generation, memory_return='none', trim_every=1,
                 query_strategy=None, query_window=None):
        self.lock = threading.Lock()
        command = [str(binary), backend, str(snapshot), str(generation)]
        if memory_return != 'none':
            command += ['--memory-return', memory_return]
        if trim_every != 1:
            command += ['--trim-every', str(trim_every)]
        if query_strategy is not None:
            command += ['--query-strategy', query_strategy]
        if query_window is not None:
            command += ['--query-window', str(query_window)]
        self.process = subprocess.Popen(command,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        text=True, bufsize=1)
        try:
            self.info = self._read()
        except Exception:
            self.close()
            raise

    def _read(self):
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError('engine exited')
        result = json.loads(line)
        if not result.get('ok'):
            raise RuntimeError(result.get('error', 'engine failed'))
        return result

    def command(self, *parts):
        if any('\n' in str(p) or '\t' in str(p) for p in parts):
            raise ValueError('invalid control-channel field')
        with self.lock:
            self.process.stdin.write('\t'.join(map(str, parts)) + '\n')
            self.process.stdin.flush()
            result = self._read()
            if parts and parts[0] == 'REFRESH':
                self.info = result
            return result

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()


class Replica:
    def __init__(self, name, binary, backend, snapshot, generation, demo_controls=False, memory_return='none', trim_every=1, max_active=128,
                 query_strategy=None, query_window=None, failure_domain=None):
        self.name, self.generation = name, unsigned(generation)
        self.failure_domain = failure_domain or socket.gethostname()
        self.cv = threading.Condition()
        self.update_lock = threading.Lock()
        self.active = 0
        self.max_active = max_active
        self.holds = {}
        self.state = 'STARTING'
        self.demo_controls = demo_controls
        self.memory_return = memory_return
        self.trim_every = trim_every
        # Copy before hashing/loading: a caller cannot retarget a mutable source path.
        self.artifact = tempfile.TemporaryDirectory(prefix='index-snapshot-')
        owned = Path(self.artifact.name) / 'snapshot'
        shutil.copytree(snapshot, owned, symlinks=True)
        self.identity = snapshot_id(owned)
        self.engine = Engine(binary, backend, owned, generation, memory_return, trim_every,
                             query_strategy, query_window)
        self.state = 'READY'
        self.event('READY')

    def event(self, event, **fields):
        print(json.dumps(dict(event=event, replica=self.name, generation=self.generation,
                              time=time.time(), **fields)), flush=True)

    def status(self):
        with self.cv:
            if self.state == 'READY' and self.engine.process.poll() is not None:
                self.state = 'FAILED'
            return dict(replica=self.name, state=self.state, generation=self.generation,
                        snapshot_id=self.identity, active=self.active,
                        held_batches=sorted(self.holds), memory_return=self.memory_return, trim_every=self.trim_every,
                        max_active=self.max_active, failure_domain=self.failure_domain,
                        compact_stats=self.engine.info.get('compact_stats'))

    @contextmanager
    def admit(self, request):
        generation = unsigned(request.get('generation'))
        keys = request.get('keys')
        if not isinstance(keys, list) or not 1 <= len(keys) <= 1024:
            raise ProtocolError(400, 'keys must contain 1..1024 integers')
        for key in keys:
            unsigned(key)
        for field in ('request_id', 'attempt_id'):
            if not isinstance(request.get(field), str) or not 1 <= len(request[field]) <= 128:
                raise ProtocolError(400, 'request_id and attempt_id are required')
        hold = request.get('hold_ms', 0)
        if type(hold) is not int or hold < 0 or hold > 10000 or (hold and not self.demo_controls):
            raise ProtocolError(400, 'hold_ms requires --demo-controls; maximum 10000')
        token = request.get('hold_token')
        if token is not None and (not self.demo_controls or not isinstance(token, str)
                                  or not 1 <= len(token) <= 64):
            raise ProtocolError(400, 'hold_token requires --demo-controls and 1..64 characters')
        gate = threading.Event() if token is not None else None
        with self.cv:
            if self.state != 'READY':
                raise ProtocolError(503, 'replica is not READY')
            if generation != self.generation or request.get('snapshot_id') != self.identity:
                raise ProtocolError(409, 'generation/snapshot mismatch')
            if self.active >= self.max_active:
                raise ProtocolError(503, 'replica admission limit reached')
            if token is not None:
                if token in self.holds:
                    raise ProtocolError(409, 'hold_token already active')
                self.holds[token] = gate
            self.active += 1
        self.event('ADMITTED', request_id=request['request_id'], attempt_id=request['attempt_id'])
        try:
            if gate is not None and not gate.wait(timeout=300):
                raise ProtocolError(408, 'demo hold expired after 300 seconds')
            if hold:
                time.sleep(hold / 1000)
            yield
        finally:
            with self.cv:
                if token is not None:
                    self.holds.pop(token, None)
                self.active -= 1
                self.cv.notify_all()
            self.event('RELEASED', request_id=request['request_id'])

    def batch(self, request):
        try:
            result = self.engine.command('BATCH', request['generation'], *request['keys'])
        except Exception as exc:
            with self.cv:
                self.state = 'FAILED'
            raise RuntimeError('engine batch failed: ' + str(exc)) from exc
        return dict(result, replica=self.name, snapshot_id=self.identity,
                    request_id=request['request_id'], attempt_id=request['attempt_id'])

    def release_demo_hold(self, request):
        if not self.demo_controls:
            raise ProtocolError(404, 'demo controls disabled')
        token = request.get('hold_token')
        with self.cv:
            if not isinstance(token, str) or token not in self.holds:
                raise ProtocolError(404, 'unknown hold_token')
            self.holds[token].set()
        return {'released': token}

    def refresh(self, request):
        generation = unsigned(request.get('generation'))
        timeout = request.get('drain_timeout', 30)
        if type(timeout) not in (float, int) or not 0 < timeout <= 300:
            raise ProtocolError(400, 'drain_timeout must be in (0,300] seconds')
        if not self.update_lock.acquire(blocking=False):
            raise ProtocolError(409, 'refresh already in progress')
        candidate = None
        try:
            candidate = tempfile.TemporaryDirectory(prefix='index-snapshot-')
            with self.cv:
                if self.state != 'READY' or generation <= self.generation:
                    raise ProtocolError(409, 'refresh requires READY and a newer generation')
            owned = Path(candidate.name) / 'snapshot'
            shutil.copytree(request['snapshot'], owned, symlinks=True)
            identity = snapshot_id(owned)
            if identity != request.get('snapshot_id'):
                raise ProtocolError(409, 'snapshot digest mismatch')
            with self.cv:
                self.state = 'DRAINING'
                self.event('DRAINING', active=self.active, target=generation)
                deadline = time.monotonic() + timeout
                while self.active:
                    if self.state != 'DRAINING':
                        raise ProtocolError(503, 'engine failed while draining')
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        self.state = 'READY'
                        self.event('DRAIN_TIMEOUT')
                        raise ProtocolError(503, 'drain timeout; old generation remains READY')
                    self.cv.wait(remaining)
                # A queued request could have observed an engine failure while draining.
                if self.state != 'DRAINING':
                    raise ProtocolError(503, 'engine failed while draining')
                self.state = 'REFRESHING'
            self.event('REFRESHING', target=generation)
            try:
                self.engine.command('REFRESH', generation, owned)
            except Exception as exc:
                with self.cv:
                    self.state = 'FAILED'
                self.event('FAILED', target=generation)
                raise RuntimeError('engine refresh failed: ' + str(exc)) from exc
            old = self.artifact
            with self.cv:
                self.artifact, candidate = candidate, None
                self.generation, self.identity = generation, identity
                self.state = 'READY'
            old.cleanup()
            self.event('READY')
            return self.status()
        finally:
            if candidate is not None:
                candidate.cleanup()
            self.update_lock.release()

    def close(self):
        self.engine.close()
        self.artifact.cleanup()


def handler_for(replica, auth_token=None):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send_json(self, status, body):
            raw = json.dumps(body, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def authorized(self):
            if auth_token is None:
                return True
            supplied = self.headers.get('Authorization', '')
            if not hmac.compare_digest(supplied.encode(), ('Bearer ' + auth_token).encode()):
                self.send_json(401, {'error': 'unauthorized'})
                return False
            return True

        def do_GET(self):
            if not self.authorized():
                return
            self.send_json(200 if self.path == '/status' else 404,
                           replica.status() if self.path == '/status' else {'error': 'not found'})

        def do_POST(self):
            try:
                if not self.authorized():
                    return
                self.connection.settimeout(15)
                length = int(self.headers.get('Content-Length', 0))
                if not 0 < length <= 131072:
                    raise ProtocolError(400, 'body must be 1..131072 bytes')
                request = json.loads(self.rfile.read(length))
                if not isinstance(request, dict):
                    raise ProtocolError(400, 'expected JSON object')
                if self.path == '/batch':
                    with replica.admit(request):
                        self.send_json(200, replica.batch(request))
                elif self.path == '/refresh':
                    self.send_json(200, replica.refresh(request))
                elif self.path == '/demo/release':
                    self.send_json(200, replica.release_demo_hold(request))
                else:
                    raise ProtocolError(404, 'not found')
            except ProtocolError as exc:
                self.send_json(exc.status, {'error': exc.message})
            except (ValueError, KeyError, TypeError, OSError) as exc:
                if isinstance(exc, (BrokenPipeError, ConnectionResetError)):
                    return
                self.send_json(400, {'error': str(exc)})
            except Exception as exc:
                self.send_json(503, {'error': str(exc)})
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--generation', type=int, required=True)
    parser.add_argument('--backend', choices=['bucket', 'neighbor', 'bucket-pool', 'neighbor-pool', 'compact'], default='neighbor-pool')
    parser.add_argument('--query-strategy', choices=['scalar', 'prefetch', 'amac', 'simd'])
    parser.add_argument('--query-window', type=int)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--listen', default='127.0.0.1', help='IPv4 address; non-loopback requires authentication')
    parser.add_argument('--require-auth', action='store_true', help='require INDEX_ENGINE_TOKEN on every request')
    parser.add_argument('--max-active', type=int, default=128, help='maximum admitted batches')
    parser.add_argument('--failure-domain', help='physical host or failure-domain label; defaults to hostname')
    parser.add_argument('--demo-controls', action='store_true')
    parser.add_argument('--memory-return', choices=['none', 'trim', 'reclaim'], default='none')
    parser.add_argument('--trim-every', type=int, default=1)
    args = parser.parse_args()
    if args.backend != 'compact' and (args.query_strategy is not None or args.query_window is not None):
        parser.error('--query-strategy and --query-window require --backend compact')
    if args.query_window is not None and not 1 <= args.query_window <= 256:
        parser.error('--query-window must be in 1..256')
    try:
        address = ipaddress.ip_address(args.listen)
        if address.version != 4:
            raise ValueError('IPv4 required')
    except ValueError:
        parser.error('--listen must be an IPv4 address')
    if not 1 <= args.max_active <= 65536:
        parser.error('--max-active must be in 1..65536')
    if args.failure_domain is not None and not 1 <= len(args.failure_domain) <= 128:
        parser.error('--failure-domain must contain 1..128 characters')
    auth_token = os.environ.get('INDEX_ENGINE_TOKEN') if args.require_auth else None
    if args.require_auth and not auth_token:
        parser.error('--require-auth requires a nonempty INDEX_ENGINE_TOKEN')
    if not address.is_loopback and not auth_token:
        parser.error('non-loopback listening requires --require-auth and INDEX_ENGINE_TOKEN')
    if not 1 <= args.trim_every <= (1 << 64) - 1:
        parser.error('--trim-every must be a positive unsigned 64-bit integer')
    if args.trim_every != 1 and args.memory_return != 'reclaim':
        parser.error('--trim-every requires --memory-return reclaim')
    replica = Replica(args.name, args.worker.resolve(), args.backend, args.snapshot, args.generation,
                      args.demo_controls, args.memory_return, args.trim_every, args.max_active,
                      args.query_strategy, args.query_window, args.failure_domain)
    try:
        server = ThreadingHTTPServer((args.listen, args.port), handler_for(replica, auth_token))
        server.daemon_threads = False
        def stop(*_):
            with replica.cv:
                for gate in replica.holds.values():
                    gate.set()
            threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    finally:
        replica.close()


if __name__ == '__main__':
    main()
