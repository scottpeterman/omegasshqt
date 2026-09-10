// app/settings.cpp

#include "app/settings.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>

namespace omega::app {
namespace {

// json.dumps' default escaping: the six short forms, control characters as
// \u00XX, and everything above ASCII as \uXXXX because ensure_ascii defaults to
// true. QString is UTF-16, so a non-BMP character is already the surrogate pair
// Python would emit -- the units are written as they are found.
QString escape(const QString &s) {
    QString out;
    out.reserve(s.size() + 2);
    for (const QChar c : s) {
        const ushort u = c.unicode();
        switch (u) {
            case '"':  out += QLatin1String("\\\""); continue;
            case '\\': out += QLatin1String("\\\\"); continue;
            case '\b': out += QLatin1String("\\b");  continue;
            case '\f': out += QLatin1String("\\f");  continue;
            case '\n': out += QLatin1String("\\n");  continue;
            case '\r': out += QLatin1String("\\r");  continue;
            case '\t': out += QLatin1String("\\t");  continue;
            default: break;
        }
        if (u < 0x20 || u > 0x7E) {
            out += QStringLiteral("\\u%1").arg(u, 4, 16, QLatin1Char('0'));
        } else {
            out += c;
        }
    }
    return out;
}

QString quoted(const QString &s) {
    return QLatin1Char('"') + escape(s) + QLatin1Char('"');
}

// One "key": value line at the top level's indent.
QString line(const QString &key, const QString &value) {
    return QStringLiteral("  ") + quoted(key) + QStringLiteral(": ") + value;
}

QString boolJson(bool v) {
    return v ? QStringLiteral("true") : QStringLiteral("false");
}

QString intOrNull(const std::optional<int> &v) {
    return v ? QString::number(*v) : QStringLiteral("null");
}

// json.dumps writes an empty list as [] with nothing inside it, and a
// populated one with each element on its own line at the next indent.
QString listJson(const QStringList &items) {
    if (items.isEmpty()) {
        return QStringLiteral("[]");
    }
    QStringList lines;
    for (const QString &item : items) {
        lines << QStringLiteral("    ") + quoted(item);
    }
    return QStringLiteral("[\n") + lines.join(QStringLiteral(",\n")) +
           QStringLiteral("\n  ]");
}

int intField(const QJsonObject &o, const char *key, int fallback,
             QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) return fallback;
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

bool boolField(const QJsonObject &o, const char *key, bool fallback,
               QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) return fallback;
    if (!v.isBool()) {
        if (warnings) {
            *warnings << QStringLiteral("%1 is not a boolean; using the default")
                             .arg(QLatin1String(key));
        }
        return fallback;
    }
    return v.toBool(fallback);
}

QString stringField(const QJsonObject &o, const char *key,
                    const QString &fallback, QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) return fallback;
    if (!v.isString()) {
        if (warnings) {
            *warnings << QStringLiteral("%1 is not a string; using the default")
                             .arg(QLatin1String(key));
        }
        return fallback;
    }
    return v.toString();
}

// Null is a real value here rather than an absence: window_x is null until the
// window has been placed, and nterm-qt writes it that way.
std::optional<int> optionalIntField(const QJsonObject &o, const char *key,
                                    QStringList *warnings) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) return std::nullopt;
    if (!v.isDouble()) {
        if (warnings) {
            *warnings << QStringLiteral("%1 is not a number; treating it as unset")
                             .arg(QLatin1String(key));
        }
        return std::nullopt;
    }
    return v.toInt();
}

}  // namespace

void AppSettings::addRecentProfile(const QString &name) {
    recent_profiles.removeAll(name);
    recent_profiles.prepend(name);
    while (recent_profiles.size() > max_recent && !recent_profiles.isEmpty()) {
        recent_profiles.removeLast();
    }
}

