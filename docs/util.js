// Pure helpers shared by the server (and its tests).

/** RFC 1459 limit; EFnet enforces it (NICKLEN=9). */
export const NICKLEN = 9;
/** Longest nickname the calculator allows. */
export const CALC_NICK_MAX = 8;
/** Longest nick shown to other people on the calculator. */
export const WHO_MAX = 15;
export const TEXT_MAX = 200;

const nickCharOk = (c, first) =>
  /[A-Za-z]/.test(c) || "[]\\`_^{|}".includes(c) || (!first && /[0-9-]/.test(c));

/**
 * A calculator nickname as SAXCE.8xp keeps it: characters IRC allows
 * (no digit or `-` first), cut to 8. Names saved by older versions
 * (up to 15 chars) come out the same as on the calculator.
 */
export function calcNick(s) {
  let out = "";
  for (const c of s) {
    if (out.length === CALC_NICK_MAX) break;
    if (nickCharOk(c, out.length === 0)) out += c;
  }
  return out;
}

/** A nick IRC will accept, as close to the calculator's as possible. */
export function ircNick(nick, attempt = 0) {
  let base = "";
  for (const c of nick) if (nickCharOk(c, base.length === 0)) base += c;
  if (!base) base = "ceuser";
  if (attempt === 0) return base.slice(0, NICKLEN);
  const tail = String(attempt);
  return base.slice(0, NICKLEN - tail.length) + tail;
}

/** Printable ASCII only (the calculator's font), cut to max. */
export function asciiClean(s, max) {
  let out = "";
  for (const c of s) {
    if (out.length >= max) break;
    out += c >= " " && c <= "~" ? c : c === "\t" ? " " : "?";
  }
  return out;
}

/** Web-SAX users relayed by a bot ("saxjax") show under their own name. */
export function unwrapRelay(sender, text) {
  const bots = ["saxjax", "sax"];
  if (!bots.includes(sender.toLowerCase())) return [sender, text];
  let t = text;
  if (t.startsWith("(")) {
    const i = t.indexOf(") ");
    if (i >= 0) t = t.slice(i + 2);
  }
  for (const [open, close] of [["[", "] "], ["<", "> "]]) {
    if (!t.startsWith(open)) continue;
    const i = t.indexOf(close, 1);
    if (i > 1) {
      const name = t.slice(1, i);
      if (!name.includes(" ")) return [name, t.slice(i + close.length)];
    }
  }
  return [sender, text];
}

/** Remove mIRC colour/bold/underline codes. */
export function stripFormatting(text) {
  return text
    .replace(/\x03(\d{1,2}(,\d{1,2})?)?/g, "")
    .replace(/[\x02\x0f\x16\x1d\x1f]/g, "");
}

/** Split an IRC line into { prefix, sender, cmd, rest }. */
export function parseIrc(line) {
  let prefix = "";
  if (line.startsWith(":")) {
    const i = line.indexOf(" ");
    prefix = i < 0 ? line.slice(1) : line.slice(1, i);
    line = i < 0 ? "" : line.slice(i + 1);
  }
  const i = line.indexOf(" ");
  const cmd = i < 0 ? line : line.slice(0, i);
  const rest = i < 0 ? "" : line.slice(i + 1);
  return { prefix, sender: prefix.split("!")[0], cmd, rest };
}
