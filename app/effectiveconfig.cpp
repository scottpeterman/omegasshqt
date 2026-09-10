// app/effectiveconfig.cpp

#include "app/effectiveconfig.h"

#include <QString>

namespace omega::app {
namespace {

QString toQ(const std::string &s) { return QString::fromStdString(s); }

}  // namespace

// The global layer. Applied first so the session layer can overwrite any of
// it, and applied identically on both entry points -- a quick-connect tab and
// a saved-session tab differ in what they override, not in what they inherit.
TabSettings resolveTabSettings(const AppSettings &settings,
                               const OmegaSettings &omega) {
    TabSettings tab;
    tab.scrollbackLines = settings.scrollback_lines;
    tab.multilinePasteThreshold = settings.multiline_paste_threshold;
    tab.fontPointSize = settings.font_size;
    tab.antiIdle = omega.anti_idle;
    tab.wheelAltScreen = omega.wheel_alt_screen;
    return tab;
}

TabSettings resolveTabSettings(const sessions::Session &session,
                               const AppSettings &settings,
                               const OmegaSettings &omega) {
    // Globals first, so the session layer can overwrite any of it.
    TabSettings tab = resolveTabSettings(settings, omega);

    if (session.scrollback_lines) {
        tab.scrollbackLines = *session.scrollback_lines;
    }
    if (session.multiline_paste_threshold) {
        tab.multilinePasteThreshold = *session.multiline_paste_threshold;
    }
    if (session.paste_baud) {
        tab.pasteBaud = *session.paste_baud;
    }
    if (session.wheel_alt_screen) {
        tab.wheelAltScreen = *session.wheel_alt_screen;
    }

    // --- appearance -------------------------------------------------------
    // The global already put settings.font_size in; this is the session
    // pinning its own. Left alone when absent, which is what keeps a session
    // following Settings > Font size for the rest of its life.
    if (session.font_size) {
        tab.fontPointSize = *session.font_size;
    }

    // Blank is treated as absent, the same as term_type: a column holding ""
    // is a row that was edited and cleared, and it means inherit rather than
    // "a theme with no name".
    if (session.theme_name && !session.theme_name->empty()) {
        tab.themeName = toQ(*session.theme_name);
    }

    // Anti-idle resolves FIELD BY FIELD, not as a block. A session that only
    // switches it on inherits the global's interval and keystroke; one that
    // only changes the keystroke stays off if the global is off. Copying the
    // whole struct on any one override would silently reset the other three.
    if (session.anti_idle_enabled) {
        tab.antiIdle.enabled = *session.anti_idle_enabled;
    }
    if (session.anti_idle_seconds) {
        tab.antiIdle.seconds = *session.anti_idle_seconds;
    }
    if (session.anti_idle_keystroke) {
        tab.antiIdle.keystroke =
            keystrokeFromName(toQ(*session.anti_idle_keystroke));
    }
    if (session.anti_idle_custom) {
        tab.antiIdle.custom = hexToBytes(toQ(*session.anti_idle_custom));
    }

    return tab;
}

const char *hostKeyPolicyName(omegassh::HostKeyPolicy policy) {
    switch (policy) {
        case omegassh::HostKeyPolicy::Tofu:     return "tofu";
        case omegassh::HostKeyPolicy::Insecure: return "insecure";
        case omegassh::HostKeyPolicy::Strict:   break;
    }
    return "strict";
}

omegassh::HostKeyPolicy hostKeyPolicyFromName(const std::string &name,
                                              bool *known) {
    if (known) *known = true;
    if (name == "tofu") return omegassh::HostKeyPolicy::Tofu;
    if (name == "insecure") return omegassh::HostKeyPolicy::Insecure;
    if (name != "strict" && known) *known = false;
    // Strict on anything unrecognised. A typo in the column must not be the
    // reason host key checking silently stopped happening.
    return omegassh::HostKeyPolicy::Strict;
}

Resolved resolveSession(const sessions::Session &session,
                        const AppSettings &settings,
                        const OmegaSettings &omega,
                        long long vaultHandle,
                        const QString &defaultCredential) {
    Resolved out;
    out.tab = resolveTabSettings(session, settings, omega);
    out.displayName = toQ(session.name);

    omegassh::Config &cfg = out.config;

    switch (session.transport) {
        case sessions::SessionTransport::Telnet:
            cfg.transport = omegassh::Transport::Telnet;
            break;
        case sessions::SessionTransport::Serial:
            cfg.transport = omegassh::Transport::Serial;
            break;
        case sessions::SessionTransport::Ssh:
            cfg.transport = omegassh::Transport::Ssh;
            break;
    }

    cfg.host = toQ(session.hostname);
    cfg.port = session.port;

    // The name crosses, never the material -- unchanged from openSession(),
    // and the pairing is the same: a handle without a name resolves nothing,
    // a name without a handle is refused before the socket opens.
    if (session.credential_name && !session.credential_name->empty()) {
        cfg.credential = toQ(*session.credential_name);
        cfg.vaultHandle = vaultHandle;
    }
    if (session.username && !session.username->empty()) {
        cfg.username = toQ(*session.username);
    }

    // --- the credential a session does NOT name ---------------------------
    //
    // SSH only. Telnet has no authentication step and serial has no network
    // identity, and the library refuses credentials on both -- so a global
    // that applied to all three would be a global that fails two of them.
    //
    // Agent sets useAgent, which nothing in the application set before this:
    // the session editor offered an "(SSH agent)" row that wrote NULL and
    // nothing downstream turned the agent on, so the row named a behaviour
    // the config never carried. It carries it now.
    if (cfg.transport == omegassh::Transport::Ssh && cfg.credential.isEmpty()) {
        switch (omega.ssh_default_auth) {
            case SshDefaultAuth::Agent:
                cfg.useAgent = true;
                break;
            case SshDefaultAuth::VaultDefault:
                if (!defaultCredential.isEmpty()) {
                    cfg.credential = defaultCredential;
                    cfg.vaultHandle = vaultHandle;
                }
                break;
            case SshDefaultAuth::Ask:
                // Left empty on purpose. Prompting is the caller's, and a
                // Resolved with no credential under this mode is what tells
                // it to.
                break;
        }
    }

    // --- ssh --------------------------------------------------------------
    if (session.host_key_policy) {
        cfg.hostKeyPolicy = hostKeyPolicyFromName(*session.host_key_policy);
    }
    if (session.legacy_algorithms) {
        cfg.legacyAlgorithms = *session.legacy_algorithms;
    }

    // --- jump host --------------------------------------------------------
    // Only meaningful with a host: Config ignores the rest without one, so
    // writing them regardless would be writing fields nothing reads.
    if (session.jump_host && !session.jump_host->empty()) {
        cfg.jumpHost = toQ(*session.jump_host);
        if (session.jump_port) cfg.jumpPort = *session.jump_port;
        if (session.jump_username) cfg.jumpUsername = toQ(*session.jump_username);
        if (session.jump_credential) {
            cfg.jumpCredential = toQ(*session.jump_credential);
            cfg.vaultHandle = vaultHandle;
        }
    }

    // --- serial -----------------------------------------------------------
    if (session.serial_port) cfg.serialPort = toQ(*session.serial_port);
    if (session.serial_baud) cfg.baud = *session.serial_baud;
    if (session.serial_data_bits) cfg.dataBits = *session.serial_data_bits;
    if (session.serial_parity) cfg.parity = toQ(*session.serial_parity);
    if (session.serial_stop_bits) cfg.stopBits = toQ(*session.serial_stop_bits);

    // --- telnet -----------------------------------------------------------
    if (session.telnet_crlf) cfg.telnetCrlf = *session.telnet_crlf;

    // --- terminal type ----------------------------------------------------
    // Session, then the global, then Config's own "xterm-256color". The
    // global is skipped when blank rather than sent as an empty TERM, which
    // is what openTab() did with it before and is the behaviour being kept.
    if (session.term_type && !session.term_type->empty()) {
        cfg.term = toQ(*session.term_type);
    } else if (!settings.default_term_type.trimmed().isEmpty()) {
        cfg.term = settings.default_term_type.trimmed();
    }

    return out;
}

Resolved resolveQuickConnect(const omegassh::Config &dialogConfig,
                             const AppSettings &settings,
                             const OmegaSettings &omega) {
    Resolved out;
    out.config = dialogConfig;
    out.tab = resolveTabSettings(settings, omega);

    // The one field the dialog does not ask for. Serial has nowhere to
    // declare a terminal type and ignores it.
    const QString term = settings.default_term_type.trimmed();
    if (!term.isEmpty()) out.config.term = term;

    return out;
}

}  // namespace omega::app