#!/usr/bin/env python3
"""SAX-CE bridge: relays chat between a TI-84 Plus CE (USB serial) and SAX.

Cemetech's SAX chat is mirrored to IRC by a relay bot, so the bridge speaks
plain IRC on the network side. The calculator side uses a tiny line protocol:

  calc -> PC : "H <nick>"           hello / nick change
               "M <text>"           say something
  PC -> calc : "M <nick>\\t<text>"  chat message
               "S <text>"           status notice

Backends:
  irc    connect to an IRC server/channel (default)
  local  offline test mode: echoes what the calculator says
"""

from __future__ import annotations

import argparse
import queue
import socket
import ssl
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: pip install pyserial")

TI_VID = 0x0451
# USB IDs a CE running a srldrvce program can show up with. srldrvce's
# CDC descriptors use the generic 16C0:05E1 "CDC-ACM" ID, not TI's own.
CE_USB_IDS = {(0x16C0, 0x05E1), (0x0451, 0xE008)}


def is_calc_port(p) -> bool:
    return (p.vid, p.pid) in CE_USB_IDS


TEXT_MAX = 200
NICK_MAX = 15


def ascii_clean(s: str, limit: int) -> str:
    out = "".join(c if " " <= c <= "~" else "?" for c in s)
    return out[:limit]


# --------------------------------------------------------------------------
# Events (the GUI subscribes; the CLI just prints)
#
#   emit("log",  text=...)                      diagnostic line
#   emit("chat", nick=..., text=..., mine=bool) chat message
#   emit("status", text=...)                    notice also shown on the calc
#   emit("calc", up=bool, port=str|None)        calculator link state
#   emit("net",  state="off|connecting|joined|down", detail=str)
# --------------------------------------------------------------------------

listeners: list = []


def emit(kind: str, **kw) -> None:
    if not listeners:
        if kind == "chat":
            print(f"<{kw['nick']}> {kw['text']}")
        elif kind in ("log", "status"):
            print(kw["text"])
        elif kind == "calc":
            print(f"[calc] {'connected on ' + str(kw.get('port')) if kw['up'] else 'disconnected'}")
        elif kind == "net":
            print(f"[net] {kw['state']} {kw.get('detail', '')}".rstrip())
        return
    for fn in list(listeners):
        try:
            fn(kind, kw)
        except Exception:
            pass


# --------------------------------------------------------------------------
# Calculator link
# --------------------------------------------------------------------------

class CalcLink:
    def __init__(self, port: str | None):
        self.port_name = port
        self.ser: serial.Serial | None = None
        self.greeted = False
        self.lock = threading.Lock()

    def find_port(self) -> str | None:
        if self.port_name:
            return self.port_name
        for p in serial.tools.list_ports.comports():
            if is_calc_port(p):
                return p.device
        return None

    def open(self) -> bool:
        port = self.find_port()
        if not port:
            return False
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.2)
        except serial.SerialException as e:
            emit("log", text=f"[calc] cannot open {port}: {e}")
            return False
        emit("calc", up=True, port=port)
        return True

    def close(self) -> None:
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def send(self, line: str) -> None:
        if not self.ser:
            return
        data = (line + "\n").encode("ascii", "replace")
        with self.lock:
            try:
                self.ser.write(data)
            except serial.SerialException:
                self.close()

    def chat(self, nick: str, text: str) -> None:
        self.send(f"M {ascii_clean(nick, NICK_MAX)}\t{ascii_clean(text, TEXT_MAX)}")

    def status(self, text: str) -> None:
        emit("status", text=text)
        self.send(f"S {ascii_clean(text, TEXT_MAX)}")

    def read_lines(self):
        """Yield lines from the calculator. Until it says hello ("H ..."),
        keep sending "R" so a calculator that linked up before the port was
        opened (and whose first hello was lost) introduces itself again."""
        buf = b""
        self.greeted = False
        last_request = 0.0
        while self.ser:
            if not self.greeted and time.monotonic() - last_request > 2:
                self.send("R")
                last_request = time.monotonic()
            try:
                chunk = self.ser.read(256)
            except Exception:  # unplugged, or closed from another thread
                self.close()
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                yield raw.decode("ascii", "replace").rstrip("\r")


# --------------------------------------------------------------------------
# Backends
# --------------------------------------------------------------------------

class LocalBackend:
    """Offline test backend."""

    def __init__(self, calc: CalcLink):
        self.calc = calc
        self.nick = "ce"

    def start(self) -> None:
        emit("net", state="joined", detail="local test mode")
        self.calc.status("Local test mode (no network).")

    def set_nick(self, nick: str) -> None:
        self.nick = nick
        self.calc.status(f"You are {nick}.")

    def say(self, text: str) -> None:
        emit("chat", nick=self.nick, text=text, mine=True)
        emit("chat", nick="bridge", text=f"echo: {text}", mine=False)
        self.calc.chat("bridge", f"echo: {text}")

    def stop(self) -> None:
        emit("net", state="off")


