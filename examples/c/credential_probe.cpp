/* examples/c/credential_probe.cpp
 *
 * The vault-backed dial path, checked from C without a server.
 *
 * Every condition here is decided before a socket is opened -- an unknown
 * vault handle, a locked vault, a missing or disabled credential -- so the
 * whole credential join can be verified with nothing listening.
 *
 * That stayed true when the dial moved onto its own goroutine, and it was
 * worth keeping deliberately. A credential reference that names nothing is a
 * mistake in the configuration, not an answer from a device, so it is still
 * refused synchronously with the reason on the calling thread rather than
 * arriving seconds later as a connection failure. The refusals below are
 * therefore unchanged; the only case in this file that moved is the last one,
 * where a manual dial is expected to reach the network and fail there. What is NOT
 * covered is a successful dial; that needs tests/labsshd.sh and lives in
 * shell_smoke.
 *
 * The property under test is the one Phase 0 exists for: a caller names a
 * credential and never holds the secret. There is no call in the vault surface
 * that returns a password, so the only way this file could obtain one is if
 * the API had grown a hole -- which is why the vault-side assertions are here
 * rather than only in the Go tests.
 *
 *   cc -o credential_probe examples/c/credential_probe.cpp \
 *      build/libomegassh.a -Iinclude -lpthread
 */

#include <omegassh/omegassh.h>
#include <omegassh/vault.h>

#include "probe_connect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;

static void fail(const char *what, const std::string &detail)
{
    printf("  FAIL  %-46s %s\n", what, detail.c_str());
    failures++;
}

static std::string last_error()
{
    char *raw = omegassh_last_error();
    std::string out = raw ? raw : "";
    omegassh_free(raw);
    return out;
}

/* A dial that must be refused, with a reason that names the cause. The message
 * matters as much as the refusal: "authentication failed" for a locked vault
 * sends an operator to check the device. */
static void refused(const char *what, const std::string &cfg, const char *expect)
{
    long long h = omegassh_open(cfg.c_str());
    if (h >= 0) {
        omegassh_close(h);
        fail(what, "the dial was accepted");
        return;
    }
    std::string err = last_error();
    if (err.find(expect) == std::string::npos) {
        fail(what, "message did not mention '" + std::string(expect) + "': " + err);
        return;
    }
    printf("  ok    %-46s %s\n", what, err.c_str());
}

static void check(const char *what, omegassh_vault_err got,
                  omegassh_vault_err want)
{
    if (got == want) {
        printf("  ok    %-46s %s\n", what, omegassh_vault_error_name(got));
        return;
    }
    fail(what, std::string("got ") + omegassh_vault_error_name(got) +
               ", want " + omegassh_vault_error_name(want) +
               " (" + last_error() + ")");
}

static void check_true(const char *what, bool cond)
{
    if (cond) {
        printf("  ok    %s\n", what);
        return;
    }
    fail(what, "");
}

/* Host chosen so that if the credential join ever stopped short-circuiting,
 * the dial would fail on the network rather than silently succeeding. */
static const char *kHost = "eng-leaf-1.lab.invalid";

static std::string cfg_with(const std::string &fields)
{
    return std::string("{\"host\":\"") + kHost + "\",\"port\":22," + fields + "}";
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/lab-cred-probe.json";
    remove(path);

    char *s = nullptr;
    char *id = nullptr;

    printf("vault at %s\n\n", path);

    printf("setup\n");
    long long v = omegassh_vault_open(path);
    check_true("vault handle", v > 0);
    check("create", omegassh_vault_create(v, "labmaster01"), OMEGASSH_VAULT_OK);
    check("store lab-admin", omegassh_vault_store(v,
        "{\"name\":\"lab-admin\",\"username\":\"labadmin\","
        "\"auth_type\":\"password\",\"password\":\"s3cret\"}", &id),
        OMEGASSH_VAULT_OK);
    check("store lab-old", omegassh_vault_store(v,
        "{\"name\":\"lab-old\",\"username\":\"labadmin\","
        "\"auth_type\":\"password\",\"password\":\"s3cret\"}", &s),
        OMEGASSH_VAULT_OK);
    check("disable lab-old", omegassh_vault_set_disabled(v, s, 1),
          OMEGASSH_VAULT_OK);
    omegassh_free(s); s = nullptr;

    /* --- the invariant ------------------------------------------------- */
    printf("\nsecrets do not cross the boundary\n");
    check("list", omegassh_vault_list(v, &s), OMEGASSH_VAULT_OK);
    check_true("no password anywhere in the metadata",
               strstr(s, "s3cret") == nullptr);
    check_true("presence is reported instead",
               strstr(s, "\"has_secret\":true") != nullptr);
    omegassh_free(s); s = nullptr;

    check("meta", omegassh_vault_meta(v, "lab-admin", &s), OMEGASSH_VAULT_OK);
    check_true("no password in a single record",
               strstr(s, "s3cret") == nullptr);
    omegassh_free(s); s = nullptr;

    /* --- misconfiguration, refused before the network ------------------ */
    printf("\nrefused before dialing\n");

    refused("credential with no vault handle",
            cfg_with("\"credential\":\"lab-admin\""),
            "no vault handle");

    refused("unknown vault handle",
            cfg_with("\"credential\":\"lab-admin\",\"vault\":999999"),
            "no such vault handle");

    refused("jump credential with no jump host",
            cfg_with("\"jump_credential\":\"lab-admin\",\"vault\":" +
                     std::to_string(v)),
            "without jump_host");

    refused("unknown credential name",
            cfg_with("\"credential\":\"not-there\",\"vault\":" +
                     std::to_string(v)),
            "not-there");

    /* Disabled must not read as missing. An operator who took a credential
     * out of service should be told that, not sent hunting for a typo. */
    refused("disabled credential",
            cfg_with("\"credential\":\"lab-old\",\"vault\":" +
                     std::to_string(v)),
            "disabled");

    /* --- locked vault --------------------------------------------------- */
    printf("\nlocked vault\n");
    omegassh_vault_lock(v);
    refused("dial against a locked vault",
            cfg_with("\"credential\":\"lab-admin\",\"vault\":" +
                     std::to_string(v)),
            "locked");
    check("unlock", omegassh_vault_unlock(v, "labmaster01"), OMEGASSH_VAULT_OK);

    /* --- the manual path stays open ------------------------------------- */
    printf("\nmanual credentials still work\n");
    /* No vault, no credential: the shape quick connect uses. It must get past
     * config building and fail on the network instead, which is what an
     * unresolvable host gives us without a server. */
    {
        /* This one is now the asynchronous half: the config is complete, so
         * it gets a handle and then fails on the wire. Which is the assertion
         * -- a manual dial must get past config building, and a failure that
         * arrives as a failed STATE rather than as -1 is the proof that it
         * did. */
        std::string err;
        bool connected = false;
        long long h = probe_open(cfg_with(
            "\"username\":\"labadmin\",\"password\":\"typed-by-hand\""),
            15000, &connected, &err);
        if (h < 0) {
            fail("manual dial reaches the network",
                 "refused before dialing: " + err);
        } else if (connected) {
            omegassh_close(h);
            fail("manual dial reaches the network", "unexpectedly connected");
        } else if (err.find("vault") != std::string::npos ||
                   err.find("credential") != std::string::npos) {
            omegassh_close(h);
            fail("manual dial reaches the network",
                 "refused for a credential reason: " + err);
        } else {
            omegassh_close(h);
            printf("  ok    %-46s %s\n",
                   "manual dial reaches the network", err.c_str());
        }
    }

    omegassh_free(id);
    omegassh_vault_close(v);
    remove(path);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}