import socket, struct, time, sys, threading
host, port, secs = sys.argv[1], 9999, int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect((host, port))
s.settimeout(0.5)
pad = b'x' * 1380
sent = 0
got = {}
stop = False

def rx():
    while not stop:
        try:
            d = s.recv(2048)
        except socket.timeout:
            continue
        except OSError:
            continue
        seq, t = struct.unpack('!Id', d[:12])
        got[seq] = time.perf_counter() - t

th = threading.Thread(target=rx, daemon=True); th.start()
t0 = time.perf_counter()
nxt = t0
while time.perf_counter() - t0 < secs:
    s.send(struct.pack('!Id', sent, time.perf_counter()) + pad)
    sent += 1
    nxt += 0.01
    d = nxt - time.perf_counter()
    if d > 0:
        time.sleep(d)
time.sleep(1.0)
stop = True
lost = [i for i in range(sent) if i not in got]
rtts = sorted(got.values())
slow = [(i, got[i]) for i in sorted(got) if got[i] > 0.05]
print(f"sent {sent}, received {len(got)}, lost {len(lost)} ({100.0*len(lost)/max(sent,1):.3f} %)")
if rtts:
    print(f"rtt ms: min {rtts[0]*1000:.2f} median {rtts[len(rtts)//2]*1000:.2f} p99 {rtts[int(len(rtts)*0.99)]*1000:.2f} max {rtts[-1]*1000:.2f}")
print(f"rtt > 50 ms: {len(slow)}")
for i, r in slow[:20]:
    print(f"  seq {i} at {i/100:.2f} s: {r*1000:.0f} ms")
# group lost into bursts
bursts = []
for i in lost:
    if bursts and i == bursts[-1][1] + 1:
        bursts[-1][1] = i
    else:
        bursts.append([i, i])
print(f"loss bursts: {len(bursts)}")
for a, b in bursts[:20]:
    print(f"  at {a/100:.2f} s: {b-a+1} packets ({(b-a+1)*10} ms)")
