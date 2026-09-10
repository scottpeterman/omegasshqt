// app/omegasettings.cpp

#include "app/omegasettings.h"

#include "app/settings.h"

#include <algorithm>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSaveFile>

namespace omega::app {
namespace {

constexpr const char *kAntiIdle = "anti_idle";
constexpr const char *kSshDefaultAuth = "ssh_default_auth";
constexpr const char *kChrome = "chrome";
constexpr const char *kTitleBar = "title_bar";
constexpr const char *kUiFontSize = "ui_font_size";
constexpr const char *kWheelAltScreen = "wheel_alt_screen";

// Small enough to be unreadable and large enough to push the menus off the
// bar are both worse than a clamp. Anything outside is pulled in rather than
// refused: a hand-edited omega.json should not stop the application starting.
constexpr int kUiFontMin = 9;
constexpr int kUiFontMax = 28;

bool boolField(const QJsonObject &o, const char *key, bool fallback,
               QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) return fallback;
    if (!v.isBool()) {
        if (warnings) {
            *warnings << QStringLiteral("%1 is not a boolean; using the default")
                             .arg(QLatin1String(key));
        }
        return fallback;
    }
    return v.toBool();
}

int intField(const QJsonObject &o, const char *key, int fallback,
             QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) return fallback;
    if (!v.isDouble()) {
        if (warnings) {
            *warnings << QStringLiteral("%1 is not a number; using %2")
                             .arg(QLatin1String(key))
                             .arg(fallback);
        }
        return fallback;
    }
    return v.toInt(fallback);
}

QString stringField(const QJsonObject &o, const char *key,
                    const QString &fallback, QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) return fallback;
    if (!v.isString()) {
        if (warnings) {
            *warnings << QStringLiteral("%1 is not a string; using the default")
                             .arg(QLatin1String(key));
        }
        return fallback;
    }
    return v.toString();
}

}  // namespace

const char *sshDefaultAuthName(SshDefaultAuth mode) {
    switch (mode) {
        case SshDefaultAuth::Ask:          return "ask";
        case SshDefaultAuth::Agent:        return "agent";
        case SshDefaultAuth::VaultDefault: break;
    }
    return "vault_default";
}

SshDefaultAuth sshDefaultAuthFromName(const QString &name, bool *known) {
    if (known) *known = true;
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("ask")) return SshDefaultAuth::Ask;
    if (n == QLatin1String("agent")) return SshDefaultAuth::Agent;
    // Anything unrecognised lands on the mode that asks a person before it
    // dials, rather than one that silently picks credentials off a typo.
    if (n != QLatin1String("vault_default") && known) *known = false;
    return SshDefaultAuth::VaultDefault;
}

QString sshDefaultAuthLabel(SshDefaultAuth mode) {
    // Lower case, because both places this is read put it after words. The
    // settings row is "When a session names no credential" and the session
    // editor's inherit row is "(default - ...)"; a capitalised label reads as
    // a new sentence inside either.
    switch (mode) {
        case SshDefaultAuth::Ask:
            return QObject::tr("ask on connect");
        case SshDefaultAuth::Agent:
            return QObject::tr("the SSH agent");
        case SshDefaultAuth::VaultDefault:
            break;
    }
    return QObject::tr("the vault's default credential");
}

const char *chromeName(Chrome chrome) {
    switch (chrome) {
        case Chrome::Classic: return "classic";
        case Chrome::Token:   break;
    }
    return "token";
}

Chrome chromeFromName(const QString &name, bool *known) {
    if (known) *known = true;
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("classic")) return Chrome::Classic;
    if (n != QLatin1String("token") && known) *known = false;
    return Chrome::Token;
}

QString chromeLabel(Chrome chrome) {
    switch (chrome) {
        case Chrome::Classic:
            return QObject::tr("Classic (matches nterm-qt)");
        case Chrome::Token:
            break;
    }
    return QObject::tr("Omega");
}

