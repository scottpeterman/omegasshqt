// examples/c/transport_probe.cpp
//
// Exercises the non-SSH half of the session surface from C++, the way the Qt
// app will: select() on the notify fd plays the part QSocketNotifier would,
// and every byte and every event is handled on this one thread.
//
// It is a probe rather than a demo. Each check asserts something the shim is
// supposed to guarantee and says which one failed, so a regression names
// itself instead of showing up as a terminal that renders nothing.
//
// Serial enumeration is checked unconditionally -- an empty list is a valid
// answer and means no adapter is plugged in, not a failure. The two session
// legs need something on the far end, so each is opt-in and they combine:
//
//     ./transport_probe                            enumeration and refusals
//     ./transport_probe <host> [port]              ... and a telnet session
//     ./transport_probe --serial <device> [baud]   ... and a serial session
//
// Point the telnet leg at a lab console server, a GNS3 or dynamips
// reverse-telnet port, or tests/faketelnetd.py. Point the serial leg at a
// real adapter -- that is the whole reason it exists, since the unit suite's
// fake port proves the lifecycle but cannot prove a driver enumerates -- or
// at a socat pty pair driven by tests/fakeconsole.py.
//
// Build: cmake --build build --target transport_probe

#include <omegassh/omegassh.h>

#include "probe_connect.h"


#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void ok(const char *what) { std::printf("  ok    %s\n", what); }

void fail(const char *what, const std::string &detail) {
    std::printf("  FAIL  %s\n        %s\n", what, detail.c_str());
    ++failures;
}

// info reports something worth seeing that is NOT a pass/fail claim. The
// serial leg needs it: an idle console with nothing plugged into the far end
// says nothing at all, and asserting that a device spoke would fail on a
// correct cable attached to a quiet switch.
void info(const char *what, const std::string &detail) {
    std::printf("  --    %s\n        %s\n", what, detail.c_str());
}

void check(bool cond, const char *what, const std::string &detail = {}) {
    if (cond) {
        ok(what);
    } else {
        fail(what, detail);
    }
}

std::string takeLastError() {
    char *raw = omegassh_last_error();
    std::string msg = raw ? raw : "";
    omegassh_free(raw);
    return msg;
}

// take converts an owned char* to a std::string and frees it, so no path
// through this file can leak one.
std::string take(char *raw) {
    std::string s = raw ? raw : "";
    omegassh_free(raw);
    return s;
}

// Drain the notify pipe, then pull everything buffered on the Go side and
// every queued event. One wake can cover many bytes and several events, which
// is why both are loops.
std::string drain(omegassh_session h, std::vector<std::string> &events) {
    probe_drain_notify(h);
    std::string out;
    char buf[8192];
    for (;;) {
        int n = omegassh_read(h, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, n);
    }
    for (;;) {
        char *ev = omegassh_next_event(h);
        if (!ev) break;
        events.push_back(take(ev));
    }
    return out;
}

std::string collect(omegassh_session h, int timeoutMs, std::vector<std::string> &events) {
    std::string out;
    for (;;) {
        if (probe_wait_notify(h, timeoutMs) <= 0) break;
        out += drain(h, events);
        timeoutMs = 300;  // settle window once bytes start flowing
    }
    return out;
}

