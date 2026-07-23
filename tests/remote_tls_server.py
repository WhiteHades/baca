#!/usr/bin/env python3

import http.server
import ssl
import sys
import threading


class RemoteHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    http_port = 0
    tls_port = 0

    def log_message(self, _format, *_arguments):
        return

    def send_body(self, status, body=b"", **headers):
        self.send_response(status)
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), value)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        if self.headers.get("User-Agent") != "mereader-tui/0.1.1":
            self.send_body(400)
        elif self.path == "/plain":
            self.send_body(200, b"secure remote text\n", Content_Type="text/plain")
        elif self.path == "/redirect":
            self.send_body(302, Location="/plain")
        elif self.path == "/loop":
            self.send_body(302, Location="/loop")
        elif self.path == "/credentials":
            location = f"https://user:secret@127.0.0.1:{self.tls_port}/plain"
            self.send_body(302, Location=location)
        elif self.path == "/downgrade":
            location = f"http://127.0.0.1:{self.http_port}/plain"
            self.send_body(302, Location=location)
        elif self.path == "/truncated":
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                b"Content-Length: 64\r\nConnection: close\r\n\r\nshort"
            )
            self.wfile.flush()
            self.close_connection = True
        elif self.path == "/interrupted":
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n10\r\npartial"
            )
            self.wfile.flush()
            self.close_connection = True
        else:
            self.send_body(404)


class QuietThreadingHTTPServer(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        return


def main():
    if len(sys.argv) != 4:
        return 2
    port_file, certificate, private_key = sys.argv[1:]
    http_server = QuietThreadingHTTPServer(("127.0.0.1", 0), RemoteHandler)
    tls_server = QuietThreadingHTTPServer(("127.0.0.1", 0), RemoteHandler)
    RemoteHandler.http_port = http_server.server_port
    RemoteHandler.tls_port = tls_server.server_port

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, private_key)
    tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)

    with open(port_file, "w", encoding="ascii") as output:
        output.write(f"{http_server.server_port} {tls_server.server_port}\n")
        output.flush()

    thread = threading.Thread(target=http_server.serve_forever, daemon=True)
    thread.start()
    tls_server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
