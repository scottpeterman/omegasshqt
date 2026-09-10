/* examples/c/vault_smoke.cpp
 *
 * Exercises the whole vault C surface against a throwaway vault, the way the
 * Qt wrapper will: open a handle, create, store, list, re-open, unlock, lock.
 *
 * It is the Phase 0 "proven on Linux before anything is built on top of it"
 * check reduced to one binary. It has no Qt in it on purpose -- if this
 * passes, the boundary is good and anything above it that fails is the
 * wrapper's problem, not the shim's.
 *
 *   cc -o vault_smoke examples/c/vault_smoke.c build/libomegavault.a \
 *      -Iinclude -lpthread
 *   ./vault_smoke /tmp/lab-vault.json
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <omegassh/vault.h>

static int failures = 0;

static void check(const char *what, omegassh_vault_err got, omegassh_vault_err want)
{
    if (got == want) {
        printf("  ok    %-34s %s\n", what, omegassh_vault_error_name(got));
        return;
    }
    char *detail = omegassh_last_error();
    printf("  FAIL  %-34s got %s, want %s (%s)\n", what,
           omegassh_vault_error_name(got), omegassh_vault_error_name(want),
           detail ? detail : "");
    omegassh_free(detail);
    failures++;
}

/* omegassh_last_error() is only meaningful after a non-OK return. Printing it
 * unconditionally is what showed a stale "incorrect master password" beside a
 * call that had just succeeded. */
static void print_result(const char *what, omegassh_vault_err code)
{
    if (code == OMEGASSH_VAULT_OK) {
        printf("  info  %s -> %s\n", what, omegassh_vault_error_name(code));
        return;
    }
    char *detail = omegassh_last_error();
    printf("  info  %s -> %s (%s)\n", what,
           omegassh_vault_error_name(code), detail ? detail : "");
    omegassh_free(detail);
}

