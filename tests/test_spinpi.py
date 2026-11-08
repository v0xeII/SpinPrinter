"""SpinPi on a PC, no Pi, no ESP32, no ffmpeg:  python -m unittest discover tests

.spin files (reading, checking, unpacking, the library, uploads), the turning direction, the
printer profile, the print log and the mDNS service file.
"""
import hashlib
import http.client
import io
import json
import os
import queue
import shutil
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SpinPi"))
import spinpi  # noqa: E402

PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 32  # enough for "is it a PNG"
VIDEO = b"\0\0\0\x18ftypisom" + os.urandom(4000)  # SpinPi never decodes it in these tests


def rb(p):
    with open(p, "rb") as f:
        return f.read()


def wb(p, data):
    with open(p, "wb") as f:
        f.write(data)


def rt(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def spin_meta(video=VIDEO, **over):
    meta = {
        "format": "spin", "version": 1, "created": "2026-11-07T21:40:00Z", "generator": "SpinSlicer 0.7.0",
        "name": "lattice_cube",
        "video": {"file": "video.mp4", "sha256": hashlib.sha256(video).hexdigest(), "frames": 1200, "fps": 30,
                  "width": 1280, "height": 800, "frames_per_rev": 1200, "rotation": "ccw_from_top",
                  "frame0_angle_deg": 0.0},
        "plan": {"revolutions": 5.35, "tolerance_pct": 6.0, "revolution_s": 40.0, "print_time_s": 214.0, "rpm": 1.5,
                 "warnings": ["thin wall"]},
        "resin": {"name": "Anycubic Clear", "alpha_per_mm": 0.08, "gel_dose_mj_cm3": 12.0},
        "printer": {"profile_id": "spinslicer-3f2a", "vial_diameter_mm": 25.0},
        "something_new": {"from": "a newer slicer"},
    }
    for k, v in over.items():
        if k.startswith("video_"):
            meta["video"][k[6:]] = v
        else:
            meta[k] = v
    return meta


def make_spin(path, meta=None, video=VIDEO, preview=PNG, slicer=True, skip=()):
    meta = spin_meta(video) if meta is None else meta
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        if "spin.json" not in skip:
            z.writestr("spin.json", meta if isinstance(meta, (str, bytes)) else json.dumps(meta))
        if "video.mp4" not in skip:
            z.writestr("video.mp4", video)
        if preview is not None:
            z.writestr("preview.png", preview)
        if slicer:
            z.writestr("slicer.json", json.dumps({"layer": 1}))
    return path


def spin_bytes(**kw):
    buf = io.BytesIO()
    make_spin(buf, **kw)
    return buf.getvalue()


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="spinpi-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self._probe = spinpi.probe
        spinpi.probe = self.fake_probe  # no ffprobe on a PC
        self.addCleanup(setattr, spinpi, "probe", self._probe)
        self.probed = []

    def fake_probe(self, path):
        self.probed.append(path)
        info = {"frames": 120, "fps": 24.0, "width": 640, "height": 400}
        if os.path.basename(path).startswith("tagged"):  # the SpinSlicer JSON in the MP4 comment tag
            info["plan"] = {"format": "spinslicer-print", "version": 1, "revolutions": 3.0, "tolerance_pct": 4.0,
                            "frames_per_rev": 60, "rotation": "cw_from_top", "frame0_angle_deg": 45.0}
        return info

    def path(self, *p):
        return os.path.join(self.tmp, *p)


# ------------------------------------------------------------------------------------------ format
class SpinFormat(Tmp):
    def test_good_file(self):
        meta = spinpi.read_spin(make_spin(self.path("a.spin")))
        self.assertEqual(meta["name"], "lattice_cube")
        plan = spinpi.spin_plan(meta)
        self.assertEqual(plan["revolutions"], 5.35)
        self.assertEqual(plan["frames_per_rev"], 1200)
        self.assertEqual(plan["rotation"], "ccw_from_top")
        self.assertEqual(plan["printer_profile_id"], "spinslicer-3f2a")
        self.assertEqual(plan["resin"]["gel_dose_mj_cm3"], 12.0)
        self.assertEqual(plan["format"], "spin")

    def test_spin_json_first_and_stored(self):
        with zipfile.ZipFile(make_spin(self.path("a.spin"))) as z:
            self.assertEqual(z.namelist()[0], "spin.json")
            self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))

    def bad(self, msg, **kw):
        p = make_spin(self.path("bad.spin"), **kw)
        with self.assertRaises(spinpi.SpinError) as c:
            spinpi.read_spin(p)
        self.assertIn(msg, str(c.exception))

    def test_rejects(self):
        self.bad("format", meta=spin_meta(format="gcode"))
        self.bad("версии 2", meta=spin_meta(version=2))
        self.bad("версии", meta=spin_meta(version="1"))
        self.bad("направление", meta=spin_meta(video_rotation="sideways"))
        self.bad("frames_per_rev", meta=spin_meta(video_frames_per_rev=0))
        self.bad("fps", meta=spin_meta(video_fps=-30))
        self.bad("SHA-256", meta=spin_meta(video_sha256="xyz"))
        self.bad("plan.revolutions", meta=spin_meta(plan={"revolutions": -1}))
        self.bad("нет spin.json", skip=("spin.json",))
        self.bad("нет видео", skip=("video.mp4",))
        self.bad("не JSON", meta="{not json")

    def test_not_a_zip(self):
        p = self.path("x.spin")
        wb(p, b"this is no zip")
        with self.assertRaises(spinpi.SpinError):
            spinpi.read_spin(p)

    def test_null_plan_is_fine(self):  # no resin data in the slicer: no plan, the user picks turns
        meta = spin_meta(plan={"revolutions": None, "print_time_s": None, "tolerance_pct": 6.0})
        spinpi.read_spin(make_spin(self.path("a.spin"), meta=meta))

    def test_extract_checks_sha(self):
        meta = spin_meta(video=b"other bytes")
        p = make_spin(self.path("a.spin"), meta=meta)
        with self.assertRaises(spinpi.SpinError) as c:
            spinpi.extract_spin(p, self.path("out"))
        self.assertIn("SHA-256", str(c.exception))
        self.assertFalse(os.path.exists(self.path("out", "video.mp4")))

    def test_extract(self):
        spinpi.extract_spin(make_spin(self.path("a.spin")), self.path("out"))
        self.assertEqual(rb(self.path("out", "video.mp4")), VIDEO)
        self.assertEqual(rb(self.path("out", "preview.png")), PNG)
        self.assertTrue(os.path.exists(self.path("out", "slicer.json")))

    def test_safe_name_keeps_spin(self):
        self.assertEqual(spinpi.safe_name("part%20one.spin"), "part one.spin")
        self.assertEqual(spinpi.safe_name("../../etc/x.spin"), "x.spin")
        self.assertEqual(spinpi.safe_name("clip"), "clip.mp4")


