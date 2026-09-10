/* include/omegassh/omegassh.h
 *
 * Stable C surface for omegassh sessions. Hand-written: cgo emits a header
 * too (libomegassh.h, in the build directory) and it is an artifact, not the
 * contract. It carries no comments, no enum names, and its declaration order
 * follows whatever the Go files happened to be in.
 *
 * WHAT THIS IS. One interactive session to a device, over SSH, telnet or a
 * serial console. The transport is chosen once, in omegassh_open, and nothing
 * afterwards depends on which was chosen: read, write, resize, alive and close
 * are the same calls with the same meanings for all three.
 *
 * DELIVERY MODEL. There is no callback into C, and there will not be one. A
 * Go callback fires on a Go-created OS thread, which in Qt means every byte
 * has to be marshalled with QMetaObject::invokeMethod, and forgetting once is
 * a rare crash rather than a compile error. Instead each session owns a
 * notifier. omegassh_notify_handle returns its readable end -- an fd on POSIX,
 * a socket on Windows -- which the caller watches with QSocketNotifier or
 * select(). When it becomes readable, drain it and then drain the session:
 *
 *     while ((n = omegassh_read(h, buf, sizeof buf)) > 0) { feed(buf, n); }
 *     while ((ev = omegassh_next_event(h)) != NULL) { handle(ev); free(ev); }
 *     if (!omegassh_alive(h)) { finished(omegassh_exit_code(h)); }
 *
 * Everything lands on the caller's thread. A readable notifier means "come and
 * look", not "here is one thing" -- one wake can cover many bytes and several
 * events, so always drain in a loop.
 *
 * THREADING. Every call is safe from any thread, and none of them blocks on
 * the far end -- omegassh_open included. It validates the configuration, files
 * a handle and returns; the dial runs behind it and reports itself through the
 * notifier like everything else. So a connect dialog can call it on the GUI
 * thread and stay responsive, which is what the connection overlay is for.
 *
 * ERRORS, AND WHICH ONE TO ASK. omegassh_last_error is per-THREAD and reports
 * the last failing call made on that thread. omegassh_error is per-SESSION and
 * reports why that session failed. The distinction matters because a dial
 * fails on a thread the caller does not own: a configuration mistake comes
 * back from omegassh_open as -1 and lands in omegassh_last_error, while a
 * refused password lands in omegassh_error and in an error event.
 *
 * MEMORY. Every char* returned belongs to the caller and is released with
 * omegassh_free -- the same deallocator the vault surface uses, not a second
 * one. NULL is a valid return and needs no freeing.
 *
 * ERRORS. A failing call returns -1 (or NULL) and leaves a message in
 * omegassh_last_error(), which is per-thread: two Qt workers dialing at once
 * do not overwrite each other's message. The vault surface reports failure
 * differently, as a code, because a caller there has to branch on why; see
 * omegassh/vault.h for that reasoning.
 */

#ifndef OMEGASSH_OMEGASSH_H
#define OMEGASSH_OMEGASSH_H

