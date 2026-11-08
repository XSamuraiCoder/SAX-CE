# SAX-CE

A Cemetech SAX chat client for the TI-84 Plus CE.

**Bridge page: https://xsamuraicoder.github.io/SAX-CE/**

The original calculator SAX program ran over CALCnet/globalCALCnet, which
needs the I/O link port. The CE doesn't have one, so this version talks over
USB instead:

```
TI-84 Plus CE  --USB (Web Serial)-->  bridge page  --WebSocket-->  relay  --IRC-->  #cemetech (EFnet)
   SAXCE.8xp                         Chrome / Edge                 Node.js           relayed to SAX
```

| Where | What |
| --- | --- |
| `dist/SAXCE.8xp` | calculator program |
| `docs/` | the bridge page (static; published with GitHub Pages) |
| `relay/relay.mjs` | the relay: one file, lets the page reach IRC |
| `start-bridge.cmd` / `start-bridge.sh` | start the relay and open the page (Windows / Linux) |
| `python/` | the original Python bridge (standalone, no relay needed) |
| `calc/` | calculator program source |

## Calculator program (`calc/`)

Send `SAXCE.8xp` to the calculator, along with the CE C libraries
(`clibs.8xg`: graphx, fontlibc, fileioc, usbdrvce, srldrvce). Run it from
Cesium, or with `Asm(prgmSAXCE)`.

| Key | Action |
| --- | --- |
| letters / numbers | type (green letter labels) |
| `alpha` | cycle letter mode: abc → ABC → 123 |
| `2nd` | show / hide the on-screen keyboard |
| arrows + `enter` | (keyboard shown) pick and press a key; Shift, Sym, Space, Del, Send |
| `del` | backspace |
| `enter` | send (keyboard hidden) |
| `graph` | send, whether the keyboard is shown or not |
| `↑` / `↓` | scroll history (keyboard hidden) |
| `y=` | next chat font |
| `mode` | change nickname |
| `clear` | clear the input line, or quit if it's empty |

**Nickname:** up to 8 characters, which is what SAX shows. It can use letters,
digits and `- _ [ ] { } | ^ \``, but it can't start with a digit or `-`. If the
nickname is taken on IRC, the bridge adds a digit. A longer name saved by an
older version is kept, cut to its first 8 valid characters (press `mode` to
change it).

**Fonts:** the chat log can use any [fontlibc](https://ce-programming.github.io/toolchain/libraries/fontlibc.html)
font pack on the calculator (an AppVar whose data starts with `FONTPACK`).
Send one or more font packs, then press `y=` to cycle through the fonts in
them; the built-in font is always first. The choice is saved, and all text
(header, chat, input line, keyboard, nickname screen) uses it, with the layout
sized to the font's height. Very tall fonts leave little room for chat while
the keyboard is shown.

Settings (nickname, keyboard on/off, font) are stored in the AppVar `SAXCFG`.

Build: `cd calc && make` (CE C toolchain, tested with CEdev v15.0).

## PC bridge

The bridge is a static web page: it runs entirely in the browser, talking to
the calculator over USB with Web Serial, so it needs **Chrome or Edge**. Open
it at https://xsamuraicoder.github.io/SAX-CE/ or locally (below).

Web pages can't open raw connections to IRC, and EFnet doesn't take WebSocket
connections, so to reach SAX the page needs the **relay** running on the same
computer. It's one file with no packages, for [Node.js](https://nodejs.org/) 18+:

- Windows: double-click `start-bridge.cmd`
- Linux: `./start-bridge.sh`
- or anywhere: `node relay/relay.mjs`

That also serves the page at `http://localhost:8317/` and opens it. Running
it again while it's already running just opens the page. Keep it running
while you chat.

The published page uses the same relay. Chrome asks once before a public
site can reach your computer: allow **local network access** when prompted
(or later under the site's settings). The local page doesn't need this.

The first time, run SAXCE on the calculator, plug it in and click **Connect
calculator**, then pick the calculator in the browser's list. After that the
page reconnects by itself whenever SAXCE starts.

- **Fast sign-on:** EFnet takes about 15 seconds to sign anyone on. The page
  remembers your last nickname and signs on as soon as it opens, so it's
  usually already in the channel by the time the calculator connects.
  Messages sent before it joins are held and sent once it's in.
- **Chat from the PC:** the message box sends as the calculator's user, and
  the message shows on the calculator too.
- **Modes** (under Settings): SAX through the relay (default); any IRC server
  that accepts WebSocket connections, with no relay; or **Local test**, which
  echoes what you send and needs nothing else.
- **Safety:** the relay only listens on this computer, and only accepts the
  bridge page (local or on GitHub Pages); other sites are refused. Use
  `--allow-origin URL` to allow a copy of the page hosted elsewhere, `--port N`
  for another port, and `--no-open` to not open a browser.

Settings are saved in the browser (local storage), separately for the local
and the published page.

**Linux note:** for Chrome to open the calculator, your user needs to be in the
`dialout` group (`sudo usermod -aG dialout $USER`, then log out and back in).

Tests: `npm test` (unit tests, plus the page's IRC client run through the real
relay against a fake IRC server).

**Python bridge:** `python/` has the original bridge in Python, which talks to
the calculator and IRC directly, with no browser or relay. See
[python/README.md](python/README.md).

## Wire protocol

ASCII, one `\n`-terminated line per message, over USB CDC serial:

| Direction | Line | Meaning |
| --- | --- | --- |
| calc → PC | `H <nick>` | hello / nick change |
| calc → PC | `M <text>` | send a chat message |
| PC → calc | `M <nick>\t<text>` | incoming chat message |
| PC → calc | `S <text>` | status notice |
| PC → calc | `R` | request a hello (sent every 0.5 s until one arrives) |
