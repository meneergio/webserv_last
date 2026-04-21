#!/usr/bin/env python3
"""Check wat de server NU teruggeeft op HEAD (na de revert)"""
import socket

s = socket.socket()
s.settimeout(3)
s.connect(('127.0.0.1', 8080))

# Fresh connectie, enkele HEAD
s.send(b'HEAD / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n\r\n')
data = b''
try:
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        data += chunk
        if b'\r\n\r\n' in data: break
except: pass

print("HEAD / response:")
print(data.decode('utf-8', errors='replace'))
print()
status = data.split(b'\r\n')[0].decode() if data else '[NO RESPONSE]'
print(f"Status line: {status}")
if '405' in status:
    print(" 405 - dit is wat de tester wil")
elif '200' in status:
    print(" 200 - dit is waarom de tester faalt")
else:
    print(f"  Onverwacht: {status}")
s.close()
