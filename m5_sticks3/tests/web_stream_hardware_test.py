#!/usr/bin/env python3
"""On-device local web/stream regression. Never prints or persists pairing secrets.
Requires pyserial and websockets. Preserves Wi-Fi credentials and radio settings.
USB is the physical authorization channel; all sessions created here are revoked.
"""
import argparse
import asyncio
import json
import statistics
import socket
import urllib.parse
import time
import urllib.error
import urllib.request
from pathlib import Path
from radio_serial_test import Console
from websockets.asyncio.client import connect


def require(value, message):
    if not value:
        raise AssertionError(message)


class Device:
    def __init__(self, port, url):
        self.console = Console(port, 8)
        self.console.ready()
        status = self.console.api('status')
        deadline = time.monotonic() + 45
        while not url and not status['ip']:
            require(time.monotonic() < deadline, 'station has no IP; provide --url for an AP connection')
            time.sleep(.25)
            status = self.console.api('status')
        self.url = (url or 'http://' + status['ip']).rstrip('/')
        self.http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.token = ''
        self.seq = 0

    def request(self, path, payload=None, headers=None):
        data = None if payload is None else json.dumps(payload, separators=(',', ':')).encode()
        request = urllib.request.Request(self.url + path, data=data, headers={
            'Content-Type': 'application/json', **(headers or {})})
        try:
            with self.http.open(request, timeout=8) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    def pair(self):
        code = self.console.api('web.pair')['code']
        status, body = self.request('/api/v1/session', {'code': code})
        require(status == 200, 'physical pairing failed')
        return json.loads(body)['session']

    def command(self, op, seq=None, **fields):
        if seq is None:
            self.seq += 1
            seq = self.seq
        status, body = self.request('/api/v1/control', {'v': 1, 'id': seq, 'op': op, **fields}, {
            'Authorization': 'Bearer ' + self.token, 'X-Command-Seq': str(seq)})
        return status, json.loads(body)

    async def stream(self):
        ws = await connect(self.url.replace('http:', 'ws:') + '/api/v1/stream',
                           origin=self.url, proxy=None, ping_interval=None, close_timeout=1)
        await ws.send(json.dumps({'op': 'subscribe', 'session': self.token}))
        result = json.loads(await asyncio.wait_for(ws.recv(), 5))
        require(result.get('event') == 'subscribed', 'stream subscription failed')
        return ws


async def measure(ws, duration):
    start = time.monotonic()
    times, sequences, arrivals, batches = [], [], [], []
    lost = 0
    while time.monotonic() - start < duration:
        data = json.loads(await asyncio.wait_for(ws.recv(), 5))
        if data.get('event') != 'motion':
            continue
        rows = data['samples']
        require(1 <= len(rows) <= 5, 'batch limit')
        require(data['valid'], 'sensor invalid')
        arrivals.append(time.monotonic())
        batches.append(len(rows))
        for seq, stamp, roll, pitch, yaw, valid in rows:
            require(valid and all(-181 <= a <= 181 for a in (roll, pitch, yaw)), 'invalid pose')
            times.append(stamp)
            sequences.append(seq)
        lost = data['lost']
    gaps = sum(max(0, b-a-1) for a, b in zip(sequences, sequences[1:]))
    require(all(b > a for a, b in zip(times, times[1:])), 'timestamps not monotonic')
    rate = (len(times)-batches[0]) / (arrivals[-1]-arrivals[0])
    require(90 < rate < 110, 'stream rate outside 100 Hz tolerance')
    return {'samples': len(times), 'rate_hz': round(rate, 2), 'sequence_gaps': gaps,
            'device_lost': lost, 'max_batch': max(batches),
            'arrival_interval_p95_ms': round(statistics.quantiles(
                [1000*(b-a) for a, b in zip(arrivals, arrivals[1:])], n=20)[18], 2)}


