import socket, time
s = socket.create_connection(("127.0.0.1", 7373), timeout=0.3)
def tx(c, wait=0.4):
    s.sendall(c.encode()); end = time.time() + wait; buf = ""
    while time.time() < end:
        try: buf += s.recv(512).decode(errors="replace")
        except socket.timeout: pass
    return [f for f in buf.split(';') if f]
try:
    print("IF", tx("IF;"))
    print("MD5", tx("MD5;"), tx("MD;"))
    for p in ("010", "020"):
        tx("PC%s;" % p); print("PC", tx("PC;"))
        tx("TX;", 0.8)
        print("  SM", tx("SM;"), tx("SM;"), "RM", tx("RM1;"), tx("RM;"))
        tx("RX;", 0.6)
finally:
    s.sendall(b"RX;"); time.sleep(0.3)
    tx("PC025;"); print("AM PC", tx("PC;"))
    print("MD2", tx("MD2;"), tx("MD;"), "PC", tx("PC;"))
