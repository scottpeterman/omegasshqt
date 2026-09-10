// tests/compat/settings_probe.cpp
//
// The C++ half of the settings differential. It prints and does not judge; the
// comparison lives in settings_differential.py, so neither side asserts against
// its own idea of the answer.
//
//   settings_probe defaults                  the defaults, re-emitted as JSON
//   settings_probe fields    <config.json>   parsed fields, one per line
//   settings_probe roundtrip <config.json>   load, then re-emit the file bytes
//   settings_probe recent    <config.json> <name>...
//                                            addRecentProfile each in turn,
//                                            then re-emit
//
// Warnings go to stderr, out of the comparison.

#include "app/settings.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace omega::app;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: settings_probe defaults\n"
                 "       settings_probe {fields|roundtrip} <config.json>\n"
                 "       settings_probe recent <config.json> <name>...\n");
    return 2;
}

bool readFile(const QString &path, QByteArray *out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(path));
        return false;
    }
    *out = f.readAll();
    return true;
}

void emitJson(const AppSettings &s) {
    const QByteArray bytes = s.toJson().toUtf8();
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
}

// One field per line, in dataclass order. None/True/False are spelled the
// Python's way so neither dump has to translate the other's booleans.
void emitFields(const AppSettings &s) {
    std::printf("theme_name=%s\n", qPrintable(s.theme_name));
    std::printf("font_size=%d\n", s.font_size);
    std::printf("multiline_paste_threshold=%d\n", s.multiline_paste_threshold);
    std::printf("scrollback_lines=%d\n", s.scrollback_lines);
    std::printf("default_term_type=%s\n", qPrintable(s.default_term_type));
    std::printf("default_keepalive_interval=%d\n", s.default_keepalive_interval);
    std::printf("auto_reconnect=%s\n", s.auto_reconnect ? "True" : "False");
    std::printf("window_width=%d\n", s.window_width);
    std::printf("window_height=%d\n", s.window_height);
    std::printf("window_x=%s\n",
                s.window_x ? qPrintable(QString::number(*s.window_x)) : "None");
    std::printf("window_y=%s\n",
                s.window_y ? qPrintable(QString::number(*s.window_y)) : "None");
    std::printf("window_maximized=%s\n", s.window_maximized ? "True" : "False");
    std::printf("tree_width=%d\n", s.tree_width);
    std::printf("recent_profiles=%s\n",
                qPrintable(s.recent_profiles.join(QLatin1Char('|'))));
    std::printf("max_recent=%d\n", s.max_recent);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) return usage();

    const QString mode = args.at(1);

    if (mode == QLatin1String("defaults")) {
        emitJson(AppSettings());
        return 0;
    }

    if (args.size() < 3) return usage();
    QByteArray text;
    if (!readFile(args.at(2), &text)) return 1;

    QStringList warnings;
    AppSettings s = AppSettings::fromJson(text, &warnings);
    for (const QString &w : warnings) {
        std::fprintf(stderr, "warning: %s\n", qPrintable(w));
    }

    if (mode == QLatin1String("roundtrip")) {
        emitJson(s);
        return 0;
    }
    if (mode == QLatin1String("fields")) {
        emitFields(s);
        return 0;
    }
    if (mode == QLatin1String("recent")) {
        for (int i = 3; i < args.size(); ++i) {
            s.addRecentProfile(args.at(i));
        }
        emitJson(s);
        return 0;
    }

    return usage();
}
