import http.server
import ssl
import sys

# Bind to 0.0.0.0 for LAN mobile camera testing, or 127.0.0.1 for local isolation
bind_address = sys.argv[1] if len(sys.argv) > 1 else '0.0.0.0'
port = 8443

handler = http.server.SimpleHTTPRequestHandler
httpd = http.server.HTTPServer((bind_address, port), handler)

import os

dir_path = os.path.dirname(os.path.realpath(__file__))
cert_path = os.path.join(dir_path, 'cert.pem')
key_path = os.path.join(dir_path, 'key.pem')

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile=cert_path, keyfile=key_path)
httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

print(f"Serving HTTPS on {bind_address}:{port}...")
httpd.serve_forever()
