SAXCE v1.0 - SAX chat for the TI-84 Plus CE
by Akhilesh Gollapudi (XSamuraiCoder)

https://github.com/XSamuraiCoder/SAX-CE


ABOUT
-----
SAXCE lets you chat on Cemetech's SAX from your calculator. The CE has no
link port for CALCnet, so it talks over the USB cable to a PC running the
SAX-CE bridge, which passes the chat on to SAX.


REQUIREMENTS
------------
- TI-84 Plus CE or TI-83 Premium CE (no other libraries needed)
- A computer with Chrome, Edge or Firefox 151+, and a USB cable
- On OS 5.5 and newer, arTIfiCE (or similar) to run assembly programs


FILES
-----
SAXCE.8xp     the program
SAXFONT.8xv   the chat font (DrSans 7)
README.txt    this file


INSTALL
-------
1. Send SAXCE.8xp and SAXFONT.8xv to the calculator with TI Connect CE
   (or TILP or WebTILP  ).
2. Close TI Connect CE afterwards. It can hold on to the USB connection.


SETTING UP THE BRIDGE (on the computer)
---------------------------------------
Web pages can't connect to SAX by themselves, so the bridge needs a small
relay program running on the computer. It needs Node.js (nodejs.org).

1. Download the project from https://github.com/XSamuraiCoder/SAX-CE
2. Run start-bridge.cmd (Windows) or start-bridge.sh (Linux). This starts
   the relay and opens the bridge page. Leave it running while you chat.

The bridge page is also online at https://xsamuraicoder.github.io/SAX-CE/
(Chrome will ask to allow local network access; say yes).


RUNNING
-------
1. Run prgmSAXCE from Cesium, or with Asm(prgmSAXCE) on the home screen.
2. The first time, it asks for a nickname. This is the name people see on
   SAX: up to 8 characters, letters, digits and - _ [ ] { } | ^ `, not
   starting with a digit or -.
3. Plug the calculator into the computer. On the bridge page, click
   "Connect calculator" and pick it from the list (only needed the first
   time). The top bar on the calculator changes to "online".
4. Type and press [enter] or [graph] to send.


KEYS
----
letter keys   type (the green letters above the keys)
[alpha]       switch abc / ABC / 123
[2nd]         show or hide the on-screen keyboard
arrows        move around the on-screen keyboard (when shown)
[enter]       send, or press the selected key when the keyboard is shown
[graph]       send (always)
[del]         backspace
up / down     scroll the chat (keyboard hidden)
[y=]          change font
[mode]        change nickname
[clear]       clear the line, or quit if it's already empty


FONTS
-----
SAXCE uses the font in SAXFONT.8xv (DrSans 7). [y=] switches between it
and a plain 8 pixel font built into the program. Your choice is saved.
Without SAXFONT on the calculator, the built-in font is used.


SETTINGS
--------
Your nickname, keyboard on/off and font are saved in the AppVar SAXCFG.
Delete it to start fresh.


TROUBLESHOOTING
---------------
"offline" in the top bar
    The USB link isn't up. Make sure SAXCE is running, unplug and replug
    the cable, and close TI Connect CE.

"Not connected" when sending
    Same as above. Your message is kept, so send it again once you're
    online.

The bridge page says the relay isn't running
    Start start-bridge.cmd / start-bridge.sh and leave it open.

The program won't start
    On OS 5.5 and newer, assembly programs need arTIfiCE (or similar).

The computer makes a disconnect sound when SAXCE starts
    That's normal: the calculator briefly drops its usual USB connection
    and comes back as a serial port.


HOW IT WORKS
------------
SAX is mirrored to the #cemetech channel on EFnet IRC. The bridge joins
that channel under your nickname and passes messages between it and the
calculator.


CREDITS
-------
Program and bridge: Akhilesh Gollapudi (XSamuraiCoder)
Built with TIpy-asm: https://github.com/XSamuraiCoder/TIpy-asm
USB serial: TIpy-asm's built-in driver, based on the CE C toolchain's
usbdrvce and srldrvce
Font: DrSans, from the CE C toolchain's fontlibc examples
SAX: Cemetech (cemetech.net)


VERSION HISTORY
---------------
1.0   First release.
