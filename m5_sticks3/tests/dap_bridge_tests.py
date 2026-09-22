#!/usr/bin/env python3
import asyncio
import pathlib
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import dap_bridge as dap

class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_split_and_coalesced_frames(self):
        reader = asyncio.StreamReader()
        wire = dap.frame(b"\x00\xff") + dap.frame(b"\x02\x01")
        async def feed():
            for byte in wire:
                reader.feed_data(bytes([byte]))
                await asyncio.sleep(0)
            reader.feed_eof()
        task = asyncio.create_task(feed())
        self.assertEqual(await dap.read_frame(reader), b"\x00\xff")
        self.assertEqual(await dap.read_frame(reader), b"\x02\x01")
        with self.assertRaises(asyncio.IncompleteReadError): await dap.read_frame(reader)
        await task

    async def test_bad_headers(self):
        for signature, length, kind, reserved in [(b"bad!",2,1,0),(b"DAP\0",0,1,0),
              (b"DAP\0",65,1,0),(b"DAP\0",2,2,0),(b"DAP\0",2,1,1)]:
            reader = asyncio.StreamReader()
            reader.feed_data(dap.HEADER.pack(signature,length,kind,reserved))
            reader.feed_eof()
            with self.assertRaises(ValueError): await dap.read_frame(reader)

    async def test_ble_mtu23_and_retained_response(self):
        class Client:
            mtu_size=23
            def __init__(self): self.writes=[]; self.reads=0
            async def write_gatt_char(self, uuid, value, response):
                assert uuid==dap.RX_UUID and response and len(value)<=20
                self.writes.append(value)
            async def read_gatt_char(self, uuid):
                assert uuid==dap.TX_UUID
                self.reads+=1
                if self.reads==1: return struct.pack('<HBH',0,1,1)+b'\x00' # old reply
                if self.reads==2: return struct.pack('<HBH',1,0,0) # busy
                return struct.pack('<HBH',1,1,2)+b'\x05\x00'
        client=Client()
        packet=b'\x05'+bytes(range(63))
        self.assertEqual(await dap.BleDap(client).exchange(packet), b'\x05\x00')
        self.assertEqual(len(client.writes),4)
        self.assertEqual(b''.join(part[4:] for part in client.writes),packet)
        self.assertEqual([part[2] for part in client.writes],[0,16,32,48])

    async def test_timeout_never_replays_write(self):
        class Client:
            mtu_size=23
            def __init__(self): self.writes=0
            async def write_gatt_char(self,*args,**kwargs): self.writes+=1
            async def read_gatt_char(self,*args): return struct.pack('<HBH',1,0,0)
        client=Client()
        with self.assertRaises(TimeoutError): await dap.BleDap(client,0.025).exchange(b'\x05\x00\x00')
        self.assertEqual(client.writes,1)

    async def test_bridge_pairs_before_listen_and_releases_on_close(self):
        paired=asyncio.Event(); listening=asyncio.Event(); released=asyncio.Event(); cleanup=asyncio.Event()
        instance=None; server=None
        original_start=asyncio.start_server
        class Client:
            mtu_size=23
            def __init__(self,*args,**kwargs):
                nonlocal instance
                instance=self; self.callback=kwargs['disconnected_callback']
                self.services=SimpleNamespace(get_service=lambda _:object())
                self.is_connected=False; self.disconnects=0; self.value=b''
            async def __aenter__(self):
                await paired.wait(); self.is_connected=True; return self
            async def __aexit__(self,*args): self.is_connected=False
            async def write_gatt_char(self,uuid,value,response):
                sequence,offset,total=struct.unpack_from('<HBB',value)
                assert offset==0
                command=value[4]
                if command==3:
                    self.disconnects+=1
                    if self.disconnects==2:
                        released.set()
                        await cleanup.wait()
                    result=b'\x03\x00'
                else: result=b'\x00\x02\x40\x00'
                self.value=struct.pack('<HBH',sequence,1,len(result))+result
            async def read_gatt_char(self,uuid): return self.value
        async def start(*args,**kwargs):
            nonlocal server
            server=await original_start(*args,**kwargs); listening.set(); return server
        args=SimpleNamespace(address='test',timeout=1,port=0)
        with patch.dict(sys.modules,{'bleak':SimpleNamespace(BleakClient=Client)}), \
             patch.object(dap.asyncio,'start_server',start):
            task=asyncio.create_task(dap.run_bridge(args))
            await asyncio.sleep(0)
            self.assertFalse(listening.is_set())
            paired.set(); await asyncio.wait_for(listening.wait(),1)
            port=server.sockets[0].getsockname()[1]
            reader,writer=await asyncio.open_connection('127.0.0.1',port)
            writer.write(dap.frame(b'\x00\xff')); await writer.drain()
            self.assertEqual(await dap.read_frame(reader,2,1),b'\x00\x02\x40\x00')
            other_reader,other_writer=await asyncio.open_connection('127.0.0.1',port)
            self.assertEqual(await asyncio.wait_for(other_reader.read(),1),b'')
            other_writer.close(); await other_writer.wait_closed()
            writer.close(); await writer.wait_closed()
            await asyncio.wait_for(released.wait(),1)
            self.assertTrue(instance.is_connected)
            # The next debugger arrives while the old session is releasing SWD.
            next_reader,next_writer=await asyncio.open_connection('127.0.0.1',port)
            next_writer.write(dap.frame(b'\x00\xff')); await next_writer.drain()
            await asyncio.sleep(0.01)
            cleanup.set()
            self.assertEqual(await dap.read_frame(next_reader,2,1),b'\x00\x02\x40\x00')
            next_writer.close(); await next_writer.wait_closed()
            instance.callback(instance)
            with self.assertRaises(ConnectionError): await asyncio.wait_for(task,1)
            self.assertFalse(instance.is_connected)

    def test_response_validation(self):
        with self.assertRaises(ValueError): dap.response_packet(b'',1)
        with self.assertRaises(ValueError): dap.response_packet(struct.pack('<HBH',1,1,65)+bytes(65),1)
        with self.assertRaises(ValueError): dap.response_packet(struct.pack('<HBH',1,2,0),1)
        self.assertIsNone(dap.response_packet(struct.pack('<HBH',2,1,1)+b'\x00',1))
        with self.assertRaises(ValueError): list(dap.fragments(bytes(65),1,23))
        for mtu in (23,64,259):
            packet=bytes(range(64)); parts=list(dap.fragments(packet,65535,mtu))
            self.assertEqual(b''.join(p[4:] for p in parts),packet)
            self.assertTrue(all(len(p)<=mtu-3 for p in parts))

if __name__=='__main__': unittest.main()
