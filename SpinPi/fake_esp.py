"""Pretend to be the SpinScreen ESP32 on a pseudo-terminal (for testing SpinPi without hardware)."""
import os, pty, time, threading, sys
master, slave = pty.openpty()
print(os.ttyname(slave), flush=True)
rpm_t, rpm, on, angle, steps_err = 4.0, 0.0, False, 0.0, 0.0
lock = threading.Lock()
def motor():
    global rpm, angle
    last = time.monotonic()
    while True:
        time.sleep(0.002)
        t = time.monotonic(); dt = t - last; last = t
        with lock:
            tgt = rpm_t if on else 0.0
            a = 6.0 * dt
            rpm += max(-a, min(a, tgt - rpm))
            angle += rpm * 6.0 * dt
threading.Thread(target=motor, daemon=True).start()
def w(s): os.write(master, (s + "\n").encode())
INJECT = os.environ.get("INJECT", "inject.txt")
def injector():  # lines written to $INJECT are sent to the Pi as if the user pressed buttons
    while True:
        time.sleep(0.2)
        if os.path.exists(INJECT):
            for l in open(INJECT).read().splitlines():
                if l.strip():
                    print("ESP>", l, flush=True); w(l.strip())
            os.unlink(INJECT)
threading.Thread(target=injector, daemon=True).start()
buf = b""
w("READY SpinScreen fake")
while True:
    buf += os.read(master, 1024)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        line = line.decode().strip()
        if line not in ("PING", "POS"): print("PI>", line, flush=True)
        with lock:
            if line == "PING": w("PONG SpinScreen fake")
            elif line == "POS": w(f"POS {angle:.4f} {rpm:.4f} {1 if on and abs(rpm-rpm_t)<1e-4 else 0}")
            elif line == "ZERO": angle = 0.0; w("ZEROED")
            elif line.startswith("SETANGLE "): angle = float(line[9:]); w("ANGLE")
            elif line.startswith("RPM "): rpm_t = float(line[4:]); w("OK")
            elif line.startswith("SPIN "): on = line[5:] == "1"; w("OK")
            elif line.startswith(("SYNC",)): pass
            else: w("OK")
