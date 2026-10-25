#!/usr/bin/env python3
"""Check actual headless Chromium/Brave playback in an isolated profile.

Only a private fixture clip is served, on loopback. CDP uses anonymous pipes,
not an existing browser or debugging endpoint. This verifies rendered progress
and reports color error; it does not measure interactive playback performance.
"""
import argparse
import base64
import contextlib
import functools
import http.server
import json
import os
import select
import signal
import subprocess
import tempfile
import threading
import time
from pathlib import Path


PAGE = r'''<!doctype html><meta charset="utf-8"><title>gpuKey private playback check</title>
<video muted playsinline></video><canvas></canvas><pre id="result">pending</pre>
<script>
const result = document.querySelector('#result');
(async () => {
  const source = await (await fetch('producer.json')).json();
  const video = document.querySelector('video'), canvas = document.querySelector('canvas');
  canvas.width = source.width; canvas.height = source.height;
  const ctx = canvas.getContext('2d', {colorSpace: 'srgb', willReadFrequently: true});
  const samples = [];
  video.onerror = () => result.textContent = JSON.stringify({error: video.error?.message || 'media error'});
  const callback = async (_, metadata) => {
    try {
      if (metadata.mediaTime >= samples.length * 0.5) {
        ctx.drawImage(video, 0, 0);
        const {data} = ctx.getImageData(0, 0, canvas.width, canvas.height);
        const identity = row => {
          let value = 0;
          for (let bit = 0; bit < 32; ++bit)
            if (data[((row * 8 + 4) * canvas.width + bit * 8 + 4) * 4] >= 128)
              value += 2 ** bit;
          return value;
        };
        const colors = [];
        for (let bar = 0; bar < 8; ++bar) {
          const x = Math.floor((bar + 0.5) * source.pattern_width / 8);
          if (x >= canvas.width) continue;
          const offset = (Math.floor(canvas.height / 4) * canvas.width + x) * 4;
          colors.push({bar, rgb: Array.from(data.slice(offset, offset + 3))});
        }
        const frame = new VideoFrame(video);
        const sample = {time: metadata.mediaTime, identity: identity(0), complement: identity(1),
                        colors, colorSpace: frame.colorSpace.toJSON(), format: frame.format};
        try {
          if (samples.length == 0 && ['I420', 'NV12'].includes(frame.format)) {
            const bytes = new Uint8Array(frame.allocationSize());
            const layout = await frame.copyTo(bytes);
            sample.decodedYuv = colors.map(({bar}) => {
              const x = Math.floor((bar + 0.5) * source.pattern_width / 8);
              const y = Math.floor(canvas.height / 4);
              const Y = bytes[layout[0].offset + y * layout[0].stride + x];
              const c = layout[1].offset + (y >> 1) * layout[1].stride;
              return frame.format == 'NV12' ?
                [Y, bytes[c + (x >> 1) * 2], bytes[c + (x >> 1) * 2 + 1]] :
                [Y, bytes[c + (x >> 1)],
                 bytes[layout[2].offset + (y >> 1) * layout[2].stride + (x >> 1)]];
            });
          }
          samples.push(sample);
        } finally { frame.close(); }
      }
      if (samples.length == 4) {
        video.pause();
        const q = video.getVideoPlaybackQuality();
        const r = video.getBoundingClientRect();
        result.textContent = JSON.stringify({samples, videoRect: {
          x: r.x, y: r.y, width: r.width, height: r.height
        }, quality: {
          totalVideoFrames: q.totalVideoFrames, droppedVideoFrames: q.droppedVideoFrames,
          corruptedVideoFrames: q.corruptedVideoFrames
        }});
      } else video.requestVideoFrameCallback(callback);
    } catch (error) { result.textContent = JSON.stringify({error: String(error)}); }
  };
  video.requestVideoFrameCallback(callback);
  video.src = 'clip.mkv'; video.muted = true;
  await video.play();
})().catch(error => result.textContent = JSON.stringify({error: String(error)}));
</script>'''


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_: object) -> None:
        pass

    def do_GET(self) -> None:
        if self.path == '/__player__.html':
            data = PAGE.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            super().do_GET()


