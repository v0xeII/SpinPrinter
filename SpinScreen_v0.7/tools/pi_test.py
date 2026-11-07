"""Pretend to be the Raspberry Pi, from your PC, to test SpinScreen without a Pi.

The screen gets a video list, the QR code, a full fake print (spin-up, dark pause,
turns, progress, "How did it come out?") - everything reacts like with the real Pi.
If a motor is wired, it really spins during the fake print.

    pip install pyserial
    python pi_test.py COM5          # Windows: the port from Arduino IDE -> Tools -> Port
    python pi_test.py /dev/ttyACM0  # Linux / Mac

Type a command and Enter to send it by hand (e.g.  MSG Привет), Ctrl+C to quit.
"""
import sys
import threading
import time

import serial

VIDEOS = [  # rpm, usb?, planned turns (-1 = none), tolerance %, seconds per turn, frames ok?, name
    (5.00, 1, 6.40, 4.0, 12.0, 1, "benchy_spinslicer.mp4"),
    (5.00, 0, 5.35, 6.0, 12.0, 1, "lattice_cube_1280x800.mp4"),
    (9.00, 1, -1, -1, 6.667, 0, "opencal_thinker_9rpm.mp4"),
]
factor_pct = 0.0
stop = threading.Event()
port = None
lock = threading.Lock()


def send(line):
    with lock:
        port.write((line + "\n").encode("utf-8"))


def fake_print(i, turns, factor, delay):
    rpm, usb, plan, tol, rev_s, cad, name = VIDEOS[i]
    stop.clear()
    send(f"FILE {name}")
    send(f"TURNS {turns}")
    send(f"RPM {rpm:.4f}")
    send("SPIN 1")
    send("STATE PRINT")
    for kind, secs in (("spin", 2), ("dark", int(delay))):
        for s in range(secs, 0, -1):
            if stop.is_set():
                break
            send(f"WAIT {s} {kind}")
            time.sleep(1)
    total = turns * rev_s
    t0 = time.time()
    while not stop.is_set():
        el = time.time() - t0
        if el >= total:
            break
        send(f"PROG {100 * el / total:.1f} {int(total - el + 0.99)}")
        send(f"SYNC {3 + int(el * 7) % 5}")
        time.sleep(0.5)
    send("SPIN 0")
    send("STATE IDLE" if stop.is_set() else "STATE DONE")


def on_event(ev):
    global factor_pct
    p = ev.split()
    if p[0] == "LIST":
        send(f"FL BEGIN {len(VIDEOS)} {factor_pct:.0f} 3 5")
        for i, v in enumerate(VIDEOS):
            send("FL {} {:.2f} {} {:.2f} {:.1f} {:.3f} {} {}".format(i, *v))
        send("FL END")
    elif p[0] == "PRINT" and len(p) >= 5:
        threading.Thread(target=fake_print, args=(int(p[1]), int(p[2]), float(p[3]), float(p[4])),
                         daemon=True).start()
    elif p[0] == "STOP":
        stop.set()
    elif p[0] == "RESULT":
        factor_pct += {"under": 5, "over": -5}.get(p[1], 0)
        send(f"MSG Поправка экспозиции: {factor_pct:+.0f}%")
    elif p[0] in ("REBOOT", "POWEROFF"):
        send("MSG (тест: Pi не трогаю)")


def reader():
    while True:
        line = port.readline().decode("utf-8", "replace").strip()
        if not line or line.startswith("POS"):
            continue
        if line not in ("PONG SpinScreen", "OK") and not line.startswith("PONG"):
            print("ESP:", line)
        if line.startswith("EV "):
            on_event(line[3:])


def main():
    global port
    if len(sys.argv) < 2:
        print(__doc__)
        return
    port = serial.Serial(sys.argv[1], 115200, timeout=0.2)
    threading.Thread(target=reader, daemon=True).start()
    time.sleep(1.5)
    for line in ("URL http://192.168.1.42", "HOST spinslicer.local", "NET тест с компьютера", "CAL 0 400"):
        send(line)

    def pinger():
        while True:
            send("PING")
            time.sleep(1)

    threading.Thread(target=pinger, daemon=True).start()
    print("Готово: на экране должно быть «Pi подключена». Жми кнопки на принтере.")
    while True:
        try:
            line = input().strip()
        except (EOFError, KeyboardInterrupt):
            return
        if line:
            send(line)


if __name__ == "__main__":
    main()
