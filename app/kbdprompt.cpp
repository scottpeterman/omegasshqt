// app/kbdprompt.cpp

#include "app/kbdprompt.h"

#include <omegassh/omegassh.h>

namespace omega::app {
namespace {

// The tags, spelled here to match sshcore/kbdprompt.go. Unlike the marker
// these are not published across the C boundary: they are fixed literals
// either side of a constant that IS published, and a mismatch fails safe --
// an unrecognised tag leaves secret=true, which masks.
constexpr char kSecretTag[] = "[secret]";
constexpr char kVisibleTag[] = "[visible]";

const QString &marker() {
    static const QString value = [] {
        char *raw = omegassh_keyboard_prompt_marker();
        const QString text = QString::fromUtf8(raw ? raw : "");
        omegassh_free(raw);
        return text;
    }();
    return value;
}

}  // namespace

KeyboardPrompt parseKeyboardPrompt(const QString &error) {
    KeyboardPrompt out;

    const QString &needle = marker();
    if (needle.isEmpty() || error.isEmpty()) {
        return out;
    }

    // Anywhere in the string: the transport prefixes its own context, so the
    // marker is not at the front by the time it reaches here.
    const int at = error.indexOf(needle);
    if (at < 0) {
        return out;
    }

    // The tag follows the marker. Search from the marker rather than from the
    // start of the string, so a "[secret]" that happened to appear in earlier
    // context cannot be picked up instead.
    const int afterMarker = at + needle.size();
    int tagAt = error.indexOf(QLatin1String(kSecretTag), afterMarker);
    int tagLen = int(sizeof(kSecretTag)) - 1;
    bool secret = true;

    const int visibleAt = error.indexOf(QLatin1String(kVisibleTag), afterMarker);
    // Whichever comes FIRST after the marker is this message's tag. A question
    // can itself contain the word "[secret]", and it sits after the real tag.
    if (visibleAt >= 0 && (tagAt < 0 || visibleAt < tagAt)) {
        tagAt = visibleAt;
        tagLen = int(sizeof(kVisibleTag)) - 1;
        secret = false;
    }
    if (tagAt < 0) {
        // Marker present but no tag: a message from a version that predates
        // the tags, or one that has been reworded. Not parseable into a
        // question, so it is not a prompt this can raise -- better to show the
        // raw error than a dialog asking nothing.
        return out;
    }

    // Everything after the tag and its ": " is the question, to the end.
    QString question = error.mid(tagAt + tagLen);
    if (question.startsWith(QLatin1String(": "))) {
        question = question.mid(2);
    }
    if (question.isEmpty()) {
        return out;
    }

    // NOT trimmed. The far end will send these bytes again on the next dial
    // and the answer is filed under them; "YubiKey for `speterman': " has a
    // trailing space and it is part of the key.
    out.question = question;
    out.secret = secret;
    out.valid = true;
    return out;
}

}  // namespace omega::app
