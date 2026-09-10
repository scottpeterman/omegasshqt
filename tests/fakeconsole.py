#!/usr/bin/env python3
"""tests/fakeconsole.py

A device on the far end of a serial line, for driving the serial transport
with no console cable in the room.

It is a test fixture, not a device emulator. It speaks the handful of things
that make a serial console recognisably a serial console rather than a socket:

  * a bare-CR line ending, which is what a console cable actually carries --
    the transport does no CR/LF expansion on serial, unlike telnet, and a
    fixture that accepted LF would hide that
  * local echo, because a real device echoes and the transport does not
  * a prompt, an ANSI-coloured banner so the emulator has something to do, and
    two commands worth typing

A real serial port is not something a test can conjure, so this needs a pty
pair. socat makes one:

    socat -d -d pty,raw,echo=0,link=/tmp/omega-console \\
                pty,raw,echo=0,link=/tmp/omega-device &

    python3 tests/fakeconsole.py /tmp/omega-device &
    ./build/examples/qt/terminal_window serial /tmp/omega-console 9600

The fixture holds the *device* end; omegassh opens the *console* end. What
this does NOT prove is that a given driver enumerates or that a real adapter
behaves -- a pty is a pty. It proves the transport, the read loop, the tap and
the widget wiring, which is everything above the driver.
"""

import argparse
import os
import sys
import termios
import tty

PROMPT = b"\r\nlab-con1> "

BANNER = (
    b"\r\n"
    b"\x1b[1;36mlab console (fake)\x1b[0m\r\n"
    b"serial line up -- type 'show version' or 'show clock'\r\n"
)

RESPONSES = {
    b"show version": (
        b"\r\nLab Console Software, Version 0.0.1\r\n"
        b"uptime is 0 minutes\r\n"
    ),
    b"show clock": b"\r\nno clock configured\r\n",
    b"?": b"\r\nshow version\r\nshow clock\r\n",
}


def configure(fd):
    """Put the pty into raw mode so it carries bytes rather than lines."""
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    # Blocking single-byte reads: VMIN 1, VTIME 0.
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[2])
    ap.add_argument("device", help="the DEVICE end of the pty pair")
    args = ap.parse_args()

    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)
    configure(fd)
    print(f"[fake-console] holding {args.device}", flush=True)

    os.write(fd, BANNER)
    os.write(fd, PROMPT)

    line = bytearray()
    while True:
        try:
            chunk = os.read(fd, 1024)
        except OSError:
            break
        if not chunk:
            break
        for b in chunk:
            # A console cable carries a bare CR on Enter. Accepting LF too
            # would mask a transport that expanded it, which telnet does and
            # serial must not.
            if b in (0x0D, 0x0A):
                text = bytes(line)
                line.clear()
                print(f"[fake-console] line: {text!r}", flush=True)
                reply = RESPONSES.get(text.strip())
                if reply is None and text.strip():
                    reply = b"\r\n% unknown command: " + text.strip() + b"\r\n"
                if reply:
                    os.write(fd, reply)
                os.write(fd, PROMPT)
                continue
            if b in (0x7F, 0x08):  # backspace
                if line:
                    line.pop()
                    os.write(fd, b"\b \b")
                continue
            line.append(b)
            os.write(fd, bytes([b]))  # local echo, as a device does

    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
