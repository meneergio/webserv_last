#!/usr/bin/env python3
"""
Doet exact wat de 42 tester doet:
  1. GET / op een fresh connection (keep-alive)
  2. POST / chunked empty body op dezelfde connectie
  3. HEAD / op dezelfde connectie  <-- hier faalt tester

Laat voor elk request precies zien wat webserv terugstuurt.
"""
import socket
import sys

HOST = '127.0.0.1'
PORT = 8080

def read_response(sock, is_head=False):
    """Leest een HTTP response (headers + body op basis van Content-Length)"""
    data = b''
    while b'\r\n\r\n' not in data:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return data, b'', '[TIMEOUT]'
        if not chunk:
            return data, b'', '[CONN CLOSED]'
        data += chunk
    
    hdr_end = data.find(b'\r\n\r\n') + 4
    hdrs = data[:hdr_end]
    body_start = data[hdr_end:]
    
    status_line = hdrs.split(b'\r\n')[0].decode('utf-8', errors='replace')
    
    cl = 0
    for line in hdrs.decode('utf-8', errors='replace').split('\r\n'):
        if line.lower().startswith('content-length:'):
            try: cl = int(line.split(':', 1)[1].strip())
            except: pass
    
    if is_head:
        return hdrs, b'', status_line
    
    body = body_start
    while len(body) < cl:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk: break
        body += chunk
    return hdrs, body[:cl], status_line

def banner(text):
    print()
    print("=" * 70)
    print(text)
    print("=" * 70)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3.0)
s.connect((HOST, PORT))

# ---------------------------------------------------------------
banner("STAP 1: GET /")
req = (b'GET / HTTP/1.1\r\n'
       b'Host: 127.0.0.1:8080\r\n'
       b'User-Agent: Go-http-client/1.1\r\n'
       b'Accept-Encoding: gzip\r\n'
       b'\r\n')
s.sendall(req)
hdrs, body, status = read_response(s)
print(hdrs.decode('utf-8', errors='replace'), end='')
print(f"[body: {len(body)} bytes]")
print(f">>> STATUS: {status}")

# ---------------------------------------------------------------
banner("STAP 2: POST / met Transfer-Encoding: chunked (empty body)")
req = (b'POST / HTTP/1.1\r\n'
       b'Host: 127.0.0.1:8080\r\n'
       b'User-Agent: Go-http-client/1.1\r\n'
       b'Accept-Encoding: gzip\r\n'
       b'Transfer-Encoding: chunked\r\n'
       b'\r\n'
       b'0\r\n'
       b'\r\n')
s.sendall(req)
hdrs, body, status = read_response(s)
print(hdrs.decode('utf-8', errors='replace'), end='')
print(f"[body: {body[:120].decode('utf-8', errors='replace')}... ({len(body)} bytes)]")
print(f">>> STATUS: {status}")

# ---------------------------------------------------------------
banner("STAP 3: HEAD /    <-- HIER FAALT DE TESTER")
req = (b'HEAD / HTTP/1.1\r\n'
       b'Host: 127.0.0.1:8080\r\n'
       b'User-Agent: Go-http-client/1.1\r\n'
       b'Accept-Encoding: gzip\r\n'
       b'\r\n')
s.sendall(req)
hdrs, body, status = read_response(s, is_head=True)
print(hdrs.decode('utf-8', errors='replace'), end='')
print(f">>> STATUS: {status}")

# ---------------------------------------------------------------
banner("EINDRESULTAAT")
if '200' in status:
    print("  HEAD 200 op keep-alive connectie: jouw webserv is CORRECT.")
    print("   De tester moet dus iets OK zien. Probleem ligt elders (TCP-timing,")
    print("   parallelle requests, of de tester gebruikt een nieuwe connectie).")
else:
    print(f"  HEAD returned: {status}")
    print("   DIT is de bug die de tester ook ziet.")

s.close()
