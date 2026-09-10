/* examples/c/probe_connect.h
 *
 * Waiting for a dial, in one place.
 *
 * omegassh_open returns as soon as the configuration has been checked and the
 * connect runs behind it, so "did it connect?" is no longer the return value
 * of anything. Every probe in this directory needs the same few lines to find
 * out, and five copies of them would be five chances to write it differently.
 *
 * WHY THIS POLLS. A real application watches the notify handle -- that is what
 * it is for, and qt/omegasshsession.cpp is the worked example. A probe cannot,
 * because the notifier is one wake for bytes AND events: draining it here to
 * learn the state would consume the wake that the banner assertion two lines
 * later is waiting on, and the failure would look like a device that went
 * quiet. Polling omegassh_state touches nothing and races nothing, which is
 * what a test harness wants; omegassh_state exists to be polled.
 */

#ifndef OMEGASSH_EXAMPLES_PROBE_CONNECT_H
#define OMEGASSH_EXAMPLES_PROBE_CONNECT_H

#include <omegassh/omegassh.h>

#include "probe_platform.h"

#include <string>

/* Waits until the session reaches CONNECTED, or ends, or the timeout runs
 * out. Returns true if it connected; otherwise reason holds why, taken from
 * omegassh_error -- the per-session message, since the dial failed on a thread
 * this one never touched and omegassh_last_error therefore knows nothing
 * about it.
 *
 * "Has it ended" is omegassh_alive, NOT a check for DISCONNECTED. The two are
 * not the same question and reading the state that way is wrong for serial:
 * disconnected is where a serial handle STARTS -- opening the port is the
 * whole handshake, so there is no connecting phase and nothing to report until
 * the port is open -- and treating that as terminal declares the session dead
 * a microsecond before it comes up. alive knows the difference between a dial
 * that has not finished and one that has nothing left to give.
 *
 * A timeout is reported as a failure with a message that says so, rather than
 * as a third outcome: a caller that has to branch three ways to find out
 * whether it has a session is a caller with a bug waiting in the third
 * branch. */
inline bool probe_await(omegassh_session h, int timeoutMs, std::string *reason) {
    const int stepMs = 10;
    for (int waited = 0;; waited += stepMs) {
        if (omegassh_state(h) == OMEGASSH_STATE_CONNECTED) {
            return true;
        }
        if (omegassh_alive(h) == 0) {
            if (reason) {
                char *raw = omegassh_error(h);
                *reason = raw ? raw : "";
                omegassh_free(raw);
                if (reason->empty()) *reason = "the session ended without saying why";
            }
            return false;
        }
        if (waited >= timeoutMs) {
            if (reason) {
                char *raw = omegassh_state_name(h);
                *reason = std::string("still ") + (raw ? raw : "?") + " after " +
                          std::to_string(timeoutMs) + "ms";
                omegassh_free(raw);
            }
            return false;
        }
        probe_sleep_ms(stepMs);
    }
}

/* Opens and waits, for the common case where a probe wants a connected
 * session or a reason. Returns the handle either way -- a handle that failed
 * to connect still owns a notifier and still has to be closed, which is the
 * one thing about the new contract that is easy to forget. */
inline omegassh_session probe_open(const std::string &cfg, int timeoutMs,
                                   bool *connected, std::string *reason) {
    omegassh_session h = omegassh_open(cfg.c_str());
    if (h < 0) {
        if (reason) {
            char *raw = omegassh_last_error();
            *reason = raw ? raw : "";
            omegassh_free(raw);
        }
        if (connected) *connected = false;
        return h;
    }
    const bool ok = probe_await(h, timeoutMs, reason);
    if (connected) *connected = ok;
    return h;
}

#endif /* OMEGASSH_EXAMPLES_PROBE_CONNECT_H */