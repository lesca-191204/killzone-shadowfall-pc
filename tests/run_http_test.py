"""Exercise the native adapter against a real loopback HTTP server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import subprocess
import sys
import threading
import time

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/slow-chunks':
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(64 * 8192))
        self.end_headers()
        try:
            for _ in range(64):
                self.wfile.write(b'x' * 8192)
                self.wfile.flush()
                time.sleep(0.15)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # The adapter is expected to cancel at its total request deadline.

    def do_POST(self):
        if (self.path != '/test?hidden=value' or self.headers.get('X-Test') != 'ok'
                or self.headers.get('X-Inherited') != 'template'
                or self.rfile.read(int(self.headers.get('Content-Length', 0))) != b'ping'):
            self.send_error(400)
            return
        self.send_response(201)
        self.send_header('Content-Length', '4')
        self.send_header('X-Test-Reply', 'ok')
        self.end_headers()
        self.wfile.write(b'pong')

    def log_message(self, *args):
        pass

if __name__ == '__main__':
    with ThreadingHTTPServer(('127.0.0.1', 17800), Handler) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            result = subprocess.run([str(Path(sys.argv[1]).resolve())], timeout=30,
                                    env={**os.environ, 'KZSF_NET_TRACE': '1'}, capture_output=True, text=True)
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode == 0:
                assert 'PRIVATEHTTP rejected-url=gameservice:unresolved' in result.stderr
                assert 'PRIVATEHTTP rejected-url=https://other.invalid/path' in result.stderr
                for hidden in ('query-secret', 'fragment-secret', 'userinfo-secret', 'hidden=value'):
                    assert hidden not in result.stderr
        finally:
            server.shutdown()
            worker.join()
    raise SystemExit(result.returncode)
