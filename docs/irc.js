// Minimal IRC client for the browser. Cemetech's SAX web chat is relayed to
// IRC, so joining the channel puts the calculator in SAX.
//
// Browsers can't open raw TCP, so it speaks IRC over a WebSocket, one IRC
// line per message (the IRCv3 "text.ircv3.net" format). That WebSocket is
// either the SAX-CE relay on this computer (relay/relay.mjs), which passes
// lines on to the real server, or any IRC server with WebSocket support.
//
// Reports through `hub`: net(state, detail), status(text), chat(nick, text, mine).

import { ircNick, parseIrc, stripFormatting, unwrapRelay } from "./util.js";

const MAX_PENDING = 20;

export class IrcBackend {
  /** cfg: { url, label, channel, nick } */
  constructor(cfg, hub) {
    this.cfg = cfg;
    this.nick = cfg.nick;
    this.hub = hub;
    this.pending = [];
    this.delay = 5;
    this.stopped = false;
    this.ws = null;
    this.connect();
    this.flushTimer = setInterval(() => this.flush(), 1000);
  }

  // ---- commands from the hub ----

  setNick(n) {
    this.nick = n;
    this.attempt = 0;
    this.raw(`NICK ${ircNick(n, 0)}`);
  }

  say(text) {
    text = text.replace(/[\r\n]+/g, " "); // one message, one IRC line
    if (this.joined && this.pending.length === 0) {
      this.raw(`PRIVMSG ${this.cfg.channel} :${text}`);
      this.hub.chat(this.current, text, true);
    } else {
      this.queue(text);
    }
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.timer);
    clearInterval(this.flushTimer);
    if (this.ws) {
      this.raw("QUIT :SAX-CE bridge closing");
      const ws = this.ws;
      this.ws = null;
      setTimeout(() => ws.close(), 300);
    }
  }

  // ---- connection ----

  connect() {
    this.hub.net("connecting", this.cfg.label);
    this.hub.status(`Connecting to ${this.cfg.label}...`);
    this.joined = false;
    this.current = "";
    this.attempt = 0;
    this.started = Date.now();
    let opened = false;
    let ws;
    try {
      ws = new WebSocket(this.cfg.url, ["text.ircv3.net"]);
    } catch (e) {
      this.retry(`bad connection URL (${e.message})`);
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      opened = true;
      this.raw(`NICK ${ircNick(this.nick, 0)}`);
      this.raw("USER saxce 0 * :SAX-CE TI-84 Plus CE bridge");
    };
    ws.onmessage = async (ev) => {
      const data = typeof ev.data === "string" ? ev.data : await ev.data.text();
      for (const line of data.split(/\r?\n/)) if (line) this.handle(line);
    };
    ws.onclose = (ev) => {
      if (this.stopped || this.ws !== ws) return;
      this.ws = null;
      this.joined = false;
      const why = opened
        ? ev.reason || "connection closed"
        : this.cfg.relay
          ? "the relay isn't running on this computer (start it with start-bridge)"
          : "couldn't reach the WebSocket server";
      this.retry(why);
    };
  }

  retry(why) {
    // Retry quickly at first, backing off so a dead server isn't hammered.
    if (Date.now() - this.started > 120000) this.delay = 5;
    const d = this.delay;
    this.delay = Math.min(this.delay * 2, 60);
    this.hub.net("down", `${why}; retrying in ${d}s`);
    this.hub.status(`IRC: ${why}. Retrying in ${d}s.`);
    this.timer = setTimeout(() => this.connect(), d * 1000);
  }

  raw(line) {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(line.replace(/[\r\n]/g, ""));
  }

  queue(text) {
    if (this.pending.length >= MAX_PENDING) {
      this.hub.status("Queue full; message dropped.");
      return;
    }
    this.pending.push(text);
    this.hub.status(`Queued; sends when joined (${this.pending.length} waiting).`);
  }

  // Send queued messages about one a second so it isn't seen as a flood.
  flush() {
    if (!this.joined || this.pending.length === 0) {
      this.announced = false;
      return;
    }
    if (!this.announced) {
      const n = this.pending.length;
      this.hub.status(`Sending ${n} queued message${n === 1 ? "" : "s"}.`);
      this.announced = true;
    }
    const text = this.pending.shift();
    this.raw(`PRIVMSG ${this.cfg.channel} :${text}`);
    this.hub.chat(this.current, text, true);
  }

  handle(line) {
    const { sender, cmd, rest } = parseIrc(line);
    const ch = this.cfg.channel;
    const same = (a, b) => a.toLowerCase() === b.toLowerCase();
    switch (cmd) {
      case "PING":
        this.raw(`PONG ${rest}`);
        break;
      case "NOTICE":
        if (rest.includes("Checking Ident")) this.hub.status("Signing on (the server checks ident; ~15s)...");
        break;
      case "001":
        this.current = rest.split(" ")[0];
        if (!same(this.current, this.nick)) this.hub.status(`Your nick ${this.nick} was taken; using ${this.current}.`);
        this.hub.net("connecting", `joining ${ch}`);
        this.raw(`JOIN ${ch}`);
        break;
      case "432":
      case "433":
      case "436":
        this.attempt = (this.attempt || 0) + 1;
        if (this.attempt > 9) this.hub.status("No free IRC nick; pick another nickname on the calculator.");
        else this.raw(`NICK ${ircNick(this.nick, this.attempt)}`);
        break;
      case "NICK":
        if (same(sender, this.current)) this.current = rest.replace(/^:/, "");
        break;
      case "471":
      case "473":
      case "474":
      case "475":
      case "403":
      case "405": {
        const i = rest.indexOf(" :");
        const reason = i >= 0 ? rest.slice(i + 2) : rest;
        this.hub.net("down", `can't join ${ch}: ${reason}`);
        this.hub.status(`Can't join ${ch}: ${reason}`);
        break;
      }
      case "JOIN":
        if (same(sender, this.current)) {
          this.joined = true;
          this.hub.net("joined", `${ch} as ${this.current}`);
          this.hub.status(`Joined ${ch} as ${this.current}.`);
        }
        break;
      case "PRIVMSG": {
        const i = rest.indexOf(" :");
        const target = i >= 0 ? rest.slice(0, i) : rest;
        let text = i >= 0 ? rest.slice(i + 2) : "";
        if (!same(target, ch)) break;
        const action = text.match(/^\x01ACTION (.*)\x01$/);
        if (action) text = `* ${action[1]}`;
        else if (text.startsWith("\x01")) break;
        const [nick, msg] = unwrapRelay(sender, stripFormatting(text));
        this.hub.chat(nick, msg, false);
        break;
      }
      case "ERROR":
        this.hub.status(rest.replace(/^:/, ""));
        break;
    }
  }
}

/** Offline test mode: echoes whatever is sent. Needs nothing else running. */
export class LocalBackend {
  constructor({ nick }, hub) {
    this.nick = nick;
    this.hub = hub;
    queueMicrotask(() => {
      hub.net("joined", "local test mode");
      hub.status("Local test mode (no network).");
      hub.status(`You are ${nick}.`);
    });
  }
  setNick(n) {
    this.nick = n;
    this.hub.status(`You are ${n}.`);
  }
  say(text) {
    this.hub.chat(this.nick, text, true);
    this.hub.chat("bridge", `echo: ${text}`, false);
  }
  stop() {}
}
