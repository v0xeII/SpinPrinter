#!/usr/bin/env python3
"""Автотест всего принтера на компьютере, без окон (минуты две):

    python3 dev/selftest.py

Запускает виртуальный ESP32, SpinPi и SpinProj (без картинки, с точными «60 Гц») и проверяет:
  1. печать с экрана ESP: кнопками, как человек;
  2. печать со страницы (HTTP API), ровно N оборотов, доза по кадрам ровная, без пропусков vsync;
  3. ESP «завис» посреди печати: свет гаснет, печать остановлена с ошибкой;
  4. «Стоп» со страницы посреди печати;
  5. тестовые картинки проектора;
  6. первый запуск: своя Wi-Fi сеть, QR-код для телефона, выбор домашней сети (поддельный spinnet).
Это проверка логики и связи. Реальное время проверяется только на самой Pi (spinproj bench).
"""
import http.client
import json
import os
import shutil
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

FILE = "quick_12rpm.mp4"  # 120 frames, 5 s per turn, plan: 2 turns
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("  ✔ " if ok else "  ✘ ") + name + (f"  ({detail})" if detail else ""), flush=True)
    return ok


class Bench:
    def __init__(self):
        self.dir = os.path.join(common.WORK, "selftest")
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.usb = os.path.join(common.WORK, "usb")
        common.make_videos(self.usb, only=[FILE])
        self.usb_one = os.path.join(self.dir, "usb")  # a stick with just the quick video
        os.makedirs(self.usb_one)
        for ext in (".mp4", ".spin.json"):
            os.symlink(os.path.join(self.usb, FILE.replace(".mp4", ext)), os.path.join(self.usb_one, FILE.replace(".mp4", ext)))
        data = common.data_dir(os.path.join(self.dir, "data"), warmup_min=0, delay_s=0.5)
        self.inject = os.path.join(self.dir, "inject.txt")
        self.log = open(os.path.join(self.dir, "selftest.log"), "w")
        self.esp, tty = common.start_esp(["--headless", "--inject", self.inject], log=self.log)
        self.port = common.free_port(18080)
        os.environ["SPINNET"] = os.path.join(common.ROOT, "dev", "fake_spinnet.sh")
        os.environ["SPINNET_STATE"] = os.path.join(self.dir, "net")
        self.pi = common.start_pi(tty, self.port, data, self.usb_one, out="null", projector="640x400", log=self.log)
        self.url = f"http://127.0.0.1:{self.port}"

    def api(self, path, body=None):
        req = urllib.request.Request(self.url + path, data=None if body is None else json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def state(self):
        return self.api("/api/state")

    def keys(self, *lines, gap=0.6):
        for line in lines:
            with open(self.inject, "a") as f:
                f.write(line + "\n")
            time.sleep(gap)

    def wait(self, cond, timeout, step=0.2):
        t = time.time()
        s = None
        while time.time() - t < timeout:
            try:
                s = self.state()
            except OSError:  # SpinPi still starting
                s = None
            if s and cond(s):
                return s
            time.sleep(step)
        return s or self.state()

    def close(self):
        for p in (self.pi, self.esp):
            p.terminate()
        for p in (self.pi, self.esp):
            try:
                p.wait(3)
            except Exception:  # noqa: BLE001
                p.kill()


def main():
    common.say("собираю")
    common.build()
    b = Bench()
    try:
        common.say("запускаю виртуальный ESP32 + SpinPi + SpinProj (журнал:", b.log.name + ")")
        s = b.wait(lambda s: s["esp"] and s["projector"], 15)
        check("SpinPi видит ESP32 и проектор", s["esp"] and s["projector"], f"движок: {s.get('engine')}")
        files = b.api("/api/files")
        f = next((x for x in files if x["name"] == FILE), None)
        check("видео с «флешки» в списке", bool(f), f and f"{f['frames']} кадров, {f['rpm']:.1f} об/мин, план {f.get('plan_revs')} об")
        fid = f["id"]

        print("\n1. Печать с экрана ESP32 кнопками")
        time.sleep(max(0, 3.2 - 0))  # the boot screen leaves by itself after 2.6 s
        b.keys("KEY OK", gap=0.2)  # Печать: the list comes from the Pi (ffprobe on a fresh library can take a while)
        t_list = time.time()
        while "PI > FL END" not in open(b.log.name).read() and time.time() - t_list < 30:
            time.sleep(0.2)
        time.sleep(0.8)
        b.keys("KEY OK", "KEY OK", gap=1.0)  # first video → ПЕЧАТЬ
        s = b.wait(lambda s: s["state"] in ("print", "error"), 30)
        check("кнопка ПЕЧАТЬ на экране запустила печать", s["state"] == "print", s["msg"])
        s = b.wait(lambda s: s["state"] in ("done", "error", "idle"), 30)
        h = s["history"][-1] if s["history"] else {}
        check("печать закончилась сама после 2 оборотов", s["state"] == "done" and h.get("revs") == 2, s["msg"])
        b.keys("KEY OK")  # «Как получилось?» → Норма
        time.sleep(0.8)
        h = b.state()["history"][-1]
        check("ответ «Как получилось?» с экрана записан", h.get("result") == "ok", str(h.get("result")))

        print("\n2. Печать со страницы: 3 оборота, проверка дозы")
        t0 = time.time()
        r = b.api("/api/print", {"id": fid, "revs": 3, "delay_s": 0.5})
        check("страница запустила печать", r["ok"], r.get("error") or "")
        s = b.wait(lambda s: s["state"] in ("done", "error", "idle"), 40)
        took = time.time() - t0
        h = s["history"][-1]
        exp_flips = 3 * 5.0 * 60  # 3 turns x 5 s x 60 Hz
        check("ровно 3 оборота", s["state"] == "done" and h["revs"] == 3, f"{took:.1f} с с разгоном")
        check("свет горел 3 × 5 с (±2 кадра)", abs(h.get("flips", 0) - exp_flips) <= 2, f"{h.get('flips')} кадров, ждали {exp_flips:.0f}")
        check("ни одного пропущенного vsync", h.get("missed_vsync") == 0, str(h.get("missed_vsync")))
        # 60 Hz / 24 fps = 2.5: each frame stays up 2 or 3 refreshes per turn (as with any video
        # player), so 6..9 over 3 turns. The frame where the light came on is lit in pieces in the
        # first and the last turn: it may get one refresh more or less. No frame may be skipped.
        lo, hi = h.get("dose_min", 0), h.get("dose_max", 99)
        check("каждый кадр показан 2–3 обновления за оборот, ни один не пропущен", lo >= 3 * 2 - 1 and hi <= 3 * 3 + 1,
              f"за 3 оборота от {lo} до {hi} обновлений экрана, среднее {h.get('dose_mean')}")

        print("\n3. ESP32 завис посреди печати")
        b.api("/api/print", {"id": fid, "revs": 4, "delay_s": 0.5})
        b.wait(lambda s: s["state"] == "print", 30)
        time.sleep(1.5)
        t_f = time.time()
        b.keys("FAULT freeze", gap=0)
        s = b.wait(lambda s: s["state"] in ("error", "done", "idle"), 6, step=0.05)
        check("печать остановлена с ошибкой", s["state"] == "error", f"«{s['msg']}» через {time.time() - t_f:.1f} с")
        log = open(b.log.name).read()
        check("SpinProj сам погасил свет (угол перестал приходить)", "угол от ESP не приходит" in log)
        b.keys("FAULT ok", gap=1.5)

        print("\n4. Стоп со страницы")
        b.api("/api/print", {"id": fid, "revs": 4, "delay_s": 0.5})
        b.wait(lambda s: s["state"] == "print", 30)
        time.sleep(1.0)
        b.api("/api/stop", {})
        s = b.wait(lambda s: s["state"] != "print", 5, step=0.05)
        check("«Стоп» остановил печать", s["state"] == "idle", s["state"])

        print("\n5. Тестовые картинки проектора")
        for pat in ("grid", "center", "black"):
            r = b.api("/api/projector", {"pattern": pat})
            check(f"картинка «{pat}»", r["ok"], r.get("error") or "")
        time.sleep(0.5)
        check("SpinProj не ругался на картинки", "не читается картинка" not in open(b.log.name).read())

        print("\n6. Первый запуск: своя Wi-Fi сеть")
        w = b.api("/api/wifi?scan=1")
        check("страница видит режим точки доступа и сети вокруг", w.get("mode") == "ap" and len(w.get("networks", [])) == 2,
              ", ".join(n["ssid"] for n in w.get("networks", [])))
        log = open(b.log.name).read()
        check("экран ESP получил QR-код для входа в сеть принтера", "PI > URL WIFI:T:nopass;S:SpinSlicer-TEST;;" in log)
        c = http.client.HTTPConnection("127.0.0.1", b.port, timeout=5)  # what Android asks on joining a Wi-Fi
        c.request("GET", "/generate_204", headers={"Host": "connectivitycheck.gstatic.com"})
        r = c.getresponse()
        check("телефон сам открывает страницу (captive portal)", r.status == 302 and r.getheader("Location") == "http://192.168.4.1/",
              f"{r.status} → {r.getheader('Location')}")
        r = b.api("/api/wifi", {"ssid": "Домашняя сеть", "psk": "12345678"})
        check("выбор домашней сети принят", r["ok"], r.get("error") or "")
        r = b.api("/api/wifi", {"ssid": "x", "psk": "123"})
        check("слишком короткий пароль отклонён", not r["ok"], r.get("error") or "")
    finally:
        b.close()
    bad = [n for n, ok in results if not ok]
    print(f"\nИтог: {len(results) - len(bad)} из {len(results)} проверок прошли.", "Всё хорошо!" if not bad else "Не прошли: " + "; ".join(bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
