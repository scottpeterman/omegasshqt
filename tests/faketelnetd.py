#!/usr/bin/env python3
"""tests/faketelnetd.py

A telnet server that behaves enough like a console server to exercise the
telnet transport with no lab gear in the room.

It is a test fixture, not a device emulator. What it does do is the handful of
things the client has to get right and that a bare TCP echo server would not
provoke at all:

  * opens with IAC DO/WILL offers, so the client's negotiation state machine
    runs before any application data arrives
  * answers a TTYPE SEND, so the configured terminal type is exercised
  * accepts NAWS and prints the window size it was told, which is how a resize
    is confirmed as reaching the wire rather than merely being recorded
  * splits one negotiation sequence across two TCP writes, because an IAC
    sequence arriving in two segments is the parse-state-carried-across-reads
    case and it is the one that breaks quietly
  * sends a literal 0xFF in the banner, which the client must unescape
  * prints a prompt and echoes lines back, so there is something to read

Usage:

    python3 tests/faketelnetd.py                 # 127.0.0.1:2323
    python3 tests/faketelnetd.py --port 2300
    python3 tests/faketelnetd.py --once          # serve one session and exit

Then, in another shell:

    ./build/examples/c/transport_probe 127.0.0.1 2323
"""

import argparse
import socket
import socketserver
import sys
import threading

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_ECHO, OPT_SGA, OPT_TTYPE, OPT_NAWS = 1, 3, 24, 31
TTYPE_IS, TTYPE_SEND = 0, 1


class Handler(socketserver.BaseRequestHandler):
    def setup(self):
        self.sb_opt = None
        self.sb_buf = bytearray()
        self.state = "data"
        self.line = bytearray()

    def handle(self):
        c = self.request
        peer = c.getpeername()
        print(f"[fake-telnetd] session from {peer[0]}:{peer[1]}", flush=True)

        # Opening offers. The DO NAWS is deliberately split across two writes:
        # the client has to carry its parse state across socket reads, and a
        # server that always writes whole sequences never tests that.
        c.sendall(bytes([IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA, IAC, DO, OPT_TTYPE]))
        c.sendall(bytes([IAC, DO]))
        c.sendall(bytes([OPT_NAWS]))

        # A literal 0xFF in the banner, doubled on the wire per RFC 854
        # section 3. The client must deliver exactly one byte upward.
        c.sendall(b"\r\nlab console server (fake)\r\nraw byte: "
                  + bytes([IAC, IAC])
                  + b"\r\nlab-sw1> ")

        while True:
            try:
                chunk = c.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            for b in chunk:
                self.feed(b)
        print(f"[fake-telnetd] session from {peer[0]}:{peer[1]} closed", flush=True)

    # ---- inbound state machine ------------------------------------------

    def feed(self, b):
        st = self.state
        if st == "data":
            if b == IAC:
                self.state = "iac"
            else:
                self.on_data(b)
        elif st == "iac":
            if b == IAC:
                self.on_data(IAC)
                self.state = "data"
            elif b in (WILL, WONT, DO, DONT):
                self.state = {WILL: "will", WONT: "wont", DO: "do", DONT: "dont"}[b]
            elif b == SB:
                self.sb_opt = None
                self.sb_buf.clear()
                self.state = "sb"
            else:
                self.state = "data"
        elif st in ("will", "wont", "do", "dont"):
            self.on_negotiation(st, b)
            self.state = "data"
        elif st == "sb":
            self.sb_opt = b
            self.state = "sbdata"
        elif st == "sbdata":
            if b == IAC:
                self.state = "sbiac"
            else:
                self.sb_buf.append(b)
        elif st == "sbiac":
            if b == SE:
                self.on_subneg()
                self.state = "data"
            elif b == IAC:
                self.sb_buf.append(IAC)
                self.state = "sbdata"
            else:
                self.state = "data"

    def on_negotiation(self, verb, opt):
        name = {OPT_ECHO: "ECHO", OPT_SGA: "SGA", OPT_TTYPE: "TTYPE",
                OPT_NAWS: "NAWS"}.get(opt, str(opt))
        print(f"[fake-telnetd] client {verb.upper()} {name}", flush=True)
        if verb == "will" and opt == OPT_TTYPE:
            # Ask for it, so the client's TTYPE IS reply is exercised.
            self.request.sendall(bytes([IAC, SB, OPT_TTYPE, TTYPE_SEND, IAC, SE]))

    def on_subneg(self):
        if self.sb_opt == OPT_TTYPE and self.sb_buf[:1] == bytes([TTYPE_IS]):
            print(f"[fake-telnetd] terminal type: "
                  f"{self.sb_buf[1:].decode('ascii', 'replace')}", flush=True)
        elif self.sb_opt == OPT_NAWS and len(self.sb_buf) >= 4:
            cols = (self.sb_buf[0] << 8) | self.sb_buf[1]
            rows = (self.sb_buf[2] << 8) | self.sb_buf[3]
            print(f"[fake-telnetd] window size: {cols}x{rows}", flush=True)

    def on_data(self, b):
        if b in (0x0D, 0x0A):
            if self.line:
                text = self.line.decode("ascii", "replace")
                print(f"[fake-telnetd] line: {text!r}", flush=True)
                self.request.sendall(b"\r\nyou said: " + self.line + b"\r\nlab-sw1> ")
                self.line.clear()
            return
        self.line.append(b)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[2])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--once", action="store_true",
                    help="serve one session and exit")
    args = ap.parse_args()

    srv = Server((args.host, args.port), Handler)
    print(f"[fake-telnetd] listening on {args.host}:{args.port}", flush=True)
    try:
        if args.once:
            srv.handle_request()
        else:
            srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