TitleBar defaultTitleBar() {
#ifdef Q_OS_MACOS
    // See the header: the traffic lights are the system's and are placed
    // where the merged bar wants to be.
    return TitleBar::Native;
#else
    return TitleBar::Merged;
#endif
}

const char *titleBarName(TitleBar bar) {
    switch (bar) {
        case TitleBar::Native: return "native";
        case TitleBar::Merged: break;
    }
    return "merged";
}

TitleBar titleBarFromName(const QString &name, bool *known) {
    if (known) *known = true;
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("native")) return TitleBar::Native;
    if (n == QLatin1String("merged")) return TitleBar::Merged;
    if (known) *known = false;
    // Unrecognised lands on the platform default rather than on Merged: a
    // typo should not hand somebody a frameless window on a platform where
    // that is the wrong call.
    return defaultTitleBar();
}

QString titleBarLabel(TitleBar bar) {
    switch (bar) {
        case TitleBar::Native:
            return QObject::tr("System title bar");
        case TitleBar::Merged:
            break;
    }
    return QObject::tr("Merged into the menu bar");
}

QString OmegaSettings::toJson() const {
    QJsonObject idle;
    idle.insert(QStringLiteral("enabled"), anti_idle.enabled);
    idle.insert(QStringLiteral("seconds"), anti_idle.seconds);
    idle.insert(QStringLiteral("keystroke"), keystrokeName(anti_idle.keystroke));

    // Written whatever the keystroke is, so switching to custom and back does
    // not lose the bytes somebody worked out for a device.
    idle.insert(QStringLiteral("custom_hex"), bytesToHex(anti_idle.custom));

    QJsonObject root;
    root.insert(QLatin1String(kAntiIdle), idle);
    root.insert(QLatin1String(kSshDefaultAuth),
                QString::fromLatin1(sshDefaultAuthName(ssh_default_auth)));
    root.insert(QLatin1String(kChrome),
                QString::fromLatin1(chromeName(chrome)));
    root.insert(QLatin1String(kTitleBar),
                QString::fromLatin1(titleBarName(title_bar)));
    root.insert(QLatin1String(kUiFontSize), ui_font_size);
    root.insert(QLatin1String(kWheelAltScreen), wheel_alt_screen);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

OmegaSettings OmegaSettings::fromJson(const QByteArray &text,
                                      QStringList *warnings) {
    OmegaSettings s;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
    if (doc.isNull() || !doc.isObject()) {
        if (warnings) {
            *warnings << QStringLiteral("not a JSON object; using defaults");
        }
        return s;
    }

    const QJsonObject root = doc.object();

    // Read BEFORE anti_idle, which returns early on a file that has no
    // anti_idle object. A top-level field parsed after that block would be
    // dropped by any omega.json written before anti_idle existed.
    {
        bool knownMode = true;
        const QString mode = stringField(
            root, kSshDefaultAuth,
            QString::fromLatin1(sshDefaultAuthName(s.ssh_default_auth)), warnings);
        s.ssh_default_auth = sshDefaultAuthFromName(mode, &knownMode);
        if (!knownMode && warnings) {
            *warnings << QStringLiteral("unknown ssh_default_auth \"%1\"; using %2")
                             .arg(mode, QString::fromLatin1(
                                            sshDefaultAuthName(s.ssh_default_auth)));
        }
    }

    // Also before anti_idle, and for the same reason as the block above.
    {
        bool knownChrome = true;
        const QString name = stringField(
            root, kChrome, QString::fromLatin1(chromeName(s.chrome)), warnings);
        s.chrome = chromeFromName(name, &knownChrome);
        if (!knownChrome && warnings) {
            *warnings << QStringLiteral("unknown chrome \"%1\"; using %2")
                             .arg(name,
                                  QString::fromLatin1(chromeName(s.chrome)));
        }
    }

    // Also before anti_idle, same reason again.
    {
        const int px = intField(root, kUiFontSize, s.ui_font_size, warnings);
        const int clamped = std::clamp(px, kUiFontMin, kUiFontMax);
        if (clamped != px && warnings) {
            *warnings << QStringLiteral("ui_font_size %1 is out of range; using %2")
                             .arg(px)
                             .arg(clamped);
        }
        s.ui_font_size = clamped;
    }

    s.wheel_alt_screen =
        boolField(root, kWheelAltScreen, s.wheel_alt_screen, warnings);

    {
        bool knownBar = true;
        const QString name = stringField(
            root, kTitleBar, QString::fromLatin1(titleBarName(s.title_bar)),
            warnings);
        s.title_bar = titleBarFromName(name, &knownBar);
        if (!knownBar && warnings) {
            *warnings << QStringLiteral("unknown title_bar \"%1\"; using %2")
                             .arg(name, QString::fromLatin1(
                                            titleBarName(s.title_bar)));
        }
    }

    const QJsonValue idleValue = root.value(QLatin1String(kAntiIdle));
    if (!idleValue.isObject()) {
        if (!idleValue.isUndefined() && warnings) {
            *warnings << QStringLiteral("anti_idle is not an object; using defaults");
        }
        return s;
    }

    const QJsonObject idle = idleValue.toObject();
    s.anti_idle.enabled = boolField(idle, "enabled", s.anti_idle.enabled, warnings);
    s.anti_idle.seconds = intField(idle, "seconds", s.anti_idle.seconds, warnings);

    bool known = true;
    const QString name =
        stringField(idle, "keystroke", keystrokeName(s.anti_idle.keystroke), warnings);
    s.anti_idle.keystroke = keystrokeFromName(name, &known);
    if (!known && warnings) {
        *warnings << QStringLiteral("unknown anti_idle keystroke \"%1\"; using %2")
                         .arg(name, keystrokeName(s.anti_idle.keystroke));
    }

    const QString hex = stringField(idle, "custom_hex", QString(), warnings);
    if (!hex.isEmpty()) {
        s.anti_idle.custom = hexToBytes(hex);
        if (s.anti_idle.custom.isEmpty() && warnings) {
            // Named rather than swallowed: a custom keystroke that silently
            // does nothing looks like the feature is broken.
            *warnings << QStringLiteral(
                             "anti_idle custom_hex \"%1\" is not clean hex; ignored")
                             .arg(hex);
        }
    }

    return s;
}

OmegaSettingsManager::OmegaSettingsManager(const QString &path) : path_(path) {}

QString OmegaSettingsManager::defaultFile() {
    // Derived rather than spelled out again. This used to carry its own
    // literal, which meant the directory was defined in two places and moving
    // it would have left omega.json behind in the old one.
    return QDir(SettingsManager::defaultConfigDir())
        .filePath(QStringLiteral("omega.json"));
}

bool OmegaSettingsManager::load() {
    error_.clear();
    warnings_.clear();

    QFile f(path_);
    if (!f.exists()) {
        // A first run has no file, and defaults are the right answer.
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        error_ = QStringLiteral("cannot read %1: %2").arg(path_, f.errorString());
        return false;
    }
    settings_ = OmegaSettings::fromJson(f.readAll(), &warnings_);
    return true;
}

bool OmegaSettingsManager::save() {
    error_.clear();

    const QFileInfo info(path_);
    if (!QDir().mkpath(info.absolutePath())) {
        error_ = QStringLiteral("cannot create %1").arg(info.absolutePath());
        return false;
    }

    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error_ = QStringLiteral("cannot write %1: %2").arg(path_, f.errorString());
        return false;
    }
    const QByteArray bytes = settings_.toJson().toUtf8();
    if (f.write(bytes) != bytes.size() || !f.commit()) {
        error_ = QStringLiteral("cannot write %1: %2").arg(path_, f.errorString());
        return false;
    }
    return true;
}

}  // namespace omega::app