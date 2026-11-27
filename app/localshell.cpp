// app/localshell.cpp

#include "app/localshell.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

namespace omega::app {
namespace {

// Quoted only when it has to be, so a plain path reads as itself in the
// editor and a "Program Files" path survives splitCommand on the way back.
QString quoted(const QString &arg) {
    if (!arg.contains(QLatin1Char(' ')) && !arg.contains(QLatin1Char('\t'))) {
        return arg;
    }
    return QLatin1Char('"') + arg + QLatin1Char('"');
}

QString findProgram(const QString &program) {
    if (program.isEmpty()) return QString();
    const QFileInfo info(program);
    if (info.isAbsolute() || program.contains(QLatin1Char('/')) ||
        program.contains(QLatin1Char('\\'))) {
        return info.exists() ? info.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(program);
}

#ifndef Q_OS_WIN
// The login shell from the environment, if it names something that exists.
QString envShell() {
    const QString shell = qEnvironmentVariable("SHELL");
    if (!shell.isEmpty() && QFileInfo(shell).isExecutable()) return shell;
    return QString();
}
#endif

}  // namespace

QString defaultShellCommandLine() {
#if defined(Q_OS_WIN)
    if (!QStandardPaths::findExecutable(QStringLiteral("pwsh")).isEmpty()) {
        return QStringLiteral("pwsh -NoLogo");
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty()) {
        return QStringLiteral("powershell -NoLogo");
    }
    const QString comspec = qEnvironmentVariable("ComSpec");
    return comspec.isEmpty() ? QStringLiteral("cmd") : quoted(comspec);
#elif defined(Q_OS_MACOS)
    const QString shell = envShell();
    return quoted(shell.isEmpty() ? QStringLiteral("/bin/zsh") : shell) +
           QStringLiteral(" -l");
#else
    const QString shell = envShell();
    if (!shell.isEmpty()) return quoted(shell);
    if (QFileInfo(QStringLiteral("/bin/bash")).isExecutable()) {
        return QStringLiteral("/bin/bash");
    }
    return QStringLiteral("/bin/sh");
#endif
}

QStringList availableShellCommandLines() {
    QStringList out;
    const auto add = [&out](const QString &line) {
        if (!line.isEmpty() && !out.contains(line)) out << line;
    };
    add(defaultShellCommandLine());

#if defined(Q_OS_WIN)
    if (!QStandardPaths::findExecutable(QStringLiteral("pwsh")).isEmpty())
        add(QStringLiteral("pwsh -NoLogo"));
    if (!QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty())
        add(QStringLiteral("powershell -NoLogo"));
    add(QStringLiteral("cmd"));
    if (!QStandardPaths::findExecutable(QStringLiteral("wsl")).isEmpty())
        add(QStringLiteral("wsl"));
    // Git for Windows' bash, the one most people with Git installed have. -i
    // and -l because it is started directly, not from a mintty that would
    // otherwise ask for both.
    for (const QString &root : {qEnvironmentVariable("ProgramFiles"),
                                qEnvironmentVariable("ProgramW6432")}) {
        const QString bash = root + QStringLiteral("/Git/bin/bash.exe");
        if (!root.isEmpty() && QFileInfo::exists(bash)) {
            add(quoted(QDir::toNativeSeparators(bash)) + QStringLiteral(" -i -l"));
            break;
        }
    }
#else
    // /etc/shells is the system's own list of login shells. Only entries that
    // exist on disk, and not the nologin-style ones it sometimes carries.
    QFile shells(QStringLiteral("/etc/shells"));
    if (shells.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&shells);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            if (line.endsWith(QLatin1String("nologin")) ||
                line.endsWith(QLatin1String("/false"))) continue;
            if (!QFileInfo(line).isExecutable()) continue;
#if defined(Q_OS_MACOS)
            add(quoted(line) + QStringLiteral(" -l"));
#else
            add(quoted(line));
#endif
        }
    }
#endif
    return out;
}

ShellCommand resolveShell(const QString &commandLine) {
    const QString line = commandLine.trimmed().isEmpty() ? defaultShellCommandLine()
                                                         : commandLine.trimmed();
    QStringList parts = QProcess::splitCommand(line);
    ShellCommand cmd;
    if (parts.isEmpty()) {
        cmd.error = QObject::tr("no shell to run");
        return cmd;
    }
    const QString program = parts.takeFirst();
    // Native separators matter on Windows, and not only for looks. Qt finds
    // C:/Windows/System32/cmd.exe, and cmd.exe scans its whole command line,
    // its own path included, for switches: "/cmd.exe" reads as /c md.exe, so
    // cmd runs that and exits 0 before a prompt ever appears.
    cmd.program = QDir::toNativeSeparators(findProgram(program));
    cmd.args = parts;
    if (cmd.program.isEmpty()) {
        cmd.error = QObject::tr("shell not found: %1").arg(program);
    }
    return cmd;
}

QString shellDisplayName(const QString &commandLine) {
    const QString line = commandLine.trimmed().isEmpty() ? defaultShellCommandLine()
                                                         : commandLine.trimmed();
    const QStringList parts = QProcess::splitCommand(line);
    if (parts.isEmpty()) return QObject::tr("shell");
    QString name = QFileInfo(parts.first()).fileName();
    if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) name.chop(4);
    return name.isEmpty() ? QObject::tr("shell") : name;
}

}  // namespace omega::app
