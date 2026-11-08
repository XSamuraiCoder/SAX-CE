// SAX-CE Bridge, as a static page. Everything runs here in the browser:
// the calculator link (Web Serial), the IRC client and the chat view. The
// only outside piece is the relay (relay/relay.mjs), which passes IRC lines
// between a WebSocket and the IRC server, since pages can't open raw TCP.

import { IrcBackend, LocalBackend } from "./irc.js?v=6";
import { asciiClean, calcNick, TEXT_MAX, WHO_MAX } from "./util.js?v=6";

const $ = (id) => document.getElementById(id);

// ---------- settings (kept in this browser) ----------

const DEFAULTS = {
  backend: "irc",          // "irc" (through the relay) | "ws" (IRC WebSocket server) | "local"
  server: "irc.efnet.org",
  ircPort: 6667,
  tls: false,
  relayPort: 8317,
  wsUrl: "",
  channel: "#cemetech",
  lastNick: "",
  autoStart: true,
  usbIds: [],              // extra calculator USB IDs, "vvvv:pppp" in hex
};
const STORE = "saxce-bridge";

function loadSettings() {
  let saved = {};
  try {
    saved = JSON.parse(localStorage.getItem(STORE) || "{}");
  } catch {
    /* storage blocked: defaults */
  }
  const s = { ...DEFAULTS };
  for (const k of Object.keys(DEFAULTS)) if (typeof saved[k] === typeof DEFAULTS[k]) s[k] = saved[k];
  s.usbIds = Array.isArray(saved.usbIds) ? parseUsbIds(saved.usbIds.join(",")) : [];
  return s;
}

function saveSettings() {
  try {
    localStorage.setItem(STORE, JSON.stringify(settings));
  } catch {
    /* storage blocked: settings last until the page closes */
  }
}

let settings = loadSettings();

// ---------- chat view ----------

const hhmm = () => new Date().toTimeString().slice(0, 5);

function addLine(ev) {
  const chat = $("chat");
  const atBottom = chat.scrollHeight - chat.scrollTop - chat.clientHeight < 30;
  const div = document.createElement("div");
  const span = (cls, text) => {
    const s = document.createElement("span");
    s.className = cls;
    s.textContent = text;
    div.append(s);
  };
  span("t", `${hhmm()} `);
  if (ev.t === "chat") {
    span(ev.mine ? "me" : "n", `<${ev.nick}> `);
    div.append(document.createTextNode(ev.text));
  } else {
    span(ev.t === "log" ? "l" : "s", ev.text);
  }
  chat.append(div);
  while (chat.childElementCount > 2000) chat.firstElementChild.remove();
  if (atBottom) chat.scrollTop = chat.scrollHeight;
}

const log = (text) => addLine({ t: "log", text });

function setLight(which, cls, text) {
  $(`${which}-light`).className = `light ${cls}`;
  $(`${which}-text`).textContent = text;
}

// ---------- hub ----------
// Routes between the calculator, the network backend and the chat view.

let backend = null;
let running = false;
let nick = "";        // the calculator's nick, once it says hello
let earlyNick = "";   // nick the backend was started with before hello
let net = { state: "off", detail: "" };

const hub = {
  net(state, detail) {
    net = { state, detail };
    const relayDown = settings.backend === "irc" && state === "down" && /relay/.test(detail);
    $("relay-help").hidden = !relayDown;
    if (relayDown) checkLocalAccess();
    render();
  },
  status(text) {
    calcWrite(`S ${asciiClean(text, TEXT_MAX)}`);
    addLine({ t: "status", text });
  },
  chat(who, text, mine) {
    if (!mine) calcWrite(`M ${asciiClean(who, WHO_MAX)}\t${asciiClean(text, TEXT_MAX)}`);
    addLine({ t: "chat", nick: who, text, mine });
  },
};

// Chrome asks before a public site (like GitHub Pages) can reach this
// computer. If that was refused, say how to allow it.
async function checkLocalAccess() {
  let denied = false;
  for (const name of ["local-network-access", "loopback-network"]) {
    try {
      if ((await navigator.permissions.query({ name })).state === "denied") denied = true;
    } catch {
      /* permission not known to this browser */
    }
  }
  const local = /^(localhost|127\.0\.0\.1)$/.test(location.hostname);
  $("lna-denied").hidden = !denied || local;
}

function relayUrl() {
  // Served by the relay itself: use the same address. Otherwise (GitHub
  // Pages) the relay on this computer.
  const local = location.protocol === "http:" && /^(localhost|127\.0\.0\.1)$/.test(location.hostname);
  const base = local ? `ws://${location.host}` : `ws://127.0.0.1:${settings.relayPort}`;
  const q = new URLSearchParams({ host: settings.server, port: String(settings.ircPort), tls: settings.tls ? "1" : "0" });
  return `${base}/irc?${q}`;
}

