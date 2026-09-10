// examples/c/shell_smoke.cpp
//
// Proof-of-concept C++ driver for the Go SSH core built as a c-archive.
// Stands in for the Qt app: select() on the notify fd plays the part
// QSocketNotifier would, and every byte is handled on this one thread.
//
// Build:
//   g++ -std=c++17 main.cpp ../libsshcore.a -lpthread -o sshdemo

#include <omegassh/omegassh.h>

#include "probe_connect.h"

#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string takeLastError() {
    char* raw = omegassh_last_error();
    std::string msg = raw ? raw : "";
    omegassh_free(raw);
    return msg;
}

// Drain the notify pipe, then pull everything buffered on the Go side.
// Returns the bytes collected; sets exited when the shell is gone.
std::string drain(omegassh_session h, bool& exited) {
    probe_drain_notify(h);
    std::string out;
    char buf[8192];
    for (;;) {
        int n = omegassh_read(h, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, n);
    }
    exited = omegassh_alive(h) == 0;
    return out;
}

// Wait up to timeoutMs for output, collecting whatever arrives.
std::string collect(omegassh_session h, int timeoutMs, bool& exited) {
    std::string out;
    for (;;) {
        if (probe_wait_notify(h, timeoutMs) <= 0) break;
        out += drain(h, exited);
        if (exited) break;
        timeoutMs = 400;  // settle window once bytes start flowing
    }
    return out;
}

void send(omegassh_session h, const std::string& s) {
    omegassh_write(h, (s.data()), static_cast<int>(s.size()));
}

