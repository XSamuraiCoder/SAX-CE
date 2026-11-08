#!/usr/bin/env node
// SAX-CE relay: lets the bridge page (a static web page) reach IRC.
//
// Browsers can't open raw TCP connections, and EFnet has no WebSocket
// support, so this passes IRC lines between a WebSocket and the IRC server,
// one line per message (the IRCv3 "text.ircv3.net" format). It also serves
// the page itself at http://localhost:8317/ so everything works offline.
//
// Node 18+, no dependencies. Only listens on this computer, and only
// accepts the bridge page (here or on GitHub Pages) as a client.
//
//   node relay.mjs [--port 8317] [--no-open] [--allow-origin https://example.org]

import http from "node:http";
import net from "node:net";
import tls from "node:tls";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.resolve(here, "..", "docs");
// relay.mjs downloaded on its own has no page next to it: use the published one
const HAVE_SITE = fs.existsSync(path.join(SITE, "index.html"));
const PUBLISHED = "https://xsamuraicoder.github.io/SAX-CE/";
const args = process.argv.slice(2);
const opt = (name) => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined);
const PORT = Number(opt("--port")) || Number(process.env.SAXCE_PORT) || 8317;
const OPEN = !args.includes("--no-open");

// Pages allowed to use the relay: the local copy and the published site.
const ORIGINS = new Set([
  `http://localhost:${PORT}`,
  `http://127.0.0.1:${PORT}`,
  "https://xsamuraicoder.github.io",
  ...args.flatMap((a, i) => (args[i - 1] === "--allow-origin" ? [a] : [])),
]);
const HOSTS = new Set([`localhost:${PORT}`, `127.0.0.1:${PORT}`]);

// ---------- WebSocket (RFC 6455), just what IRC needs ----------

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function frame(opcode, payload) {
  const len = payload.length;
  const head = len < 126 ? Buffer.alloc(2) : len < 65536 ? Buffer.alloc(4) : Buffer.alloc(10);
  head[0] = 0x80 | opcode;
  if (len < 126) head[1] = len;
  else if (len < 65536) {
    head[1] = 126;
    head.writeUInt16BE(len, 2);
  } else {
    head[1] = 127;
    head.writeBigUInt64BE(BigInt(len), 2);
  }
  return Buffer.concat([head, payload]);
}

/** Wrap an upgraded socket: calls onText(str) per message, onClose() once. */
function wsWrap(sock, onText, onClose) {
  let buf = Buffer.alloc(0);
  let parts = [];
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    try {
      sock.end(frame(8, Buffer.alloc(0)));
    } catch {
      /* already gone */
    }
    onClose();
  };
  sock.on("data", (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      const fin = buf[0] & 0x80;
      const op = buf[0] & 0x0f;
      const masked = buf[1] & 0x80;
      let len = buf[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (buf.length < 4) return;
        len = buf.readUInt16BE(2);
        off = 4;
      } else if (len === 127) {
        if (buf.length < 10) return;
        len = Number(buf.readBigUInt64BE(2));
        off = 10;
      }
      if (len > 1 << 20) return close(); // no IRC line is that long
      const maskAt = off;
      if (masked) off += 4;
      if (buf.length < off + len) return;
      const data = Buffer.from(buf.subarray(off, off + len));
      if (masked) for (let i = 0; i < len; i++) data[i] ^= buf[maskAt + (i & 3)];
      buf = buf.subarray(off + len);
      if (op === 8) return close();
      if (op === 9) sock.write(frame(10, data));
      else if (op === 0 || op === 1 || op === 2) {
        parts.push(data);
        if (fin) {
          onText(Buffer.concat(parts).toString("utf8"));
          parts = [];
        }
      }
    }
  });
  sock.on("close", () => {
    closed = true;
    onClose();
  });
  sock.on("error", () => {});
  return {
    send: (text) => !closed && sock.write(frame(1, Buffer.from(text, "utf8"))),
    close,
  };
}

// ---------- IRC passthrough ----------

