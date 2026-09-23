#!/usr/bin/env python3
"""Offline tests of the production client framing, correlation and pagination."""
import asyncio
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import AsyncMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ble_gateway as gateway


class ClientTests(unittest.IsolatedAsyncioTestCase):
    def test_framing(self):
        framer = gateway.RecordFramer()
        payload = b'\x1e{"v":1,"id":7,"ok":true}\n'
        records = []
        for byte in b"boot token=DO-NOT-PRINT\n" + payload + payload:
            records.extend(framer.feed(bytes([byte])))
        self.assertEqual([r["id"] for r in records], [7, 7])
        self.assertEqual(framer.feed(b"logs" * 10000), [])
        with self.assertRaises(gateway.GatewayError):
            framer.feed(b"\x1e" + b"x" * 769)
        self.assertEqual(len(framer.feed(payload)), 1)

    async def test_pagination(self):
        data = bytes(range(256)) * 2
        async def call(operation, **fields):
            self.assertEqual(operation, "ble.gatt.result")
            self.assertEqual(fields["ticket"], 9)
            offset = fields["offset"]
            return dict(ticket=9, generation=3, length=512, offset=offset,
                        data=data[offset:offset + 64].hex())
        endpoint = AsyncMock()
        endpoint.call.side_effect = call
        first = dict(ticket=9, generation=3, length=512, offset=0, data=data[:64].hex())
        result = await gateway.collect_value(endpoint, first, "ble.gatt.result", ticket=9, generation=3)
        self.assertEqual(result, data)
        self.assertEqual(endpoint.call.await_count, 7)
        endpoint.call.return_value = {}
        endpoint.call.side_effect = lambda *a, **kw: dict(first, offset=64, generation=4)
        with self.assertRaisesRegex(gateway.GatewayError, "changed"):
            await gateway.collect_value(endpoint, first, "ble.gatt.result", ticket=9, generation=3)

    async def test_missing_page(self):
        endpoint = AsyncMock()
        for first in (dict(length=1, offset=0, data=""), dict(length=513),
                      dict(length=1, offset=0, data="ffff"), dict(length=1, offset=0, data="xx")):
            with self.assertRaises(gateway.GatewayError):
                await gateway.collect_value(endpoint, first, "ble.gatt.result")
        endpoint.call.assert_not_awaited()

    async def test_results_do_not_resubmit(self):
        endpoint = AsyncMock()
        endpoint.call.side_effect = [dict(state="queued"), dict(state="running"), dict(state="complete")]
        self.assertEqual((await gateway.wait_result(endpoint, 17, 2))["state"], "complete")
        self.assertTrue(all(call.args == ("ble.gatt.result",) for call in endpoint.call.await_args_list))
        endpoint.call.side_effect = None
        endpoint.call.return_value = dict(state="failed", error_code=-5, error_name="timeout")
        with self.assertRaisesRegex(gateway.GatewayError, "timeout"):
            await gateway.wait_result(endpoint, 17, 1)

    async def test_envelope_and_auth(self):
        args = gateway.parser().parse_args(["--url", "http://192.0.2.1", "status"])
        args.token = "1" * 32
        endpoint = gateway.Endpoint(args)
        async def exchange(payload, request_id, timeout):
            request = json.loads(payload)
            self.assertEqual(request["token"], args.token)
            return dict(v=1, id=request_id, ok=True)
        endpoint.exchange = AsyncMock(side_effect=exchange)
        self.assertTrue((await endpoint.call("ble.gatt.status"))["ok"])
        endpoint.exchange.side_effect = None
        endpoint.exchange.return_value = dict(v=1, id=-1, ok=True)
        with self.assertRaisesRegex(gateway.GatewayError, "mismatch"):
            await endpoint.call("ble.gatt.status")
        args.token = None
        with self.assertRaisesRegex(gateway.GatewayError, "M5_API_TOKEN"):
            await endpoint.call("ble.gatt.status")


if __name__ == "__main__":
    unittest.main()
