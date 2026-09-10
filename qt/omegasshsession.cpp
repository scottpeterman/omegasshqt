// qt/omegasshsession.cpp

#include "omegasshsession.h"

#include <omegassh/omegassh.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace omegassh {
namespace {

// Bytes pulled per read() call. A full-screen redraw on a wide terminal runs
// to tens of kilobytes, so a small buffer just means more round trips.
constexpr int kReadChunk = 32 * 1024;

// Set by serialPorts() on failure. Static rather than thrown or returned
// alongside the list because the caller that cares is a form, and a form has
// one place to put a message.
QString g_enumerationError;

const char *policyName(HostKeyPolicy p) {
    switch (p) {
        case HostKeyPolicy::Tofu:
            return "tofu";
        case HostKeyPolicy::Insecure:
            return "insecure";
        case HostKeyPolicy::Strict:
            break;
    }
    return "strict";
}

const char *transportName(Transport t) {
    switch (t) {
        case Transport::Telnet:
            return "telnet";
        case Transport::Serial:
            return "serial";
        case Transport::Ssh:
            break;
    }
    return "ssh";
}

Transport transportFromName(const QString &s) {
    if (s == QLatin1String("telnet")) return Transport::Telnet;
    if (s == QLatin1String("serial")) return Transport::Serial;
    return Transport::Ssh;
}

State stateFromCode(int code) {
    switch (code) {
        case OMEGASSH_STATE_CONNECTING:
            return State::Connecting;
        case OMEGASSH_STATE_AUTHENTICATING:
            return State::Authenticating;
        case OMEGASSH_STATE_CONNECTED:
            return State::Connected;
        case OMEGASSH_STATE_RECONNECTING:
            return State::Reconnecting;
        case OMEGASSH_STATE_FAILED:
            return State::Failed;
        default:
            break;
    }
    return State::Disconnected;
}

// take converts an owned char* from the library into a QString and frees it
// with the library's own deallocator, so no path through this file leaks one.
QString take(char *raw) {
    QString s = QString::fromUtf8(raw ? raw : "");
    omegassh_free(raw);
    return s;
}

// Clears the notify handle's readable state. The handle is a wakeup, never a
// byte count, so whatever comes back is discarded.
void drainNotify(long long handle) {
    if (handle < 0) return;
    char scratch[256];
#ifdef _WIN32
    while (::recv(static_cast<SOCKET>(handle), scratch, sizeof(scratch), 0) > 0) {
    }
#else
    while (::read(static_cast<int>(handle), scratch, sizeof(scratch)) > 0) {
    }
#endif
}

}  // namespace

QString SerialPortInfo::displayName() const {
    if (!isUsb || (vid.isEmpty() && pid.isEmpty())) return name;
    QString label = name;
    if (!serialNumber.isEmpty()) label += QStringLiteral(" [%1]").arg(serialNumber);
    if (!vid.isEmpty() && !pid.isEmpty()) {
        label += QStringLiteral(" (%1:%2)").arg(vid, pid);
    }
    return label;
}

QJsonObject Config::toJson() const {
    QJsonObject o;
    o["transport"] = QString::fromLatin1(transportName(transport));

    // Only the fields this transport actually honours are sent. The library
    // refuses several of them per transport rather than ignoring them -- a
    // jump host silently dropped on telnet would put the session on the wire
    // in plaintext across a link the operator believed was tunneled -- so
    // sending a field that does not apply turns into a failed dial, not a
    // quiet downgrade.
    switch (transport) {
        case Transport::Ssh:
            o["host"] = host;
            if (port > 0) o["port"] = port;
            o["username"] = username;
            if (!password.isEmpty()) o["password"] = password;
            if (!privateKeyPath.isEmpty()) o["private_key_path"] = privateKeyPath;
            if (!keyPassphrase.isEmpty()) o["key_passphrase"] = keyPassphrase;
            if (useAgent) o["use_agent"] = true;
            if (!credential.isEmpty()) {
                o["credential"] = credential;
                o["vault"] = vaultHandle;
            }
            if (!jumpHost.isEmpty()) {
                o["jump_host"] = jumpHost;
                o["jump_port"] = jumpPort;
                if (!jumpUsername.isEmpty()) o["jump_username"] = jumpUsername;
                if (!jumpPassword.isEmpty()) o["jump_password"] = jumpPassword;
                if (!jumpKeyPath.isEmpty()) o["jump_key_path"] = jumpKeyPath;
                if (!jumpCredential.isEmpty()) o["jump_credential"] = jumpCredential;
            }
            if (!keyboardAnswers.isEmpty()) {
                QJsonObject answers;
                for (auto it = keyboardAnswers.cbegin();
                     it != keyboardAnswers.cend(); ++it) {
                    answers[it.key()] = it.value();
                }
                o["keyboard_answers"] = answers;
            }
            o["host_key_policy"] = QString::fromLatin1(policyName(hostKeyPolicy));
            if (!knownHostsPath.isEmpty()) o["known_hosts_path"] = knownHostsPath;
            if (legacyAlgorithms) o["legacy_algorithms"] = true;
            o["term"] = term;
            break;

        case Transport::Telnet:
            o["host"] = host;
            if (port > 0) o["port"] = port;
            // Sent unconditionally: the library treats absent as "on", and a
            // deliberate false has to be distinguishable from a default.
            o["telnet_crlf"] = telnetCrlf;
            o["term"] = term;
            break;

        case Transport::Serial:
            o["serial_port"] = serialPort;
            if (baud > 0) o["baud"] = baud;
            if (dataBits > 0) o["data_bits"] = dataBits;
            if (!parity.isEmpty()) o["parity"] = parity;
            if (!stopBits.isEmpty()) o["stop_bits"] = stopBits;
            // No term: a serial console has nowhere to declare one.
            break;
    }

    o["cols"] = cols;
    o["rows"] = rows;
    if (timeoutSeconds > 0) o["timeout_seconds"] = timeoutSeconds;

    if (log.enabled || !log.path.isEmpty()) {
        o["log"] = true;
        if (!log.path.isEmpty()) o["log_path"] = log.path;
        if (!log.dir.isEmpty()) o["log_dir"] = log.dir;
        if (log.includeInput) o["log_input"] = true;
        if (log.append) o["log_append"] = true;
    }
    return o;
}