# ------------------------------------------------------------------------------------------ library
class LibraryTest(Tmp):
    def setUp(self):
        super().setUp()
        self.usb = self.path("usb", "STICK")
        os.makedirs(self.usb)
        self.lib = spinpi.Library(self.path("data"), lambda: [self.usb])

    def test_spin_on_usb_unpacked_once(self):
        make_spin(os.path.join(self.usb, "cube.spin"))
        calls = []
        real = spinpi.extract_spin
        spinpi.extract_spin = lambda *a: (calls.append(a), real(*a))[1]
        self.addCleanup(setattr, spinpi, "extract_spin", real)
        f = self.lib.list()[0]
        self.lib.list()
        self.lib.list()
        self.assertEqual(len(calls), 1)
        self.assertEqual(f["id"], "usb/STICK/cube.spin")
        self.assertEqual(f["format"], "spin")
        self.assertEqual(f["source"], "usb")
        self.assertEqual(f["plan_revs"], 5.35)
        self.assertEqual(f["tolerance_pct"], 6.0)
        self.assertAlmostEqual(f["rev_s"], 40.0)
        self.assertAlmostEqual(f["rpm"], 1.5)
        self.assertEqual(f["frames_per_rev"], 1200)
        self.assertEqual(f["warnings"], ["thin wall"])
        self.assertTrue(f["preview"])
        self.assertNotIn("video", f)  # no file system paths in the API
        self.assertEqual(self.probed, [])  # spin.json had everything
        thumb, ctype = self.lib.thumb_file(f["id"])
        self.assertEqual(ctype, "image/png")
        self.assertEqual(rb(thumb), PNG)
        entry, play = self.lib.resolve(f["id"])
        self.assertEqual(rb(play), VIDEO)  # SpinProj plays the unpacked video

    def test_changed_spin_unpacked_again(self):
        p = make_spin(os.path.join(self.usb, "cube.spin"))
        self.lib.list()
        v2 = VIDEO + b"more"
        make_spin(p, meta=spin_meta(video=v2, plan={"revolutions": 7.0}), video=v2)
        os.utime(p, (time.time() + 5, time.time() + 5))
        f = self.lib.list()[0]
        self.assertEqual(f["plan_revs"], 7.0)
        self.assertEqual(rb(self.lib.resolve(f["id"])[1]), v2)

    def test_broken_spin_listed_with_error(self):
        make_spin(os.path.join(self.usb, "broken.spin"), meta=spin_meta(video=b"x"))  # wrong SHA
        f = self.lib.list()[0]
        self.assertIn("SHA-256", f["error"])
        self.assertIsNone(f.get("rpm"))

    def test_spin_without_numbers_asks_ffprobe(self):
        meta = spin_meta()
        for k in ("frames", "fps", "width", "height", "frames_per_rev"):
            del meta["video"][k]
        make_spin(os.path.join(self.usb, "c.spin"), meta=meta)
        f = self.lib.list()[0]
        self.assertEqual(f["frames"], 120)
        self.assertAlmostEqual(f["rev_s"], 5.0)  # one video = one turn
        self.assertEqual(len(self.probed), 1)

    def test_plain_video_and_sidecar_still_work(self):
        wb(os.path.join(self.usb, "quick_12rpm.mp4"), VIDEO)
        wb(os.path.join(self.usb, "side.mp4"), VIDEO)
        wb(os.path.join(self.usb, "side.spin.json"),
           json.dumps({"format": "spinslicer-print-1", "revolutions": 2, "tolerance_pct": 5.0}).encode())
        wb(os.path.join(self.usb, "tagged.mp4"), VIDEO)
        files = {f["name"]: f for f in self.lib.list()}
        self.assertEqual(set(files), {"quick_12rpm.mp4", "side.mp4", "tagged.mp4"})  # the .spin.json is no print
        self.assertAlmostEqual(files["quick_12rpm.mp4"]["rpm"], 12.0)  # OpenCAL name
        self.assertEqual(files["side.mp4"]["plan_revs"], 2.0)
        self.assertEqual(files["side.mp4"]["format"], "video")
        t = files["tagged.mp4"]  # MP4 tag with the new fields
        self.assertEqual(t["plan_revs"], 3.0)
        self.assertEqual(t["rotation"], "cw_from_top")
        self.assertEqual(t["frame0_angle_deg"], 45.0)
        self.assertAlmostEqual(t["rev_s"], 2.5)  # 60 frames per turn at 24 fps
        self.assertAlmostEqual(t["revs"], 2.0)  # the video holds two turns

    def test_remove_lib_spin(self):
        make_spin(self.lib.lib_path("cube.spin"))
        f = self.lib.added("cube.spin")
        d = self.lib.spin_dir(f["id"])
        self.assertTrue(os.path.isdir(d))
        self.assertTrue(self.lib.remove(f["id"]))
        self.assertFalse(os.path.exists(d))
        self.assertEqual([x for x in self.lib.list() if x["source"] == "lib"], [])


