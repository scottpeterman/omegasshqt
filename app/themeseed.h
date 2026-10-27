// app/themeseed.h
//
// The shipped themes, copied into ~/.omega/themes and loaded from there.
//
// WHY A USER COPY AT ALL. The shipped directory is read-only on every packaged
// target that matters -- WindowsApps under MSIX, the squashfs inside an
// AppImage, a signed .app -- so a theme a user edits or adds has nowhere to go
// unless the directory Omega loads from is one it owns. ~/.omega is already
// that directory for config.json, omega.json, sessions.db and the vault.
//
// WHY THE MANIFEST. Copy-if-missing alone freezes every theme at the version
// first installed: a later release that fixes a theme reaches nobody who
// already ran an older one. .shipped.json records the hash of each file as it
// was copied. On the next seed a file whose hash still matches the record is
// one the user never touched, so a newer shipped version replaces it; a file
// whose hash differs has been edited and is left alone. A theme the user
// deleted is copied back, which is the price of not needing a "deleted" list.
//
// QtCore only, so it lives in omega_settings and themeseed_probe exercises it
// with no display.

#pragma once

#include <QString>
#include <QStringList>

namespace omega::app {

struct ThemeSeedResult {
    int copied = 0;     // new in the user directory
    int updated = 0;    // replaced an untouched older copy
    int kept = 0;       // user-modified, left alone
    int unchanged = 0;  // already identical
    QStringList errors;
};

// ~/.omega/themes
QString defaultUserThemeDir();

// The themes shipped with this build, if they can be found beside the
// executable: <appdir>/Resources/themes (Windows zip and MSIX, where the exe
// sits at the root of the package) or <appdir>/../Resources/themes (the .app
// bundle, AppImage usr/bin, the portable Linux tree's bin/). Empty when
// neither exists -- a build-tree run, which the caller handles.
QString findBundledThemeDir(const QString &appDir);

// Copies every *.yaml in shippedDir into userDir under the rules above.
// Creates userDir if needed. Never deletes anything.
ThemeSeedResult seedUserThemes(const QString &shippedDir,
                               const QString &userDir);

// True when dir exists and holds at least one *.yaml.
bool hasThemeFiles(const QString &dir);

}  // namespace omega::app
