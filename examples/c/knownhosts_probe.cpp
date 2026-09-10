// examples/c/knownhosts_probe.cpp
//
// Isolates one question: does a single unparseable line in known_hosts break
// verification for every host in the file, or only for the host on that line?
// x/crypto/ssh/knownhosts parses the whole file up front, so this is worth
// knowing before shipping a client that writes to a shared known_hosts.
//
// Build:
//   g++ -std=c++17 khcheck.cpp ../libsshcore.a -lpthread -o khcheck

#include <omegassh/omegassh.h>

#include "probe_connect.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <key-path> <known-hosts-path>\n", argv[0]);
        return 2;
    }
    const std::string cfg =
        R"({"host":"127.0.0.1","port":2222,"username":"labuser",)"
        R"("private_key_path":")" + std::string(argv[1]) + R"(",)"
        R"("cols":80,"rows":24,"timeout_seconds":10,)"
        R"("host_key_policy":"tofu","known_hosts_path":")" + argv[2] + R"("})";

    // The question is whether verification succeeded, which is now something
    // the dial reports rather than something omegassh_open returns. A handle
    // comes back either way and is closed either way.
    std::string reason;
    bool connected = false;
    omegassh_session h = probe_open(cfg, 10000, &connected, &reason);
    if (h >= 0) omegassh_close(h);
    if (connected) {
        printf("CONNECTED (handle %lld)\n", h);
        return 0;
    }
    printf("REFUSED: %s\n", reason.c_str());
    return 1;
}