# ------------------------------------------------------------------------------------------ fakes
class FakeEsp:
    def __init__(self):
        self.sent, self.connected, self.version, self.greeting = [], True, "0.7-test", []
        self.rpm, self.angle = 0.0, 0.0

    def send(self, line):
        self.sent.append(line)
        if line.startswith("RPM "):
            self.rpm = float(line[4:])
        return True

    def hello(self):
        pass

    def query_pos(self, timeout=0.15):
        time.sleep(0.005)
        self.angle += 1.0
        return self.angle, self.rpm, True, time.monotonic(), 0.001


class FakeSpinProj:
    """Answers like SpinProj: LOAD -> LOADED, ARM -> LIGHT and, a moment later, DONE."""
    engine = "angle"

    def __init__(self):
        self.sent, self.events, self.ok, self.err, self.hz, self.stat = [], queue.Queue(), True, "", 60.0, None
        self.size = (1280, 800)

    def send(self, line):
        self.sent.append(line)
        if line.startswith("LOAD "):
            self.events.put(("LOADED", "120 1280 800 1.0 0.5 0.1"))
        elif line.startswith("ARM "):
            self.events.put(("LIGHT", "123.5000"))
            threading.Timer(0.2, lambda: self.events.put(("DONE", "600 0 5 5 5.0 1.0"))).start()
        return True

    def drain(self):
        while not self.events.empty():
            self.events.get_nowait()

    black = lambda self: self.send("BLACK")  # noqa: E731
    stop = lambda self: self.send("STOP")  # noqa: E731
    show_image = lambda self, p: self.send("IMAGE " + p)  # noqa: E731
    load = spinpi.AngleProjector.load
    angle = spinpi.AngleProjector.angle
    arm = spinpi.AngleProjector.arm


