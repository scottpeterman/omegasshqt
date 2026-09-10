// app/antiidle.cpp

#include "app/antiidle.h"

namespace omega::app {

QByteArray antiIdleBytes(const AntiIdleConfig &config) {
    switch (config.keystroke) {
        case AntiIdleKeystroke::Backspace:
            return QByteArray(1, '\x08');
        case AntiIdleKeystroke::SpaceBackspace:
            return QByteArray("\x20\x08", 2);
        case AntiIdleKeystroke::Nul:
            return QByteArray(1, '\0');
        case AntiIdleKeystroke::Custom:
            return config.custom;
    }
    return QByteArray();
}

QString keystrokeName(AntiIdleKeystroke keystroke) {
    switch (keystroke) {
        case AntiIdleKeystroke::Backspace:
            return QStringLiteral("backspace");
        case AntiIdleKeystroke::SpaceBackspace:
            return QStringLiteral("space-backspace");
        case AntiIdleKeystroke::Nul:
            return QStringLiteral("nul");
        case AntiIdleKeystroke::Custom:
            return QStringLiteral("custom");
    }
    return QStringLiteral("backspace");
}

AntiIdleKeystroke keystrokeFromName(const QString &name, bool *known) {
    if (known) {
        *known = true;
    }
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("backspace")) return AntiIdleKeystroke::Backspace;
    if (n == QLatin1String("space-backspace")) {
        return AntiIdleKeystroke::SpaceBackspace;
    }
    if (n == QLatin1String("nul")) return AntiIdleKeystroke::Nul;
    if (n == QLatin1String("custom")) return AntiIdleKeystroke::Custom;

    if (known) {
        *known = false;
    }
    return AntiIdleKeystroke::Backspace;
}

QString bytesToHex(const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toHex());
}

QByteArray hexToBytes(const QString &hex) {
    const QString trimmed = hex.trimmed();
    if (trimmed.isEmpty() || (trimmed.size() % 2) != 0) {
        return QByteArray();
    }
    for (const QChar c : trimmed) {
        if (!isxdigit(c.toLatin1())) {
            return QByteArray();
        }
    }
    // fromHex ignores what it cannot parse rather than refusing, so the digit
    // check above is doing the actual validation. Reaching here means the
    // string is clean and the conversion cannot lose anything.
    return QByteArray::fromHex(trimmed.toLatin1());
}

bool antiIdleAllowed(const AntiIdleConfig &config, bool connected,
                     bool alternateScreen, bool pasting) {
    if (!config.enabled || config.seconds <= 0) {
        return false;
    }
    if (antiIdleBytes(config).isEmpty()) {
        return false;
    }
    return connected && !alternateScreen && !pasting;
}

}  // namespace omega::app
