"""Serves the serial panel on localhost and opens it in the browser.

Web Serial only works on a secure origin, and http://localhost counts as
one, so the page cannot simply be opened as a file.

    python run_hmi.py          panel for a real board
    python run_hmi.py --demo   panel with simulated data, no board needed
"""
import http.server
import os
import socket
import socketserver
import sys
import threading
import time
import webbrowser

DEFAULT_PORT = 8090


def find_available_port(start_port):
    for port in range(start_port, start_port + 30):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(("127.0.0.1", port)) != 0:
                return port
    return start_port


def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    port = find_available_port(DEFAULT_PORT)
    url = f"http://localhost:{port}/index.html"
    if "--demo" in sys.argv:
        url += "?demo"

    print("=" * 60)
    print(" Arm Controller Serial Panel")
    print("=" * 60)
    print(f"[*] Serving {url}")
    print(" 1. Plug the ESP32-S3 in through its UART USB port")
    print(" 2. Close any other serial monitor on that port")
    print(" 3. In the page, click 'Connect serial' and pick the port")
    print(" 4. Keep this window open. Ctrl + C stops the server")
    print("=" * 60)

    def open_browser():
        time.sleep(1.0)
        webbrowser.open(url)

    threading.Thread(target=open_browser, daemon=True).start()

    class QuietHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, format, *args):
            if len(args) > 1 and str(args[1]) in ("200", "304"):
                return
            super().log_message(format, *args)

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), QuietHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[*] Server stopped.")


if __name__ == "__main__":
    main()
