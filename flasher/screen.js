// Step 2 of the installer: which screen and buttons the board has, and how they are wired.
// Talks to SpinScreen over Web Serial: reads the settings (CONFIG?), shows the wiring for the
// choice, stores it (CONFIG key=value ...), waits for the board to reboot and checks it answers
// with the same settings. Works any time later too, without flashing.
"use strict";

const PANELS = {
  st7789_170x320: { name: '1.9" ST7789 170×320', size: "320×170", rect: true, spi: 40,
    pins: ["GND", "VCC", "SCL", "SDA", "RES", "DC", "CS", "BLK"] },
  st7789_240x240: { name: '1.3" / 1.54" ST7789 240×240', size: "240×240", rect: false, spi: 40,
    pins: ["GND", "VCC", "SCL", "SDA", "RES", "DC", "CS", "BLK"] },
  st7789_240x320: { name: '2.0" / 2.4" / 2.8" ST7789 240×320', size: "320×240", rect: true, spi: 40,
    pins: ["GND", "VCC", "SCL", "SDA", "RES", "DC", "CS", "BLK"] },
  ili9341: { name: '2.4" / 2.8" / 3.2" ILI9341 240×320', size: "320×240", rect: true, spi: 40,
    pins: ["VCC", "GND", "CS", "RESET", "DC", "SDI (MOSI)", "SCK", "LED"] },
  st7735: { name: '1.8" ST7735 128×160', size: "160×128", rect: true, spi: 27,
    pins: ["GND", "VCC", "SCL", "SDA", "RES", "DC (A0)", "CS", "BL"] },
  gc9a01: { name: '1.28" GC9A01 round 240×240', size: "240×240", rect: false, spi: 40,
    pins: ["VCC", "GND", "SCL", "SDA", "DC", "CS", "RST", "BL"] },
};
// which of our signals each of the module's pin names is
const SIG = { SCL: "scl", SCK: "scl", SDA: "sda", "SDI (MOSI)": "sda", RES: "res", RESET: "res", RST: "res",
  DC: "dc", "DC (A0)": "dc", CS: "cs", BLK: "blk", LED: "blk", BL: "blk", VCC: "3V3", GND: "GND" };
const LAYOUTS = {
  wires: { scl: 12, sda: 11, res: 8, dc: 9, cs: 10, blk: 7, a: 4, b: 5, c: 6, step: 1, dir: 2, en: 13, pi_rx: 44, pi_tx: 43 },
  breadboard: { scl: 43, sda: 44, res: 1, dc: 2, cs: 3, blk: 4, a: 13, b: 11, c: 12, step: 5, dir: 6, en: 7, pi_rx: -1, pi_tx: -1 },
};
const INPUTS = {
  buttons: { keys: ["up", "down", "ok"], pins: ["UP", "DOWN", "OK"], extra: ["GND"] },
  encoder: { keys: ["clk", "dt", "sw"], pins: ["CLK", "DT", "SW"], extra: ["+ → 3V3", "GND"] },
  joystick: { keys: ["vrx", "vry", "sw"], pins: ["VRx", "VRy", "SW"], extra: ["+5V → 3V3 (!)", "GND"] },
};
const TXT = {
  ru: { connect: "Подключиться к плате", connected: "Плата подключена", noserial: "Этот браузер не умеет Web Serial: открой страницу в Chrome или Edge на компьютере.",
    read: "Настройки платы прочитаны.", saving: "Записываю, плата перезагрузится…", waiting: "Жду, пока плата перезагрузится…",
    ok: "Готово: плата перезагрузилась и работает с этими настройками. Посмотри на экран.", mismatch: "Плата ответила, но настройки другие:",
    lost: "Плата не вернулась за 15 секунд. Нажми «Подключиться» и выбери порт снова.", err: "Плата не приняла настройки:",
    reset: "Сброшено к значениям из config.h, плата перезагружается.", noanswer: "Плата не отвечает. Сначала прошей её (шаг 1) и закрой окно прошивки.",
    noscreen: "Плата работает, но экран не запустился: проверь тип экрана и пины.", busy: "Порт занят: закрой окно прошивки, Arduino IDE и другие программы, которые открыли плату.",
    how: { buttons: "Вверх / вниз — по списку, OK — выбрать, долго держать OK — назад.",
      encoder: "Крутить — по списку (по часовой — вниз, в правке значения — больше; быстро — крупный шаг), нажать — OK, долго держать — назад.",
      joystick: "Вверх / вниз — по списку (держи — листает), вправо или нажать — OK, влево — назад. При включении ручку не трогать: запоминается центр." },
    screen: "Экран", input: "Управление", motor: "Мотор (драйвер STEP/DIR)", pi: "Raspberry Pi", usbOnly: "только USB-кабель", board: "ESP32-S3 Super Mini" },
  en: { connect: "Connect to the board", connected: "Board connected", noserial: "This browser has no Web Serial: open the page in Chrome or Edge on a computer.",
    read: "Read the board's settings.", saving: "Saving, the board will reboot…", waiting: "Waiting for the board to reboot…",
    ok: "Done: the board rebooted and runs with these settings. Look at the screen.", mismatch: "The board answered, but its settings differ:",
    lost: "The board did not come back within 15 seconds. Press Connect and pick the port again.", err: "The board refused the settings:",
    reset: "Reset to config.h's values, the board is rebooting.", noanswer: "No answer. Flash the board first (step 1) and close the flashing dialog.",
    noscreen: "The board runs but the screen did not start: check the screen type and pins.", busy: "The port is busy: close the flashing dialog, Arduino IDE and other programs that opened the board.",
    how: { buttons: "Up / down = the list, OK = choose, hold OK = back.",
      encoder: "Turn = the list (clockwise = down; while editing = more; fast = big steps), press = OK, hold = back.",
      joystick: "Up / down = the list (hold to scroll), right or press = OK, left = back. Leave the stick alone at power-up: it takes the centre then." },
    screen: "Screen", input: "Controls", motor: "Motor (STEP/DIR driver)", pi: "Raspberry Pi", usbOnly: "USB cable only", board: "ESP32-S3 Super Mini" },
};
const t = (k) => TXT[document.documentElement.lang === "en" ? "en" : "ru"][k];
const $ = (id) => document.getElementById(id);