class FakeMpv(FakeSpinProj):
    engine = "mpv"


class FakeWarm:
    connected = True

    def left(self, minutes):
        return 0.0


class FakeNet:
    exe = ""

    def status(self):
        return {"available": False, "mode": "none"}

    def scan(self):
        return []


def make_printer(tmp, proj=None, **settings):
    data = os.path.join(tmp, "data")
    os.makedirs(data, exist_ok=True)
    s = {"warmup_min": 0, "delay_s": 0}
    s.update(settings)
    wb(os.path.join(data, "settings.json"), json.dumps(s).encode())
    usb = os.path.join(tmp, "usb", "STICK")
    os.makedirs(usb, exist_ok=True)
    lib = spinpi.Library(data, lambda: [usb])
    esp = FakeEsp()
    pr = spinpi.Printer(esp, proj or FakeSpinProj(), lib, data, 1280, 800, FakeWarm())
    return pr, usb


def wait_state(pr, states, timeout=5):
    t = time.time()
    while pr.state not in states and time.time() - t < timeout:
        time.sleep(0.02)
    return pr.state


# ------------------------------------------------------------------------------------------ turning direction
class Direction(Tmp):
    def print_one(self, motor_dir, rotation, frame0=0.0, proj=None):
        pr, usb = make_printer(self.tmp, proj, profile={"motor_dir": motor_dir})
        make_spin(os.path.join(usb, "cube.spin"),
                  meta=spin_meta(video_rotation=rotation, video_frame0_angle_deg=frame0, video_frames_per_rev=600))
        fid = pr.lib.list()[0]["id"]
        return pr, fid, pr.start(fid, revs=2)

    def test_same_direction_plays_forward(self):
        pr, fid, err = self.print_one("ccw_from_top", "ccw_from_top")
        self.assertIsNone(err)
        self.assertEqual(wait_state(pr, ("done", "error")), "done", pr.msg)
        sent = pr.proj.sent
        self.assertIn("REVERSE 0", sent)
        self.assertIn("PHASE 0.0000", sent)
        load = next(x for x in sent if x.startswith("LOAD "))
        self.assertTrue(load.startswith("LOAD 600 "), load)  # frames_per_rev, not all 1200 frames
        self.assertIn("ARM 2", sent)
        self.assertFalse(pr.history[-1]["reversed"])

    def test_other_direction_plays_backwards(self):
        pr, fid, err = self.print_one("cw_from_top", "ccw_from_top", frame0=90.0)
        f = pr.annotate(pr.lib.list())[0]
        self.assertTrue(f["reversed"])
        self.assertIsNone(err)
        self.assertEqual(wait_state(pr, ("done", "error")), "done", pr.msg)
        self.assertIn("REVERSE 1", pr.proj.sent)
        self.assertIn("PHASE 90.0000", pr.proj.sent)
        h = pr.history[-1]
        self.assertTrue(h["reversed"])
        self.assertEqual(h["frame0_angle_deg"], 90.0)
        self.assertEqual(h["angle0"], 123.5)  # where SpinProj switched the light on
        # the order of commands: direction and phase before the frames are unpacked
        s = pr.proj.sent
        self.assertLess(s.index("REVERSE 1"), next(i for i, x in enumerate(s) if x.startswith("LOAD ")))

    def test_unknown_direction_plays_forward(self):  # an old video: no rotation field
        pr, usb = make_printer(self.tmp, profile={"motor_dir": "cw_from_top"})
        wb(os.path.join(usb, "old.mp4"), VIDEO)
        f = pr.annotate(pr.lib.list())[0]
        self.assertFalse(f["reversed"])

    def test_mpv_refuses_reversed(self):
        pr, fid, err = self.print_one("cw_from_top", "ccw_from_top", proj=FakeMpv())
        self.assertIn("angle", err)
        self.assertEqual(pr.state, "idle")

    def test_fl_lines_carry_the_plan(self):
        pr, usb = make_printer(self.tmp)
        make_spin(os.path.join(usb, "cube.spin"))
        make_spin(os.path.join(usb, "broken.spin"), meta=spin_meta(video=b"x"))
        pr.send_list()
        fl = [x for x in pr.esp.sent if x.startswith("FL ")]
        self.assertTrue(fl[0].startswith("FL BEGIN 1 "), fl)
        self.assertEqual(fl[1].split()[:8], ["FL", "0", "1.50", "1", "5.35", "6.0", "40.000", "1"])
        self.assertTrue(fl[1].endswith(" cube.spin"))

    def test_print_log_and_feedback(self):
        pr, fid, err = self.print_one("ccw_from_top", "ccw_from_top")
        wait_state(pr, ("done", "error"))
        h = pr.history[-1]
        self.assertEqual(h["file"], "cube.spin")
        self.assertEqual(h["format"], "spin")
        self.assertEqual(h["revs"], 2)
        self.assertEqual(h["plan_revs"], 5.35)
        self.assertEqual(h["plan"]["revolutions"], 5.35)
        self.assertEqual(h["resin"]["name"], "Anycubic Clear")
        self.assertEqual(h["printer_id"], pr.settings["printer_id"])
        self.assertIsNone(h["result"])
        pr.result("under")
        h = pr.history[-1]
        self.assertEqual(h["result"], "under")
        self.assertGreater(h["factor_after"], 1.0)
        on_disk = json.loads(rt(pr.history_path))
        self.assertEqual(on_disk[-1]["result"], "under")


