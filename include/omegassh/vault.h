/* include/omegassh/vault.h
 *
 * Stable C surface for the credential vault. Hand-written, like
 * omegassh/omegassh.h -- cgo emits a header too, and it is not this one.
 *
 * SCOPE. omegassh's own scope statement says credential storage belongs to the
 * application, and for the SSH surface that is still true: sshcore takes
 * credentials, it does not keep them. The vault is here anyway for a
 * mechanical reason. Two Go c-archives cannot link into one process -- each
 * carries a full runtime -- so an application that wants both SSH and a vault
 * from Go gets them from one archive or writes one of them twice. Keeping the
 * packages separate below the shim is what preserves the original intent:
 * sshcore does not import vault, vault does not import sshcore, and either is
 * importable alone by a Go program that skips the C layer entirely.
 *
 * The shape of this surface is deliberate:
 *
 *   Metadata comes OUT. Secrets only go IN.
 *
 * There is no call here that returns a password, a passphrase or a key, and
 * that is not an oversight to correct later when something seems to need one.
 * A dial resolves its own credential inside Go from a reference passed with
 * the connection parameters, which is what keeps plaintext off the C++ heap.
 * Adding a getter here would undo it in one commit.
 *
 * THREADING. Every call is synchronous and safe from any thread; the Go side
 * is mutex-guarded. Three run Argon2id and cost real time --
 * omegassh_vault_create, omegassh_vault_unlock, omegassh_vault_change_master,
 * measured at ~40ms on four cores and ~140ms on one. Call those from a worker,
 * not the GUI thread. Everything else is microseconds, or a sub-millisecond
 * file rewrite, and can be called inline.
 *
 * That is the difference from the session API. A dial is seconds long and
 * needs a notifier; this does not. There is no fd to watch here and no
 * callback to marshal.
 *
 * MEMORY. Every char** out-parameter is allocated by Go and belongs to the
 * caller, who releases it with omegassh_free -- the same deallocator the
 * session surface uses, not a second one. Out-parameters are never written on
 * a failure return, so nothing needs freeing on that path.
 *
 * ENCODING. Structured values cross as UTF-8 JSON rather than as C structs,
 * matching omegassh_open. The metadata record carries three string arrays, and
 * hand-rolling nested variable-length marshalling for them is a lot of code
 * that buys nothing.
 */

#ifndef OMEGASSH_VAULT_H
#define OMEGASSH_VAULT_H

/* omegassh_free and omegassh_last_error are shared with the session surface;
 * strings returned here are released with the same deallocator. */
#include <omegassh/omegassh.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Errors
 *
 * The session surface reports failure as -1 plus omegassh_last_error(). That
 * is not enough here. A caller has to branch on WHY an unlock failed -- a
 * wrong password re-prompts, an unreachable keyring offers manual credentials
 * instead -- and a human-readable string cannot carry that decision. So the
 * vault calls return a code, and the string remains available for detail.
 *
 * Numbering is stable; append only. The range that matters is
 * OMEGASSH_VAULT_ERR_KEYRING_*: everything in it means "the vault could not be
 * reached", as opposed to "the vault said no".
 */

