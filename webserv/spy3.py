#!/usr/bin/env python3
"""
Transparante proxy: zit tussen tester en echte server.
Logt exact wat tester stuurt EN wat server teruggeeft.
"""
import socket, threading

TESTER_PORT = 8080      # tester verbindt hiermee
SERVER_HOST = '127.0.0.1'
SERVER_PORT = 8081      # echte server draait hierop

def pipe(src, dst, label):
    try:
        while True:
            data = src.recv(4096)
            if not data:
                print(f"\n[{label}] verbinding gesloten")
                break
            print(f"\n[{label} {len(data)} bytes]:\n{data.decode('utf-8', errors='replace')}")
            print(f"[RAW]: {data!r}")
            dst.send(data)
    except Exception as e:
        print(f"[{label} ERROR]: {e}")
    finally:
        try: src.close()
        except: pass
        try: dst.close()
        except: pass

def handle(tester_conn, addr):
    print(f"\n=== NIEUWE CONNECTIE van tester {addr} ===")
    try:
        server_conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_conn.connect((SERVER_HOST, SERVER_PORT))
    except Exception as e:
        print(f"[FOUT] kan niet verbinden met server: {e}")
        tester_conn.close()
        return

    t1 = threading.Thread(target=pipe, args=(tester_conn, server_conn, "TESTER→SERVER"), daemon=True)
    t2 = threading.Thread(target=pipe, args=(server_conn, tester_conn, "SERVER→TESTER"), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    print(f"=== CONNECTIE {addr} GESLOTEN ===\n")

proxy = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
proxy.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
proxy.bind(('127.0.0.1', TESTER_PORT))
proxy.listen(10)
print(f"Proxy op :{TESTER_PORT} → echte server op :{SERVER_PORT}")
print(f"Start je echte server op poort {SERVER_PORT}")
print(f"Run dan: ./tester http://127.0.0.1:{TESTER_PORT}\n")

while True:
    conn, addr = proxy.accept()
    threading.Thread(target=handle, args=(conn, addr), daemon=True).start()