# ------------------------------------------------------------------------------------------ HTTP API
class Api(Tmp):
    def setUp(self):
        super().setUp()
        self.pr, self.usb = make_printer(self.tmp)
        app = spinpi.App()
        app.lib, app.printer, app.net, app.proj = self.pr.lib, self.pr, FakeNet(), self.pr.proj
        spinpi.Handler.app = app
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), spinpi.Handler)
        self.srv.daemon_threads = True
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)

    def req(self, method, path, body=None, raw=False):
        c = http.client.HTTPConnection("127.0.0.1", self.srv.server_port, timeout=10)
        data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
        c.request(method, path, body=data, headers={"Content-Type": "application/json"} if data else {})
        r = c.getresponse()
        out = r.read()
        c.close()
        return r.status, (out if raw else json.loads(out)), r.getheader("Content-Type")

    def test_upload_spin(self):
        code, r, _ = self.req("PUT", "/api/files/lattice%20cube.spin", spin_bytes())
        self.assertEqual(code, 200, r)
        self.assertTrue(r["ok"])
        self.assertEqual(r["name"], "lattice cube.spin")
        self.assertEqual(r["info"]["plan_revs"], 5.35)
        self.assertTrue(r["info"]["other_printer"])  # sliced for "spinslicer-3f2a", not for this printer
        code, files, _ = self.req("GET", "/api/files")
        f = next(x for x in files if x["id"] == "lib/lattice cube.spin")
        self.assertEqual(f["format"], "spin")
        self.assertTrue(f["preview"])
        code, png, ctype = self.req("GET", "/api/thumb?id=lib/lattice%20cube.spin", raw=True)
        self.assertEqual((code, ctype, png), (200, "image/png", PNG))
        code, plan, _ = self.req("POST", "/api/plan", {"id": f["id"], "factor": 1.0})
        self.assertEqual(plan["revs"], 5)
        self.assertEqual(plan["warnings"], ["thin wall"])

    def test_upload_spin_for_this_printer(self):
        pid = self.pr.settings["printer_id"]
        code, r, _ = self.req("PUT", "/api/files/mine.spin", spin_bytes(meta=spin_meta(printer={"profile_id": pid})))
        self.assertFalse(r["info"]["other_printer"])

    def test_upload_bad_spin(self):
        code, r, _ = self.req("PUT", "/api/files/x.spin", b"not a zip at all")
        self.assertEqual(code, 400)
        self.assertFalse(r["ok"])
        code, r, _ = self.req("PUT", "/api/files/y.spin", spin_bytes(meta=spin_meta(video=b"x")))
        self.assertEqual(code, 400)
        self.assertIn("SHA-256", r["error"])
        code, files, _ = self.req("GET", "/api/files")
        self.assertEqual([x for x in files if x["source"] == "lib"], [])
        self.assertEqual(sorted(os.listdir(self.pr.lib.root)), [])

    def test_upload_same_name_twice(self):
        self.req("PUT", "/api/files/c.spin", spin_bytes())
        code, r, _ = self.req("PUT", "/api/files/c.spin", spin_bytes(meta=spin_meta(plan={"revolutions": 9.0})))
        self.assertEqual(r["info"]["plan_revs"], 9.0)

    def test_printer_profile(self):
        code, p, _ = self.req("GET", "/api/printer")
        self.assertEqual(code, 200)
        self.assertEqual(p["api"], 1)
        self.assertTrue(p["id"].startswith("spinpi-"))
        self.assertEqual((p["projector"]["width"], p["projector"]["height"]), (1280, 800))
        self.assertEqual(p["motor"]["direction"], "ccw_from_top")
        self.assertEqual(p["calibration"]["vial_width_px"], 400)
        self.assertEqual(p["software"]["spinpi"], spinpi.VERSION)
        self.assertEqual(p["software"]["spinscreen"], "0.7-test")
        pid = p["id"]
        code, r, _ = self.req("POST", "/api/printer", {"name": "Шкаф", "irradiance_mw_cm2": "4.1", "vial_diameter_mm": 25,
                                                      "vial_wall_mm": 1.2, "vial_n": 1.47, "contrast": 1000,
                                                      "motor_gear": 3, "motor_dir": "cw_from_top"})
        self.assertTrue(r["ok"], r)
        code, p, _ = self.req("GET", "/api/printer")
        self.assertEqual(p["id"], pid)  # stable
        self.assertEqual(p["name"], "Шкаф")
        self.assertEqual(p["projector"]["irradiance_mw_cm2"], 4.1)
        self.assertEqual(p["vial"], {"diameter_mm": 25.0, "wall_mm": 1.2, "n_glass": 1.47})
        self.assertEqual(p["motor"], {"gear": 3.0, "direction": "cw_from_top"})
        saved = json.loads(rt(self.pr.settings_path))
        self.assertEqual(saved["profile"]["vial_n"], 1.47)
        code, r, _ = self.req("POST", "/api/printer", {"motor_dir": "up"})
        self.assertEqual(code, 400)
        code, r, _ = self.req("POST", "/api/printer", {"vial_n": 9})
        self.assertEqual(code, 400)
        code, r, _ = self.req("POST", "/api/printer", {"irradiance_mw_cm2": None})
        self.assertIsNone(r["printer"]["projector"]["irradiance_mw_cm2"])
        # a new start keeps the id
        again = spinpi.Printer(FakeEsp(), FakeSpinProj(), self.pr.lib, self.pr.data, 1280, 800, FakeWarm())
        self.assertEqual(again.settings["printer_id"], pid)

    def test_history(self):
        self.req("PUT", "/api/files/c.spin", spin_bytes())
        code, r, _ = self.req("POST", "/api/print", {"id": "lib/c.spin", "revs": 3, "factor": 1.1})
        self.assertTrue(r["ok"], r)
        self.assertEqual(wait_state(self.pr, ("done", "error")), "done", self.pr.msg)
        self.req("POST", "/api/result", {"result": "over"})
        code, h, _ = self.req("GET", "/api/history")
        self.assertEqual(code, 200)
        self.assertEqual(h["printer_id"], self.pr.settings["printer_id"])
        e = h["prints"][-1]
        self.assertEqual((e["file"], e["revs"], e["result"], e["exposure_pct"]), ("c.spin", 3, "over", 10.0))
        self.assertEqual(e["plan"]["revolutions"], 5.35)
        self.assertEqual(e["n"], 1)