OmegaSshSession::OmegaSshSession(QObject *parent) : QObject(parent) {
    // Registered here rather than at static-init time so it happens exactly
    // when a session first exists, in whatever order the application's
    // translation units were initialised. qRegisterMetaType is idempotent.
    qRegisterMetaType<omegassh::State>("omegassh::State");
    qRegisterMetaType<omegassh::Transport>("omegassh::Transport");
}

OmegaSshSession::~OmegaSshSession() { teardown(); }

QString OmegaSshSession::version() { return take(omegassh_version()); }

QString OmegaSshSession::lastEnumerationError() { return g_enumerationError; }

QVector<SerialPortInfo> OmegaSshSession::serialPorts() {
    g_enumerationError.clear();

    char *raw = omegassh_serial_ports();
    if (!raw) {
        g_enumerationError = take(omegassh_last_error());
        if (g_enumerationError.isEmpty()) {
            g_enumerationError = tr("serial ports could not be enumerated");
        }
        return {};
    }
    const QByteArray json = QByteArray(raw);
    omegassh_free(raw);

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        g_enumerationError = perr.errorString();
        return {};
    }

    QVector<SerialPortInfo> out;
    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        SerialPortInfo p;
        p.name = o.value("name").toString();
        p.isUsb = o.value("is_usb").toBool();
        p.vid = o.value("vid").toString();
        p.pid = o.value("pid").toString();
        p.serialNumber = o.value("serial_number").toString();
        if (!p.name.isEmpty()) out.push_back(p);
    }
    return out;
}

QString OmegaSshSession::takeLibraryError() { return take(omegassh_last_error()); }

bool OmegaSshSession::start(const Config &config) { return start(config.toJson()); }

bool OmegaSshSession::start(const QJsonObject &config) {
    if (m_handle > 0) {
        m_error = tr("session is already running");
        emit errorOccurred(m_error);
        return false;
    }

    const QByteArray json = QJsonDocument(config).toJson(QJsonDocument::Compact);
    const long long handle = omegassh_open(json.constData());
    if (handle <= 0) {
        // A refusal here is a configuration mistake, decided before anything
        // was opened, and omegassh_last_error is the right place to read it:
        // this thread made the call and this thread failed it. A dial that
        // fails later does NOT come through here -- it arrives as an event.
        m_error = takeLibraryError();
        if (m_error.isEmpty()) m_error = tr("connection failed");
        setState(State::Failed);
        emit errorOccurred(m_error);
        return false;
    }

    const long long notify = omegassh_notify_handle(handle);
    if (notify < 0) {
        omegassh_close(handle);
        m_error = tr("session opened but exposed no notify handle");
        setState(State::Failed);
        emit errorOccurred(m_error);
        return false;
    }

    m_handle = handle;
    m_finishedEmitted = false;
    m_exitCode = -1;
    m_error.clear();
    m_transport = transportFromName(take(omegassh_transport(handle)));
    m_summary = take(omegassh_summary(handle));

    m_notifier = new QSocketNotifier(static_cast<qintptr>(notify),
                                     QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            &OmegaSshSession::onNotifyActivated);
    m_notifier->setEnabled(true);

    // Read the state rather than assuming Connecting. The library decides it
    // per transport and the transports differ honestly: SSH and telnet start
    // connecting, serial starts disconnected because opening the port is the
    // whole handshake and there is no phase in between to be in.
    setState(stateFromCode(omegassh_state(handle)));

    // The dial may already have moved, or even finished, between open and the
    // notifier being armed -- and an edge that fired before this point is not
    // repeated. Draining once here is what makes that harmless.
    drain();
    return true;
}

