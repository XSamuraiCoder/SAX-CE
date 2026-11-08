#!/usr/bin/env python3
"""SAX-CE Bridge: a desktop window for sax_bridge.py.

Run:  python bridge/sax_gui.py
"""

from __future__ import annotations

import json
import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sax_bridge as sb  # noqa: E402

CONFIG_PATH = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "SAX-CE", "bridge.json")
DEFAULTS = {
    "backend": "irc",
    "server": "irc.efnet.org",
    "irc_port": 6667,
    "tls": False,
    "channel": "#cemetech",
    "suffix": "_ce",
    "port": "Auto",
}
AUTO = "Auto"

# chat palette (matches the calculator program)
BG = "#10121c"
FG = "#e6e6e6"
NICK = "#78beff"
SELF = "#8ce68c"
STATUS = "#f0c85a"
DIM = "#787d8c"
OK = "#2fa84f"
WARN = "#d9a400"
BAD = "#d64545"


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg.update({k: v for k, v in json.load(f).items() if k in DEFAULTS})
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg: dict) -> None:
    try:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    except OSError:
        pass


class Light(ttk.Frame):
    """A coloured dot with a label: e.g.  ● Calculator: connected on COM5"""

    def __init__(self, master, title: str):
        super().__init__(master)
        self.dot = tk.Canvas(self, width=12, height=12, highlightthickness=0, bd=0)
        self.dot.pack(side="left", padx=(0, 6))
        self.oval = self.dot.create_oval(1, 1, 11, 11, fill=DIM, outline="")
        ttk.Label(self, text=title + ":", font=("Segoe UI", 9, "bold")).pack(side="left")
        self.text = ttk.Label(self, text="", width=34)
        self.text.pack(side="left", padx=(4, 0))

    def set(self, color: str, text: str) -> None:
        self.dot.itemconfigure(self.oval, fill=color)
        self.text.configure(text=text)


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.cfg = load_config()
        self.events: queue.Queue = queue.Queue()
        self.bridge: sb.Bridge | None = None
        self.thread: threading.Thread | None = None
        self.net_state = "off"

        root.title("SAX-CE Bridge")
        root.geometry("760x560")
        root.minsize(600, 420)
        try:
            ttk.Style().theme_use("vista" if sys.platform == "win32" else "clam")
        except tk.TclError:
            pass

        self._build_settings()
        self._build_status()
        self._build_chat()
        self._build_input()
        self._on_backend_change()
        self._refresh_ports()

        sb.listeners.append(lambda kind, kw: self.events.put((kind, kw)))
        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.after(50, self._pump)
        self._line("Press Start, then run SAXCE on the calculator and plug it in.", "dim")

    # ---------------- layout ----------------

    def _build_settings(self) -> None:
        box = ttk.LabelFrame(self.root, text="Connection", padding=(10, 6))
        box.pack(fill="x", padx=10, pady=(10, 4))

        self.v_backend = tk.StringVar(value=self.cfg["backend"])
        self.v_server = tk.StringVar(value=self.cfg["server"])
        self.v_port = tk.StringVar(value=str(self.cfg["irc_port"]))
        self.v_tls = tk.BooleanVar(value=self.cfg["tls"])
        self.v_channel = tk.StringVar(value=self.cfg["channel"])
        self.v_suffix = tk.StringVar(value=self.cfg["suffix"])
        self.v_serial = tk.StringVar(value=self.cfg["port"])

        mode = ttk.Frame(box)
        mode.grid(row=0, column=0, columnspan=8, sticky="w", pady=(0, 6))
        ttk.Label(mode, text="Mode:").pack(side="left")
        for val, label in (("irc", "SAX via IRC"), ("local", "Local test (echo)")):
            ttk.Radiobutton(mode, text=label, value=val, variable=self.v_backend,
                            command=self._on_backend_change).pack(side="left", padx=(8, 0))

        self.net_widgets = []

        def field(row, col, label, var, width, **kw):
            ttk.Label(box, text=label).grid(row=row, column=col, sticky="e", padx=(0, 4), pady=2)
            w = ttk.Entry(box, textvariable=var, width=width, **kw)
            w.grid(row=row, column=col + 1, sticky="we", padx=(0, 12), pady=2)
            return w

        self.net_widgets += [
            field(1, 0, "Server", self.v_server, 22),
            field(1, 2, "Port", self.v_port, 6),
            field(1, 4, "Channel", self.v_channel, 14),
            field(2, 2, "Nick suffix", self.v_suffix, 6),
        ]
        tls = ttk.Checkbutton(box, text="TLS", variable=self.v_tls)
        tls.grid(row=1, column=6, sticky="w")
        self.net_widgets.append(tls)

        ttk.Label(box, text="Calculator").grid(row=2, column=0, sticky="e", padx=(0, 4), pady=2)
        sp = ttk.Frame(box)
        sp.grid(row=2, column=1, sticky="we", padx=(0, 12))
        self.serial_box = ttk.Combobox(sp, textvariable=self.v_serial, width=14, state="readonly")
        self.serial_box.pack(side="left", fill="x", expand=True)
        self.refresh_btn = ttk.Button(sp, text="↻", width=3, command=self._refresh_ports)
        self.refresh_btn.pack(side="left", padx=(4, 0))

        self.start_btn = ttk.Button(box, text="Start", command=self._toggle, width=10)
        self.start_btn.grid(row=1, column=7, rowspan=2, sticky="nse", padx=(8, 0))
        box.columnconfigure(1, weight=1)
        box.columnconfigure(5, weight=1)

        self.setting_widgets = self.net_widgets + [self.serial_box, self.refresh_btn] + \
            [w for w in mode.winfo_children() if isinstance(w, ttk.Radiobutton)]

    def _build_status(self) -> None:
        bar = ttk.Frame(self.root, padding=(12, 2))
        bar.pack(fill="x")
        self.calc_light = Light(bar, "Calculator")
        self.calc_light.pack(side="left")
        self.net_light = Light(bar, "Chat")
        self.net_light.pack(side="left", padx=(16, 0))
        self.calc_light.set(DIM, "stopped")
        self.net_light.set(DIM, "stopped")

    def _build_chat(self) -> None:
        frame = ttk.Frame(self.root)
        frame.pack(fill="both", expand=True, padx=10, pady=6)
        self.chat = tk.Text(frame, bg=BG, fg=FG, insertbackground=FG, wrap="word",
                            font=("Consolas", 11), padx=8, pady=6, relief="flat", height=8,
                            state="disabled", cursor="arrow")
        sb_ = ttk.Scrollbar(frame, command=self.chat.yview)
        self.chat.configure(yscrollcommand=sb_.set)
        self.chat.pack(side="left", fill="both", expand=True)
        sb_.pack(side="right", fill="y")
        self.chat.tag_configure("time", foreground=DIM)
        self.chat.tag_configure("nick", foreground=NICK, font=("Consolas", 11, "bold"))
        self.chat.tag_configure("self", foreground=SELF, font=("Consolas", 11, "bold"))
        self.chat.tag_configure("status", foreground=STATUS)
        self.chat.tag_configure("dim", foreground=DIM)

    def _build_input(self) -> None:
        row = ttk.Frame(self.root, padding=(10, 0, 10, 10))
        row.pack(fill="x")
        self.v_msg = tk.StringVar()
        self.msg = ttk.Entry(row, textvariable=self.v_msg, font=("Segoe UI", 10))
        self.msg.pack(side="left", fill="x", expand=True)
        self.msg.bind("<Return>", lambda e: self._send())
        self.send_btn = ttk.Button(row, text="Send", command=self._send)
        self.send_btn.pack(side="left", padx=(6, 0))
        self.counter = ttk.Label(row, text="0/200", width=7, anchor="e", foreground=DIM)
        self.counter.pack(side="left")
        self.v_msg.trace_add("write", self._on_type)
        self._set_input_enabled(False)

    # ---------------- helpers ----------------

    def _line(self, text: str, tag: str = "", nick: str | None = None, nick_tag: str = "nick") -> None:
        at_bottom = self.chat.yview()[1] > 0.999
        self.chat.configure(state="normal")
        self.chat.insert("end", time.strftime("%H:%M "), "time")
        if nick is not None:
            self.chat.insert("end", nick + ": ", nick_tag)
        self.chat.insert("end", text + "\n", tag)
        # keep the transcript bounded
        if int(self.chat.index("end-1c").split(".")[0]) > 2000:
            self.chat.delete("1.0", "500.0")
        self.chat.configure(state="disabled")
        if at_bottom:
            self.chat.see("end")

    def _set_input_enabled(self, on: bool) -> None:
        state = "normal" if on else "disabled"
        self.msg.configure(state=state)
        self.send_btn.configure(state=state)

    def _on_type(self, *_):
        v = self.v_msg.get()
        if len(v) > sb.TEXT_MAX:
            self.v_msg.set(v[:sb.TEXT_MAX])
            return
        self.counter.configure(text=f"{len(v)}/{sb.TEXT_MAX}")

    def _on_backend_change(self) -> None:
        state = "normal" if self.v_backend.get() == "irc" and not self.bridge else "disabled"
        for w in self.net_widgets:
            w.configure(state=state)

    def _refresh_ports(self) -> None:
        ports = [AUTO]
        for p in sb.serial.tools.list_ports.comports():
            tag = " (TI)" if sb.is_calc_port(p) else ""
            ports.append(p.device + tag)
        self.serial_box.configure(values=ports)
        if self.v_serial.get() not in ports:
            self.v_serial.set(AUTO)

    def _settings(self) -> dict | None:
        try:
            port = int(self.v_port.get())
        except ValueError:
            self._line("Port must be a number.", "status")
            return None
        serial_port = self.v_serial.get().replace(" (TI)", "")
        return {
            "backend": self.v_backend.get(),
            "server": self.v_server.get().strip(),
            "irc_port": port,
            "tls": bool(self.v_tls.get()),
            "channel": self.v_channel.get().strip() or "#cemetech",
            "suffix": self.v_suffix.get().strip(),
            "port": serial_port,
        }

    # ---------------- actions ----------------

    def _toggle(self) -> None:
        if self.bridge:
            self._stop()
        else:
            self._start()

    def _start(self) -> None:
        cfg = self._settings()
        if not cfg:
            return
        self.cfg = cfg
        save_config(cfg)
        self.bridge = sb.Bridge(
            port=None if cfg["port"] == AUTO else cfg["port"],
            backend=cfg["backend"], server=cfg["server"], irc_port=cfg["irc_port"],
            tls=cfg["tls"], channel=cfg["channel"], suffix=cfg["suffix"])
        self.thread = threading.Thread(target=self.bridge.run, daemon=True)
        self.thread.start()
        self.start_btn.configure(text="Stop")
        for w in self.setting_widgets:
            w.configure(state="disabled")
        self.calc_light.set(WARN, "waiting for SAXCE...")
        self.net_light.set(DIM, "starts when the calculator says hello")

    def _stop(self) -> None:
        bridge, self.bridge = self.bridge, None
        if bridge:
            threading.Thread(target=bridge.stop, daemon=True).start()
        self.start_btn.configure(text="Start")
        for w in self.setting_widgets:
            w.configure(state="readonly" if w is self.serial_box else "normal")
        self._on_backend_change()
        self._set_input_enabled(False)
        self.net_state = "off"
        self.calc_light.set(DIM, "stopped")
        self.net_light.set(DIM, "stopped")
        self._line("Bridge stopped.", "dim")

    def _send(self) -> None:
        text = self.v_msg.get().strip()
        if text and self.bridge and self.bridge.say_from_pc(text):
            self.v_msg.set("")

    def _on_close(self) -> None:
        if self.bridge:
            self.bridge.stop()
        self.root.destroy()

    # ---------------- events from the bridge thread ----------------

    def _pump(self) -> None:
        try:
            while True:
                kind, kw = self.events.get_nowait()
                self._handle(kind, kw)
        except queue.Empty:
            pass
        self.root.after(50, self._pump)

    def _handle(self, kind: str, kw: dict) -> None:
        if not self.bridge:
            return  # stale events after Stop
        if kind == "chat":
            self._line(kw["text"], "", nick=kw["nick"], nick_tag="self" if kw.get("mine") else "nick")
        elif kind == "status":
            self._line(kw["text"], "status")
        elif kind == "log":
            self._line(kw["text"], "dim")
        elif kind == "calc":
            if kw["up"]:
                self.calc_light.set(OK, f"connected on {kw.get('port')}")
                self._set_input_enabled(self.net_state != "off")
            else:
                self.calc_light.set(WARN, "waiting for SAXCE...")
                self._set_input_enabled(False)
        elif kind == "net":
            state, detail = kw["state"], kw.get("detail", "")
            self.net_state = state
            color = {"joined": OK, "connecting": WARN, "down": BAD}.get(state, DIM)
            label = {"joined": "in", "connecting": "connecting to", "down": "disconnected,",
                     "off": "off"}.get(state, state)
            self.net_light.set(color, f"{label} {detail}".strip())
            # Before joining, messages are queued by the bridge, so typing is fine.
            self._set_input_enabled(state != "off")
            if state != "off":
                self.send_btn.configure(text=f"Send as {self.bridge.nick or 'you'}")


def enable_dpi_awareness() -> None:
    """Crisp text on scaled Windows displays."""
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass


def main() -> None:
    enable_dpi_awareness()
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