# ------------------------------------------------------------------------------------------ ESP link
class EspLink(unittest.TestCase):
    """Prints stopped by themselves: a line from the ESP read in two halves crashed the reader,
    the port was closed and the print ended with "ESP не отвечает"."""

    def test_lines_come_whole(self):
        r = spinpi.Lines()
        self.assertEqual(r.feed(b"POS 12.5 1.5"), [])
        self.assertEqual(r.feed(b"00 1\r\nPONG SpinScreen 0.8.0\nEV ST"), ["POS 12.5 1.500 1", "PONG SpinScreen 0.8.0"])
        self.assertEqual(r.feed(b"OP\n"), ["EV STOP"])

    def test_endless_line_is_dropped(self):
        r = spinpi.Lines()
        self.assertEqual(r.feed(b"x" * 5000), [])
        self.assertEqual(r.feed(b"\nPONG a\n"), ["PONG a"])

    def test_bad_line_does_not_drop_the_port(self):
        esp = spinpi.Esp.__new__(spinpi.Esp)  # no reader thread
        esp.pos_cv, esp.pos, esp.connected, esp.greeting = threading.Condition(), None, False, []
        esp.on_event, esp.port, esp.ser, esp.lock = (lambda ev: None), "/dev/ttyACM0", None, threading.Lock()
        for bad in ("POS 12.5", "POS a b c", "POS", "PONG"):
            esp.on_line(bad)  # must not raise
        esp.on_line("POS 1.0 2.0 1")
        self.assertEqual(esp.pos[:3], (1.0, 2.0, True))

    def test_usb_only(self):
        esp = spinpi.Esp.__new__(spinpi.Esp)
        esp.port_hint = ""
        real_glob, real_exists = spinpi.glob.glob, spinpi.os.path.exists
        spinpi.glob.glob = lambda pat: ["/dev/ttyACM0"] if "ttyACM" in pat else []
        spinpi.os.path.exists = lambda p: True
        try:
            self.assertEqual(esp._candidates(), ["/dev/ttyACM0"])
        finally:
            spinpi.glob.glob, spinpi.os.path.exists = real_glob, real_exists


