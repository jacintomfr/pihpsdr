import os, termios, time, sys
fd = os.open("/dev/ttyUSB0", os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
a[4] = a[5] = termios.B57600
termios.tcsetattr(fd, termios.TCSANOW, a)
termios.tcflush(fd, termios.TCIOFLUSH)
def q(cmd):
    os.write(fd, cmd.encode()); buf = b""; t = time.time()
    while time.time() - t < 0.4:
        try: buf += os.read(fd, 64)
        except BlockingIOError: time.sleep(0.01)
        if buf.endswith(b";"): break
    t1 = time.time() - t
    print("%-6s -> %-20s (%.0f ms)" % (cmd, buf.decode(errors="replace"), t1 * 1000))
for c in ["AI;", "FA;", "MD;", "SM;", "SM0;", "ID;", "IF;"]:
    q(c)
os.close(fd)