typedef enum {
    OMEGASSH_VAULT_OK                       = 0,

    /* Caller error -- a bug above the boundary, not a condition to report. */
    OMEGASSH_VAULT_ERR_BAD_HANDLE           = 1,  /* closed, or never opened */
    OMEGASSH_VAULT_ERR_BAD_ARGUMENT         = 2,  /* null pointer, empty path, bad JSON */

    /* Vault state. */
    OMEGASSH_VAULT_ERR_LOCKED               = 10, /* unlock first */
    OMEGASSH_VAULT_ERR_NOT_FOUND            = 11, /* no vault file at that path */
    OMEGASSH_VAULT_ERR_EXISTS               = 12, /* create over an existing one */
    OMEGASSH_VAULT_ERR_WRONG_PASSWORD       = 13, /* GCM auth failure on unlock */
    OMEGASSH_VAULT_ERR_CORRUPT              = 14, /* envelope unparseable or wrong version */
    OMEGASSH_VAULT_ERR_IO                   = 15, /* read/write/rename failed */
    OMEGASSH_VAULT_ERR_WEAK_PASSWORD        = 16, /* below the minimum length on create */

    /* Credential records. */
    OMEGASSH_VAULT_ERR_CRED_NOT_FOUND       = 20,
    OMEGASSH_VAULT_ERR_DUPLICATE_NAME       = 21,
    OMEGASSH_VAULT_ERR_EMPTY_NAME           = 22,

    /* The OS keyring. A separate failure domain from opening the vault, which
     * is the whole reason it gets its own range. */
    /* Reserved. Nothing returns this: the quiet unlock treats "keyring
     * reachable, nothing filed" as a source that had no answer rather than a
     * failure, and falls through to the environment. Use the "has_entry"
     * field of omegassh_vault_keyring_status to ask that question. The number
     * stays allocated because this enum is append-only. */
    OMEGASSH_VAULT_ERR_KEYRING_NO_ENTRY     = 30,
    OMEGASSH_VAULT_ERR_KEYRING_UNAVAILABLE  = 31, /* could not be reached at all */
    OMEGASSH_VAULT_ERR_KEYRING_STALE        = 32, /* filed, but no longer unlocks */
    OMEGASSH_VAULT_ERR_NEEDS_PASSWORD       = 33, /* quiet unlock exhausted its sources */

    OMEGASSH_VAULT_ERR_INTERNAL             = 99
} omegassh_vault_err;

/* A short stable identifier for a code ("OMEGASSH_VAULT_ERR_LOCKED"), for
 * logs. Owned by the library, never null, and NOT freed by the caller -- it is
 * the one exception to the omegassh_free rule, because it is a constant. */
const char *omegassh_vault_error_name(omegassh_vault_err code);

/* Detail text for the last failure comes from omegassh_last_error(), shared
 * with the session surface. Free it with omegassh_free.
 *
 * READ IT ONLY AFTER A NON-OK RETURN. Every vault call clears the message on
 * success, so an OK return leaves nothing to read -- but the session surface
 * does not, and the two share the store. A string fetched after a call that
 * worked is either empty or somebody else's.
 *
 * The message is kept per OS thread, so a worker that checks a return code
 * and then reads the text gets its own even while another thread is failing.
 *
 * It is the reason OMEGASSH_VAULT_ERR_KEYRING_UNAVAILABLE is one code rather
 * than three. A locked Keychain, an absent D-Bus session and an unreachable
 * Credential Manager are three different things to tell a user, and the
 * underlying library surfaces them as wrapped platform errors with no stable
 * discriminant -- so promoting them to codes would mean string-matching
 * platform messages and pretending the result is reliable. One code plus the
 * text is honest. Show the text; do not parse it. */

/* ---------------------------------------------------------------------------
 * Handles
 *
 * Same scheme as the session surface: a long long into a Go-side registry, not
 * a pointer. Go's pointer-passing rules make handing out a Go pointer a thing
 * to get wrong quietly. Vault handles and session handles are separate
 * namespaces; do not mix them.
 */

/* Bind a handle to a vault file. Touches nothing on disk -- the file is not
 * read until create or unlock. Returns -1 on failure, with the reason in
 * omegassh_last_error(). */
long long omegassh_vault_open(const char *path);

/* Release the handle, locking it first. Safe on any value. */
void omegassh_vault_close(long long v);

/* 1 if a vault file is present, 0 otherwise or on a bad handle. */
int omegassh_vault_exists(long long v);

/* 1 if locked. A bad handle reports locked, which is the safe answer. */
int omegassh_vault_is_locked(long long v);

/* The path the handle is bound to. Caller frees with omegassh_free. */
omegassh_vault_err omegassh_vault_path(long long v, char **path_out);

/* ---------------------------------------------------------------------------
 * Unlocking
 */

/* Create a new vault, leaving it unlocked. Fails with _EXISTS if one is there.
 * Runs Argon2id. */
