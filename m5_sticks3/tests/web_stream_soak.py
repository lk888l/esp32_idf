#!/usr/bin/env python3
"""One-client long run; writes progress every minute and never stores secrets.
Leaves one stream/session slot free. Uses USB only during initial authorization.
Revokes its own session on exit; it doesn't revoke other browsers' sessions.
"""
import argparse
import asyncio
import json
import signal
import time
from pathlib import Path
from web_stream_hardware_test import Device, require

async def main(args):
    d=Device(args.port,args.url)
    try:
        d.token=d.pair()
    finally:
        d.console.close()
    path=Path(args.output)
    report={'state':'running','duration_target_s':args.duration,'url':d.url,'samples':0,
            'gaps':0,'device_lost':0,'measurements':[],'started_unix':time.time()}
    def save():
        temporary=path.with_suffix('.tmp')
        temporary.write_text(json.dumps(report,indent=2)+'\n')
        temporary.replace(path)
    start=time.monotonic();next_check=start;last_seq=None;boot=None
    ws=None
    loop=asyncio.get_running_loop()
    task=asyncio.current_task()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, task.cancel)
    try:
        ws=await d.stream()
        while time.monotonic()-start<args.duration:
            data=json.loads(await asyncio.wait_for(ws.recv(),5))
            if data.get('event')=='motion':
                if boot is None:boot=data['boot']
                require(boot==data['boot'],'device restarted during soak')
                require(data['valid'],'sensor became invalid')
                for row in data['samples']:
                    if last_seq is not None:report['gaps']+=max(0,row[0]-last_seq-1)
                    last_seq=row[0];report['samples']+=1
                report['device_lost']=data['lost']
            now=time.monotonic()
            if now>=next_check:
                status,body=await asyncio.to_thread(d.command,'status')
                require(status==200 and body['ok'],'status heartbeat failed')
                elapsed=now-start
                report['elapsed_s']=round(elapsed,1)
                report['rate_hz']=round(report['samples']/max(elapsed,.001),2)
                report['measurements'].append({'elapsed_s':round(elapsed),'heap':body['heap'],
                    'internal_heap':body['internal_heap'],'rssi':body['rssi'],'uptime_s':body['uptime_s']})
                save()
                next_check=now+60
        report['state']='passed'
    except asyncio.CancelledError:
        report['state']='stopped'
    except Exception as error:
        report['state']='failed';report['error']=str(error)
        raise
    finally:
        report['elapsed_s']=round(time.monotonic()-start,1)
        save()
        if ws:await ws.close()
        d.request('/api/v1/session',{'op':'revoke'},{'Authorization':'Bearer '+d.token})
    print(json.dumps({k:v for k,v in report.items() if k!='measurements'},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='/dev/ttyACM0');p.add_argument('--url')
    p.add_argument('--duration',type=int,default=86400)
    p.add_argument('--output',default='/tmp/sticks3-soak.json')
    asyncio.run(main(p.parse_args()))
