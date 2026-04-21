#!/usr/bin/env python3
"""
Simuleer exact wat de tester doet bij youpla.bla:
- Open connectie
- Stuur 100 MB POST body naar /directory/youpla.bla (niet-bestaand bestand)
- Meet hoe lang het duurt, wat er terugkomt
"""
import socket, time

HOST = '127.0.0.1'
PORT = 8080
SIZE = 100_000_000  # 100 MB zoals tester

s = socket.socket()
s.settimeout(30)
s.connect((HOST, PORT))

headers = (
    f'POST /directory/youpla.bla HTTP/1.1\r\n'
    f'Host: 127.0.0.1:8080\r\n'
    f'Content-Length: {SIZE}\r\n'
    f'Content-Type: application/octet-stream\r\n'
    f'\r\n'
)
s.sendall(headers.encode())
print(f"[SENT HEADERS, now sending {SIZE} bytes]")

# Stuur 100 MB in chunks
chunk = b'X' * 65536
sent = 0
start = time.time()
try:
    while sent < SIZE:
        to_send = min(len(chunk), SIZE - sent)
        n = s.send(chunk[:to_send])
        sent += n
        # Elke ~10 MB even status printen
        if sent % (10_000_000) < 65536:
            elapsed = time.time() - start
            print(f"  [{sent:>12} / {SIZE} bytes sent, {elapsed:.1f}s]")
except Exception as e:
    print(f"[SEND ERROR after {sent} bytes: {e}]")

print(f"[TOTAL SENT: {sent}]")
print()
print("[Reading response...]")

data = b''
try:
    while True:
        c = s.recv(4096)
        if not c: break
        data += c
        if b'\r\n\r\n' in data and len(data) > 200:
            break
except socket.timeout:
    print("[READ TIMEOUT]")
except Exception as e:
    print(f"[READ ERROR: {e}]")

print(f"[RESPONSE {len(data)} bytes]:")
print(data[:500].decode('utf-8', errors='replace'))
s.close()
