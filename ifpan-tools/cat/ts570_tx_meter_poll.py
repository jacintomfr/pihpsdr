import socket, time
s = socket.create_connection(("127.0.0.1", 7373), timeout=0.5)
t0 = time.time(); last = None
while time.time() - t0 < 240:
    s.sendall(b"IF;RM;SM;"); time.sleep(0.3)
    try: r = s.recv(512).decode(errors="replace")
    except socket.timeout: r = "(timeout)"
    frames = [f for f in r.split(";") if f]
    d = {}
    for f in frames:
        if f.startswith("IF") and len(f) >= 30: d["TX"] = f[28]; d["MD"] = f[29]
        else: d.setdefault(f[:2], []).append(f)
    line = str(d)
    if line != last:
        print(f"{time.time()-t0:6.1f} {line}", flush=True); last = line