omegassh_vault_err omegassh_vault_create(long long v, const char *master);

/* Unlock with a master the caller already has. Never consults the keyring,
 * never prompts. Runs Argon2id -- and runs it on a wrong password too, at the
 * same cost, because there is no separate hash to check against. */
omegassh_vault_err omegassh_vault_unlock(long long v, const char *master);

/* Unlock from the OS keyring, then the environment, without asking anyone.
 * This is the startup path: try, and leave it locked rather than blocking.
 *
 * _NEEDS_PASSWORD means no source had one. Normal, not a failure -- put up a
 * dialog and call omegassh_vault_unlock.
 *
 * _KEYRING_STALE means an entry exists and no longer opens the vault. This is
 * the case that must never surface as _WRONG_PASSWORD: the user typed nothing,
 * and telling them they mistyped sends them looking in the wrong place. Offer
 * to re-file the entry.
 *
 * _KEYRING_UNAVAILABLE is returned only when nothing else answered; an
 * unreachable keyring is not fatal on its own.
 *
 * The environment fallback is OMEGASSH_VAULT_PASSWORD, and it must be present
 * when the PROCESS STARTS. Go snapshots the environment at runtime init, so a
 * setenv() from C afterwards is invisible here -- and invisible is the whole
 * problem: it does not report a bad variable, it reports whatever the keyring
 * said, which sends you debugging the wrong thing. To supply a master
 * programmatically, use omegassh_vault_unlock. */
omegassh_vault_err omegassh_vault_unlock_quiet(long long v);

/* Drop the key. Safe when already locked.
 *
 * What this guarantees: the derived AES key is overwritten and the credential
 * list is released. What it does NOT guarantee: that secret bytes are gone
 * from memory. Credentials hold Go strings, which are immutable -- clearing a
 * field drops the reference and leaves the bytes for the collector, and Go
 * offers no way to overwrite them. Treat a locked vault as "cannot be read
 * through this API", not as "scrubbed". */
void omegassh_vault_lock(long long v);

/* Re-key under a new master. Verifies the old one against the file first.
 * Runs Argon2id twice.
 *
 * This does NOT update the keyring entry, which is then stale. A UI offering a
 * change-password button and not re-filing the entry has silently broken
 * auto-unlock -- follow a success with omegassh_vault_keyring_set. */
omegassh_vault_err omegassh_vault_change_master(long long v,
                                                const char *old_master,
                                                const char *new_master);

/* ---------------------------------------------------------------------------
 * Credentials
 *
 * Records cross as JSON. Outbound is redacted metadata:
 *
 *   { "id": "...", "name": "lab-admin", "username": "labadmin",
 *     "auth_label": "Password", "description": "", "priority": 0,
 *     "tags": [], "scope": { "domain_suffix": "", "cidrs": [], "platforms": [] },
 *     "is_default": false, "disabled": false,
 *     "last_used": "2026-08-27T09:14:02Z", "has_secret": true }
 *
 * has_secret is how a UI shows material is present without holding it. Arrays
 * are always present and never null, so a C++ parser needs one code path.
 *
 * Inbound accepts the same field names plus "password", "key_path" and
 * "key_passphrase", and uses "auth_type" ("password", "publickey",
 * "keyboard-interactive", "agent") rather than the display label. An "id"
 * selects an existing record to replace; omit it to add.
 */

/* All credentials, redacted, as a JSON array sorted default-first then by
 * name. Caller frees. An empty vault yields "[]", never null. */
omegassh_vault_err omegassh_vault_list(long long v, char **json_out);

/* One credential, redacted, as a JSON object, resolved by exact id, then
 * exact name, then case-insensitive name. Caller frees.
 *
 * Note what this does not do: it resolves a reference so a UI can show which
 * credential a session will use. It does not hand over the secret, and there
 * is no variant that does. */
omegassh_vault_err omegassh_vault_meta(long long v, const char *id_or_name,
                                       char **json_out);