// A dial that must not succeed. The refusal can arrive either way now -- as
// -1 if the configuration was wrong, or as a failed state if the far end said
// no -- and this case is the second kind: a host key that is not in
// known_hosts is only discovered once the server has presented one.
//
// The handle is closed on both paths. A session that failed to connect still
// owns a notifier until somebody says otherwise.
int expectFail(const char* label, const std::string& cfg) {
    std::string reason;
    bool connected = false;
    omegassh_session h = probe_open(cfg, 10000, &connected, &reason);
    if (connected) {
        printf("[FAIL] %s: connected but should have been refused\n", label);
        omegassh_close(h);
        return 1;
    }
    if (h >= 0) omegassh_close(h);
    printf("[ ok ] %s: refused -- %s\n", label, reason.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <key-path> [known-hosts-path]\n", argv[0]);
        return 2;
    }
    const std::string key = argv[1];
    const std::string knownHosts = argc > 2 ? argv[2] : "";
    int failures = 0;

    const std::string base =
        R"({"host":"127.0.0.1","port":2222,"username":"labuser",)"
        R"("private_key_path":")" + key + R"(","cols":100,"rows":30,)"
        R"("term":"xterm-256color","timeout_seconds":10)";

    // 1. Strict policy against a host with no known_hosts entry must refuse.
    failures += expectFail("strict, unknown host",
                           base + R"(,"host_key_policy":"strict","known_hosts_path":")" +
                               knownHosts + R"("})");

    // 2. Insecure is the explicit lab opt-in and must connect.
    const std::string labCfg = base + R"(,"host_key_policy":"insecure"})";
    std::string reason;
    bool connected = false;
    omegassh_session h = probe_open(labCfg, 10000, &connected, &reason);
    if (h <= 0) {
        printf("[FAIL] insecure connect: refused before dialing -- %s\n",
               reason.c_str());
        return 1;
    }
    if (!connected) {
        printf("[FAIL] insecure connect: %s\n", reason.c_str());
        omegassh_close(h);
        return 1;
    }
    printf("[ ok ] insecure connect: session handle %lld\n", h);

    // The states the dial published on its way here. Before the dial moved off
    // the calling goroutine this sequence did not exist: open returned a
    // session that was already connected, and connecting and authenticating
    // were states no SSH session was ever observed in.
    {
        std::vector<std::string> seen;
        for (;;) {
            char* ev = omegassh_next_event(h);
            if (!ev) break;
            seen.push_back(ev);
            omegassh_free(ev);
        }
        std::string joined;
        for (const std::string& e : seen) joined += e + " ";
        bool sawConnecting = joined.find("\"state\":1") != std::string::npos;
        bool sawAuth = joined.find("\"state\":2") != std::string::npos;
        bool sawConnected = joined.find("\"state\":3") != std::string::npos;
        printf("[ %s ] dial published connecting -> authenticating -> connected\n",
               (sawConnecting && sawAuth && sawConnected) ? " ok " : "FAIL");
        if (!(sawConnecting && sawAuth && sawConnected)) {
            printf("        events: %s\n", joined.c_str());
            failures++;
        }
    }

    bool exited = false;
    std::string banner = collect(h, 3000, exited);
    printf("[ ok ] shell responded with %zu bytes\n", banner.size());

    // 3. Round-trip a command through the pty.
    send(h, "echo LAB_ROUNDTRIP_$((6*7))\n");
    std::string out = collect(h, 3000, exited);
    if (out.find("LAB_ROUNDTRIP_42") == std::string::npos) {
        printf("[FAIL] command round-trip: marker missing\n");
        failures++;
    } else {
        printf("[ ok ] command round-trip: marker returned\n");
    }

    // 4. The pty must report the geometry we asked for.
    send(h, "stty size\n");
    out = collect(h, 3000, exited);
    if (out.find("30 100") == std::string::npos) {
        printf("[FAIL] pty geometry: expected 30 100, got:\n%s\n", out.c_str());
        failures++;
    } else {
        printf("[ ok ] pty geometry: 30 rows x 100 cols\n");
    }

    // 5. Resize must propagate to the remote tty.
    if (omegassh_resize(h, 132, 43) != 0) {
        printf("[FAIL] resize: %s\n", takeLastError().c_str());
        failures++;
    } else {
        send(h, "stty size\n");
        out = collect(h, 3000, exited);
        if (out.find("43 132") == std::string::npos) {
            printf("[FAIL] resize: remote tty still reports:\n%s\n", out.c_str());
            failures++;
        } else {
            printf("[ ok ] resize: remote tty now 43 rows x 132 cols\n");
        }
    }

    // 6. Full-screen app: confirm escape sequences survive the round trip.
    send(h, "TERM=xterm-256color tput setaf 2; echo COLOR_OK; tput sgr0\n");
    out = collect(h, 3000, exited);
    bool sawEsc = out.find("\x1b[") != std::string::npos;
    printf("[ %s ] escape sequences %s intact through the channel\n",
           sawEsc ? " ok " : "FAIL", sawEsc ? "arrive" : "do NOT arrive");
    if (!sawEsc) failures++;

    // 7. Remote exit must be observable without polling the transport.
    send(h, "exit\n");
    collect(h, 3000, exited);
    if (!exited) {
        printf("[FAIL] exit detection: session still reports alive\n");
        failures++;
    } else {
        printf("[ ok ] exit detection: shell exit surfaced through notify fd\n");
    }

    omegassh_close(h);
    omegassh_close(h);  // idempotent
    printf("[ ok ] close is idempotent\n");

    // 8. Closing a handle whose dial is still in flight.
    //
    // This path did not exist before: a dial that blocked its caller could not
    // be closed underneath itself, because the caller was inside it. Now the
    // handle is retired at once and the dial finds it gone when it lands, so
    // the two things worth asserting are that the close returns promptly --
    // an operator closing a tab must not wait out a connect timeout -- and
    // that doing it repeatedly neither crashes nor leaks a session onto a
    // device.
    //
    // Half the attempts go to the lab sshd, which will be mid-handshake, and
    // half to a black hole, which will still be waiting on a SYN. The two land
    // in different places inside the dial and both have to be survivable.
    {
        const std::string live = base + R"(,"host_key_policy":"insecure"})";
        const std::string dead =
            R"({"host":"10.255.255.1","port":22,"username":"labuser",)"
            R"("password":"x","timeout_seconds":30,"host_key_policy":"insecure"})";

        long worst = 0;
        bool refused = false;
        for (int i = 0; i < 20; i++) {
            const std::string& cfg = (i % 2) ? dead : live;
            omegassh_session ch = omegassh_open(cfg.c_str());
            if (ch < 0) {
                refused = true;
                break;
            }
            if (i % 4 == 0) probe_sleep_ms(2);  // sometimes land mid-dial, sometimes at once

            // steady_clock, not clock_gettime: CLOCK_MONOTONIC is POSIX and
            // has no MSVC equivalent. steady_clock is the same guarantee --
            // monotonic, unaffected by wall-clock changes -- and portable.
            const auto t0 = std::chrono::steady_clock::now();
            omegassh_close(ch);
            const auto t1 = std::chrono::steady_clock::now();

            const long ms = static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            if (ms > worst) worst = ms;
        }
        if (refused) {
            printf("[FAIL] close during dial: a dial was refused outright\n");
            failures++;
        } else if (worst > 250) {
            printf("[FAIL] close during dial: slowest close took %ldms\n", worst);
            failures++;
        } else {
            printf("[ ok ] close during dial: 20 closes, slowest %ldms\n", worst);
        }
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}