// ------------------------------------------------------------------ serial
let port = null, reader = null, lines = [], waiters = [], closing = false;
async function open(p) {
  port = p;
  await port.open({ baudRate: 115200 });
  closing = false;
  readLoop();
}
async function readLoop() {
  const dec = new TextDecoder();
  let buf = "";
  try {
    reader = port.readable.getReader();
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const l = buf.slice(0, i).replace(/\r$/, "");
        buf = buf.slice(i + 1);
        if (l) gotLine(l);
      }
    }
  } catch (e) { /* the board rebooted: the port went away */ }
  try { reader.releaseLock(); } catch (e) {}
  if (!closing) { try { await port.close(); } catch (e) {} }
  port = null;
  status(false);
}
function gotLine(l) {
  log("← " + l);
  for (const w of waiters.slice()) if (w.test(l)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(l); }
}
function waitFor(test, ms) {
  return new Promise((resolve) => {
    const w = { test, resolve };
    waiters.push(w);
    setTimeout(() => { const i = waiters.indexOf(w); if (i >= 0) { waiters.splice(i, 1); resolve(null); } }, ms);
  });
}
async function send(l) {
  if (!port || !port.writable) throw new Error("no port");
  log("→ " + l);
  const w = port.writable.getWriter();
  await w.write(new TextEncoder().encode(l + "\n"));
  w.releaseLock();
}
async function ask(l, test, ms = 1500) {
  const p = waitFor(test, ms);
  await send(l);
  return p;
}
function log(s) { const el = $("cfg-log"); el.textContent += s + "\n"; el.scrollTop = el.scrollHeight; }
function say(s, bad) { const el = $("cfg-msg"); el.textContent = s; el.className = bad ? "warn" : "okmsg"; }
function status(on) {
  $("cfg-connect").textContent = on ? t("connected") : t("connect");
  $("cfg-connect").disabled = on;
  for (const id of ["cfg-save", "cfg-reset"]) $(id).disabled = !on;
}

async function connect() {
  if (!("serial" in navigator)) { say(t("noserial"), true); return; }
  let p;
  try { p = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] }); } catch (e) { return; }
  try { await open(p); } catch (e) { say(t("busy"), true); return; }
  status(true);
  await readConfig();
}
async function readConfig() {
  // the board may still be starting: ask a few times
  for (let i = 0; i < 4; i++) {
    const l = await ask("CONFIG?", (l) => l.startsWith("CONFIG ") && l.includes("panel="), 1200);
    if (l) { fromLine(l.slice(7)); say(t("read")); return parse(l.slice(7)); }
  }
  say(t("noanswer"), true);
  return null;
}
const parse = (s) => Object.fromEntries(s.trim().split(/\s+/).map((kv) => kv.split("=")));

