// tests/compat/themeseed_probe.cpp
//
// The ~/.omega/themes seed, end to end in temp directories.
//
//   themeseed_probe
//
// Checks, in order:
//   1. findBundledThemeDir finds Resources/themes at the exe's level (Windows)
//      and one level up (.app, AppImage, portable Linux) -- the miss at the
//      first of those is the bug this exists for.
//   2. first seed copies everything, and the copies are user-writable.
//   3. a second seed is a no-op.
//   4. a shipped update replaces an untouched copy.
//   5. a shipped update leaves a user-edited copy alone.
//   6. a deleted theme comes back.
//   7. a pre-manifest identical file is adopted, so it updates next time.
//
// No display, no network, nothing outside QTemporaryDir.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

#include "app/themeseed.h"

using omega::app::findBundledThemeDir;
using omega::app::seedUserThemes;
using omega::app::ThemeSeedResult;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (QFileInfo::exists(path)) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
        std::exit(2);
    }
    f.write(data);
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 2;
    const QDir root(tmp.path());

    // --- 1. lookup at both depths --------------------------------------
    {
        const QString flat = root.filePath(QStringLiteral("flat"));
        writeFile(flat + QStringLiteral("/Resources/themes/a.yaml"), "name: a\n");
        check(findBundledThemeDir(flat) ==
                  QDir::cleanPath(flat + QStringLiteral("/Resources/themes")),
              "flat layout (exe at package root) finds Resources/themes");

        const QString nested = root.filePath(QStringLiteral("nested"));
        QDir().mkpath(nested + QStringLiteral("/bin"));
        writeFile(nested + QStringLiteral("/Resources/themes/a.yaml"), "name: a\n");
        check(findBundledThemeDir(nested + QStringLiteral("/bin")) ==
                  QDir::cleanPath(nested + QStringLiteral("/Resources/themes")),
              "nested layout (exe in bin/ or MacOS/) finds ../Resources/themes");

        const QString none = root.filePath(QStringLiteral("none"));
        QDir().mkpath(none);
        check(findBundledThemeDir(none).isEmpty(), "no bundle -> empty");
    }

    const QString shipped = root.filePath(QStringLiteral("shipped"));
    const QString user = root.filePath(QStringLiteral("user/themes"));
    writeFile(shipped + QStringLiteral("/one.yaml"), "name: one\nv: 1\n");
    writeFile(shipped + QStringLiteral("/two.yaml"), "name: two\nv: 1\n");
    writeFile(shipped + QStringLiteral("/notes.txt"), "ignored\n");
    // Read-only source, the way WindowsApps and a squashfs present it.
    for (const char *n : {"one.yaml", "two.yaml"}) {
        QFile::setPermissions(shipped + QLatin1Char('/') + QLatin1String(n),
                              QFileDevice::ReadOwner | QFileDevice::ReadUser);
    }

    // --- 2. first seed -------------------------------------------------
    ThemeSeedResult r = seedUserThemes(shipped, user);
    check(r.copied == 2 && r.errors.isEmpty(), "first seed copies both yaml");
    check(!QFileInfo::exists(user + QStringLiteral("/notes.txt")),
          "non-yaml is not copied");
    check(QFileInfo(user + QStringLiteral("/one.yaml")).isWritable(),
          "copy of a read-only source is user-writable");
    check(QFileInfo::exists(user + QStringLiteral("/.shipped.json")),
          "manifest written");

    // --- 3. idempotent -------------------------------------------------
    r = seedUserThemes(shipped, user);
    check(r.copied == 0 && r.updated == 0 && r.unchanged == 2,
          "second seed is a no-op");

    // --- 4 & 5. shipped update vs user edit ----------------------------
    writeFile(user + QStringLiteral("/two.yaml"), "name: two\nmine: true\n");
    writeFile(shipped + QStringLiteral("/one.yaml"), "name: one\nv: 2\n");
    writeFile(shipped + QStringLiteral("/two.yaml"), "name: two\nv: 2\n");
    r = seedUserThemes(shipped, user);
    check(r.updated == 1 && r.kept == 1, "one updated, one kept");
    check(readFile(user + QStringLiteral("/one.yaml")).contains("v: 2"),
          "untouched copy picked up the shipped fix");
    check(readFile(user + QStringLiteral("/two.yaml")).contains("mine: true"),
          "edited copy survived the update");

    // --- 6. deleted comes back -----------------------------------------
    QFile::remove(user + QStringLiteral("/one.yaml"));
    r = seedUserThemes(shipped, user);
    check(r.copied == 1, "deleted theme is re-seeded");

    // --- 7. pre-manifest adoption --------------------------------------
    QFile::remove(user + QStringLiteral("/.shipped.json"));
    r = seedUserThemes(shipped, user);  // one.yaml identical -> adopted
    check(r.unchanged == 1 && r.kept == 1, "identical file adopted, edit kept");
    writeFile(shipped + QStringLiteral("/one.yaml"), "name: one\nv: 3\n");
    r = seedUserThemes(shipped, user);
    check(r.updated == 1 &&
              readFile(user + QStringLiteral("/one.yaml")).contains("v: 3"),
          "adopted file updates on the next release");

    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