class StopReason(Tmp):
    def test_stop_from_the_screen_is_said(self):
        pr, _ = make_printer(self.tmp)
        pr.job, pr.state = {"file": "x"}, "print"
        pr.on_esp_event("STOP")
        self.assertTrue(pr.stop_flag.is_set())
        self.assertIn("экран", pr.stop_reason)


# ------------------------------------------------------------------------------------------ mDNS
class MdnsTest(Tmp):
    def test_service_file(self):
        d = self.path("avahi", "services")
        os.makedirs(os.path.dirname(d))
        m = spinpi.Mdns(80, d)
        self.assertTrue(m.publish("Принтер <1>", "spinpi-1234abcd"))
        text = rt(os.path.join(d, "spinprinter.service"))
        self.assertIn("<type>_spinprinter._tcp</type>", text)
        self.assertIn("<port>80</port>", text)
        for t in ("name=Принтер &lt;1&gt;", "id=spinpi-1234abcd", "api=1"):
            self.assertIn(f"<txt-record>{t}</txt-record>", text)
        self.assertFalse(m.publish("Принтер <1>", "spinpi-1234abcd"))  # nothing changed
        self.assertTrue(m.publish("Другое", "spinpi-1234abcd"))

    def test_no_avahi_no_file(self):
        m = spinpi.Mdns(80, self.path("no", "avahi", "services"))
        self.assertFalse(m.publish("x", "y"))
        self.assertFalse(os.path.exists(self.path("no")))

    def test_shipped_file_matches(self):  # the installer's first copy, before SpinPi rewrites it
        here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SpinPi", "spinprinter.avahi.service")
        text = rt(here)
        self.assertIn("<type>_spinprinter._tcp</type>", text)
        self.assertIn("<txt-record>api=1</txt-record>", text)


if __name__ == "__main__":
    unittest.main()