std::string jsonEscape(const std::string &in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------

void probeSerialPorts() {
    std::printf("serial enumeration\n");

    std::string json = take(omegassh_serial_ports());
    if (json.empty()) {
        fail("omegassh_serial_ports returns a list", takeLastError());
        return;
    }
    // An empty array is a valid answer -- no adapter plugged in is not a
    // failure -- so the assertion is about shape, not about content.
    check(json.front() == '[' && json.back() == ']',
          "omegassh_serial_ports returns a JSON array", json);
    std::printf("        %s\n", json.c_str());
}

// Every case here is a configuration mistake, and every one of them is still
// decided synchronously: omegassh_open returns -1 with the reason on this
// thread, before anything is opened and before any of it reaches the network.
// That is the half of the contract the asynchronous dial deliberately did not
// take. A caller can still fix what it is told about here.
void probeRefusals() {
    std::printf("\nrefusals decided before a socket opens\n");

    struct Case {
        const char *what;
        const char *cfg;
    };
    const Case cases[] = {
        {"unknown transport is named, not defaulted",
         R"({"transport":"rlogin","host":"lab-console"})"},
        {"telnet with no host",
         R"({"transport":"telnet"})"},
        {"serial with no serial_port",
         R"({"transport":"serial"})"},
        {"serial with an impossible parity",
         R"({"transport":"serial","serial_port":"/dev/null","parity":"maybe"})"},
        // Telnet has no authentication step and no bastion. A jump host
        // silently dropped would put the session on the wire in plaintext
        // across a link the operator believed was tunneled, so it is refused
        // rather than ignored.
        {"telnet refuses a jump host rather than ignoring it",
         R"({"transport":"telnet","host":"lab-console","jump_host":"lab-bastion"})"},
        {"telnet refuses a vault credential",
         R"({"transport":"telnet","host":"lab-console","credential":"lab-admin"})"},
        {"a log path that cannot be created fails the dial",
         R"({"transport":"telnet","host":"lab-console",)"
         R"("log_path":"/proc/omegassh/nope.log"})"},
    };

    for (const Case &c : cases) {
        omegassh_session h = omegassh_open(c.cfg);
        if (h >= 0) {
            omegassh_close(h);
            fail(c.what, "open succeeded when it should have been refused");
            continue;
        }
        std::string msg = takeLastError();
        check(!msg.empty(), c.what, "refused with an empty message");
        if (!msg.empty()) std::printf("        %s\n", msg.c_str());
    }
}

void probeTelnet(const std::string &host, int port) {
    std::printf("\ntelnet against %s:%d\n", host.c_str(), port);

    std::string log = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                      "/omegassh-transport-probe.log";
    std::string cfg =
        R"({"transport":"telnet","host":")" + jsonEscape(host) + R"(","port":)" +
        std::to_string(port) +
        R"(,"cols":132,"rows":40,"timeout_seconds":10,"log_path":")" +
        jsonEscape(log) + R"("})";

    omegassh_session h = omegassh_open(cfg.c_str());
    if (h < 0) {
        fail("omegassh_open(telnet)", takeLastError());
        return;
    }
    ok("omegassh_open(telnet)");

    // Both answerable before the dial has landed: the transport was chosen in
    // the configuration and the summary was built from it, so a tab is titled
    // while it is still connecting.
    check(take(omegassh_transport(h)) == "telnet", "omegassh_transport reports telnet");
    check(omegassh_state(h) == OMEGASSH_STATE_CONNECTING,
          "state is connecting the moment open returns",
          take(omegassh_state_name(h)));

    std::string why;
    check(probe_await(h, 10000, &why), "the dial reaches connected", why);

    std::string summary = take(omegassh_summary(h));
    check(!summary.empty(), "omegassh_summary is set", summary);
    std::printf("        summary: %s\n", summary.c_str());

    // Serial ignores this and telnet acts on it; the caller does not branch.
    check(omegassh_resize(h, 132, 40) == 0, "omegassh_resize", takeLastError());

    std::vector<std::string> events;
    std::string banner = collect(h, 3000, events);
    check(!banner.empty(), "the device said something", "no bytes before the timeout");
    if (!banner.empty()) {
        std::printf("        first bytes: %.60s\n", banner.c_str());
    }

    static const char cmd[] = "show version\r";
    int n = omegassh_write(h, cmd, static_cast<int>(sizeof(cmd) - 1));
    // The count is the caller's logical byte count, not the on-wire count:
    // telnet doubles a literal 0xFF and expands CR, and a caller comparing
    // this against its own length should not have to know that.
    check(n == static_cast<int>(sizeof(cmd) - 1),
          "omegassh_write reports the logical byte count",
          "wrote " + std::to_string(n) + " of " + std::to_string(sizeof(cmd) - 1));

    collect(h, 2000, events);
    check(omegassh_alive(h) == 1, "session is still alive");

    // Telnet carries no exit status. Reporting 0 would be indistinguishable
    // from a shell that exited cleanly.
    omegassh_close(h);
    check(omegassh_exit_code(h) == -1, "telnet reports no exit status");

    FILE *f = std::fopen(log.c_str(), "rb");
    if (!f) {
        fail("the session log was written", log + " does not exist");
    } else {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fclose(f);
        check(size > 0, "the session log has content",
              log + " is empty");
        std::printf("        log: %s (%ld bytes)\n", log.c_str(), size);
    }
}