class IrcBackend:
    def __init__(self, calc: CalcLink, server: str, port: int, use_tls: bool,
                 channel: str, suffix: str):
        self.calc = calc
        self.server, self.port, self.use_tls = server, port, use_tls
        self.channel = channel
        self.suffix = suffix
        self.nick = "saxce"
        self.sock: socket.socket | None = None
        self.out: queue.Queue[str] = queue.Queue()
        self.running = False
        self.joined = False
        self.nick_attempt = 0
        self.current_nick = ""   # what the server actually calls us
        self.pending: list[str] = []   # said before we were in the channel
        self.pending_lock = threading.Lock()

    # -- public --
    def start(self) -> None:
        self.running = True
        threading.Thread(target=self._run, daemon=True).start()

    def stop(self) -> None:
        self.running = False
        if self.sock:
            try:
                self._raw("QUIT :SAX-CE bridge closing")
                self.sock.close()
            except OSError:
                pass

    def set_nick(self, nick: str) -> None:
        self.nick = nick
        if self.sock:
            self._raw(f"NICK {self._irc_nick()}")

    MAX_PENDING = 20

    def say(self, text: str) -> None:
        with self.pending_lock:
            if not self.joined:
                if len(self.pending) >= self.MAX_PENDING:
                    self.calc.status("Queue full; message dropped.")
                    return
                self.pending.append(text)
                n = len(self.pending)
                self.calc.status(f"Queued; sends when joined ({n} waiting).")
                return
        self._send_chat(text)

    def _send_chat(self, text: str) -> None:
        self._raw(f"PRIVMSG {self.channel} :{text}")
        emit("chat", nick=self.nick, text=text, mine=True)

    def _flush_pending(self) -> None:
        """Send queued messages, about one per second so the server doesn't
        treat the burst as a flood."""
        while self.running and self.joined:
            with self.pending_lock:
                if not self.pending:
                    return
                text = self.pending.pop(0)
            self._send_chat(text)
            time.sleep(1)

    # -- internals --
    # RFC 1459 limit; EFnet enforces it (NICKLEN=9). Longer nicks get cut
    # by the server, so build one that fits instead.
    NICKLEN = 9

    def _irc_nick(self) -> str:
        tail = self.suffix + (str(self.nick_attempt) if self.nick_attempt else "")
        tail = tail[: self.NICKLEN - 1]
        base = self.nick[: self.NICKLEN - len(tail)] or "ce"
        return base + tail

    def _raw(self, line: str) -> None:
        if self.sock:
            try:
                self.sock.sendall((line + "\r\n").encode("utf-8", "replace"))
            except OSError:
                pass

    def _run(self) -> None:
        while self.running:
            try:
                self._session()
            except OSError as e:
                if self.running:
                    self.calc.status(f"IRC error: {e}")
            self.joined = False
            self.sock = None
            if not self.running:
                break
            emit("net", state="down", detail="retrying in 15s")
            self.calc.status("IRC disconnected; retrying in 15s.")
            for _ in range(150):
                if not self.running:
                    break
                time.sleep(0.1)
        emit("net", state="off")

    def _session(self) -> None:
        emit("net", state="connecting", detail=f"{self.server}:{self.port}")
        self.calc.status(f"Connecting to {self.server}...")
        raw = socket.create_connection((self.server, self.port), timeout=30)
        if self.use_tls:
            raw = ssl.create_default_context().wrap_socket(raw, server_hostname=self.server)
        raw.settimeout(300)
        self.sock = raw
        self._raw(f"NICK {self._irc_nick()}")
        self._raw(f"USER saxce 0 * :SAX-CE TI-84 Plus CE bridge")
        buf = b""
        while self.running:
            data = raw.recv(4096)
            if not data:
                break
            buf += data
            while b"\r\n" in buf:
                line, buf = buf.split(b"\r\n", 1)
                self._handle(line.decode("utf-8", "replace"))
        self.sock = None

    def _handle(self, line: str) -> None:
        prefix = ""
        if line.startswith(":"):
            prefix, _, line = line[1:].partition(" ")
        cmd, _, rest = line.partition(" ")
        sender = prefix.split("!", 1)[0]

        if cmd == "PING":
            self._raw(f"PONG {rest}")
        elif cmd == "NOTICE" and "Checking Ident" in rest:
            self.calc.status("Signing on (the server checks ident; ~15s)...")
        elif cmd == "001":
            self.current_nick = rest.split(" ", 1)[0]
            emit("net", state="connecting", detail=f"joining {self.channel}")
            self._raw(f"JOIN {self.channel}")
        elif cmd in ("432", "433", "436"):  # nick invalid / in use / collision
            self.nick_attempt += 1
            if self.nick_attempt > 9:
                self.calc.status("No free IRC nick; change your nick.")
                return
            self._raw(f"NICK {self._irc_nick()}")
        elif cmd == "NICK" and sender.lower() == self.current_nick.lower():
            self.current_nick = rest.lstrip(":")
        elif cmd in ("471", "473", "474", "475", "403", "405"):  # can't join
            reason = rest.split(" :", 1)[-1]
            emit("net", state="down", detail=f"can't join {self.channel}: {reason}")
            self.calc.status(f"Can't join {self.channel}: {reason}")
        elif cmd == "JOIN" and sender.lower() == self.current_nick.lower():
            with self.pending_lock:
                self.joined = True
                queued = len(self.pending)
            emit("net", state="joined", detail=f"{self.channel} as {sender}")
            self.calc.status(f"Joined {self.channel} as {sender}.")
            if queued:
                self.calc.status(f"Sending {queued} queued message{'s' if queued > 1 else ''}.")
                threading.Thread(target=self._flush_pending, daemon=True).start()
        elif cmd == "PRIVMSG":
            target, _, text = rest.partition(" :")
            if target.lower() != self.channel.lower():
                return
            if text.startswith("\x01ACTION ") and text.endswith("\x01"):
                text = "* " + text[8:-1]
            elif text.startswith("\x01"):
                return
            text = strip_irc_formatting(text)
            sender, text = unwrap_relay(sender, text)
            emit("chat", nick=sender, text=text, mine=False)
            self.calc.chat(sender, text)
        elif cmd in ("ERROR",):
            self.calc.status(rest.lstrip(":"))


