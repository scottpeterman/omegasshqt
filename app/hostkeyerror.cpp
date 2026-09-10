// app/hostkeyerror.cpp

#include "app/hostkeyerror.h"

#include <QRegularExpression>

#include <omegassh/omegassh.h>

namespace omega::app {
namespace {

// The marker as the library defines it, fetched once. Cheap to hold: it is a
// constant on the Go side for the life of the process.
const QString &marker() {
    static const QString value = [] {
        char *raw = omegassh_unknown_hostkey_marker();
        const QString text = QString::fromUtf8(raw ? raw : "");
        omegassh_free(raw);
        return text;
    }();
    return value;
}

}  // namespace

HostKeyInfo parseHostKeyError(const QString &error) {
    HostKeyInfo info;

    const QString &needle = marker();
    if (needle.isEmpty() || !error.contains(needle)) {
        return info;
    }

    // "<marker> <host> (<type> <fingerprint>); not in <path>"
    //
    // Anchored on the marker rather than on the whole line, because the
    // transport prefixes context ahead of it. The fingerprint is matched as
    // "not a space and not a close paren" rather than as base64: SHA256
    // fingerprints carry / and + and = , and a character class that forgot
    // one would truncate a fingerprint into something that looks valid.
    static const QRegularExpression pattern(
        QStringLiteral("(\\S+)\\s+\\(([^\\s)]+)\\s+([^\\s)]+)\\);\\s*not in\\s+(.+)$"));

    const int at = error.indexOf(needle) + needle.size();
    const QRegularExpressionMatch match = pattern.match(error, at);
    if (!match.hasMatch()) {
        // The marker is there but the shape is not. Better to treat it as an
        // ordinary failure than to raise a dialog with blank fields in it.
        return info;
    }

    info.unknownHost = true;
    info.hostname = match.captured(1);
    info.keyType = match.captured(2);
    info.fingerprint = match.captured(3);
    info.knownHostsPath = match.captured(4).trimmed();
    return info;
}

}  // namespace omega::app