#ifdef __cplusplus
extern "C" {
#endif

/* A session handle. Opaque; the only valid operations are the calls below.
 * -1 is the failure value returned by omegassh_open and is never a handle.
 *
 * long long rather than a pointer so the value survives being marshalled
 * through Qt property systems, signal arguments and JSON without a cast that
 * would compile either way. */
typedef long long omegassh_session;

/* ---------------------------------------------------------------------------
 * Library
 */

/* Library version, e.g. "0.2.0". Caller frees. */
char *omegassh_version(void);

/* The exact opening bytes of a FIRST-CONTACT host key failure, e.g.
 * "unknown host key for". Caller frees.
 *
 * A prompt callback cannot cross this boundary, so a UI that wants to offer a
 * fingerprint dialog lets the dial fail, tests omegassh_error() for this
 * marker, asks, and re-dials with the TOFU policy. Match on this rather than
 * on a literal: the mismatch message is a DIFFERENT failure that must stay
 * fatal, and a hand-copied prefix is how the two end up conflated. */
char *omegassh_unknown_hostkey_marker(void);

/* The exact opening bytes of a CREDENTIAL REJECTION, e.g.
 * "authentication failed for". Caller frees.
 *
 * Same flow as the host key marker: let the dial fail, test omegassh_error()
 * for this marker, ask for credentials, and re-dial with the new ones. Match
 * on this rather than on a literal -- the underlying wording belongs to
 * x/crypto and is re-wrapped by this library precisely so that a UI does not
 * depend on it.
 *
 * Guaranteed not to match a host key failure, a refused connection, a name
 * that does not resolve, or a timeout. None of those are fixed by asking for
 * a password again. */
char *omegassh_auth_failed_marker(void);

/* The exact opening bytes of a KEYBOARD-INTERACTIVE QUESTION that could not be
 * answered, e.g. "keyboard-interactive prompt". Caller frees.
 *
 * The full message is:
 *
 *   keyboard-interactive prompt [secret]: YubiKey for `speterman':
 *   <--------- marker --------> <-tag-->  <------- question ------>
 *
 * The tag is "[secret]" or "[visible]" and says whether the answer should be
 * masked. The question is everything after the tag and its ": ", to the END of
 * the string -- it can contain colons, quotes and backticks, so nothing
 * delimits its end.
 *
 * Same flow as the other two markers: let the dial fail, test omegassh_error()
 * for this, put the question to a person, and dial again with the answer in
 * the request's "keyboard_answers" object, keyed by the question verbatim.
 *
 * NOT USABLE for a challenge whose answer is bound to the connection that
 * asked it: the second dial gets a new challenge. It suits a one-time password
 * the operator holds -- a Yubico OTP, a TOTP code -- and not a server-issued
 * nonce. */
char *omegassh_keyboard_prompt_marker(void);

/* The directory under $HOME this library writes session logs into, e.g.
 * ".omega". Caller frees.
 *
 * Published to be CHECKED against the host application's own idea of that
 * directory, not to be used in place of it. This library cannot call up into
 * the application, so it carries its own copy of the name; comparing the two
 * is how the copies are kept from drifting. */
char *omegassh_config_dir_name(void);

/* Releases any string returned by this library or by omegassh/vault.h.
 * Passing NULL is safe. */
void omegassh_free(char *s);

/* The last failure on THIS thread, or an empty string. Caller frees. */
char *omegassh_last_error(void);

/* ---------------------------------------------------------------------------
 * Session lifecycle
 */

/* Opens a session. Returns a handle, or -1 with the reason in
 * omegassh_last_error().
 *
 * IT DOES NOT WAIT FOR THE DIAL. The handle comes back as soon as the
 * configuration has been checked, and the connect, the authentication and the
 * shell request happen behind it. Watch the notifier: the session moves
 * CONNECTING -> AUTHENTICATING -> CONNECTED, or it moves to FAILED, and either
 * way the transitions arrive as events and omegassh_state answers at any
 * moment in between.
 *
 * So the two failures are reported in two different places, and the split is
 * the point:
 *
 *   -1 from this call        the configuration is wrong and you can fix it --
 *                            an unknown transport, a credential that names
 *                            nothing, an impossible parity, a log file that
 *                            cannot be created. Nothing was opened and nothing
 *                            was sent. The message is in omegassh_last_error.
 *
 *   a handle, then FAILED    the far end's answer -- unreachable, refused,
 *                            wrong key, host key mismatch. The reason is in
 *                            omegassh_error(h) and in an error event.
 *
 * A HANDLE MUST BE CLOSED EVEN IF IT NEVER CONNECTS. -1 needs no cleanup; any
 * other return owns a notifier and a registry entry until omegassh_close.
 * A session that reaches FAILED is finished, not closed.
 *
 * Before the session exists, omegassh_transport and omegassh_summary already
 * answer -- both come from the configuration -- so a tab has its title while
 * it is still connecting. omegassh_read and omegassh_pending return 0,
 * omegassh_alive returns 1, and omegassh_resize is remembered and applied when
 * the session arrives. omegassh_write is refused: keystrokes typed at a
 * connection overlay are not input to a device that has not answered yet.
 *
 * cfg_json is a UTF-8 JSON object. JSON rather than a wide argument list so
 * the surface can grow without breaking the ABI. Every field is optional
 * except where noted.
 *
 *   TRANSPORT
 *     "transport"          "ssh" (default), "telnet" or "serial". Absent
 *                          means ssh, so a caller written before the selector
 *                          existed keeps working unchanged.
 *
 *   SHARED
 *     "host"               target for ssh and telnet; required for both
 *     "port"               0 defaults per transport: 22 ssh, 23 telnet
 *     "term"               terminal type declared to the far end. On ssh the
 *                          pty request; on telnet the answer to a TTYPE
 *                          subnegotiation. Serial has nowhere to declare it.
 *     "cols", "rows"       initial window size
 *     "timeout_seconds"    bounds the connect
 *
 *   SSH
 *     "username"           required
 *     "password"           a secret the operator just typed. See CREDENTIALS.
 *     "private_key_path", "key_passphrase", "use_agent"
 *     "credential"         name of a vault entry to fill the blanks above
 *     "vault"              handle from omegassh_vault_open; required with
 *                          "credential"
 *     "jump_host", "jump_port", "jump_username", "jump_password",
 *     "jump_key_path", "jump_credential"
 *     "host_key_policy"    "strict" (default), "tofu", "insecure"
 *     "known_hosts_path"   empty => ~/.ssh/known_hosts
 *     "legacy_algorithms"  appends the old KEX/cipher/MAC/host-key tail that
 *                          aging network gear still requires
 *
 *   TELNET
 *     "telnet_crlf"        expand a lone CR to CR LF on write. Null means on,
 *                          which is what RFC 854 makes the telnet newline and
 *                          what console servers expect. Set false only for a
 *                          device that echoes a doubled newline.
 *
 *   SERIAL
 *     "serial_port"        OS device name; required. COM3, /dev/ttyUSB0,
 *                          /dev/cu.usbserial-XXXX. Enumerate with
 *                          omegassh_serial_ports.
 *     "baud"               0 => 9600
 *     "data_bits"          5-8, 0 => 8
 *     "parity"             none|odd|even|mark|space, "" => none
 *     "stop_bits"          1|1.5|2, "" => 1
 *
 *   LOGGING
 *     "log"                turn on session logging
 *     "log_path"           exact file; implies "log"
 *     "log_dir"            where a generated, timestamped name lands.
 *                          Empty => ~/.omega/logs, a user directory and never
 *                          the launch directory.
 *     "log_input"          also record what the operator types. Off by
 *                          default and deliberately: a device's own login
 *                          prompt is ordinary session data on telnet and on a
 *                          serial console, so the password answering it is
 *                          ordinary keystrokes, and this writes it to a
 *                          plaintext file.
 *     "log_append"         append instead of truncate
 *
 * A log that cannot be opened fails the call rather than being skipped
 * silently, and does so synchronously -- opening a file is not the network's
 * business. An operator who turned logging on for a change window needs to
 * know before the change, not afterwards.
 *
 * The credential and jump-host fields are refused on telnet and serial rather
 * than ignored. Telnet has no authentication step and no bastion, and a jump
 * host silently dropped would put the session on the wire in plaintext across
 * a link the operator believed was tunneled.
 *
 * CREDENTIALS. "credential" names a vault entry; its material is read on the
 * Go side during the dial and nothing comes back up. The plaintext fields are
 * first-class and stay that way -- quick connect with a typed-in password is
 * real use, not a fallback. The invariant is precisely that STORED secrets
 * never cross this boundary. */
omegassh_session omegassh_open(const char *cfg_json);

/* Ends the session and releases the handle. Safe on a session that already
 * ended, safe on one that never connected, and safe to call more than once.
 * After it returns, the notify handle is closed and no further call on this
 * handle is valid.
 *
 * It does not block, including on a session that is still dialing: the handle
 * is retired at once and the dial tidies up behind it. The dial itself is not
 * interrupted, so a socket can outlive its handle by up to
 * "timeout_seconds" -- invisible to the caller, but it is why closing a tab
 * against unreachable gear is instant. */
void omegassh_close(omegassh_session h);

/* Nonzero while the session is usable, INCLUDING while it is still dialing:
 * a session that has not connected yet is one that has not ended. It goes to
 * zero when the session ends, and for a dial that failed.
 *
 * Note the ordering this promises: EOF on the byte stream is not the end of an
 * SSH session, because the exit status arrives on the channel afterwards. This
 * keeps reporting 1 until the status has been collected, so a caller that
 * drains to EOF and then asks for the exit code gets the real one instead of
 * racing it. */
int omegassh_alive(omegassh_session h);

/* Which transport this handle is running over: "ssh", "telnet" or "serial".
 * NULL for an unknown handle. Caller frees. */
char *omegassh_transport(omegassh_session h);

/* The target in short human-readable form -- "10.0.0.1:23", "/dev/ttyUSB0
 * 9600 8N1" -- for tab titles and logs. NULL for an unknown handle. Caller
 * frees. */
char *omegassh_summary(omegassh_session h);

/* ---------------------------------------------------------------------------
 * Data path
 */

/* The readable end of this session's notifier: an fd on POSIX, a SOCKET on
 * Windows. Watch it; do not read, write or close it. -1 for an unknown handle.
 *
 * Readable means "come and look", not "one thing is waiting". */
long long omegassh_notify_handle(omegassh_session h);

/* Moves up to max buffered bytes into dst and returns the count. 0 means
 * nothing is pending -- it never blocks, and it never signals the end, so
 * check omegassh_alive separately. -1 for an unknown handle. */
int omegassh_read(omegassh_session h, char *dst, int max);

/* Sends operator input. Returns the number of bytes accepted, or -1 with the
 * reason in omegassh_last_error().
 *
 * The count is the caller's logical byte count, not the on-wire count: telnet
 * doubles a literal 0xFF and may expand CR, and a caller comparing the return
 * against n should not have to know that. */
int omegassh_write(omegassh_session h, const char *src, int n);

/* How many bytes are waiting to be read. -1 for an unknown handle. */
int omegassh_pending(omegassh_session h);

/* Reports a new window geometry to the far end. 0 on success, -1 on failure.
 *
 * A serial console has no window-change concept, so this succeeds and does
 * nothing there -- the caller resizes on every layout pass without branching
 * on which transport it holds. Telnet pushes NAWS if the peer negotiated it,
 * and records the size either way, so a device that agrees late still gets the
 * real width. */
int omegassh_resize(omegassh_session h, int cols, int rows);

/* The shell's exit status once it has ended: the program's own status, or
 * 128 + signal number where a signal ended it -- the same normalization
 * anytermqt's PtySession applies to a local child, so an application does not
 * have to know whether it is driving a pty or an SSH channel.
 *
 * -1 while the session is still running, for a session that ended without
 * reporting a status, and always for telnet and serial: neither protocol
 * carries an exit status, and reporting 0 would be indistinguishable from a
 * shell that exited cleanly. */
int omegassh_exit_code(omegassh_session h);

/* ---------------------------------------------------------------------------
 * State and events
 *
 * Two calls, because they answer different questions. omegassh_state answers
 * "what is it doing now", allocates nothing, and is what a connection overlay
 * needs. omegassh_next_event replays the transitions in order, for a caller
 * that wants every one of them.
 */

/* Session lifecycle states. The six are nterm-qt's, so an overlay displays
 * what the transport publishes rather than mapping one vocabulary onto
 * another.
 *
 * Not every transport reaches every state, and none of them fake one to look
 * uniform. AUTHENTICATING is SSH only -- telnet has no authentication step,
 * and a login prompt on it is ordinary session data arriving after the socket
 * is up. CONNECTING is SSH and telnet; opening a serial port is the whole
 * handshake, so serial goes straight to CONNECTED. RECONNECTING is published
 * by the reconnect policy above the transport; no transport enters it on its
 * own. */
typedef enum {
    OMEGASSH_STATE_DISCONNECTED   = 0,
    OMEGASSH_STATE_CONNECTING     = 1,
    OMEGASSH_STATE_AUTHENTICATING = 2,
    OMEGASSH_STATE_CONNECTED      = 3,
    OMEGASSH_STATE_RECONNECTING   = 4,
    OMEGASSH_STATE_FAILED         = 5
} omegassh_state_t;

/* Event categories, as they appear in the "kind" field of an event object. */
typedef enum {
    OMEGASSH_EVENT_STATE_CHANGED        = 0,
    OMEGASSH_EVENT_DATA                 = 1,
    OMEGASSH_EVENT_ERROR                = 2,
    OMEGASSH_EVENT_INTERACTION_REQUIRED = 3
} omegassh_event_t;

/* The current state, as an omegassh_state_t. -1 for an unknown handle. */
int omegassh_state(omegassh_session h);

/* The current state as a string -- "connected", "failed" -- for an overlay
 * that would rather render text than map an enum. NULL for an unknown handle.
 * Caller frees. */
char *omegassh_state_name(omegassh_session h);

/* Removes and returns the oldest queued event as a JSON object, or NULL when
 * the queue is empty. Caller frees a non-NULL result.
 *
 *   {"kind":0,"state":3}                         moved to connected
 *   {"kind":2,"error":"connection reset by peer"} the session reported an error
 *   {"kind":3,"ask":{"question":"Verification code:","echo":false}}
 *
 * Drain in a loop until it returns NULL, the same way omegassh_read is
 * drained: one wake can cover several events.
 *
 * OMEGASSH_EVENT_DATA is never emitted here. Bytes cross this boundary through
 * omegassh_read precisely so that no Go-created thread ever calls into Qt.
 *
 * OMEGASSH_EVENT_INTERACTION_REQUIRED is declared and not yet emitted. The
 * dial is asynchronous now, so it can park in AUTHENTICATING and publish a
 * question; what is missing is the answer coming back, which is one more call
 * alongside omegassh_write and not an ABI change. See Phase 2 in ROADMAP.md. */
char *omegassh_next_event(omegassh_session h);

/* Why THIS SESSION last failed, or an empty string. NULL for an unknown
 * handle. Caller frees.
 *
 * This is the one to ask after a dial fails. omegassh_last_error cannot carry
 * that reason: the dial runs on a thread of the library's, not one of yours,
 * so there is no per-thread slot of the caller's for it to land in. The
 * message is the same one the error event carried, kept here so that a caller
 * polling omegassh_state does not have to have been draining events at the
 * moment it happened. */
char *omegassh_error(omegassh_session h);

/* ---------------------------------------------------------------------------
 * Serial ports
 */

/* The available serial ports, as a JSON array, or NULL with the reason in
 * omegassh_last_error(). Caller frees.
 *
 *   [{"name":"/dev/ttyUSB0","is_usb":true,"vid":"0403","pid":"6001",
 *     "serial_number":"AB0KM1XY"}]
 *
 * An empty array is a valid answer and means no ports, not a failure. If the
 * detailed enumeration fails, this falls back to bare names rather than
 * returning nothing: a form showing device names with no vendor strings is
 * still usable, an empty form is not.
 *
 * Ports cross as data, not as a handle -- the same pattern as the vault
 * metadata surface. That is what lets a quick-connect form render whatever
 * this returns instead of pulling QSerialPort into the build and into
 * windeployqt for the sake of a list of strings. */
char *omegassh_serial_ports(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OMEGASSH_OMEGASSH_H */
