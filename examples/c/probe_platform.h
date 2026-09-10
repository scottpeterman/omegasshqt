/* examples/c/probe_platform.h
 *
 * The three things a probe does to a notify handle, on both platforms.
 *
 * The handle is a pipe fd on POSIX and a SOCKET on Windows -- see
 * capi/notify_windows.go for why: there is no fd a QSocketNotifier can watch
 * on Windows, so the wakeup is a connected loopback socket pair instead. That
 * makes read() wrong there and recv() wrong here, and the same three-line
 * #ifdef was on its way into four example files. qt/omegasshsession.cpp
 * already carries this shim for the real wrapper; this is the same pattern for
 * the probes, in one place, for the same reason probe_connect.h exists.
 *
 * select() itself is portable enough to share: Windows has it in winsock2 and
 * it works on SOCKETs, ignoring the nfds argument. What is NOT portable is
 * <sys/select.h>, the fd type, and nfds, which is all this hides.
 *
 * No WSAStartup call here. Winsock is already initialized in this process --
 * the Go archive created the socket pair -- and a probe that called it would
 * be initializing a library it does not own.
 */

#ifndef OMEGASSH_EXAMPLES_PROBE_PLATFORM_H
#define OMEGASSH_EXAMPLES_PROBE_PLATFORM_H

#include <omegassh/omegassh.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

/* Sleeps. std::this_thread rather than usleep, which is POSIX and was
 * removed from POSIX.1-2008 besides. */
inline void probe_sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* Clears the notify handle's readable state. Whatever comes back is
 * discarded: the handle is a wakeup, never a channel. */
inline void probe_drain_notify(omegassh_session h) {
    const long long handle = omegassh_notify_handle(h);
    if (handle < 0) return;
    char scratch[256];
#ifdef _WIN32
    while (::recv(static_cast<SOCKET>(handle), scratch, sizeof(scratch), 0) > 0) {
    }
#else
    while (::read(static_cast<int>(handle), scratch, sizeof(scratch)) > 0) {
    }
#endif
}

/* Waits up to timeoutMs for the notify handle to go readable. Returns >0 if
 * it did, 0 on timeout, <0 on error -- the same contract select() has, so the
 * call sites keep reading the way they did. */
inline int probe_wait_notify(omegassh_session h, int timeoutMs) {
    const long long handle = omegassh_notify_handle(h);
    if (handle < 0) return -1;

    fd_set rfds;
    FD_ZERO(&rfds);
#ifdef _WIN32
    FD_SET(static_cast<SOCKET>(handle), &rfds);
    const int nfds = 0; /* ignored on Windows; the set carries the SOCKETs */
#else
    const int fd = static_cast<int>(handle);
    FD_SET(fd, &rfds);
    const int nfds = fd + 1;
#endif
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return select(nfds, &rfds, nullptr, nullptr, &tv);
}

#endif /* OMEGASSH_EXAMPLES_PROBE_PLATFORM_H */