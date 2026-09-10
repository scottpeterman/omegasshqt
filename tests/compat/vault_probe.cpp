// tests/compat/vault_probe.cpp
//
// Drives omegassh::Vault the way the shell drives it, with no display and no
// keyring, so the claims the wrapper makes that cannot be checked by reading
// get checked by doing them:
//
//   - an edit through updateMetadata() keeps the password
//   - a replace through store() with no material clears it, still
//   - lastError() is empty after a call that worked
//   - the keyring codes stay out of the wrong-password branch
//
// examples/c/vault_smoke covers the same ground one layer down. This is the
// layer above it: if vault_smoke passes and this fails, the marshalling in
// omegasshvault.cpp is where to look, not the shim.
//
//   vault_probe /tmp/lab-vault.json
//
// The file is created and left behind, so a second run over the same path
// exercises the "already exists" branch.

#include <QCoreApplication>
#include <QStringList>

#include "omegasshvault.h"

#include <cstdio>

using omegassh::AuthMethod;
using omegassh::CredentialInput;
using omegassh::CredentialMeta;
using omegassh::KeyringStatus;
using omegassh::Vault;
using omegassh::VaultError;

static int failures = 0;

static void check(const char *what, VaultError got, VaultError want,
                  const Vault &v) {
    if (got == want) {
        std::printf("  ok    %-38s %s\n", what,
                    qPrintable(Vault::errorName(got)));
        return;
    }
    std::printf("  FAIL  %-38s got %s, want %s (%s)\n", what,
                qPrintable(Vault::errorName(got)),
                qPrintable(Vault::errorName(want)), qPrintable(v.lastError()));
    failures++;
}