RELAY_BOTS = {"saxjax", "sax"}


def unwrap_relay(sender: str, text: str) -> tuple[str, str]:
    """Show web-SAX users under their own name when a relay bot forwards them.

    Handles "[nick] text", "<nick> text" and "(SAX) [nick] text" style lines.
    """
    if sender.lower() not in RELAY_BOTS:
        return sender, text
    t = text
    if t.startswith("(") and ") " in t:
        t = t.split(") ", 1)[1]
    for open_, close in (("[", "] "), ("<", "> ")):
        if t.startswith(open_) and close in t:
            name, msg = t[1:].split(close, 1)
            if name and " " not in name:
                return name, msg
    return sender, text


def strip_irc_formatting(text: str) -> str:
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "\x03":  # colour code: \x03NN[,NN]
            i += 1
            for _ in range(2):
                if i < len(text) and text[i].isdigit():
                    i += 1
            if i + 1 < len(text) and text[i] == "," and text[i + 1].isdigit():
                i += 2
                if i < len(text) and text[i].isdigit():
                    i += 1
            continue
        if c in "\x02\x0f\x16\x1d\x1f":
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


# --------------------------------------------------------------------------
# Main loop
# --------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--backend", choices=["irc", "local"], default="irc")
    ap.add_argument("--server", default="irc.efnet.org", help="IRC server")
    ap.add_argument("--irc-port", type=int, default=6667)
    ap.add_argument("--tls", action="store_true", help="use TLS for IRC")
    ap.add_argument("--channel", default="#cemetech", help="IRC channel SAX is mirrored to")
    ap.add_argument("--suffix", default="_ce", help="appended to the calculator nick on IRC")
    args = ap.parse_args()

    bridge = Bridge(port=args.port, backend=args.backend, server=args.server,
                    irc_port=args.irc_port, tls=args.tls, channel=args.channel,
                    suffix=args.suffix)
    try:
        bridge.run()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.stop()


class Bridge:
    """Ties a CalcLink to a backend. run() blocks; stop() may be called from any thread."""

    def __init__(self, port: str | None = None, backend: str = "irc",
                 server: str = "irc.efnet.org", irc_port: int = 6667, tls: bool = False,
                 channel: str = "#cemetech", suffix: str = "_ce"):
        self.calc = CalcLink(port)
        self.channel = channel
        if backend == "local":
            self.backend = LocalBackend(self.calc)
        else:
            self.backend = IrcBackend(self.calc, server, irc_port, tls, channel, suffix)
        self.running = False
        self.started = False
        self.nick: str | None = None

    def run(self) -> None:
        self.running = True
        emit("log", text="Waiting for calculator (run SAXCE on the CE and plug it in)...")
        while self.running:
            if not self.calc.ser:
                if not self.calc.open():
                    time.sleep(1)
                    continue
            for line in self.calc.read_lines():
                if not self.running:
                    break
                kind, _, body = line.partition(" ")
                if kind == "H" and body:
                    self.calc.greeted = True
                    self.nick = ascii_clean(body.replace(" ", ""), NICK_MAX)
                    emit("log", text=f"[calc] hello from {self.nick}")
                    self.backend.set_nick(self.nick)
                    if not self.started:
                        self.backend.start()
                        self.started = True
                    elif getattr(self.backend, "joined", False):
                        self.calc.status(f"Still in {self.channel}.")
                elif kind == "M" and body:
                    self.backend.say(body)
            emit("calc", up=False, port=None)
            time.sleep(1)

    def say_from_pc(self, text: str) -> bool:
        """Send a message typed on the PC, as the calculator's user."""
        text = ascii_clean(text, TEXT_MAX)
        if not text:
            return False
        if not self.started:
            emit("status", text="Calculator hasn't connected yet; nothing to send as.")
            return False
        self.backend.say(text)
        self.calc.chat(self.nick or "you", text)
        return True

    def stop(self) -> None:
        self.running = False
        self.backend.stop()
        self.calc.close()


if __name__ == "__main__":
    main()