void probeSerial(const std::string &device, int baud) {
    std::printf("\nserial on %s at %d\n", device.c_str(), baud);

    // Whether the device we are about to open is one the enumerator listed.
    // Informational, not a check: a socat pty is a legitimate target for this
    // leg and will never appear in the port list.
    {
        std::string ports = take(omegassh_serial_ports());
        info("enumerated?", ports.find(device) != std::string::npos
                                ? "yes, the enumerator lists it"
                                : "no -- fine for a pty, suspicious for a real adapter");
    }

    std::string log = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                      "/omegassh-serial-probe.log";
    std::string cfg =
        R"({"transport":"serial","serial_port":")" + jsonEscape(device) +
        R"(","baud":)" + std::to_string(baud) +
        R"(,"data_bits":8,"parity":"none","stop_bits":"1")"
        R"(,"cols":132,"rows":40,"log_path":")" + jsonEscape(log) + R"("})";

    omegassh_session h = omegassh_open(cfg.c_str());
    if (h < 0) {
        fail("omegassh_open(serial)", takeLastError());
        return;
    }
    ok("omegassh_open(serial)");

    check(take(omegassh_transport(h)) == "serial", "omegassh_transport reports serial");

    // Opening the port IS the whole handshake on serial -- there is no
    // connect step and no authentication step -- so it goes from disconnected
    // straight to connected, and neither of the other two states is ever
    // published here. The dial runs on its own goroutine like the other two,
    // for one contract rather than three, but it has nothing in between to
    // report and does not invent one.
    std::string why;
    check(probe_await(h, 5000, &why), "the port opens and reports connected", why);

    std::vector<std::string> opened;
    for (;;) {
        char *ev = omegassh_next_event(h);
        if (!ev) break;
        opened.push_back(take(ev));
    }
    bool faked = false;
    for (const std::string &e : opened) {
        if (e.find("\"state\":1") != std::string::npos ||
            e.find("\"state\":2") != std::string::npos) {
            faked = true;
        }
    }
    check(!faked, "serial publishes neither connecting nor authenticating");

    std::string summary = take(omegassh_summary(h));
    check(summary.find(device) != std::string::npos &&
              summary.find("8N1") != std::string::npos,
          "summary names the device and the mode", summary);
    std::printf("        summary: %s\n", summary.c_str());

    // A serial console has no window-change concept. This must succeed and do
    // nothing, so that a widget can resize on every layout pass without
    // branching on which transport it is driving.
    check(omegassh_resize(h, 132, 40) == 0,
          "omegassh_resize succeeds and does nothing", takeLastError());

    // Nudge the far end and see whether anything comes back. A bare CR is
    // what a console cable carries on Enter -- the serial transport does no
    // CR/LF expansion, unlike telnet.
    static const char nudge[] = "\r";
    check(omegassh_write(h, nudge, 1) == 1, "omegassh_write", takeLastError());

    std::vector<std::string> events;
    std::string got = collect(h, 2500, events);
    // Not a check. An idle console, or a cable in a switch that is not at a
    // prompt, is silent and correct.
    info("device output", got.empty()
                              ? "nothing in 2.5s -- normal for an idle line"
                              : std::to_string(got.size()) + " bytes: " +
                                    got.substr(0, 60));

    check(omegassh_alive(h) == 1, "session is still alive");

    // The one thing a real port tests that the fake cannot: the lock
    // discipline. Read is called without the mutex held precisely so that a
    // blocked read does not deadlock Close, and on an idle line the read loop
    // IS blocked right now. If that ever regresses, this hangs rather than
    // failing -- so it is timed and reported either way.
    auto t0 = std::chrono::steady_clock::now();
    omegassh_close(h);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    check(ms < 2000, "close unblocks a blocked read promptly",
          std::to_string(ms) + "ms");
    std::printf("        close took %lldms\n", static_cast<long long>(ms));

    // Serial carries no exit status, the same as telnet.
    check(omegassh_exit_code(h) == -1, "serial reports no exit status");

    FILE *f = std::fopen(log.c_str(), "rb");
    if (!f) {
        fail("the session log was created", log + " does not exist");
    } else {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fclose(f);
        // Size is not asserted: a silent line logs zero bytes and that is
        // correct. The file existing is the claim.
        ok("the session log was created");
        std::printf("        log: %s (%ld bytes)\n", log.c_str(), size);
    }
}

}  // namespace

int main(int argc, char **argv) {
    // --serial <device> [baud] is pulled out first; whatever positional
    // arguments remain are the telnet leg, so the two combine in one run.
    std::string serialDevice;
    int serialBaud = 9600;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--serial" && i + 1 < argc) {
            serialDevice = argv[++i];
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                serialBaud = std::atoi(argv[++i]);
            }
            continue;
        }
        positional.push_back(a);
    }

    std::printf("omegassh %s\n\n", take(omegassh_version()).c_str());

    probeSerialPorts();
    probeRefusals();

    if (!positional.empty()) {
        int port = (positional.size() >= 2) ? std::atoi(positional[1].c_str()) : 23;
        probeTelnet(positional[0], port);
    } else {
        std::printf("\ntelnet leg skipped -- no host given\n");
        std::printf("  %s <host> [port]\n", argv[0]);
    }

    if (!serialDevice.empty()) {
        probeSerial(serialDevice, serialBaud);
    } else {
        std::printf("\nserial leg skipped -- no device given\n");
        std::printf("  %s --serial <device> [baud]\n", argv[0]);
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : (std::to_string(failures) + " check(s) FAILED").c_str());
    return failures == 0 ? 0 : 1;
}