function spawnBackend(n) {
  if (settings.backend === "local") return new LocalBackend({ nick: n }, hub);
  const ws = settings.backend === "ws";
  return new IrcBackend(
    {
      url: ws ? settings.wsUrl : relayUrl(),
      label: ws ? settings.wsUrl : `${settings.server}:${settings.ircPort}`,
      relay: !ws,
      channel: settings.channel,
      nick: n,
    },
    hub,
  );
}

function start() {
  if (running) return;
  if (settings.backend === "ws" && !/^wss?:\/\//.test(settings.wsUrl)) {
    addLine({ t: "status", text: "Enter a WebSocket URL (wss://...) under Settings first." });
    return;
  }
  running = true;
  earlyNick = "";
  log("Waiting for the calculator (run SAXCE and plug it in)...");
  const n = calcNick(settings.lastNick);
  if (settings.backend !== "local" && n) {
    log(`Signing on early as ${n} (last used nick) to save time.`);
    backend = spawnBackend(n);
    earlyNick = n;
  } else if (nick) {
    backend = spawnBackend(nick);
  }
  render();
}

function stop() {
  if (!running) return;
  running = false;
  backend?.stop();
  backend = null;
  net = { state: "off", detail: "" };
  $("relay-help").hidden = true;
  log("Bridge stopped.");
  render();
}

function calcHello(raw) {
  const n = calcNick(raw) || asciiClean(raw.replace(/ /g, ""), WHO_MAX);
  if (!n) return;
  log(`[calc] hello from ${n}`);
  const firstHello = !nick;
  nick = n;
  if (settings.lastNick !== n) {
    settings.lastNick = n;
    saveSettings();
  }
  if (running) {
    if (backend) {
      // Already signed on early with the same nick: nothing to change.
      if (!(firstHello && earlyNick === n)) backend.setNick(n);
    } else {
      backend = spawnBackend(n);
    }
  }
  render();
}

function calcSay(text) {
  text = asciiClean(text, TEXT_MAX).trim();
  if (!text) return;
  if (backend) backend.say(text);
  else hub.status("The bridge isn't started; message not sent.");
}

function pcSay(text) {
  text = asciiClean(text.trim(), TEXT_MAX);
  if (!text) return;
  if (!backend || !nick) {
    addLine({ t: "status", text: "The calculator hasn't connected yet; nothing to send as." });
    return;
  }
  calcWrite(`M ${nick}\t${text}`);
  backend.say(text);
}

// ---------- rendering & settings form ----------

function render() {
  const { state, detail } = net;
  const label = { off: "off", connecting: "connecting", joined: "in", down: "down" }[state] || state;
  const cls = { off: "off", connecting: "warn", joined: "ok", down: "bad" }[state] || "bad";
  setLight("net", cls, `Network: ${label} ${detail}`.trim());
  $("run-btn").textContent = running ? "Stop" : "Start";
  $("run-hint").textContent = running ? "" : "The bridge is stopped.";
  for (const el of $("settings").querySelectorAll("input"))
    el.disabled = running && el.id !== "autoStart" && el.id !== "usbIds";
  const ok = running && !!nick && state !== "off";
  $("draft").disabled = !ok;
  $("send-btn").disabled = !ok;
  $("send-btn").textContent = `Send as ${nick || "you"}`;
  $("draft").placeholder = ok ? "Message SAX from the PC" : "Connect the calculator to chat from here too";
  renderCalc();
}

function showModeFields() {
  const mode = document.querySelector("input[name=backend]:checked")?.value || "irc";
  for (const el of document.querySelectorAll("[data-for]")) el.toggleAttribute("data-hidden", el.dataset.for !== mode);
}

function fillForm() {
  for (const r of document.querySelectorAll("input[name=backend]")) r.checked = r.value === settings.backend;
  $("server").value = settings.server;
  $("ircPort").value = settings.ircPort;
  $("tls").checked = settings.tls;
  $("relayPort").value = settings.relayPort;
  $("wsUrl").value = settings.wsUrl;
  $("channel").value = settings.channel;
  $("autoStart").checked = settings.autoStart;
  $("usbIds").value = settings.usbIds.join(", ");
  showModeFields();
}