bool OmegaSshSession::running() const {
    return m_handle > 0 && omegassh_alive(m_handle) == 1;
}

QString OmegaSshSession::error() const { return m_error; }

State OmegaSshSession::state() const { return m_state; }

Transport OmegaSshSession::transport() const { return m_transport; }

QString OmegaSshSession::summary() const { return m_summary; }

int OmegaSshSession::exitCode() const { return m_exitCode; }

void OmegaSshSession::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void OmegaSshSession::write(const QByteArray &data) {
    if (m_handle <= 0 || data.isEmpty()) return;
    const int n = omegassh_write(m_handle, data.constData(), data.size());
    if (n < 0) {
        m_error = takeLibraryError();
        emit errorOccurred(m_error);
    }
}

void OmegaSshSession::resize(int cols, int rows) {
    if (m_handle <= 0 || cols <= 0 || rows <= 0) return;
    if (omegassh_resize(m_handle, cols, rows) != 0) {
        m_error = takeLibraryError();
        emit errorOccurred(m_error);
    }
}

void OmegaSshSession::onNotifyActivated() {
    drainNotify(m_handle > 0 ? omegassh_notify_handle(m_handle) : -1);
    drain();
}

// drainEvents replays the queued state changes in order. One wake can cover
// several, which is why it is a loop, and it runs before the byte drain so a
// failure state is published before the last bytes that preceded it -- an
// overlay that goes red should not do so after the output that explains why.
void OmegaSshSession::drainEvents() {
    if (m_handle <= 0) return;
    for (;;) {
        char *raw = omegassh_next_event(m_handle);
        if (!raw) break;
        const QByteArray json = QByteArray(raw);
        omegassh_free(raw);

        const QJsonObject o = QJsonDocument::fromJson(json).object();
        switch (o.value("kind").toInt()) {
            case OMEGASSH_EVENT_STATE_CHANGED:
                setState(stateFromCode(o.value("state").toInt()));
                break;
            case OMEGASSH_EVENT_ERROR: {
                const QString msg = o.value("error").toString();
                if (!msg.isEmpty()) {
                    m_error = msg;
                    emit errorOccurred(msg);
                }
                break;
            }
            case OMEGASSH_EVENT_INTERACTION_REQUIRED: {
                const QJsonObject ask = o.value("ask").toObject();
                emit interactionRequired(ask.value("question").toString(),
                                         ask.value("echo").toBool());
                break;
            }
            default:
                break;
        }
    }
}

// drain moves everything buffered in the library into dataReceived signals,
// then decides whether the session has ended. Order matters: bytes can still
// be buffered after the session ends, and dropping the last screenful of a
// command is exactly the bug nobody can reproduce.
void OmegaSshSession::drain() {
    if (m_handle <= 0) return;

    drainEvents();

    QByteArray chunk;
    chunk.resize(kReadChunk);
    for (;;) {
        const int n = omegassh_read(m_handle, chunk.data(), chunk.size());
        if (n <= 0) break;
        emit dataReceived(QByteArray(chunk.constData(), n));
    }

    if (omegassh_alive(m_handle) == 0 && omegassh_pending(m_handle) <= 0) {
        const bool wasRunning = !m_finishedEmitted;
        // Read the status and the final state before teardown retires the
        // handle: afterwards there is nothing to ask.
        m_exitCode = omegassh_exit_code(m_handle);
        const State last = stateFromCode(omegassh_state(m_handle));
        // A dial that failed reports its reason per-session, not per-thread,
        // because it failed on a thread this object has never run on. Normally
        // the error event above has already delivered it; this covers the case
        // where the session ended and was drained in one pass and nothing was
        // listening yet.
        if (last == State::Failed && m_error.isEmpty()) {
            m_error = take(omegassh_error(m_handle));
            if (!m_error.isEmpty()) emit errorOccurred(m_error);
        }
        teardown();
        setState(last);
        if (wasRunning) {
            m_finishedEmitted = true;
            emit finished(m_exitCode);
        }
    }
}

void OmegaSshSession::terminate() {
    if (m_handle <= 0) return;
    // Deliver whatever already arrived before tearing the session down.
    drain();
    if (m_handle <= 0) return;  // drain() may have finished it already

    m_exitCode = omegassh_exit_code(m_handle);
    teardown();
    setState(State::Disconnected);
    if (!m_finishedEmitted) {
        m_finishedEmitted = true;
        emit finished(m_exitCode);
    }
}

void OmegaSshSession::teardown() {
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_handle > 0) {
        omegassh_close(m_handle);
        m_handle = -1;
    }
}

}  // namespace omegassh
