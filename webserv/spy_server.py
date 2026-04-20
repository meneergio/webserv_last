#!/usr/bin/env python3
"""
Verbeterde spy: logt alles maar antwoordt correct op chunked requests.
"""
import socket, threading

def handle(conn, addr):
    print(f"\n=== CONNECTIE {addr} ===")
    conn.settimeout(5.0)
    buf = b""
    req_count = 0
    
    try:
        while True:
            try:
                data = conn.recv(4096)
            except socket.timeout:
                print("[timeout]")
                break
            if not data:
                print("[closed by client]")
                break
            buf += data
            print(f"[RECV {len(data)}]: {data!r}")
            
            # Verwerk complete requests
            while True:
                # Zoek header einde
                if b"\r\n\r\n" not in buf:
                    break
                
                hdr_end = buf.index(b"\r\n\r\n")
                hdr = buf[:hdr_end].decode('utf-8', errors='replace')
                lines = hdr.split("\r\n")
                method = lines[0].split(" ")[0] if lines else "?"
                
                # Check transfer encoding en content-length
                te = ""
                cl = 0
                for l in lines[1:]:
                    if l.lower().startswith("transfer-encoding:"):
                        te = l.split(":",1)[1].strip().lower()
                    if l.lower().startswith("content-length:"):
                        try: cl = int(l.split(":",1)[1].strip())
                        except: pass
                
                body_start = hdr_end + 4
                
                if te == "chunked":
                    # Lees chunked body volledig
                    rest = buf[body_start:]
                    # Zoek "0\r\n\r\n" terminator
                    if b"0\r\n\r\n" not in rest:
                        print(f"  [chunked body nog niet compleet, wacht...]")
                        break
                    term_pos = rest.index(b"0\r\n\r\n")
                    body = rest[:term_pos]
                    buf = rest[term_pos + 5:]  # na "0\r\n\r\n"
                    print(f"  [chunked body gelezen: {len(body)} bytes]")
                elif cl > 0:
                    rest = buf[body_start:]
                    if len(rest) < cl:
                        break
                    body = rest[:cl]
                    buf = rest[cl:]
                else:
                    body = b""
                    buf = buf[body_start:]
                
                req_count += 1
                print(f"\n>>> REQUEST #{req_count}: {lines[0]}")
                print(f"    Transfer-Encoding: {te}, Content-Length: {cl}")
                
                # Stuur response
                if method == "GET":
                    resp = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\nHello World!\n"
                elif method == "POST":
                    resp = b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nAllow: GET\r\nConnection: keep-alive\r\n\r\n"
                elif method == "HEAD":
                    resp = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 13\r\nConnection: close\r\n\r\n"
                else:
                    resp = b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n"
                
                print(f"<<< RESPONSE: {resp[:60]!r}...")
                conn.send(resp)
                
                if method == "HEAD" or b"Connection: close" in resp:
                    print("[closing after response]")
                    return
                    
    except Exception as e:
        print(f"[ERROR: {e}]")
    finally:
        conn.close()
        print(f"=== EINDE {addr} (totaal {req_count} requests) ===\n")

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 8080))
s.listen(10)
print("Spy v2 op 127.0.0.1:8080 - run tester nu")
while True:
    conn, addr = s.accept()
    threading.Thread(target=handle, args=(conn,addr), daemon=True).start()