static void check_true(const char *what, int cond)
{
    if (cond) {
        printf("  ok    %s\n", what);
        return;
    }
    printf("  FAIL  %s\n", what);
    failures++;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/lab-vault.json";
    remove(path);

    char *s = nullptr;
    char *id = nullptr;

    printf("vault at %s\n\n", path);

    /* --- handle ------------------------------------------------------- */
    printf("handle\n");
    long long v = omegassh_vault_open(path);
    check_true("open", v > 0);
    check_true("no file yet", omegassh_vault_exists(v) == 0);
    check_true("starts locked", omegassh_vault_is_locked(v) == 1);

    check("path", omegassh_vault_path(v, &s), OMEGASSH_VAULT_OK);
    check_true("path round-trips", s && strcmp(s, path) == 0);
    omegassh_free(s); s = nullptr;

    /* A handle that was never opened must be rejected, not crash. */
    check("bad handle rejected", omegassh_vault_list(99999, &s),
          OMEGASSH_VAULT_ERR_BAD_HANDLE);

    /* --- create ------------------------------------------------------- */
    printf("\ncreate\n");
    check("short master refused", omegassh_vault_create(v, "short"),
          OMEGASSH_VAULT_ERR_WEAK_PASSWORD);
    check("create", omegassh_vault_create(v, "labmaster01"), OMEGASSH_VAULT_OK);
    check_true("file exists", omegassh_vault_exists(v) == 1);
    check_true("unlocked after create", omegassh_vault_is_locked(v) == 0);
    check("create again refused", omegassh_vault_create(v, "labmaster01"),
          OMEGASSH_VAULT_ERR_EXISTS);

    /* --- store -------------------------------------------------------- */
    printf("\nstore\n");
    check("store", omegassh_vault_store(v,
        "{\"name\":\"lab-admin\",\"username\":\"labadmin\","
        "\"auth_type\":\"password\",\"password\":\"s3cret\","
        "\"description\":\"lab gear\",\"tags\":[\"lab\"],"
        "\"scope\":{\"domain_suffix\":\"lab.local\",\"cidrs\":[],\"platforms\":[]},"
        "\"is_default\":true}", &id), OMEGASSH_VAULT_OK);
    check_true("id returned", id && strlen(id) > 0);

    check("duplicate name refused", omegassh_vault_store(v,
        "{\"name\":\"lab-admin\",\"username\":\"other\","
        "\"auth_type\":\"password\",\"password\":\"x\"}", nullptr),
        OMEGASSH_VAULT_ERR_DUPLICATE_NAME);

    check("empty name refused", omegassh_vault_store(v,
        "{\"name\":\"\",\"username\":\"x\",\"auth_type\":\"password\"}", nullptr),
        OMEGASSH_VAULT_ERR_EMPTY_NAME);

    check("malformed json refused", omegassh_vault_store(v, "{not json", nullptr),
          OMEGASSH_VAULT_ERR_BAD_ARGUMENT);

    char *id2 = nullptr;
    check("store second", omegassh_vault_store(v,
        "{\"name\":\"lab-ro\",\"username\":\"labro\","
        "\"auth_type\":\"publickey\",\"key_path\":\"~/.ssh/id_ed25519\"}",
        &id2), OMEGASSH_VAULT_OK);

    /* --- read back ---------------------------------------------------- */
    printf("\nread back\n");
    check("list", omegassh_vault_list(v, &s), OMEGASSH_VAULT_OK);
    printf("        %s\n", s);
    check_true("no password in list", strstr(s, "s3cret") == nullptr);
    check_true("has_secret reported", strstr(s, "\"has_secret\":true") != nullptr);
    check_true("empty arrays not null", strstr(s, "\"cidrs\":[]") != nullptr);
    omegassh_free(s); s = nullptr;

    check("meta by name", omegassh_vault_meta(v, "lab-admin", &s), OMEGASSH_VAULT_OK);
    check_true("no password in meta", strstr(s, "s3cret") == nullptr);
    omegassh_free(s); s = nullptr;

    check("meta unknown", omegassh_vault_meta(v, "not-there", &s),
          OMEGASSH_VAULT_ERR_CRED_NOT_FOUND);

    check("default name", omegassh_vault_default_name(v, &s), OMEGASSH_VAULT_OK);
    check_true("default is lab-admin", s && strcmp(s, "lab-admin") == 0);
    omegassh_free(s); s = nullptr;

    /* --- mutate ------------------------------------------------------- */
    printf("\nmutate\n");
    check("rename", omegassh_vault_rename(v, id, "lab-admin-renamed"), OMEGASSH_VAULT_OK);
    check("meta by new name", omegassh_vault_meta(v, "lab-admin-renamed", &s),
          OMEGASSH_VAULT_OK);
    /* The rename must not have dropped the secret it never saw. */
    check_true("secret survived rename",
               strstr(s, "\"has_secret\":true") != nullptr);
    omegassh_free(s); s = nullptr;

    check("set disabled", omegassh_vault_set_disabled(v, id, 1), OMEGASSH_VAULT_OK);
    check("default name empty when disabled",
          omegassh_vault_default_name(v, &s), OMEGASSH_VAULT_OK);
    check_true("disabled default not offered", s && strlen(s) == 0);
    omegassh_free(s); s = nullptr;
    check("re-enable", omegassh_vault_set_disabled(v, id, 0), OMEGASSH_VAULT_OK);

    check("clear default", omegassh_vault_clear_default(v), OMEGASSH_VAULT_OK);
    check("set default", omegassh_vault_set_default(v, id2), OMEGASSH_VAULT_OK);
    check("delete", omegassh_vault_delete(v, id2), OMEGASSH_VAULT_OK);
    check("delete again", omegassh_vault_delete(v, id2),
          OMEGASSH_VAULT_ERR_CRED_NOT_FOUND);

    /* --- lock and re-open --------------------------------------------- */
    printf("\nlock and re-open\n");
    omegassh_vault_lock(v);
    check_true("locked", omegassh_vault_is_locked(v) == 1);
    check("list refused while locked", omegassh_vault_list(v, &s),
          OMEGASSH_VAULT_ERR_LOCKED);
    check("store refused while locked", omegassh_vault_store(v,
          "{\"name\":\"x\",\"auth_type\":\"password\"}", nullptr),
          OMEGASSH_VAULT_ERR_LOCKED);

    check("wrong master", omegassh_vault_unlock(v, "wrongpassword"),
          OMEGASSH_VAULT_ERR_WRONG_PASSWORD);
    check("unlock", omegassh_vault_unlock(v, "labmaster01"), OMEGASSH_VAULT_OK);
    check("list after unlock", omegassh_vault_list(v, &s), OMEGASSH_VAULT_OK);
    check_true("one credential left", strstr(s, "lab-admin-renamed") != nullptr);
    omegassh_free(s); s = nullptr;

    /* --- change master ------------------------------------------------ */
    printf("\nchange master\n");
    /* A fresh handle, never unlocked -- the shape a C++ caller has, and the
     * one that used to seal an empty list under the new password. */
    long long v2 = omegassh_vault_open(path);
    check_true("second handle", v2 > 0);
    check("change master on a fresh handle",
          omegassh_vault_change_master(v2, "labmaster01", "labmaster02"),
          OMEGASSH_VAULT_OK);
    omegassh_vault_close(v2);

    long long v3 = omegassh_vault_open(path);
    check("old master rejected", omegassh_vault_unlock(v3, "labmaster01"),
          OMEGASSH_VAULT_ERR_WRONG_PASSWORD);
    check("new master accepted", omegassh_vault_unlock(v3, "labmaster02"),
          OMEGASSH_VAULT_OK);
    check("list after re-key", omegassh_vault_list(v3, &s), OMEGASSH_VAULT_OK);
    check_true("CREDENTIALS SURVIVED THE RE-KEY",
               strstr(s, "lab-admin-renamed") != nullptr);
    printf("        %s\n", s);
    omegassh_free(s); s = nullptr;
    omegassh_vault_close(v3);

    /* --- keyring ------------------------------------------------------ */
    printf("\nkeyring\n");
    check("keyring status", omegassh_vault_keyring_status(v, &s), OMEGASSH_VAULT_OK);
    printf("        %s\n", s);
    check_true("status names the vault path", strstr(s, path) != nullptr);
    omegassh_free(s); s = nullptr;

    /* On a box with no Secret Service this is the interesting path: the quiet
     * unlock must report why it could not proceed, and must never claim the
     * password was wrong. */
    long long v4 = omegassh_vault_open(path);
    omegassh_vault_err q = omegassh_vault_unlock_quiet(v4);
    print_result("unlock_quiet", q);
    check_true("quiet unlock never reports a wrong password",
               q != OMEGASSH_VAULT_ERR_WRONG_PASSWORD);
    check_true("quiet unlock reports a keyring or needs-password condition",
               q == OMEGASSH_VAULT_OK ||
               q == OMEGASSH_VAULT_ERR_NEEDS_PASSWORD ||
               q == OMEGASSH_VAULT_ERR_KEYRING_UNAVAILABLE ||
               q == OMEGASSH_VAULT_ERR_KEYRING_STALE);
    omegassh_vault_close(v4);

    /* Environment fallback, for the headless case.
     *
     * This must be set BEFORE the process starts. A setenv() here does not
     * work, and the reason is worth knowing: the Go runtime snapshots the
     * environment at init, so a variable the C side sets afterwards is
     * invisible to os.LookupEnv. It fails silently, as a keyring error rather
     * than as anything pointing at the environment.
     *
     *   OMEGASSH_VAULT_PASSWORD=labmaster02 ./vault_smoke
     */
    if (getenv("OMEGASSH_VAULT_PASSWORD")) {
        long long v5 = omegassh_vault_open(path);
        check("quiet unlock via environment", omegassh_vault_unlock_quiet(v5),
              OMEGASSH_VAULT_OK);
        omegassh_vault_close(v5);
    } else {
        printf("  skip  quiet unlock via environment "
               "(re-run with OMEGASSH_VAULT_PASSWORD=labmaster02)\n");
    }

    /* --- stale keyring entry ------------------------------------------
     *
     * Opt-in, because it is the only part of this harness that writes to the
     * real login keyring. A default run must leave it untouched.
     *
     *   OMEGASSH_VAULT_SMOKE_KEYRING=1 ./vault_smoke
     *
     * The condition under test is the one the error range exists for: an
     * entry that was valid, a vault that has since been re-keyed, and a quiet
     * unlock that must say STALE rather than WRONG_PASSWORD. Nobody typed a
     * password, so reporting one as wrong sends the user to re-type something
     * they never entered instead of re-filing the entry.
     */
    if (getenv("OMEGASSH_VAULT_SMOKE_KEYRING")) {
        printf("\nstale keyring entry\n");
        const char *kpath = "/tmp/lab-vault-keyring.json";
        remove(kpath);

        long long k = omegassh_vault_open(kpath);
        check_true("open", k > 0);
        check("create", omegassh_vault_create(k, "labmaster01"),
              OMEGASSH_VAULT_OK);
        check("store", omegassh_vault_store(k,
              "{\"name\":\"lab-admin\",\"username\":\"labadmin\","
              "\"auth_type\":\"password\",\"password\":\"s3cret\"}", nullptr),
              OMEGASSH_VAULT_OK);

        check("keyring set", omegassh_vault_keyring_set(k, "labmaster01"),
              OMEGASSH_VAULT_OK);
        check("status reports the entry",
              omegassh_vault_keyring_status(k, &s), OMEGASSH_VAULT_OK);
        check_true("has_entry is true",
                   strstr(s, "\"has_entry\":true") != nullptr);
        omegassh_free(s); s = nullptr;

        /* The filed entry opens the vault. */
        omegassh_vault_lock(k);
        check("quiet unlock from the keyring", omegassh_vault_unlock_quiet(k),
              OMEGASSH_VAULT_OK);

        /* Re-key without re-filing. This is the mistake being modelled. */
        omegassh_vault_lock(k);
        check("change master", omegassh_vault_change_master(k,
              "labmaster01", "labmaster02"), OMEGASSH_VAULT_OK);

        omegassh_vault_err st = omegassh_vault_unlock_quiet(k);
        print_result("unlock_quiet after re-key", st);
        check("stale entry, not a wrong password", st,
              OMEGASSH_VAULT_ERR_KEYRING_STALE);

        /* Re-filing is the documented recovery, and it must work. */
        check("re-file the entry", omegassh_vault_keyring_set(k, "labmaster02"),
              OMEGASSH_VAULT_OK);
        check("quiet unlock works again", omegassh_vault_unlock_quiet(k),
              OMEGASSH_VAULT_OK);

        check("keyring clear", omegassh_vault_keyring_clear(k),
              OMEGASSH_VAULT_OK);
        check("clear again is not an error",
              omegassh_vault_keyring_clear(k), OMEGASSH_VAULT_OK);
        check("status reports no entry",
              omegassh_vault_keyring_status(k, &s), OMEGASSH_VAULT_OK);
        check_true("has_entry is false",
                   strstr(s, "\"has_entry\":false") != nullptr);
        omegassh_free(s); s = nullptr;

        omegassh_vault_close(k);
        remove(kpath);
    } else {
        printf("\n  skip  stale keyring entry "
               "(re-run with OMEGASSH_VAULT_SMOKE_KEYRING=1 -- writes to your "
               "login keyring)\n");
    }

    omegassh_free(id);
    omegassh_free(id2);
    omegassh_vault_close(v);
    remove(path);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}