// ------------------------------------------------------------------ the form
function fromLine(s) {
  const c = parse(s);
  if (PANELS[c.panel]) $("cfg-panel").value = c.panel;
  if (INPUTS[c.input]) $("cfg-input").value = c.input;
  const k = INPUTS[$("cfg-input").value].keys;
  const v = { scl: c.scl, sda: c.sda, res: c.res, dc: c.dc, cs: c.cs, blk: c.blk, a: c[k[0]], b: c[k[1]], c: c[k[2]],
    step: c.step, dir: c.dir, en: c.en, pi_rx: c.pi_rx, pi_tx: c.pi_tx };
  for (const [key, val] of Object.entries(v)) if (val !== undefined) $("pin-" + key).value = val;
  $("cfg-rot").value = c.rot === "2" || c.rot === "3" ? "180" : "0";
  $("cfg-spi").value = c.spi || 40;
  $("cfg-nocs").checked = c.cs === "-1";
  $("cfg-layout").value = Object.keys(LAYOUTS).find((n) => Object.entries(LAYOUTS[n]).every(([k2, x]) => +$("pin-" + k2).value === x)) || "custom";
  draw();
}
function applyLayout() {
  const L = LAYOUTS[$("cfg-layout").value];
  if (!L) return;
  for (const [k, v] of Object.entries(L)) $("pin-" + k).value = v;
  $("cfg-nocs").checked = false;
  draw();
}
function onPanel() {
  $("cfg-spi").value = PANELS[$("cfg-panel").value].spi;
  draw();
}
function line() {
  const panel = $("cfg-panel").value, P = PANELS[panel], input = $("cfg-input").value;
  const k = INPUTS[input].keys, flip = $("cfg-rot").value === "180";
  const rot = P.rect ? (flip ? 3 : 1) : (flip ? 2 : 0);
  const nocs = $("cfg-nocs").checked;
  const v = (id) => $("pin-" + id).value.trim();
  let s = `panel=${panel} rot=${rot} spi=${+$("cfg-spi").value || 40} mode=${nocs || $("cfg-layout").value === "breadboard" ? 3 : 0}` +
    ` scl=${v("scl")} sda=${v("sda")} res=${v("res")} dc=${v("dc")} cs=${nocs ? -1 : v("cs")} blk=${v("blk")}` +
    ` input=${input} ${k[0]}=${v("a")} ${k[1]}=${v("b")} ${k[2]}=${v("c")}` +
    ` step=${v("step")} dir=${v("dir")} en=${v("en")} pi_rx=${v("pi_rx")} pi_tx=${v("pi_tx")}`;
  return s;
}

// ------------------------------------------------------------------ wiring picture
function draw() {
  const P = PANELS[$("cfg-panel").value], I = INPUTS[$("cfg-input").value];
  const nocs = $("cfg-nocs").checked;
  const pin = (id) => $("pin-" + id).value.trim();
  const gpio = (sig) => sig === "3V3" || sig === "GND" ? sig : (sig === "cs" && nocs ? "—" : (+pin(sig) < 0 ? "—" : "GPIO " + pin(sig)));
  const label = (n) => (n === "GPIO 43" ? "TX (43)" : n === "GPIO 44" ? "RX (44)" : n);
  const groups = [
    { title: t("screen") + " · " + P.name, rows: P.pins.map((n) => [n, label(gpio(SIG[n]))]) },
    { title: t("input"), rows: I.pins.map((n, i) => [n, label(gpio("abc"[i]))]).concat(I.extra.map((e) => [e.split(" → ")[0], e.includes("→") ? e.split(" → ")[1] : "GND"])) },
    { title: t("motor"), rows: [["STEP", label(gpio("step"))], ["DIR", label(gpio("dir"))], ["EN", label(gpio("en"))], ["GND", "GND"]] },
    { title: t("pi"), rows: +pin("pi_rx") < 0 ? [["USB", t("usbOnly")]] : [["Pi TX (pin 8)", label(gpio("pi_rx"))], ["Pi RX (pin 10)", label(gpio("pi_tx"))], ["GND (pin 6)", "GND"]] },
  ];
  const rowH = 26, gap = 18, W = 700;
  let y = 20, body = "";
  for (const g of groups) {
    const h = 34 + g.rows.length * rowH;
    body += `<rect x="20" y="${y}" width="330" height="${h}" rx="14" fill="#1c151e" stroke="#2a2029"/>` +
      `<text x="36" y="${y + 23}" font-size="14" font-weight="700" fill="#ff8db5">${esc(g.title)}</text>`;
    g.rows.forEach(([a, b], i) => {
      const ry = y + 34 + i * rowH + 13;
      const warn = b.includes("(!)"), col = b === "—" ? "#5a4652" : (b === "GND" ? "#a88f9b" : b.startsWith("3V3") ? "#ffcf6e" : "#ff8db5");
      body += `<circle cx="342" cy="${ry - 4}" r="4" fill="${col}"/><text x="36" y="${ry}" font-size="13" fill="#f7e3ec">${esc(a)}</text>` +
        (b === "—" ? "" : `<path d="M346 ${ry - 4} L 520 ${ry - 4}" stroke="${col}" stroke-width="2" fill="none" opacity=".85"/>`) +
        `<rect x="520" y="${ry - 15}" width="${warn ? 150 : 110}" height="22" rx="8" fill="#0a080b" stroke="${col}"/>` +
        `<text x="${warn ? 595 : 575}" y="${ry}" font-size="13" text-anchor="middle" fill="${col}">${esc(b.replace(" (!)", ""))}</text>`;
    });
    y += h + gap;
  }
  body += `<text x="${W - 20}" y="${y - 4}" font-size="12" text-anchor="end" fill="#a88f9b">${esc(t("board"))} · ${esc(P.size)}</text>`;
  $("cfg-wiring").innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${y + 6}" font-family="Onest, system-ui, sans-serif">${body}</svg>`;
  $("cfg-line").textContent = "CONFIG " + line();
  $("cfg-how").textContent = t("how")[$("cfg-input").value];
}
const esc = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;");

