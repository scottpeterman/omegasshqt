// app/localshell.h
//
// What "a local shell" means on each platform: the default when a session
// names none, the list a session editor offers, and turning one command line
// into a program and its arguments.
//
// Kept apart from TabSession so the choices can be tested without starting
// anything, and so the editor can offer the same list the tab will run.
//
// DEFAULTS
//   Windows  PowerShell 7 (pwsh) when it is installed, else Windows PowerShell,
//            else %ComSpec%. -NoLogo on both PowerShells: the banner is noise in
//            a terminal that is opened often.
//   macOS    $SHELL as a login shell (-l), which is what Terminal.app does and
//            what a Mac account's PATH set-up assumes.
//   Linux    $SHELL, not as a login shell -- what desktop terminals do -- else
//            /bin/bash, else /bin/sh.

#ifndef OMEGA_APP_LOCALSHELL_H
#define OMEGA_APP_LOCALSHELL_H

#include <QString>
#include <QStringList>

namespace omega::app {

struct ShellCommand {
    QString program;     // resolved to a full path when it could be found
    QStringList args;
    QString error;       // non-empty when nothing runnable was found
    bool ok() const { return error.isEmpty() && !program.isEmpty(); }
};

// The command line used when a session leaves its own empty.
QString defaultShellCommandLine();

// Shells worth offering on this machine, as command lines, default first.
// Only ones that exist are listed.
QStringList availableShellCommandLines();

// Splits a command line the way a shell would and finds the program on PATH.
// An empty line means the default.
ShellCommand resolveShell(const QString &commandLine);

// Short name for a title or a ribbon: "pwsh", "zsh", "cmd".
QString shellDisplayName(const QString &commandLine);

}  // namespace omega::app

#endif  // OMEGA_APP_LOCALSHELL_H
