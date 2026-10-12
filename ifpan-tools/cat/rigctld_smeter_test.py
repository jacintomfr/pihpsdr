import socket, time
s = socket.create_connection(("127.0.0.1", 4532)); s.settimeout(3)
def cmd(c):
    s.sendall(c); buf = b""
    while not buf.endswith(b"\n"): buf += s.recv(256)
    return buf.decode().strip()
for i in range(5):
    t0 = time.perf_counter(); v = cmd(b"l STRENGTH\n"); dt = (time.perf_counter() - t0) * 1000
    raw = cmd(b"l RAWSTR\n") if i == 0 else ""
    print("STRENGTH=%s dB (rel S9) in %.0f ms %s" % (v, dt, ("RAWSTR=" + raw) if raw else ""))
    time.sleep(0.3)
