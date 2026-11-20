#!/usr/bin/env python3
"""Serves the patch editor on this Mac, with a pretend controller behind it,
so the pages can be worked on without the board. Nothing reaches the strip;
saves last until the server stops. Standard library only.

    tools/editor_server.py          then open http://localhost:8000/
"""
import json
import pathlib
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = pathlib.Path(__file__).resolve().parent.parent
FILES = {
    '/': (ROOT / 'main/editor.html', 'text/html'),
    '/patch.js': (ROOT / 'components/patch/patch.js', 'text/javascript'),
    '/api/presets': (ROOT / 'main/presets.json', 'application/json'),
    '/wifi': (ROOT / 'components/wifi/setup.html', 'text/html'),
}
# The WiFi page sees a board that has just joined, as a phone on the hotspot would
WIFI_STATUS = {'state': 'connected', 'hotspot': True, 'ssid': 'HomeWiFi', 'ip': '192.168.1.50', 'error': ''}
TESTS = ['RGB TEST', 'DIM TEST']
LEDS = 10

library = None   # saved JSON text; None = presets
state = {'mode': 0, 'draft': False, 'brightness': 50, 'speed': 1.0, 'leds': LEDS, 'bpm': 124.0, 'beat_confidence': 0.4, 'beat_found': True}


def library_text():
    return library if library is not None else (ROOT / 'main/presets.json').read_text()


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, kind='text/plain'):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        return self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()

    def do_GET(self):
        path = urlparse(self.path).path
        if path in FILES:
            file, kind = FILES[path]
            return self.send(200, file.read_bytes(), kind)
        if path == '/api/patches':
            return self.send(200, library_text(), 'application/json')
        if path == '/api/state':
            names = [p['name'] for p in json.loads(library_text())['patches']] + TESTS
            return self.send(200, json.dumps({**state, 'modes': names}), 'application/json')
        if path == '/wifi/status':
            return self.send(200, json.dumps(WIFI_STATUS), 'application/json')
        if path == '/wifi/scan':
            return self.send(200, json.dumps([{'ssid': 'HomeWiFi', 'rssi': -55, 'secure': True}]), 'application/json')
        self.send(404, 'Not found')

    def do_POST(self):
        global library
        url = urlparse(self.path)
        text = self.body()
        if url.path == '/api/patches':
            if len(text.encode()) > 8192:
                return self.send(400, 'Too big: the limit is 8KB')
            library = text
            state['draft'] = False
            print('Saved library:', len(text), 'bytes', flush=True)
            return self.send(200, 'Saved')
        if url.path == '/api/draft':
            index = parse_qs(url.query).get('index', ['0'])[0]
            state['draft'] = True
            print('Draft playing patch', index, flush=True)
            return self.send(200, 'Playing')
        if url.path == '/api/state':
            change = json.loads(text)
            if 'mode' in change:
                state['mode'], state['draft'] = int(change['mode']), False
            if 'brightness' in change:
                state['brightness'] = max(0, min(100, int(change['brightness'])))
            return self.send(200, 'OK')
        self.send(404, 'Not found')

    def do_DELETE(self):
        if urlparse(self.path).path == '/api/draft':
            state['draft'] = False
            return self.send(200, 'Stopped')
        self.send(404, 'Not found')

    def log_message(self, format, *args):
        pass


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f'Patch editor on http://localhost:{port}/ (pretend controller, {LEDS} LEDs)', flush=True)
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
