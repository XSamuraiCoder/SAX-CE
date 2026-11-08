# SAX-CE Bridge (Python)

The first version of the PC bridge, in Python. It does the same job as the web
bridge: it talks to the calculator over USB serial and relays chat to SAX
through IRC. Unlike the web bridge, it needs nothing else running, since
Python can open IRC connections itself.

```bash
pip install -r requirements.txt
python sax_gui.py          # window with settings, status lights and chat
python sax_bridge.py       # same, in the terminal
python sax_bridge.py --help
```

Needs Python 3.10+ and pyserial. The window uses Tkinter, which ships with
Python on Windows (on Linux, install `python3-tk`).

Differences from the web bridge: it adds `_ce` to the calculator's nickname on
IRC by default (`--suffix ""` to turn that off), and it doesn't sign on early.
