#!/usr/bin/env python3
import socket

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', 8080))
s.settimeout(3.0)

# Stuur HEAD direct
s.send(b'HEAD / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUser-Agent: Go-http-client/1.1\r\n\r\n')

resp = b""
try:
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
except:
    pass

print(f"HEAD response ({len(resp)} bytes):")
print(repr(resp))
print()
print("Decoded:")
print(resp.decode('utf-8', errors='replace'))
s.close()