class Protocol:
    def __init__(self, send: int, receive: int):
        self.send, self.receive = send, receive
        self.serial = 0
        self.buffer = bytearray()

    def call(self, method: str, params: dict | None = None, session: str | None = None) -> dict:
        self.serial += 1
        message = {'id': self.serial, 'method': method, 'params': params or {}}
        if session:
            message['sessionId'] = session
        pending = memoryview(json.dumps(message).encode()+b'\0')
        while pending:
            pending = pending[os.write(self.send,pending):]
        deadline = time.monotonic()+10
        while True:
            while b'\0' in self.buffer:
                end = self.buffer.index(0)
                answer = json.loads(self.buffer[:end])
                del self.buffer[:end+1]
                if answer.get('id') == self.serial:
                    if 'error' in answer:
                        raise RuntimeError(answer['error'])
                    return answer.get('result',{})
            left = deadline-time.monotonic()
            if left <= 0 or not select.select([self.receive],[],[],left)[0]:
                raise TimeoutError(f'CDP timeout: {method}')
            data = os.read(self.receive,65536)
            if not data:
                raise EOFError('browser closed its protocol pipe')
            self.buffer.extend(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--browser', required=True)
    parser.add_argument('--browser-arg', action='append', default=[], help='diagnostic browser option')
    parser.add_argument('--label', default='browser')
    args = parser.parse_args()
    if not args.label.replace('-','').replace('_','').isalnum():
        parser.error('label must be a simple filename component')
    root = args.directory.resolve()
    source = json.loads((root/'producer.json').read_text())
    pictures = json.loads((root/'pictures.json').read_text())
    if source['width'] < 256 or source['height'] < 160 or source['pattern_width'] != source['width']:
        raise ValueError('browser color patches require the uncropped scene fixture')
    output = root/f'{args.label}-playback.json'
    if output.exists():
        raise FileExistsError(output)
    handler = functools.partial(Handler,directory=root)
    with http.server.ThreadingHTTPServer(('127.0.0.1',0),handler) as server:
        thread = threading.Thread(target=server.serve_forever,daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='gpukey-browser-') as profile:
                incoming, send = os.pipe()
                receive, outgoing = os.pipe()
                with contextlib.ExitStack() as stack:
                    for fd in (send,receive):
                        stack.callback(os.close,fd)
                    log = stack.enter_context((root/f'{args.label}-playback.log').open('x'))
                    # Remap inherited descriptors in a single-threaded child shell;
                    # no preexec_fn is run after forking this threaded Python process.
                    launcher = ('readfd=$1; writefd=$2; shift 2; '
                                'exec 3<&"$readfd" 4>&"$writefd"; '
                                'if (( readfd > 4 )); then exec {readfd}<&-; fi; '
                                'if (( writefd > 4 )); then exec {writefd}>&-; fi; exec "$@"')
                    command = ['bash','-c',launcher,'--',str(incoming),str(outgoing),args.browser,
                               '--headless','--remote-debugging-pipe',f'--user-data-dir={profile}',
                               '--no-first-run','--no-default-browser-check','--disable-extensions',
                               '--disable-background-networking','--disable-component-update',
                               '--force-color-profile=srgb','--force-device-scale-factor=1',
                               '--autoplay-policy=no-user-gesture-required',*args.browser_arg,'about:blank']
                    try:
                        process = subprocess.Popen(command,pass_fds=(incoming,outgoing),
                                                   stdout=log,stderr=log,start_new_session=True)
                    finally:
                        os.close(incoming)
                        os.close(outgoing)
                    protocol = Protocol(send,receive)
                    try:
                        version = protocol.call('Browser.getVersion')
                        gpu = protocol.call('SystemInfo.getInfo')['gpu']
                        target = protocol.call('Target.createTarget',{'url':'about:blank'})['targetId']
                        session = protocol.call('Target.attachToTarget',{'targetId':target,'flatten':True})['sessionId']
                        protocol.call('Page.navigate',{'url':f'http://127.0.0.1:{server.server_port}/__player__.html'},session)
                        deadline = time.monotonic()+20
                        while True:
                            answer = protocol.call('Runtime.evaluate',{
                                'expression': 'document.querySelector("#result")?.textContent',
                                'returnByValue': True},session)
                            value = answer.get('result',{}).get('value')
                            if value and value != 'pending':
                                result = json.loads(value)
                                break
                            if time.monotonic() >= deadline:
                                raise TimeoutError('video did not complete the playback samples')
                            time.sleep(0.05)
                        result['browser'] = version
                        result['browser_options'] = args.browser_arg
                        result['gpu'] = {key:gpu.get(key) for key in ('devices','featureStatus')}
                        if 'error' not in result:
                            shot = protocol.call('Page.captureScreenshot',{
                                'format':'png', 'captureBeyondViewport':True,
                                'clip':{**result['videoRect'],'scale':1}},session)
                            with (root/f'{args.label}-video.png').open('xb') as image:
                                image.write(base64.b64decode(shot['data']))
                    finally:
                        with contextlib.suppress(Exception):
                            protocol.call('Browser.close')
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid,signal.SIGTERM)
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                os.killpg(process.pid,signal.SIGKILL)
                                process.wait()
        finally:
            server.shutdown()
            thread.join()
    if 'error' not in result:
        samples = result['samples']
        identities = [s['identity'] for s in samples]
        if (len(set(identities)) != 4 or identities != sorted(identities)
                or any(i not in pictures['identities'] for i in identities)
                or any(s['identity'] ^ s['complement'] != 0xffffffff for s in samples)):
            result['error'] = 'rendered identities did not advance correctly'
        expected = [(255,255,255),(255,255,0),(0,255,255),(0,255,0),
                    (255,0,255),(255,0,0),(0,0,255),(0,0,0)]
        errors = [abs(a-b) for sample in samples for color in sample['colors']
                  for a,b in zip(color['rgb'],expected[color['bar']],strict=True)]
        result['rgb_patch_mae'] = sum(errors)/len(errors)
        result['rgb_patch_max'] = max(errors)
    with output.open('x') as report:
        report.write(json.dumps(result,indent=2)+'\n')
    with (root/f'{args.label}-playback.html').open('x') as page:
        page.write(PAGE)
    print(json.dumps(result))
    if 'error' in result:
        raise RuntimeError(result['error'])


if __name__ == '__main__':
    main()
