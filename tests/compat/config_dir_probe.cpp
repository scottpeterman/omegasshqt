// tests/compat/config_dir_probe.cpp
//
// Two claims made in comments that nothing else checks.
//
//   config_dir_probe
//
// FIRST: the config directory is defined twice on purpose. The C++ side owns
// it in SettingsManager::defaultConfigDir(); transport/logtap.go carries its
// own copy because it sits below the C boundary and cannot call up through it.
// Both files say the other exists and point here. If they drift, session logs
// land in a directory nothing else uses, and nothing fails loudly -- captures
// just quietly appear somewhere else. That is exactly the failure a comment
// cannot prevent and an assertion can.
//
// SECOND: the auth-failure classifier. isAuthFailure() decides whether the
// login modal re-opens, and the expensive direction is the false positive: a
// host key mismatch answered with a password box is a dialog over the wrong
// problem, and the operator has to dismiss it before the real reason is
// readable. The Go side asserts this over its own error strings
// (sshcore/authfail_test.go); this asserts it again through the C boundary,
// which is where the marker the C++ actually matches on comes from.
//
// No display, no network, no files written.

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include <cstdio>

#include <omegassh/omegassh.h>

#include "app/autherror.h"
#include "app/settings.h"

using omega::app::AuthPromptLimit;
using omega::app::isAuthFailure;
using omega::app::SettingsManager;

namespace {

int failures = 0;

void ok(bool condition, const char *what, const QString &detail = QString()) {
    std::printf("  %-5s %-46s %s\n", condition ? "ok" : "FAIL", what,
                detail.toUtf8().constData());
    if (!condition) ++failures;
}

QString goConfigDirName() {
    char *raw = omegassh_config_dir_name();
    const QString value = QString::fromUtf8(raw ? raw : "");
    omegassh_free(raw);
    return value;
}

QString authMarker() {
    char *raw = omegassh_auth_failed_marker();
    const QString value = QString::fromUtf8(raw ? raw : "");
    omegassh_free(raw);
    return value;
}

QString hostKeyMarker() {
    char *raw = omegassh_unknown_hostkey_marker();
    const QString value = QString::fromUtf8(raw ? raw : "");
    omegassh_free(raw);
    return value;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    std::printf("config directory\n");

    const QString cppDir = SettingsManager::defaultConfigDir();
    const QString cppName = QDir(cppDir).dirName();
    const QString goName = goConfigDirName();

    ok(!cppDir.isEmpty(), "C++ names a directory", cppDir);
    ok(!goName.isEmpty(), "Go names a directory", goName);

    // The comparison this file exists for.
    ok(cppName == goName, "THE TWO DEFINITIONS AGREE",
       QStringLiteral("C++ %1 / Go %2").arg(cppName, goName));

    // Under $HOME rather than the launch directory, which is the property the
    // fallback in DefaultLogDir() is guarding and the one that would make a
    // packaged build write into Program Files.
    ok(cppDir.startsWith(QDir::homePath()), "under the home directory", cppDir);

    ok(SettingsManager::defaultConfigFile().startsWith(cppDir),
       "config.json is inside it", SettingsManager::defaultConfigFile());

    std::printf("\nauth failure classifier\n");

    const QString marker = authMarker();
    const QString hostKey = hostKeyMarker();

    ok(!marker.isEmpty(), "the marker crosses the boundary", marker);
    ok(!hostKey.isEmpty(), "so does the host key marker", hostKey);
    ok(!marker.contains(hostKey) && !hostKey.contains(marker),
       "the two markers do not overlap");

    // A rejection, wrapped the way the transport wraps one.
    const QString rejected =
        QStringLiteral("%1 lab-sw-01:22: ssh: unable to authenticate, "
                       "attempted methods [none publickey], no supported "
                       "methods remain")
            .arg(marker);
    ok(isAuthFailure(rejected), "a rejection is recognised");

    // The three that must NOT re-open the login modal. The host key cases are
    // the sharp ones: they fail during the same handshake.
    ok(!isAuthFailure(QStringLiteral(
           "%1 lab-sw-01:22 (ssh-ed25519 SHA256:abc+def/ghi=); not in "
           "/home/op/.ssh/known_hosts")
                          .arg(hostKey)),
       "an unknown host key is NOT one");

    ok(!isAuthFailure(QStringLiteral(
           "host key verification failed for lab-sw-01:22: offered key "
           "(ssh-ed25519 SHA256:zzz) does not match the pinned entry")),
       "a host key MISMATCH is NOT one");

    ok(!isAuthFailure(QStringLiteral(
           "connect to lab-sw-01:22: dial tcp 10.0.0.1:22: connect: "
           "connection refused")),
       "a refused connection is NOT one");

    ok(!isAuthFailure(QStringLiteral(
           "connect to lab-sw-01.lab.invalid:22: dial tcp: lookup "
           "lab-sw-01.lab.invalid: no such host")),
       "a name that does not resolve is NOT one");

    ok(!isAuthFailure(QString()), "an empty error is NOT one");

    ok(AuthPromptLimit > 0 && AuthPromptLimit <= 5,
       "the prompt cap is bounded",
       QStringLiteral("%1").arg(AuthPromptLimit));

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
