// The page's IRC client (docs/irc.js), run in Node, through the real relay,
// against a fake IRC server.

import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { IrcBackend } from "../docs/irc.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const RELAY_PORT = 19000 + Math.floor(Math.random() * 1000);
const ORIGIN = "https://xsamuraicoder.github.io";

/** A tiny IRC server: welcomes, joins, and says hi from a SAX relay bot. */
function fakeIrc() {
  const got = [];
  const server = net.createServer((sock) => {
    let buf = "";
    let nick = "";
    sock.setEncoding("utf8");
    sock.on("error", () => {}); // resets when the test tears down
    sock.on("data", (d) => {
      buf += d;
      let i;
      while ((i = buf.indexOf("\r\n")) >= 0) {
        const line = buf.slice(0, i);
        buf = buf.slice(i + 2);
        got.push(line);
        const [cmd, arg] = line.split(" ");
        if (cmd === "NICK" && arg === "taken") sock.write(":srv 433 * taken :Nickname is already in use\r\n");
        else if (cmd === "NICK") nick = arg;
        if (cmd === "USER" || (cmd === "NICK" && got.some((l) => l.startsWith("USER")) && arg !== "taken"))
          if (nick) sock.write(`:srv 001 ${nick} :Welcome\r\n`);
        if (cmd === "JOIN") {
          sock.write(`:${nick}!u@h JOIN ${arg}\r\n`);
          sock.write(`:saxjax!s@h PRIVMSG ${arg} :(SAX) [Kerm] \x02hello\x02 calc\r\n`);
        }
      }
    });
  });
  return new Promise((res) => server.listen(0, "127.0.0.1", () => res({ server, got, port: server.address().port })));
}

async function startRelay(t) {
  const relay = spawn(process.execPath, [path.join(here, "..", "relay", "relay.mjs"), "--port", String(RELAY_PORT), "--no-open"]);
  t.after(() => relay.kill());
  await new Promise((res) => relay.stdout.once("data", res));
}

const waitFor = async (fn, what) => {
  for (let i = 0; i < 100; i++) {
    const v = fn();
    if (v) return v;
    await new Promise((r) => setTimeout(r, 50));
  }
  assert.fail(`timed out waiting for ${what}`);
};

test("relay serves the page and refuses other sites", async (t) => {
  await startRelay(t);
  const page = await fetch(`http://localhost:${RELAY_PORT}/`);
  assert.match(await page.text(), /SAX-CE Bridge/);
  const health = await fetch(`http://127.0.0.1:${RELAY_PORT}/health`);
  assert.equal((await health.json()).relay, "saxce");

  const ws = new WebSocket(`ws://127.0.0.1:${RELAY_PORT}/irc?host=127.0.0.1&port=1`, { headers: { Origin: "https://evil.example" } });
  const result = await new Promise((res) => {
    ws.onopen = () => res("open");
    ws.onerror = () => res("refused");
  });
  assert.equal(result, "refused");
});

test("page IRC client signs on, joins and chats through the relay", async (t) => {
  await startRelay(t);
  const irc = await fakeIrc();
  t.after(() => irc.server.close());

  // Browsers send the page's Origin; Node's WebSocket needs it set.
  const NodeWS = globalThis.WebSocket;
  globalThis.WebSocket = class extends NodeWS {
    constructor(url, protocols) {
      super(url, { protocols, headers: { Origin: ORIGIN } });
    }
  };
  t.after(() => (globalThis.WebSocket = NodeWS));

  const events = [];
  const hub = {
    net: (state, detail) => events.push({ t: "net", state, detail }),
    status: (text) => events.push({ t: "status", text }),
    chat: (nick, text, mine) => events.push({ t: "chat", nick, text, mine }),
  };
  const b = new IrcBackend(
    { url: `ws://127.0.0.1:${RELAY_PORT}/irc?host=127.0.0.1&port=${irc.port}&tls=0`, label: "fake", relay: true, channel: "#cemetech", nick: "xsamcalc" },
    hub,
  );
  t.after(() => b.stop());

  b.say("queued before joining");
  await waitFor(() => events.find((e) => e.t === "net" && e.state === "joined"), "join");
  assert.ok(irc.got.includes("NICK xsamcalc"));
  assert.ok(irc.got.includes("JOIN #cemetech"));

  const kerm = await waitFor(() => events.find((e) => e.t === "chat" && !e.mine), "relayed chat");
  assert.deepEqual([kerm.nick, kerm.text], ["Kerm", "hello calc"]);

  await waitFor(() => irc.got.includes("PRIVMSG #cemetech :queued before joining"), "queued message sent");
  b.say("hi from the CE\r\nQUIT :sneaky");
  await waitFor(() => irc.got.some((l) => l.startsWith("PRIVMSG #cemetech :hi from the CE")), "message");
  assert.ok(!irc.got.includes("QUIT :sneaky"), "a message can't inject another IRC line");

  // The relay alone also keeps one message to one line.
  const ws = new globalThis.WebSocket(`ws://127.0.0.1:${RELAY_PORT}/irc?host=127.0.0.1&port=${irc.port}`, ["text.ircv3.net"]);
  await new Promise((res) => (ws.onopen = res));
  ws.send("NICK raw\r\nQUIT :sneaky2");
  await waitFor(() => irc.got.some((l) => l.startsWith("NICK rawQUIT")), "relay joins lines");
  assert.ok(!irc.got.includes("QUIT :sneaky2"));
  ws.close();
});
