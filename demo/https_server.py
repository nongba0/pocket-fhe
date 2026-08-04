import http.server
import ssl
import sys

# Bind to 0.0.0.0 for LAN mobile camera testing, or 127.0.0.1 for local isolation
bind_address = sys.argv[1] if len(sys.argv) > 1 else '0.0.0.0'
port = 8443

handler = http.server.SimpleHTTPRequestHandler
httpd = http.server.HTTPServer((bind_address, port), handler)

import os
import mimetypes

mimetypes.add_type('application/javascript', '.mjs')
mimetypes.add_type('application/wasm', '.wasm')

dir_path = os.path.dirname(os.path.realpath(__file__))
os.chdir(dir_path)

cert_path = os.path.join(dir_path, 'cert.pem')
key_path = os.path.join(dir_path, 'key.pem')

if not os.path.exists(cert_path) or not os.path.exists(key_path):
    print("SSL certificate not found. Generating temporary self-signed cert.pem / key.pem...")
    import subprocess
    try:
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key_path, "-out", cert_path, "-days", "365",
            "-nodes", "-subj", "/CN=localhost"
        ], check=True)
    except Exception as err:
        print(f"Warning: Could not auto-generate SSL cert via openssl ({err}). Please provide cert.pem and key.pem manually.")

class CustomHandler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        # Remove trailing query strings
        path_only = path.split('?')[0]
        # Allow /demo/ prefix or root /
        if path_only.startswith('/demo/'):
            path_only = path_only[5:]
        if path_only == '/' or path_only == '':
            path_only = '/index.html'
        return super().translate_path(path_only)

httpd = http.server.HTTPServer((bind_address, port), CustomHandler)

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile=cert_path, keyfile=key_path)
httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

print(f"Serving HTTPS on https://localhost:{port}/ (or https://127.0.0.1:{port}/)...")
httpd.serve_forever()
