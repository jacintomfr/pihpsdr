import socket, time
s = socket.create_connection(("127.0.0.1", 7373), timeout=0.3)
def tx(c, wait=0.6):
    s.sendall(c.encode()); end = time.time() + wait; buf = ""
    while time.time() < end:
        try: buf += s.recv(512).decode(errors="replace")
        except socket.timeout: pass
    return [f for f in buf.split(';') if f.startswith(("AC", "?"))]
print("read         ", tx("AC;"))
print("AC 00 (off)  ", tx("AC 00;"), "-> read", tx("AC;"), tx("AC;"))
print("AC 10 (on)   ", tx("AC 10;"), "-> read", tx("AC;"), tx("AC;"))
