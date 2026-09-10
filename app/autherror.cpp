// app/autherror.cpp

#include "app/autherror.h"

#include <omegassh/omegassh.h>

namespace omega::app {
namespace {

// The marker as the library defines it, fetched once. Cheap to hold: it is a
// constant on the Go side for the life of the process. Same treatment as
// hostkeyerror.cpp's marker().
const QString &marker() {
    static const QString value = [] {
        char *raw = omegassh_auth_failed_marker();
        const QString text = QString::fromUtf8(raw ? raw : "");
        omegassh_free(raw);
        return text;
    }();
    return value;
}

}  // namespace

bool isAuthFailure(const QString &error) {
    const QString &needle = marker();
    if (needle.isEmpty() || error.isEmpty()) {
        return false;
    }
    // Anywhere in the string, not only at the front: the transport prefixes
    // its own context ahead of the marker, the same as it does for host keys.
    return error.contains(needle);
}

}  // namespace omega::app