/** "16c0:05e1, 0x0451:0xe008" -> ["16c0:05e1", "0451:e008"]; anything else is dropped. */
function parseUsbIds(text) {
  const ids = [];
  for (const part of text.toLowerCase().split(/[\s,;]+/)) {
    const m = part.match(/^(?:0x)?([0-9a-f]{1,4})[:/]?(?:0x)?([0-9a-f]{4})$/);
    if (m) {
      const id = `${m[1].padStart(4, "0")}:${m[2]}`;
      if (!ids.includes(id)) ids.push(id);
    }
  }
  return ids;
}

function readForm() {
  const num = (id, what) => {
    const n = Number($(id).value);
    if (Number.isInteger(n) && n >= 1 && n <= 65535) return n;
    addLine({ t: "status", text: `${what} must be a number from 1 to 65535.` });
    return null;
  };
  const ircPort = num("ircPort", "Port");
  const relayPort = num("relayPort", "Relay port");
  if (!ircPort || !relayPort) return false;
  settings = {
    ...settings,
    backend: document.querySelector("input[name=backend]:checked")?.value || "irc",
    server: $("server").value.trim() || DEFAULTS.server,
    ircPort,
    tls: $("tls").checked,
    relayPort,
    wsUrl: $("wsUrl").value.trim(),
    channel: $("channel").value.trim() || DEFAULTS.channel,
    autoStart: $("autoStart").checked,
  };
  saveSettings();
  return true;
}

for (const r of document.querySelectorAll("input[name=backend]")) r.addEventListener("change", showModeFields);

$("run-btn").addEventListener("click", () => {
  if (running) stop();
  else if (readForm()) start();
});

$("usbIds").addEventListener("change", () => {
  settings.usbIds = parseUsbIds($("usbIds").value).filter((id) => !KNOWN_USB_IDS.includes(id));
  $("usbIds").value = settings.usbIds.join(", ");
  saveSettings();
  if (!port && "serial" in navigator) autoConnect(); // a newly listed device may already be allowed
});

$("autoStart").addEventListener("change", () => {
  settings.autoStart = $("autoStart").checked;
  saveSettings();
});

// ---------- sending from the PC ----------

$("draft").addEventListener("input", () => {
  $("count").textContent = `${$("draft").value.length}/200`;
});

$("send-form").addEventListener("submit", (e) => {
  e.preventDefault();
  pcSay($("draft").value);
  $("draft").value = "";
  $("count").textContent = "0/200";
});

// ---------- calculator (Web Serial) ----------

// USB IDs a CE running SAXCE can show up with: srldrvce's generic CDC-ACM
// ID, and TI's own. More can be added under Settings, or by picking a
// device with "Use another device".
const KNOWN_USB_IDS = ["16c0:05e1", "0451:e008"];

let port = null;
let greeted = false;
let writeChain = Promise.resolve();
let helloTimer = null;
let closing = null;
let opening = null;

const hex4 = (n) => n.toString(16).padStart(4, "0");

const usbIds = () => [...new Set([...KNOWN_USB_IDS, ...settings.usbIds])];

const usbFilters = () =>
  usbIds().map((id) => {
    const [v, p] = id.split(":");
    return { usbVendorId: parseInt(v, 16), usbProductId: parseInt(p, 16) };
  });

function portId(p) {
  const i = p.getInfo();
  return i.usbVendorId === undefined ? "" : `${hex4(i.usbVendorId)}:${hex4(i.usbProductId)}`;
}

const isCalc = (p) => usbIds().includes(portId(p));

function renderCalc() {
  if (!port) {
    setLight("calc", "bad", "Calculator: not connected");
    $("calc-btn").textContent = "Connect calculator";
  } else if (!greeted) {
    setLight("calc", "warn", "Calculator: linked, waiting for SAXCE");
    $("calc-btn").textContent = "Disconnect";
  } else {
    setLight("calc", "ok", `Calculator: ${nick || "connected"}`);
    $("calc-btn").textContent = "Disconnect";
  }
}

function calcWrite(line) {
  const p = port;
  if (!p?.writable) return;
  const data = new TextEncoder().encode(line + "\n");
  writeChain = writeChain.then(async () => {
    if (port !== p || !p.writable) return;
    const w = p.writable.getWriter();
    try {
      await w.write(data);
    } catch {
      /* the read loop notices a dead link */
    } finally {
      w.releaseLock();
    }
  });
}

