const WS_URL = `ws://${location.hostname}:8765`;

let ws = null;
let reconnectTimer = null;

function connect() {
  ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    document.getElementById("conn-status").textContent = "● Connected";
    document.getElementById("conn-status").classList.add("connected");
    clearTimeout(reconnectTimer);
  };

  ws.onclose = () => {
    document.getElementById("conn-status").textContent = "● Disconnected";
    document.getElementById("conn-status").classList.remove("connected");
    reconnectTimer = setTimeout(connect, 2000);
  };

  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    handleMessage(msg);
  };
}

function handleMessage(msg) {
  switch (msg.type) {
    case "init":
      applyInitState(msg.state);
      break;
    case "led":
      setLed(msg.line, msg.value);
      break;
    case "button":
      setButtonVisual(msg.line, msg.value);
      break;
    case "range":
      setRange(msg.value);
      break;
    case "rfid":
      setRfid(msg.uid, msg.present);
      break;
    case "lcd":
      if (msg.pixels) drawLcd(msg.pixels);
      break;
    case "oled":
      if (msg.framebuf) drawOled(msg.framebuf);
      break;
  }
}

function applyInitState(state) {
  const leds = state?.gpio?.leds ?? {};
  for (const [line, val] of Object.entries(leds)) setLed(Number(line), val);

  const buttons = state?.gpio?.buttons ?? {};
  for (const [line, val] of Object.entries(buttons)) setButtonVisual(Number(line), val);

  const range = state?.i2c?.vl53l0x?.range_mm ?? 300;
  setRange(range);
  document.getElementById("range-slider").value = range;

  const rfid = state?.spi?.mfrc522 ?? {};
  setRfid(rfid.uid, rfid.present);
}

/* ---- LED ---- */
function setLed(line, on) {
  const el  = document.getElementById(`led-${line}`);
  const val = document.getElementById(`led-${line}-val`);
  if (!el) return;
  el.classList.toggle("on", Boolean(on));
  if (val) val.textContent = on ? "ON" : "OFF";
}

/* ---- Button ---- */
function setButtonVisual(line, pressed) {
  const el = document.getElementById(`btn-${line}`);
  if (el) el.classList.toggle("pressed", Boolean(pressed));
}

function sendButton(line, value) {
  send({ type: "button", line, value });
  setButtonVisual(line, value);
}

/* ---- Range ---- */
function setRange(mm) {
  const v = Number(mm);
  const el  = document.getElementById("range-value");
  const bar = document.getElementById("range-bar");
  if (el)  el.textContent = v;
  if (bar) bar.style.width = `${Math.min(100, (v / 2000) * 100).toFixed(1)}%`;
}

function sendRange(value) {
  setRange(value);
  send({ type: "range_set", value: Number(value) });
}

/* ---- RFID ---- */
function setRfid(uid, present) {
  const area = document.getElementById("rfid-area");
  const uidEl = document.getElementById("rfid-uid");
  if (!area) return;
  area.classList.toggle("present", Boolean(present));
  uidEl.textContent = present && uid ? uid : "No card";
}

function sendRfidTap() {
  send({ type: "rfid_tap", uid: "04:AB:CD:EF:01:23" });
}

function sendRfidRemove() {
  send({ type: "rfid_remove" });
}

/* ---- OLED (SSD1306 128x64 monochrome) ---- */
function drawOled(framebufB64) {
  const canvas = document.getElementById("oled-canvas");
  const ctx = canvas.getContext("2d");
  const bytes = Uint8Array.from(atob(framebufB64), c => c.charCodeAt(0));
  const imgData = ctx.createImageData(128, 64);
  /* SSD1306 layout: 8 pages × 128 columns, each byte = 8 vertical pixels (LSB top) */
  for (let page = 0; page < 8; page++) {
    for (let col = 0; col < 128; col++) {
      const b = bytes[page * 128 + col];
      for (let bit = 0; bit < 8; bit++) {
        const y = page * 8 + bit;
        const px = (b >> bit) & 1;
        const idx = (y * 128 + col) * 4;
        imgData.data[idx + 0] = px ? 0x88 : 0x00;
        imgData.data[idx + 1] = px ? 0xCC : 0x00;
        imgData.data[idx + 2] = px ? 0xFF : 0x10;
        imgData.data[idx + 3] = 255;
      }
    }
  }
  ctx.putImageData(imgData, 0, 0);
}

/* ---- LCD ---- */
function drawLcd(pixelsB64) {
  const canvas = document.getElementById("lcd-canvas");
  const ctx = canvas.getContext("2d");
  const bytes = Uint8Array.from(atob(pixelsB64), c => c.charCodeAt(0));
  const imgData = ctx.createImageData(240, 240);
  /* Expect RGB565 packed as 2 bytes per pixel */
  for (let i = 0; i < 240 * 240; i++) {
    const hi = bytes[i * 2];
    const lo = bytes[i * 2 + 1];
    const rgb565 = (hi << 8) | lo;
    imgData.data[i * 4 + 0] = ((rgb565 >> 11) & 0x1F) << 3;
    imgData.data[i * 4 + 1] = ((rgb565 >> 5)  & 0x3F) << 2;
    imgData.data[i * 4 + 2] = ( rgb565        & 0x1F) << 3;
    imgData.data[i * 4 + 3] = 255;
  }
  ctx.putImageData(imgData, 0, 0);
}

/* ---- helpers ---- */
function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN)
    ws.send(JSON.stringify(obj));
}

connect();