// ------------------------------------------------------------------ store, reboot, check
async function save() {
  const want = line();
  const tryAns = await ask("CONFIG TRY " + want, (l) => l.startsWith("CONFIG OK") || l.startsWith("ERR"), 1500);
  if (!tryAns) { say(t("noanswer"), true); return; }
  if (tryAns.startsWith("ERR")) { say(t("err") + " " + tryAns.replace("ERR CONFIG ", ""), true); return; }
  const expect = parse(tryAns.slice("CONFIG OK ".length));
  say(t("saving"));
  const ans = await ask("CONFIG " + want, (l) => l.startsWith("CONFIG SAVED") || l.startsWith("ERR"), 2000);
  if (!ans || ans.startsWith("ERR")) { say(t("err") + " " + (ans || ""), true); return; }
  await reconnectAndCheck(expect);
}
async function reconnectAndCheck(expect) {
  say(t("waiting"));
  const old = port;
  // the board restarts: its USB port disappears and comes back (the browser remembers it)
  const t0 = Date.now();
  while (port && Date.now() - t0 < 4000) await sleep(100);
  if (port) { closing = true; try { await reader.cancel(); } catch (e) {} await sleep(200); }
  while (Date.now() - t0 < 15000) {
    await sleep(700);
    const ports = await navigator.serial.getPorts();
    const p = ports.find((x) => x === old) || ports.find((x) => (x.getInfo().usbVendorId || 0) === 0x303a);
    if (!p) continue;
    try { await open(p); } catch (e) { continue; }
    status(true);
    const ready = await waitFor((l) => l.startsWith("READY"), 2500);
    const got = await readConfig();
    if (!got) return;
    const diff = Object.keys(expect).filter((k) => String(expect[k]) !== String(got[k]));
    if (diff.length) say(t("mismatch") + " " + diff.map((k) => `${k}=${got[k]}`).join(" "), true);
    else if (ready && ready.includes("noscreen")) say(t("noscreen"), true);
    else say(t("ok"));
    return;
  }
  say(t("lost"), true);
}
async function reset() {
  await ask("CONFIG RESET", (l) => l.startsWith("CONFIG RESET"), 1500);
  say(t("reset"));
  const d = {};
  await reconnectAndCheck(d);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ------------------------------------------------------------------
function init() {
  const sel = $("cfg-panel");
  for (const [k, p] of Object.entries(PANELS)) sel.add(new Option(p.name, k));
  sel.onchange = onPanel;
  $("cfg-layout").onchange = applyLayout;
  for (const el of document.querySelectorAll("#cfg-form input, #cfg-form select"))
    if (el.id !== "cfg-panel" && el.id !== "cfg-layout") el.addEventListener("input", () => {
      if (el.id.startsWith("pin-")) $("cfg-layout").value = "custom";
      draw();
    });
  $("cfg-connect").onclick = connect;
  $("cfg-save").onclick = () => save().catch((e) => say(String(e), true));
  $("cfg-reset").onclick = () => reset().catch((e) => say(String(e), true));
  $("lang").addEventListener("click", () => setTimeout(draw, 0));
  applyLayout();
  status(false);
  if (!("serial" in navigator)) say(t("noserial"), true);
}
init();