async function openCalc(p) {
  // The load-time reconnect and Chrome's "connect" event can both fire for
  // the same device; only one open may be in progress.
  if (port || closing || opening) return;
  opening = p;
  try {
    await p.open({ baudRate: 115200 });
  } catch (e) {
    if (!port)
      addLine({
        t: "status",
        text: `Couldn't open the calculator (${e.message}). Is it already open in another tab or program, like TI Connect CE?`,
      });
    return;
  } finally {
    opening = null;
  }
  port = p;
  greeted = false;
  try {
    await p.setSignals({ dataTerminalReady: true });
  } catch {
    /* not all drivers support it */
  }
  log("Calculator linked over USB.");
  renderCalc();
  // Ask for a hello until we get one: the calculator's own hello is often
  // sent before the port was opened, and lost.
  calcWrite("R");
  helloTimer = setInterval(() => !greeted && calcWrite("R"), 500);
  readLoop(p);
}

async function readLoop(p) {
  const dec = new TextDecoder();
  let buf = "";
  while (port === p && p.readable) {
    const reader = p.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let i;
        while ((i = buf.indexOf("\n")) >= 0) {
          calcLine(buf.slice(0, i).replace(/\r$/, ""));
          buf = buf.slice(i + 1);
        }
        if (buf.length > 1000) buf = "";
      }
    } catch {
      break; // device gone
    } finally {
      reader.releaseLock();
    }
  }
  if (port === p) closeCalc("Calculator disconnected.");
}

function calcLine(line) {
  const sp = line.indexOf(" ");
  const kind = sp < 0 ? line : line.slice(0, sp);
  const body = sp < 0 ? "" : line.slice(sp + 1);
  if (kind === "H" && body) {
    greeted = true;
    calcHello(body);
  } else if (kind === "M" && body) {
    calcSay(body);
  }
}

async function closeCalc(msg) {
  const p = port;
  if (!p) return;
  port = null;
  greeted = false;
  clearInterval(helloTimer);
  renderCalc();
  closing = (async () => {
    try {
      await writeChain;
      await p.close();
    } catch {
      /* already gone */
    }
  })();
  await closing;
  closing = null;
  if (msg) log(msg);
}

$("calc-btn").addEventListener("click", async () => {
  if (port) {
    closeCalc("Calculator disconnected (by you).");
    return;
  }
  try {
    let p;
    try {
      p = await navigator.serial.requestPort({ filters: usbFilters() });
    } catch (e) {
      // a browser that doesn't take USB filters: show every port instead
      if (e.name !== "TypeError") throw e;
      p = await navigator.serial.requestPort();
    }
    openCalc(p);
  } catch (e) {
    if (e.name !== "NotFoundError") addLine({ t: "status", text: `Couldn't pick a port: ${e.message}` });
  }
});

// Any serial device. Its USB ID is remembered, so it reconnects by itself
// like the known ones.
$("other-btn").addEventListener("click", async () => {
  if (port) return;
  let p;
  try {
    p = await navigator.serial.requestPort();
  } catch (e) {
    if (e.name !== "NotFoundError") addLine({ t: "status", text: `Couldn't pick a port: ${e.message}` });
    return;
  }
  const id = portId(p);
  if (id && !usbIds().includes(id)) {
    settings.usbIds = [...settings.usbIds, id];
    saveSettings();
    $("usbIds").value = settings.usbIds.join(", ");
    log(`Added USB ID ${id}; it will reconnect by itself from now on.`);
  }
  openCalc(p);
});

async function autoConnect() {
  // Calculators this page was allowed before reconnect without a click.
  let ports = [];
  try {
    ports = await navigator.serial.getPorts();
  } catch {
    /* browser doesn't remember ports: Connect is needed each time */
  }
  for (const p of ports) {
    if (isCalc(p)) {
      await openCalc(p);
      if (port) return;
    }
  }
}

// ---------- start up ----------

const firefox = /\bFirefox\/(\d+)/.exec(navigator.userAgent);

fillForm();
render();
if ("serial" in navigator) {
  try {
    navigator.serial.addEventListener("connect", (e) => isCalc(e.target) && openCalc(e.target));
    navigator.serial.addEventListener("disconnect", (e) => e.target === port && closeCalc("Calculator unplugged."));
  } catch {
    /* no plug/unplug events: Connect is clicked by hand, and the read loop notices unplugging */
  }
  if (firefox)
    $("calc-hint").textContent =
      "Run SAXCE, plug it in, then click Connect. The first time, Firefox asks you to allow serial access for this site.";
  autoConnect();
} else {
  $("no-serial").hidden = false;
  if (firefox && Number(firefox[1]) < 151) $("no-serial-ff").hidden = false;
  $("calc-btn").disabled = true;
  $("other-btn").disabled = true;
  $("calc-hint").textContent = "Needs Chrome, Edge, or Firefox 151 or newer.";
}
if (settings.autoStart) start();
window.addEventListener("pagehide", () => backend?.stop());
