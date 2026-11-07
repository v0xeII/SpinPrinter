#!/usr/bin/env python3
"""Весь принтер на компьютере, без железа.

    python3 dev/bench.py

Откроются:
  * окно «виртуальный ESP32»: настоящий интерфейс экрана, кнопки ↑ ↓ Enter, Esc = назад;
  * окно «проектор»: что SpinProj показал бы на проекторе (кадр по углу флакона);
  * страница принтера в браузере (как http://spinslicer.local на настоящем принтере).
В «флешке» лежат тестовые видео. На них стрелка в углу кадра должна смотреть туда же, куда
стрелка флакона в окне ESP32: так видно, что кадры идут по углу.

Остановить: Ctrl+C в этом терминале.
"""
import argparse
import os
import signal
import sys
import time
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--headless", action="store_true", help="без окон (ESP и проектор невидимы)")
    ap.add_argument("--no-browser", action="store_true", help="не открывать браузер")
    ap.add_argument("--port", type=int, default=0, help="порт страницы (по умолчанию первый свободный с 8080)")
    a = ap.parse_args()

    common.say("собираю SpinProj и виртуальный ESP32")
    common.build()
    usb = os.path.join(common.WORK, "usb")
    common.make_videos(usb, only=[v[0] for v in common.VIDEOS[:2]])
    data = common.data_dir(os.path.join(common.WORK, "data"), warmup_min=0.1, delay_s=2)
    port = a.port or common.free_port()
    logf = open(os.path.join(common.WORK, "bench.log"), "w")

    esp, tty = common.start_esp(["--headless"] if a.headless else [], log=logf)
    common.say("виртуальный ESP32 на порту", tty)
    pi = common.start_pi(tty, port, data, usb, out="null" if a.headless else "sdl", log=logf)
    url = f"http://localhost:{port}"
    time.sleep(1.5)
    if pi.poll() is not None:
        sys.exit("SpinPi не запустился, смотри " + logf.name)
    common.say("страница принтера:", url)
    common.say("журнал:", logf.name)
    if not a.no_browser:
        webbrowser.open(url)
    print("""
  Что попробовать:
   1. На экране ESP: OK (Печать) → видео → OK на розовой кнопке ПЕЧАТЬ.
   2. Или на странице: выбрать видео → «Печать».
   3. Во время печати в окне ESP нажать F (ESP «завис»): свет должен сразу погаснуть,
      на экране и странице будет ошибка. Ещё раз F: ESP снова на связи.
   4. Сервис → Проектор → Сетка / Центровка: картинки в окне проектора.
  Ctrl+C: остановить всё.
""", flush=True)
    try:
        while esp.poll() is None and pi.poll() is None:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    for p in (pi, esp):
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
    for p in (pi, esp):
        try:
            p.wait(3)
        except Exception:  # noqa: BLE001
            p.kill()
    common.say("остановлено")


if __name__ == "__main__":
    main()
