#!/usr/bin/env python3
"""Exercise an already-connected device's runtime Wi-Fi and recovery AP.
Does not change credentials. Requires pyserial; interrupts active local clients.
Re-enables Wi-Fi in finally if interrupted during the power cycle.
"""
import argparse
import json
import time
import urllib.request
from pathlib import Path
from radio_serial_test import Console, require


def main(args):
    c=Console(args.port,8)
    report={'state':'running','checks':[]}
    off=False
    try:
        c.drain(.5)
        initial=c.api('status')
        require(initial['wifi']=='connected','requires an already-connected station')
        require(c.applied(c.api('wifi.disable'))['state']=='applied','disable failed')
        off=True
        require(c.api('wifi.status')['enabled']==0,'radio did not stop')
        time.sleep(1)
        start=time.monotonic()
        require(c.applied(c.api('wifi.enable'))['state']=='applied','enable failed')
        off=False
        deadline=start+45
        while True:
            status=c.api('status')
            if status['wifi']=='connected' and status['ip']:break
            require(time.monotonic()<deadline,'station did not reconnect')
            time.sleep(.25)
        report['reconnect_s']=round(time.monotonic()-start,2)
        report['checks'].append('wifi_disable_enable_restores_station')
        require(c.applied(c.api('wifi.hotspot'))['state']=='applied','recovery hotspot failed')
        time.sleep(.5)
        status=c.api('status')
        require(status['ap_ip']=='192.168.4.1','recovery AP not available')
        report['checks'].append('recovery_ap_opened')
        report['state']='checking_ap_timeout'
        Path(args.output).write_text(json.dumps(report,indent=2)+'\n')
        url='http://'+status['ip']+'/api/v1/status'
    finally:
        if off:c.applied(c.api('wifi.enable'))
        c.close()
    # USB is free during the AP timeout so other read-only / stream validation
    # can run. No AP client is connected by this test.
    http=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    start=time.monotonic()
    while time.monotonic()-start<150:
        with http.open(url,timeout=8) as response:status=json.load(response)
        require(status['wifi']=='connected','STA lost while AP active')
        if not status['ap_ip']:
            report['ap_idle_close_s']=round(time.monotonic()-start,2)
            require(report['ap_idle_close_s']>=110,'AP closed before recovery window')
            report['checks'].append('unused_ap_closes_without_losing_station')
            report['state']='passed'
            break
        time.sleep(1)
    require(report['state']=='passed','AP did not close (check whether a client joined it)')
    Path(args.output).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='/dev/ttyACM0')
    p.add_argument('--output',default='/tmp/sticks3-wifi-recovery.json')
    main(p.parse_args())
