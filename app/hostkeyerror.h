// app/hostkeyerror.h
//
// Recognising a first-contact host key failure in a dial's error text.
//
// WHY BY STRING. The Go core has the mechanism for a prompt --
// sshcore.Config.HostKeyPrompt is handed the hostname, the key and the
// remote, and persists an accepted key to known_hosts. It cannot be reached
// from here: a callback firing on the dial's goroutine into Qt is the
// foreign-thread problem the C boundary exists to avoid, so capi/open.go
// flattens the policy to two ends -- Strict fails, TOFU accepts silently.
//
// So the flow is: let the dial fail, recognise the failure, ask, re-dial
// under TOFU. Everything the dialog needs is already in the message, because
// the message was written to carry it.
//
// The marker comes from omegassh_unknown_hostkey_marker() at runtime rather
// than being copied here. A literal in this file would be a second definition
// of a load-bearing string, and the failure mode of the two drifting apart is
// a prompt that silently stops appearing.
//
// The MISMATCH failure -- pinned to a different key, re-key or MITM -- must
// never parse. It fails closed on purpose, and offering to trust the offered
// key there is the one outcome worse than refusing to connect. That is
// asserted on the Go side (sshcore/hostkey_message_test.go) and again here.

#ifndef OMEGA_APP_HOSTKEYERROR_H
#define OMEGA_APP_HOSTKEYERROR_H

#include <QString>

namespace omega::app {

struct HostKeyInfo {
    // False for anything that is not a first-contact failure, the mismatch
    // case included. Nothing else in this struct is meaningful when false.
    bool unknownHost = false;

    QString hostname;        // as the far end was addressed, e.g. lab-sw-01:22
    QString keyType;         // ssh-ed25519, ecdsa-sha2-nistp256, ...
    QString fingerprint;     // SHA256:...
    QString knownHostsPath;  // where the entry would be written
};

// Classifies a dial error. The text arrives WRAPPED -- the transport prefixes
// its own context -- so the marker is looked for anywhere in the string, not
// only at the front.
HostKeyInfo parseHostKeyError(const QString &error);

}  // namespace omega::app

#endif  // OMEGA_APP_HOSTKEYERROR_H
