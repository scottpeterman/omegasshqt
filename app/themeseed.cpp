// app/themeseed.cpp -- see themeseed.h

#include "app/themeseed.h"

#include "app/settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace omega::app {

namespace {

constexpr char kManifest[] = ".shipped.json";

QString sha256(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return {};
    return QString::fromLatin1(h.result().toHex());
}

QJsonObject readManifest(const QString &userDir) {
    QFile f(QDir(userDir).filePath(QLatin1String(kManifest)));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

bool writeManifest(const QString &userDir, const QJsonObject &m) {
    QSaveFile f(QDir(userDir).filePath(QLatin1String(kManifest)));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(m).toJson(QJsonDocument::Indented));
    return f.commit();
}

// The source is read-only on every packaged target and QFile::copy carries
// the permissions across, so a copy out of WindowsApps or a squashfs would be
// a read-only file the user cannot edit -- and that the next update could not
// replace without this either.
void makeUserWritable(const QString &path) {
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ReadUser | QFileDevice::WriteUser |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther);
}

bool replaceWith(const QString &src, const QString &dst) {
    if (QFileInfo::exists(dst)) {
        makeUserWritable(dst);
        if (!QFile::remove(dst)) return false;
    }
    if (!QFile::copy(src, dst)) return false;
    makeUserWritable(dst);
    return true;
}

}  // namespace

QString defaultUserThemeDir() {
    return QDir(SettingsManager::defaultConfigDir())
        .filePath(QStringLiteral("themes"));
}

QString findBundledThemeDir(const QString &appDir) {
    const QString candidates[] = {
        QDir::cleanPath(appDir + QStringLiteral("/Resources/themes")),
        QDir::cleanPath(appDir + QStringLiteral("/../Resources/themes")),
    };
    for (const QString &c : candidates) {
        if (hasThemeFiles(c)) return c;
    }
    return {};
}

bool hasThemeFiles(const QString &dir) {
    const QDir d(dir);
    return d.exists() &&
           !d.entryList({QStringLiteral("*.yaml")}, QDir::Files).isEmpty();
}

ThemeSeedResult seedUserThemes(const QString &shippedDir,
                               const QString &userDir) {
    ThemeSeedResult r;
    const QDir src(shippedDir);
    if (!src.exists()) {
        r.errors << QStringLiteral("shipped theme directory not found: %1")
                        .arg(shippedDir);
        return r;
    }
    if (!QDir().mkpath(userDir)) {
        r.errors << QStringLiteral("cannot create %1").arg(userDir);
        return r;
    }

    const QDir dst(userDir);
    QJsonObject manifest = readManifest(userDir);
    bool manifestDirty = false;

    const QStringList files =
        src.entryList({QStringLiteral("*.yaml")}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        const QString from = src.filePath(name);
        const QString to = dst.filePath(name);
        const QString shippedHash = sha256(from);
        if (shippedHash.isEmpty()) {
            r.errors << QStringLiteral("cannot read %1").arg(from);
            continue;
        }

        if (!QFileInfo::exists(to)) {
            if (!replaceWith(from, to)) {
                r.errors << QStringLiteral("cannot copy %1 to %2").arg(from, to);
                continue;
            }
            manifest.insert(name, shippedHash);
            manifestDirty = true;
            ++r.copied;
            continue;
        }

        const QString userHash = sha256(to);
        if (userHash == shippedHash) {
            // Same bytes. Make sure the record agrees, which also adopts a
            // file that predates the manifest as untouched.
            if (manifest.value(name).toString() != shippedHash) {
                manifest.insert(name, shippedHash);
                manifestDirty = true;
            }
            ++r.unchanged;
            continue;
        }

        const QString recorded = manifest.value(name).toString();
        if (!recorded.isEmpty() && userHash == recorded) {
            // Untouched since we put it there; the shipped one is newer.
            if (!replaceWith(from, to)) {
                r.errors << QStringLiteral("cannot update %1").arg(to);
                continue;
            }
            manifest.insert(name, shippedHash);
            manifestDirty = true;
            ++r.updated;
            continue;
        }

        // Edited by the user, or there before we kept records. Theirs.
        ++r.kept;
    }

    if (manifestDirty && !writeManifest(userDir, manifest)) {
        r.errors << QStringLiteral("cannot write %1/%2")
                        .arg(userDir, QLatin1String(kManifest));
    }
    return r;
}

}  // namespace omega::app
