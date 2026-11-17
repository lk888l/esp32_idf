#!/usr/bin/env python3
"""Desktop Chromium and mobile viewport checks against the actual device.
A mobile viewport is not a substitute for Android/iOS hardware acceptance.
Requires playwright, pyserial and websockets (shared hardware-test helper).
"""
import argparse
import asyncio
import json
from pathlib import Path
from playwright.async_api import async_playwright, expect
from web_stream_hardware_test import Device, require

async def main(args):
    device = Device(args.port, args.url)
    report = []
    try:
        device.console.api('web.revoke')
        async with async_playwright() as p:
            browser = await p.chromium.launch(executable_path=args.chromium, headless=True,
                args=['--no-sandbox', '--no-proxy-server'])
            for name, viewport in [('desktop', {'width': 1280, 'height': 920}),
                                   ('mobile_viewport', {'width': 390, 'height': 844})]:
                context = await browser.new_context(viewport=viewport)
                page = await context.new_page()
                errors = []
                page.on('pageerror', lambda error: errors.append(str(error)))
                await page.goto(device.url, wait_until='networkidle')
                code = device.console.api('web.pair')['code']
                await page.locator('#code').fill(str(code))
                await page.locator('#pair button').click()
                await expect(page.locator("#connection")).to_have_text("实时连接", timeout=15000)
                await page.wait_for_timeout(5000)
                require(await page.locator('#roll').inner_text() != '—', 'no live pose')
                require(await page.evaluate('() => document.documentElement.scrollWidth <= innerWidth'), 'horizontal overflow')
                await page.screenshot(path=f'/tmp/sticks3-{name}.png', full_page=True)
                await page.locator('#stop').click()
                await page.wait_for_timeout(1000)
                await page.locator('#start').click()
                await expect(page.locator("#connection")).to_have_text("实时连接", timeout=15000)
                await page.reload(wait_until='networkidle')
                await expect(page.locator("#connection")).to_have_text("实时连接", timeout=15000)
                await context.set_offline(True)
                await page.wait_for_timeout(2000)
                await context.set_offline(False)
                await expect(page.locator("#connection")).to_have_text("实时连接", timeout=15000)
                await page.locator('#revoke').click()
                await page.locator('#pair').wait_for(state='visible')
                require(not errors, 'browser JavaScript error: ' + ';'.join(errors))
                report.append({'case': name, 'passed': True, 'console_errors': errors})
                await context.close()
            await browser.close()
    finally:
        device.console.api('web.revoke')
        device.console.close()
    Path(args.output).write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='/dev/ttyACM0');p.add_argument('--url')
    p.add_argument('--chromium',default='/usr/bin/google-chrome')
    p.add_argument('--output',default='/tmp/sticks3-browser.json')
    asyncio.run(main(p.parse_args()))