QString AppSettings::toJson() const {
    QStringList lines;
    lines << line(QStringLiteral("theme_name"), quoted(theme_name));
    lines << line(QStringLiteral("font_size"), QString::number(font_size));
    lines << line(QStringLiteral("multiline_paste_threshold"),
                  QString::number(multiline_paste_threshold));
    lines << line(QStringLiteral("scrollback_lines"),
                  QString::number(scrollback_lines));
    lines << line(QStringLiteral("default_term_type"), quoted(default_term_type));
    lines << line(QStringLiteral("default_keepalive_interval"),
                  QString::number(default_keepalive_interval));
    lines << line(QStringLiteral("auto_reconnect"), boolJson(auto_reconnect));
    lines << line(QStringLiteral("window_width"), QString::number(window_width));
    lines << line(QStringLiteral("window_height"), QString::number(window_height));
    lines << line(QStringLiteral("window_x"), intOrNull(window_x));
    lines << line(QStringLiteral("window_y"), intOrNull(window_y));
    lines << line(QStringLiteral("window_maximized"), boolJson(window_maximized));
    lines << line(QStringLiteral("tree_width"), QString::number(tree_width));
    lines << line(QStringLiteral("recent_profiles"), listJson(recent_profiles));
    lines << line(QStringLiteral("max_recent"), QString::number(max_recent));

    // No trailing newline: Python writes the return of json.dumps and nothing
    // after it.
    return QStringLiteral("{\n") + lines.join(QStringLiteral(",\n")) +
           QStringLiteral("\n}");
}

AppSettings AppSettings::fromJson(const QByteArray &text, QStringList *warnings) {
    AppSettings s;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (warnings) {
            *warnings << QStringLiteral("not a JSON object; using defaults");
        }
        return s;
    }
    const QJsonObject o = doc.object();

    s.theme_name = stringField(o, "theme_name", s.theme_name, warnings);
    s.font_size = intField(o, "font_size", s.font_size, warnings);
    s.multiline_paste_threshold = intField(o, "multiline_paste_threshold",
                                           s.multiline_paste_threshold, warnings);
    s.scrollback_lines = intField(o, "scrollback_lines", s.scrollback_lines,
                                  warnings);
    s.default_term_type = stringField(o, "default_term_type",
                                      s.default_term_type, warnings);
    s.default_keepalive_interval = intField(o, "default_keepalive_interval",
                                            s.default_keepalive_interval,
                                            warnings);
    s.auto_reconnect = boolField(o, "auto_reconnect", s.auto_reconnect, warnings);
    s.window_width = intField(o, "window_width", s.window_width, warnings);
    s.window_height = intField(o, "window_height", s.window_height, warnings);
    s.window_x = optionalIntField(o, "window_x", warnings);
    s.window_y = optionalIntField(o, "window_y", warnings);
    s.window_maximized = boolField(o, "window_maximized", s.window_maximized,
                                   warnings);
    s.tree_width = intField(o, "tree_width", s.tree_width, warnings);
    s.max_recent = intField(o, "max_recent", s.max_recent, warnings);

    const QJsonValue recents = o.value(QLatin1String("recent_profiles"));
    if (recents.isArray()) {
        for (const QJsonValue &v : recents.toArray()) {
            if (v.isString()) s.recent_profiles << v.toString();
        }
    } else if (!recents.isUndefined() && warnings) {
        *warnings << QStringLiteral("recent_profiles is not a list; ignoring it");
    }

    return s;
}

SettingsManager::SettingsManager(const QString &configPath) : path_(configPath) {}

QString SettingsManager::defaultConfigDir() {
    return QDir::homePath() + QStringLiteral("/.omega");
}

QString SettingsManager::defaultConfigFile() {
    return defaultConfigDir() + QStringLiteral("/config.json");
}

bool SettingsManager::load() {
    error_.clear();
    warnings_.clear();

    QFile f(path_);
    if (!f.exists()) {
        // Not an error, and not worth a warning either. A first run has no
        // config file and defaults are the right answer.
        settings_ = AppSettings();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        error_ = QStringLiteral("cannot read %1: %2").arg(path_, f.errorString());
        settings_ = AppSettings();
        return false;
    }

    settings_ = AppSettings::fromJson(f.readAll(), &warnings_);
    return true;
}

bool SettingsManager::save() {
    error_.clear();

    const QFileInfo info(path_);
    if (!QDir().mkpath(info.absolutePath())) {
        error_ = QStringLiteral("cannot create %1").arg(info.absolutePath());
        return false;
    }

    // QSaveFile rather than QFile: the two applications write this file, and a
    // crash partway through a plain write leaves a truncated one that the other
    // then reports as corrupt. This writes a temporary and renames it.
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