async def main(args):
    d = Device(args.port, args.url)
    report = {'url': d.url, 'checks': []}
    try:
        require(d.request('/')[0] == 200, 'web page unavailable')
        require(d.request('/app.js')[0] == 200, 'web script unavailable')
        status, body = d.request('/api/v1/web')
        require(status == 200 and json.loads(body)['rate_hz'] == 100, 'capabilities')
        report['checks'].append('embedded_assets_and_capabilities')
        require(d.request('/api/v1/control', {'v': 1, 'id': 1, 'op': 'ping'})[0] == 401,
                'unauthenticated control accepted')
        d.console.api('web.revoke')
        d.token = d.pair()
        code = d.console.api('web.pair')['code']
        for _ in range(5):
            require(d.request('/api/v1/session', {'code': code + 1})[0] == 403, 'bad code accepted')
        require(d.request('/api/v1/session', {'code': code})[0] == 403, 'pairing lockout failed')
        report['checks'].append('physical_pairing_and_attempt_limit')
        # A scan changes an observable accepted counter without changing credentials.
        accepted_before = d.console.api('status')['accepted']
        first = d.command('wifi.scan', seq=1)
        require(first[1].get('result') == 'accepted', 'scan was not accepted')
        require(first == d.command('wifi.scan', seq=1), 'mutation replay differs')
        await asyncio.sleep(.2)
        require(d.console.api('status')['accepted'] == accepted_before + 1, 'duplicate mutation executed twice')
        # Scanning moves the radio off its home channel; wait through that
        # intentional disruption using the independent physical console.
        deadline = time.monotonic() + 15
        while d.console.api('wifi.status')['scanning']:
            require(time.monotonic() < deadline, 'scan did not finish')
            await asyncio.sleep(.25)
        require(d.command('ping', seq=1)[0] == 409, 'sequence reuse not rejected')
        for n in range(2, 35):
            require(d.command('ping', seq=n)[0] == 200, 'control query failed')
        require(d.command('wifi.scan', seq=1)[0] == 409, 'expired replay not rejected')
        d.seq = 34
        require(d.command('web.pair')[1].get('error') == 'physical_only', 'remote pairing escalation')
        report['checks'].append('replay_conflict_expiry_and_physical_boundary')
        try:
            async with connect(d.url.replace('http:', 'ws:') + '/api/v1/stream',
                               origin='http://untrusted.invalid', proxy=None):
                raise AssertionError('foreign origin accepted')
        except Exception as e:
            require('403' in str(e), 'unexpected origin rejection')
        async with connect(d.url.replace('http:', 'ws:') + '/api/v1/stream', origin=d.url,
                           proxy=None, close_timeout=1) as unauthorized:
            await unauthorized.send('{"op":"subscribe","session":"invalid"}')
            try:
                await asyncio.wait_for(unauthorized.recv(), 3)
                raise AssertionError('unauthorized stream accepted')
            except Exception as e:
                require('close' in str(e).lower(), 'unauthorized connection did not close')
        report['checks'].append('websocket_origin_and_authentication')
        one, two = await d.stream(), await d.stream()
        ping = await one.ping(b'')
        await asyncio.wait_for(ping, 3)
        report['checks'].append('empty_ping_pong')
        report['dual_client'] = await asyncio.gather(measure(one, args.duration), measure(two, args.duration))
        await two.close()
        require(two.close_code == 1000, 'normal close handshake failed')
        report['checks'].append('normal_close_handshake')
        report['remaining_client'] = await measure(one, 3)
        await one.close()
        await asyncio.sleep(.5)
        # Fill a receiver's TCP window while another continues reading.
        endpoint = urllib.parse.urlsplit(d.url)
        transport = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        transport.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        transport.settimeout(8)
        transport.connect((endpoint.hostname, endpoint.port or 80))
        transport.setblocking(False)
        slow = await connect(d.url.replace('http:', 'ws:') + '/api/v1/stream', origin=d.url,
                             sock=transport, proxy=None, max_queue=1, ping_interval=None, close_timeout=1)
        await slow.send(json.dumps({'op': 'subscribe', 'session': d.token}))
        await slow.recv()
        fast = await d.stream()
        report['slow_peer_isolation'] = await measure(fast, max(20, args.duration))
        await slow.close()
        await fast.close()
        restored = await d.stream()
        report['reconnect'] = await measure(restored, 3)
        status, _ = d.request('/api/v1/session', {'op': 'revoke'}, {'Authorization': 'Bearer ' + d.token})
        require(status == 200, 'revocation failed')
        require(d.command('ping')[0] == 401, 'revoked session accepted')
        await asyncio.sleep(.3)
        require(restored.state.name == 'CLOSED', 'revoked stream remained active')
        report['checks'].append('session_revoke_closes_stream')
        await asyncio.sleep(.5)
        require(d.console.api('telemetry')['valid'] == 0, 'last subscriber did not release motion')
        report['checks'].append('last_subscriber_releases_sensor')
        report['status_after'] = d.console.api('status')
        d.console.drain(.3)
        report['passed'] = True
    finally:
        d.console.api('web.revoke')
        d.console.close()
        if args.output:
            Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', default='/dev/ttyACM0')
    p.add_argument('--url')
    p.add_argument('--duration', type=float, default=30)
    p.add_argument('--output')
    asyncio.run(main(p.parse_args()))