function relayIrc(sock, host, port, useTls, log) {
  let irc = null;
  let ircBuf = "";
  const ws = wsWrap(
    sock,
    (text) => {
      // Exactly one IRC line per message: line breaks inside are dropped,
      // so a message can never smuggle in a second command.
      const line = text.replace(/[\r\n]/g, "");
      if (line && irc) irc.write(line + "\r\n");
    },
    () => irc?.destroy(),
  );
  const opts = { host, port, timeout: 20000 };
  irc = useTls ? tls.connect({ ...opts, servername: host }) : net.connect(opts);
  irc.setEncoding("utf8");
  irc.once(useTls ? "secureConnect" : "connect", () => {
    irc.setTimeout(0);
    log(`IRC: connected to ${host}:${port}`);
  });
  irc.on("timeout", () => irc.destroy(new Error("connection timed out")));
  irc.on("data", (d) => {
    ircBuf += d;
    let i;
    while ((i = ircBuf.indexOf("\n")) >= 0) {
      const line = ircBuf.slice(0, i).replace(/\r$/, "");
      ircBuf = ircBuf.slice(i + 1);
      if (line) ws.send(line);
    }
  });
  irc.on("error", (e) => log(`IRC: ${e.message}`));
  irc.on("close", () => {
    log(`IRC: ${host} closed`);
    ws.close();
  });
}

// ---------- HTTP ----------

const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css", ".png": "image/png", ".svg": "image/svg+xml", ".ico": "image/x-icon" };

const server = http.createServer((req, res) => {
  if (!HOSTS.has(req.headers.host)) return res.writeHead(403).end();
  const url = new URL(req.url, `http://${req.headers.host}`);
  if (url.pathname === "/health") {
    res.writeHead(200, { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" });
    return res.end(JSON.stringify({ relay: "saxce", version: 1 }));
  }
  if (req.method !== "GET") return res.writeHead(405).end();
  if (!HAVE_SITE) return res.writeHead(302, { Location: PUBLISHED }).end();
  const rel = url.pathname === "/" ? "index.html" : decodeURIComponent(url.pathname.slice(1));
  const file = path.join(SITE, path.normalize(rel));
  if (!file.startsWith(SITE + path.sep)) return res.writeHead(404).end();
  fs.readFile(file, (err, data) => {
    if (err) return res.writeHead(404).end();
    res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream", "Cache-Control": "no-cache" });
    res.end(data);
  });
});

server.on("upgrade", (req, sock) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const key = req.headers["sec-websocket-key"];
  const origin = req.headers.origin || "";
  const reject = (code, why) => {
    sock.end(`HTTP/1.1 ${code} ${why}\r\nConnection: close\r\n\r\n`);
  };
  if (!HOSTS.has(req.headers.host)) return reject(403, "Forbidden");
  if (!ORIGINS.has(origin)) {
    console.log(`Refused a connection from ${origin || "an unknown page"}.`);
    return reject(403, "Forbidden");
  }
  if (url.pathname !== "/irc" || !key) return reject(404, "Not Found");
  const host = url.searchParams.get("host") || "irc.efnet.org";
  const port = Number(url.searchParams.get("port")) || 6667;
  const useTls = url.searchParams.get("tls") === "1";
  if (!/^[A-Za-z0-9.-]+$/.test(host) || port < 1 || port > 65535) return reject(400, "Bad Request");

  const accept = crypto.createHash("sha1").update(key + GUID).digest("base64");
  const protos = String(req.headers["sec-websocket-protocol"] || "").split(",").map((s) => s.trim());
  const proto = protos.includes("text.ircv3.net") ? "Sec-WebSocket-Protocol: text.ircv3.net\r\n" : "";
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n${proto}\r\n`);
  sock.setNoDelay(true);
  const who = origin.replace(/^https?:\/\//, "");
  console.log(`Page connected (${who}); relaying to ${host}:${port}${useTls ? " (TLS)" : ""}.`);
  relayIrc(sock, host, port, useTls, (m) => console.log(m));
});

function openBrowser(url) {
  const [cmd, argv] = process.platform === "win32" ? ["cmd", ["/c", "start", "", url]]
    : process.platform === "darwin" ? ["open", [url]]
    : ["xdg-open", [url]];
  try {
    spawn(cmd, argv, { stdio: "ignore", detached: true }).unref();
  } catch {
    /* no browser; the URL is printed */
  }
}

const PAGE = HAVE_SITE ? `http://localhost:${PORT}/` : PUBLISHED;

server.on("error", (e) => {
  if (e.code === "EADDRINUSE") {
    console.log(`The SAX-CE relay is already running at ${PAGE}`);
    if (OPEN) openBrowser(PAGE);
    process.exit(0);
  }
  throw e;
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`SAX-CE relay running. Bridge page: ${PAGE}  (Ctrl+C to quit)`);
  if (OPEN) openBrowser(PAGE);
});
