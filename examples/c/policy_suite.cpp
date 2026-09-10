// examples/c/policy_suite.cpp
//
// Second-stage checks for the c-archive shim: TOFU persistence and
// jump-host tunneling. The lab sshd plays both bastion and target, which is
// enough to exercise the two-hop dial path end to end.
//
// Build:
//   g++ -std=c++17 stage2.cpp ../libsshcore.a -lpthread -o stage2

#include <omegassh/omegassh.h>

#include "probe_connect.h"


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

std::string collect(omegassh_session h, int timeoutMs) {
    std::string out;

    char buf[8192];
    for (;;) {
        if (probe_wait_notify(h, timeoutMs) <= 0) break;
        probe_drain_notify(h);
        for (;;) {
            int n = omegassh_read(h, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, n);
        }
        if (omegassh_alive(h) == 0) break;
        timeoutMs = 400;
    }
    return out;
}

void send(omegassh_session h, const std::string& s) {
    omegassh_write(h, (s.data()), static_cast<int>(s.size()));
}

size_t lineCount(const std::string& path) {
    std::ifstream in(path);
    size_t n = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) n++;
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <key-path> <known-hosts-path> <decoy-pubkey>\n",
                argv[0]);
        return 2;
    }
    const std::string key = argv[1];
    const std::string kh = argv[2];
    const std::string decoy = argv[3];
    int failures = 0;

    const std::string base =
        R"({"host":"127.0.0.1","port":2222,"username":"labuser",)"
        R"("private_key_path":")" + key + R"(","cols":90,"rows":24,)"
        R"("timeout_seconds":10,"known_hosts_path":")" + kh + R"(")";

    // 1. TOFU on an empty known_hosts: accept and persist.
    if (lineCount(kh) != 0) {
        printf("[FAIL] precondition: known_hosts is not empty\n");
        return 1;
    }
    std::string why;
    bool connected = false;
    omegassh_session h = probe_open(base + R"(,"host_key_policy":"tofu"})",
                                    10000, &connected, &why);
    if (!connected) {
        printf("[FAIL] tofu first contact: %s\n", why.c_str());
        if (h >= 0) omegassh_close(h);
        return 1;
    }
    collect(h, 3000);
    omegassh_close(h);
    if (lineCount(kh) != 1) {
        printf("[FAIL] tofu persistence: expected 1 known_hosts line, got %zu\n",
               lineCount(kh));
        failures++;
    } else {
        printf("[ ok ] tofu first contact accepted and pinned the key\n");
    }

    // 2. Strict now succeeds against the key TOFU just pinned.
    h = probe_open(base + R"(,"host_key_policy":"strict"})", 10000, &connected, &why);
    if (!connected) {
        printf("[FAIL] strict after pinning: %s\n", why.c_str());
        if (h >= 0) omegassh_close(h);
        failures++;
    } else {
        printf("[ ok ] strict now succeeds against the pinned key\n");
        collect(h, 3000);
        omegassh_close(h);
    }

    // 3. A pin to a DIFFERENT valid key must fail closed, and TOFU must not
    //    rescue it. Uses a decoy key rather than a corrupted blob: an
    //    unparseable line is a file-load error, not a verification result.
    {
        std::ifstream dec(decoy);
        std::string decoyLine;
        std::getline(dec, decoyLine);
        dec.close();
        std::ofstream out(kh, std::ios::trunc);
        out << "[127.0.0.1]:2222 " << decoyLine << "\n";
        out.close();

        std::string err;
        bool up = false;
        omegassh_session bh = probe_open(base + R"(,"host_key_policy":"tofu"})",
                                         10000, &up, &err);
        if (up) {
            printf("[FAIL] mismatch under tofu: connected anyway\n");
            omegassh_close(bh);
            failures++;
        } else {
            if (bh >= 0) omegassh_close(bh);
            // The reason now comes from omegassh_error rather than from a -1:
            // a host key that does not match is the far end's answer, not a
            // mistake in the configuration, so it arrives as a failed dial.
            bool named = err.find("does not match") != std::string::npos;
            printf("[ %s ] mismatch fails closed even under tofu\n",
                   named ? " ok " : "FAIL");
            if (!named) {
                printf("        got: %s\n", err.c_str());
                failures++;
            }
        }
        std::ofstream reset(kh, std::ios::trunc);
    }

    // 4. Jump host: dial the target through the bastion. Same box on both
    //    hops, which is exactly the case that used to produce false host-key
    //    mismatches when the two dial paths disagreed on algorithm order.
    const std::string jump =
        R"({"host":"127.0.0.1","port":2222,"username":"labuser",)"
        R"("private_key_path":")" + key + R"(",)"
        R"("jump_host":"127.0.0.1","jump_port":2222,"jump_username":"labuser",)"
        R"("jump_key_path":")" + key + R"(",)"
        R"("cols":90,"rows":24,"timeout_seconds":10,)"
        R"("host_key_policy":"tofu","known_hosts_path":")" + kh + R"("})";

    omegassh_session jh = probe_open(jump, 15000, &connected, &why);
    if (!connected) {
        printf("[FAIL] jump-host dial: %s\n", why.c_str());
        if (jh >= 0) omegassh_close(jh);
        failures++;
    } else {
        printf("[ ok ] jump-host dial: tunneled session established\n");
        collect(jh, 3000);
        send(jh, "echo JUMP_OK_$((21*2))\n");
        std::string out = collect(jh, 3000);
        if (out.find("JUMP_OK_42") == std::string::npos) {
            printf("[FAIL] jump-host round-trip: marker missing\n");
            failures++;
        } else {
            printf("[ ok ] jump-host round-trip: marker returned through bastion\n");
        }
        omegassh_close(jh);
    }

    // 5. Legacy algorithm tail must not break a modern server.
    omegassh_session lh = probe_open(
        base + R"(,"host_key_policy":"tofu","legacy_algorithms":true})",
        10000, &connected, &why);
    if (!connected) {
        printf("[FAIL] legacy tail against modern server: %s\n", why.c_str());
        if (lh >= 0) omegassh_close(lh);
        failures++;
    } else {
        printf("[ ok ] legacy tail negotiates fine against a modern server\n");
        collect(lh, 3000);
        omegassh_close(lh);
    }

    // 6. Concurrent sessions must not collide in the handle registry.
    //
    // These two dials genuinely overlap now. They used to be sequential --
    // open blocked until its session was up -- so the registry was only ever
    // written by one dial at a time and this check could not have caught a
    // collision between two. Now both are in flight together, which is also
    // what a session tree does when somebody opens a folder full of devices.
    const std::string tofuCfg = base + R"(,"host_key_policy":"tofu"})";
    omegassh_session a = omegassh_open(tofuCfg.c_str());
    omegassh_session b = omegassh_open(tofuCfg.c_str());
    std::string whyA, whyB;
    bool upA = a >= 0 && probe_await(a, 10000, &whyA);
    bool upB = b >= 0 && probe_await(b, 10000, &whyB);
    if (!upA || !upB || a == b) {
        printf("[FAIL] concurrent sessions: handles %lld / %lld (%s / %s)\n",
               a, b, whyA.c_str(), whyB.c_str());
        if (a >= 0) omegassh_close(a);
        if (b >= 0) omegassh_close(b);
        failures++;
    } else {
        collect(a, 3000);
        collect(b, 3000);
        send(a, "echo SESSION_A\n");
        send(b, "echo SESSION_B\n");
        std::string oa = collect(a, 3000);
        std::string ob = collect(b, 3000);
        bool clean = oa.find("SESSION_A") != std::string::npos &&
                     oa.find("SESSION_B") == std::string::npos &&
                     ob.find("SESSION_B") != std::string::npos;
        printf("[ %s ] two concurrent sessions stay isolated (handles %lld, %lld)\n",
               clean ? " ok " : "FAIL", a, b);
        if (!clean) failures++;
        omegassh_close(a);
        omegassh_close(b);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}