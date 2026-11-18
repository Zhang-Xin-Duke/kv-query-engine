"""Bounded, version-scoped client cache and replica routing for BatchGet.

Each client owns its cache and counters. This module does not provide a
cluster-wide hot-key oracle or a coordinator for replica placement.
"""
from collections import Counter, OrderedDict
from concurrent.futures import Future, TimeoutError as FutureTimeout
import math
import threading
import time
import uuid

if __package__:
    from .client import Unavailable, rpc, statuses, validate_response
    from .replica import unsigned
else:
    from client import Unavailable, rpc, statuses, validate_response
    from replica import unsigned


class HotKeyClient:
    """A reusable client; instantiate once per application process.

    Cached entries and in-flight requests are scoped by generation and
    snapshot digest. Positive results are cached; missing keys are not.
    Each batch first checks that at least one exact-version replica is READY.
    """

    def __init__(self, endpoints, *, capacity=10000, ttl=10.0,
                 tracked_keys=10000, window=60.0, max_inflight=4096,
                 attempts=2, timeout=5.0, cooldown=0.5):
        if not endpoints or len(set(endpoints)) != len(endpoints):
            raise ValueError('require distinct replica endpoints')
        if (type(capacity) is not int or capacity < 0 or type(tracked_keys) is not int
                or tracked_keys < 1 or type(max_inflight) is not int or max_inflight < 1):
            raise ValueError('invalid cache or tracking capacity')
        if (type(attempts) is not int or attempts < 1 or
                any(not isinstance(x, (int, float)) or not math.isfinite(x) or x <= 0
                    for x in (ttl, window, timeout, cooldown))):
            raise ValueError('ttl, window, attempts, timeout and cooldown must be positive')
        self.endpoints = tuple(endpoints)
        self.capacity, self.ttl, self.tracked_keys = capacity, ttl, tracked_keys
        self.window, self.window_start, self.max_inflight = window, time.monotonic(), max_inflight
        self.attempts, self.timeout = attempts, timeout
        self.cooldown, self.quarantined_until = cooldown, {}
        self.lock = threading.Lock()
        self.cache = OrderedDict()
        self.inflight = {}
        self.counts = OrderedDict()
        self.metrics = Counter()
        self.replica_requests = Counter()
        self.cursor = 0

    def _ready(self, generation, identity):
        states = statuses(self.endpoints, timeout=min(2, self.timeout))
        ready = [(endpoint, state) for endpoint, state in states.items()
                 if state.get('state') == 'READY' and type(state.get('generation')) is int
                 and state.get('generation') == generation]
        if any(not isinstance(state.get('snapshot_id'), str) or not state['snapshot_id']
               for _, state in ready):
            raise Unavailable('conflicting or missing artifacts for the same generation')
        identities = {state['snapshot_id'] for _, state in ready}
        if len(identities) > 1:
            raise Unavailable('conflicting or missing artifacts for the same generation')
        if identity is None and identities:
            identity = next(iter(identities))
        candidates = [endpoint for endpoint, state in ready if state.get('snapshot_id') == identity]
        if not candidates:
            raise Unavailable('no READY replica for the requested generation and snapshot')
        return identity, candidates

    def _fetch(self, keys, generation, identity, candidates):
        errors = []
        request_id = uuid.uuid4().hex
        with self.lock:
            start = self.cursor % len(candidates)
            self.cursor += 1
        ordered = candidates[start:] + candidates[:start]
        for _ in range(self.attempts):
            for endpoint in ordered:
                with self.lock:
                    if self.quarantined_until.get(endpoint, 0) > time.monotonic():
                        self.metrics['cooldown_skips'] += 1
                        continue
                attempt_id = uuid.uuid4().hex
                request = dict(keys=keys, generation=generation, snapshot_id=identity,
                               request_id=request_id, attempt_id=attempt_id)
                with self.lock:
                    self.metrics['rpc_attempts'] += 1
                    self.replica_requests[endpoint] += 1
                try:
                    result = rpc(endpoint, '/batch', request, timeout=self.timeout)
                    validate_response(result, keys, generation, identity, request_id, attempt_id)
                    with self.lock:
                        self.quarantined_until.pop(endpoint, None)
                    return result
                except (OSError, ValueError, Unavailable) as exc:
                    errors.append(str(exc))
                    with self.lock:
                        self.metrics['rpc_failures'] += 1
                        self.quarantined_until[endpoint] = time.monotonic() + self.cooldown
        raise Unavailable('all bounded replica attempts failed: ' + '; '.join(errors))

    def batch_get(self, keys, generation, identity=None):
        unsigned(generation)
        if not isinstance(keys, (tuple, list)) or not 1 <= len(keys) <= 1024:
            raise ValueError('require 1..1024 keys')
        for key in keys:
            unsigned(key)
        identity, candidates = self._ready(generation, identity)
        now = time.monotonic()
        unique = list(dict.fromkeys(keys))
        results, waiting, leaders = {}, {}, {}
        with self.lock:
            self._reset_window(now)
            self.metrics['batches'] += 1
            self.metrics['keys_requested'] += len(keys)
            self.metrics['batch_duplicates'] += len(keys) - len(unique)
            for key in keys:
                self.counts[key] = self.counts.get(key, 0) + 1
                self.counts.move_to_end(key)
                if len(self.counts) > self.tracked_keys:
                    self.counts.popitem(last=False)
                    self.metrics['tracking_evictions'] += 1
            needed = sum(
                (generation, identity, key) not in self.inflight and
                ((generation, identity, key) not in self.cache or
                 self.cache[(generation, identity, key)][0] <= now)
                for key in unique)
            if len(self.inflight) + needed > self.max_inflight:
                self.metrics['admission_rejected'] += 1
                raise Unavailable('in-flight key limit reached')
            for key in unique:
                scoped = (generation, identity, key)
                entry = self.cache.get(scoped)
                if entry is not None and entry[0] > now:
                    self.cache.move_to_end(scoped)
                    results[key] = dict(entry[1])
                    self.metrics['cache_hits'] += 1
                    continue
                if entry is not None:
                    del self.cache[scoped]
                    self.metrics['cache_expired'] += 1
                future = self.inflight.get(scoped)
                if future is None:
                    future = Future()
                    self.inflight[scoped] = future
                    leaders[key] = future
                    self.metrics['cache_misses'] += 1
                else:
                    self.metrics['coalesced_keys'] += 1
                waiting[key] = future

        if leaders:
            try:
                response = self._fetch(list(leaders), generation, identity, candidates)
                with self.lock:
                    for row in response['records']:
                        key = row['key']
                        scoped = (generation, identity, key)
                        if row['found'] and self.capacity:
                            self.cache[scoped] = (time.monotonic() + self.ttl, dict(row))
                            self.cache.move_to_end(scoped)
                            while len(self.cache) > self.capacity:
                                self.cache.popitem(last=False)
                                self.metrics['cache_evictions'] += 1
                        self.inflight.pop(scoped, None)
                        leaders[key].set_result(dict(row))
            except Exception as exc:
                with self.lock:
                    for key, future in leaders.items():
                        self.inflight.pop((generation, identity, key), None)
                        if not future.done():
                            future.set_exception(exc)
                raise
        for key, future in waiting.items():
            try:
                results[key] = dict(future.result(timeout=self.timeout * (len(self.endpoints) * self.attempts + 2)))
            except FutureTimeout as exc:
                raise Unavailable('timed out waiting for an in-flight key') from exc
        return dict(ok=True, generation=generation, snapshot_id=identity,
                    records=[dict(results[key]) for key in keys])

    def _reset_window(self, now):
        if now - self.window_start >= self.window:
            self.counts.clear()
            self.window_start = now

    def statistics(self, top=10):
        """Return process-local window counts; tracked keys are LRU-bounded."""
        with self.lock:
            self._reset_window(time.monotonic())
            ranked = sorted(self.counts.items(), key=lambda item: (-item[1], item[0]))[:top]
            return dict(counters=dict(self.metrics), cache_entries=len(self.cache),
                        in_flight=len(self.inflight), replica_attempts=dict(self.replica_requests),
                        hot_keys=[dict(key=k, count=v) for k, v in ranked],
                        tracked_key_limit=self.tracked_keys, window_seconds=self.window,
                        max_inflight=self.max_inflight)