/* Add or replace. With no "id" the record is added and its new id written to
 * *id_out; with an "id" the record is replaced and *id_out repeats it. Names
 * are unique case-insensitively. Caller frees *id_out.
 *
 * On replace this stores exactly the record you send, so an omitted secret
 * field CLEARS it. That is the sharpest corner in this API: metadata from
 * omegassh_vault_meta carries no password, so sending it back with an edited
 * name wipes the password. Use omegassh_vault_rename and
 * omegassh_vault_set_disabled for the field-at-a-time cases -- they do the
 * read-modify-write inside Go, where the secret is reachable. Reserve store
 * for a form where the user supplied the credential material. */
omegassh_vault_err omegassh_vault_store(long long v, const char *json_in,
                                        char **id_out);

/* Edit a credential WITHOUT touching its material.
 *
 * This is the call an editing form should use, and store is the one it should
 * not. A form populated from omegassh_vault_meta holds no password, no key
 * path and no passphrase -- so sending that record back through store writes a
 * record with none, which is a lost secret with no error to notice.
 *
 * Takes the same JSON store takes, minus the material: an "id" is required,
 * and "password", "key_path", "key_passphrase" and "auth_type" are REFUSED
 * with _BAD_ARGUMENT rather than ignored. A caller sending material here
 * believes it is being stored; absorbing it silently would be the same class
 * of bug this call exists to prevent.
 *
 * Everything else is merged onto the stored record: name, username,
 * description, priority, tags, scope, disabled. Auth method, material,
 * created_at and last_used are left as they were.
 *
 * "is_default" is honoured in one direction only. true promotes this
 * credential and clears any other; false leaves the current default alone. A
 * redacted record says is_default:false because it is not the default, which
 * is not the same statement as "there should be no default" -- use
 * omegassh_vault_clear_default for that.
 *
 * So the write surface is two calls with one question between them: did the
 * user supply credential material? Yes, store. No, this. */
omegassh_vault_err omegassh_vault_update_meta(long long v, const char *json_in);

/* Remove by id. */
omegassh_vault_err omegassh_vault_delete(long long v, const char *id);

/* Rename by id, preserving everything else including the secret. */
omegassh_vault_err omegassh_vault_rename(long long v, const char *id,
                                         const char *new_name);

/* Mark one credential the default, clearing any other. */
omegassh_vault_err omegassh_vault_set_default(long long v, const char *id);

/* Leave no credential marked default. Not an error when none was. */
omegassh_vault_err omegassh_vault_clear_default(long long v);

/* Name of the default credential, or "" when there is none or it is disabled.
 * Caller frees. Exists so a form can show what a blank field would use without
 * fetching a record that carries a secret. */
omegassh_vault_err omegassh_vault_default_name(long long v, char **name_out);

/* Take a credential out of automatic selection, or put it back. A disabled
 * credential is still listed and still fetchable by name; it is skipped by the
 * default and by resolution. */
omegassh_vault_err omegassh_vault_set_disabled(long long v, const char *id,
                                               int disabled);

/* ---------------------------------------------------------------------------
 * Keyring
 *
 * Entries are keyed by the absolute vault path, so a lab vault and a
 * production vault on one machine never share one.
 */

/* What the quiet-unlock path would find, without returning the secret:
 *
 *   { "disabled": false, "available": true, "has_entry": true,
 *     "account": "/home/user/.pathfinderssh/vault.json", "error": "" }
 *
 * Caller frees. Returns OK even when the keyring is unreachable -- that is the
 * status being reported, not a failure to report it. */
omegassh_vault_err omegassh_vault_keyring_status(long long v, char **json_out);

/* File the master password for this vault.
 *
 * The caller must have verified it against the vault first. Storing an
 * unverified string is how an entry becomes a lockout, and this call cannot
 * check for you -- it has no key. */
omegassh_vault_err omegassh_vault_keyring_set(long long v, const char *master);

/* Remove the entry. Not an error when there was none. */
omegassh_vault_err omegassh_vault_keyring_clear(long long v);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OMEGASSH_VAULT_H */   