static void checkTrue(const char *what, bool cond) {
    std::printf("  %s  %s\n", cond ? "ok   " : "FAIL ", what);
    if (!cond) failures++;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString path =
        args.size() > 1 ? args.at(1) : QStringLiteral("/tmp/lab-vault.json");

    Vault v(path);
    std::printf("vault at %s\n\n", qPrintable(v.path()));

    std::printf("handle\n");
    checkTrue("open", v.isOpen());
    checkTrue("handle usable as Config::vaultHandle", v.handle() > 0);
    checkTrue("starts locked", v.isLocked());

    std::printf("\ncreate and unlock\n");
    if (v.exists()) {
        check("create over an existing vault", v.create(QStringLiteral("labmaster02")),
              VaultError::Exists, v);
        check("unlock", v.unlock(QStringLiteral("labmaster02")), VaultError::Ok, v);
    } else {
        check("create", v.create(QStringLiteral("labmaster02")), VaultError::Ok, v);
    }
    checkTrue("unlocked", !v.isLocked());
    checkTrue("lastError empty after success", v.lastError().isEmpty());

    check("wrong master", v.unlock(QStringLiteral("not-the-master")),
          VaultError::WrongPassword, v);
    checkTrue("failure carries detail", !v.lastError().isEmpty());
    check("unlock again", v.unlock(QStringLiteral("labmaster02")), VaultError::Ok, v);

    // --- add, with material --------------------------------------------
    std::printf("\ncredentials\n");
    CredentialInput input;
    input.name = QStringLiteral("lab-admin");
    input.username = QStringLiteral("labadmin");
    input.auth = AuthMethod::Password;
    input.password = QStringLiteral("labsecret");
    input.description = QStringLiteral("lab gear");
    input.tags = QStringList{QStringLiteral("lab")};
    input.scope.domainSuffix = QStringLiteral("lab.local");

    QString id;
    VaultError e = v.store(input, &id);
    if (e == VaultError::DuplicateName) {
        // Left over from a previous run over the same file. Take the id and
        // carry on rather than making the second run a different test.
        CredentialMeta existing;
        v.meta(QStringLiteral("lab-admin"), &existing);
        id = existing.id;
        std::printf("  info  reusing lab-admin from a previous run\n");
    } else {
        check("store (add)", e, VaultError::Ok, v);
    }
    checkTrue("got an id", !id.isEmpty());
    input.wipeSecrets();

    CredentialMeta meta;
    check("meta by name", v.meta(QStringLiteral("lab-admin"), &meta), VaultError::Ok, v);
    checkTrue("metadata carries has_secret", meta.hasSecret);
    checkTrue("metadata auth label is Go's", meta.authLabel == QStringLiteral("Password"));
    checkTrue("scope survived", meta.scope.domainSuffix == QStringLiteral("lab.local"));

    // --- the whole point: edit without losing the password --------------
    std::printf("\nediting\n");
    CredentialMeta edited = meta;
    edited.description = QStringLiteral("lab access switches");
    edited.username = QStringLiteral("labops");
    edited.priority = 5;
    check("updateMetadata", v.updateMetadata(edited), VaultError::Ok, v);

    CredentialMeta after;
    v.meta(id, &after);
    checkTrue("EDIT KEPT THE SECRET", after.hasSecret);
    checkTrue("edit landed", after.username == QStringLiteral("labops") &&
                                 after.priority == 5);

    // And the counter-check, which is why the two types are separate: a
    // replace with no material still clears it. If this ever stops being true
    // the two write paths have collapsed and the UI's choice stopped meaning
    // anything.
    CredentialInput blanked;
    blanked.id = id;
    blanked.name = after.name;
    blanked.username = after.username;
    blanked.auth = AuthMethod::Password;
    check("store (replace, no material)", v.store(blanked), VaultError::Ok, v);
    v.meta(id, &after);
    checkTrue("replace cleared the secret, as documented", !after.hasSecret);

    // Put it back, so a repeat run starts from the same place.
    blanked.password = QStringLiteral("labsecret");
    check("store (replace, with material)", v.store(blanked), VaultError::Ok, v);
    blanked.wipeSecrets();

    check("rename", v.rename(id, QStringLiteral("lab-admin")), VaultError::Ok, v);
    v.meta(id, &after);
    checkTrue("rename kept the secret", after.hasSecret);

    check("set default", v.setDefault(id), VaultError::Ok, v);
    QString defaultName;
    check("default name", v.defaultName(&defaultName), VaultError::Ok, v);
    checkTrue("default name is the credential's",
              defaultName == QStringLiteral("lab-admin"));

    check("disable", v.setDisabled(id, true), VaultError::Ok, v);
    v.defaultName(&defaultName);
    checkTrue("a disabled default is not offered", defaultName.isEmpty());
    check("re-enable", v.setDisabled(id, false), VaultError::Ok, v);

    QVector<CredentialMeta> all;
    check("list", v.list(&all), VaultError::Ok, v);
    checkTrue("list is not empty", !all.isEmpty());

    // --- refusals -------------------------------------------------------
    std::printf("\nrefusals\n");
    CredentialMeta unknown = after;
    unknown.id = QStringLiteral("no-such-id");
    check("updateMetadata on an unknown id", v.updateMetadata(unknown),
          VaultError::CredNotFound, v);

    CredentialInput nameless;
    check("store with no name", v.store(nameless), VaultError::EmptyName, v);

    v.lock();
    checkTrue("locked", v.isLocked());
    check("list while locked", v.list(&all), VaultError::Locked, v);
    check("updateMetadata while locked", v.updateMetadata(edited), VaultError::Locked, v);

    // --- keyring --------------------------------------------------------
    //
    // Whatever this machine has. The assertion is not which answer comes back,
    // it is that a keyring answer never arrives dressed as a wrong password --
    // that is the confusion the code range exists to prevent, and the one that
    // would send somebody retyping a password they never typed.
    std::printf("\nkeyring\n");
    KeyringStatus status;
    check("keyring status", v.keyringStatus(&status), VaultError::Ok, v);
    std::printf("  info  available=%d has_entry=%d disabled=%d %s\n",
                status.available, status.hasEntry, status.disabled,
                qPrintable(status.error));

    const VaultError quiet = v.unlockQuiet();
    std::printf("  info  unlockQuiet -> %s\n", qPrintable(Vault::errorName(quiet)));
    checkTrue("quiet unlock never reports a wrong password",
              quiet != VaultError::WrongPassword);
    checkTrue("quiet unlock answered Ok or a keyring-range code",
              quiet == VaultError::Ok || omegassh::isKeyringError(quiet));

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
