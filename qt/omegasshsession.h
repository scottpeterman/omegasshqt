// qt/omegasshsession.h
//
// Qt wrapper around the omegassh C API.
//
// The signal/slot surface deliberately matches qtpyte::PtySession name for
// name: dataReceived, write, resize, terminate, finished(int), running,
// error. A TerminalWidget wired to one can be wired to the other without
// knowing which it has, which is the whole point of both projects being
// narrow -- and the three connections are the same three, so
// omegassh::attach mirrors qtpyte::attach.
//
// Note which direction dataReady runs in qtpyte: it is the WIDGET's outbound
// signal, keystrokes heading for the transport. The transport's inbound
// signal is dataReceived. Naming this class's output dataReady would have
// read correctly in isolation and wired up backwards next to the widget.
//
// This class owns the notify handle via QSocketNotifier, so every byte is
// delivered on the thread that owns this object. Nothing from the library's
// internals ever reaches the caller's thread unannounced.
//
// The class is still called OmegaSshSession now that it also drives telnet
// and a serial console. The name is the library's, not the protocol's, and
// renaming it would churn every consumer -- the anytermqt integration header,
// the Qt examples -- for no behavioural change. Everything below that names a
// transport does so explicitly.

#ifndef OMEGASSH_QT_OMEGASSHSESSION_H
#define OMEGASSH_QT_OMEGASSHSESSION_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QMap>
#include <QString>
#include <QVector>

class QSocketNotifier;

namespace omegassh {

// Transport selects what the session runs over. It is chosen once, at start,
// and nothing afterwards depends on it: write, resize, terminate and the
// signals are the same for all three.
enum class Transport {
    Ssh,
    Telnet,
    Serial,
};

// HostKeyPolicy mirrors the JSON values the C API accepts. SSH only.
enum class HostKeyPolicy {
    Strict,    // known_hosts only; an unknown host is refused
    Tofu,      // accept and pin on first contact; a MISMATCH still refuses
    Insecure,  // no verification at all -- lab opt-in, never a default
};

// State mirrors omegassh_state_t. The six values are nterm-qt's, so a
// connection overlay renders what the transport publishes rather than mapping
// one vocabulary onto another.
//
// Not every transport reaches every state and none of them fake one.
// Authenticating is SSH only: telnet has no authentication step, and a login
// prompt on it is ordinary session data arriving after the socket is up.
// Connecting is SSH and telnet; opening a serial port is the whole handshake.
// Reconnecting belongs to a reconnect policy above the transport and is not
// yet published by anything.
enum class State {
    Disconnected = 0,
    Connecting = 1,
    Authenticating = 2,
    Connected = 3,
    Reconnecting = 4,
    Failed = 5,
};

// SerialPortInfo is one entry from serialPorts(). The USB fields are empty
// where the platform did not supply them, which is not an error -- a form
// showing device names with no vendor strings is still usable.
struct SerialPortInfo {
    QString name;  // COM3, /dev/ttyUSB0, /dev/cu.usbserial-XXXX
    bool isUsb = false;
    QString vid;
    QString pid;
    QString serialNumber;

    // "usbserial-A50285BI (0403:6001)" or just the device name. What a
    // quick-connect combo box should show: two FTDI adapters in one laptop
    // are otherwise told apart only by which /dev node the kernel handed out.
    QString displayName() const;
};

// SessionLog describes optional session logging, tapped at the byte-stream
// boundary below the emulator, so it covers every transport identically.
struct SessionLog {
    bool enabled = false;

    // path names an exact file; setting it implies enabled.
    QString path;

    // dir is where a generated, timestamped name lands. Empty means
    // ~/.omega/logs -- a user directory, never the launch directory.
    QString dir;

    // includeInput also records what the operator types.
    //
    // Off by default and deliberately: a device's own login prompt is
    // ordinary session data on telnet and on a serial console, so the
    // password answering it is ordinary keystrokes, and this writes it to a
    // plaintext file. Useful for reproducing a config change; not something
    // to get by accident.
    bool includeInput = false;

    bool append = false;
};

// Config is a typed front end for the JSON the C API takes. Which fields
// matter depends on transport; the rest are omitted from the JSON rather than
// sent and ignored.
struct Config {
    Transport transport = Transport::Ssh;

    // --- ssh and telnet -------------------------------------------------
    QString host;
    int port = 0;  // 0 => 22 for ssh, 23 for telnet

    // --- ssh ------------------------------------------------------------
    QString username;
    QString password;
    QString privateKeyPath;
    QString keyPassphrase;
    bool useAgent = false;

    // Names a vault entry whose material fills whichever of the fields above
    // were left blank. The name goes down and the secret is read on the Go
    // side during the dial; nothing comes back up. Requires vaultHandle.
    QString credential;
    long long vaultHandle = 0;

    // Answers to keyboard-interactive questions, keyed by the exact question
    // the server sent. Filled on a SECOND dial, after a first one failed with
    // the keyboard-prompt marker in its error and the question was put to a
    // person -- the library cannot call back up here to ask mid-handshake.
    //
    // Per dial, never reused: a one-time password is spent by the attempt
    // that submits it.
    QMap<QString, QString> keyboardAnswers;

    // Bastion. The remaining jump fields are ignored when jumpHost is empty.
    QString jumpHost;
    int jumpPort = 22;
    QString jumpUsername;
    QString jumpPassword;
    QString jumpKeyPath;
    QString jumpCredential;

    HostKeyPolicy hostKeyPolicy = HostKeyPolicy::Strict;
    QString knownHostsPath;  // empty => ~/.ssh/known_hosts

    // Appends the old KEX/cipher/MAC/host-key tail that aging network gear
    // still requires. Off by default; harmless against a modern server.
    bool legacyAlgorithms = false;

    // --- telnet ---------------------------------------------------------

    // Expands a lone CR on write to CR LF, which is what RFC 854 makes the
    // telnet newline and what console servers expect. On by default; turn it
    // off only for a device that echoes a doubled newline.
    bool telnetCrlf = true;

    // --- serial ---------------------------------------------------------
    QString serialPort;              // required for Transport::Serial
    int baud = 9600;
    int dataBits = 8;
    QString parity = QStringLiteral("none");   // none|odd|even|mark|space
    QString stopBits = QStringLiteral("1");    // 1|1.5|2

    // --- shared ---------------------------------------------------------

    // Declared to the far end: the pty request on ssh, the answer to a TTYPE
    // subnegotiation on telnet. Serial has nowhere to declare it.
    QString term = QStringLiteral("xterm-256color");
    int cols = 80;
    int rows = 24;
    int timeoutSeconds = 30;

    SessionLog log;

    QJsonObject toJson() const;
};

class OmegaSshSession : public QObject {
    Q_OBJECT

public:
    explicit OmegaSshSession(QObject *parent = nullptr);
    ~OmegaSshSession() override;

    OmegaSshSession(const OmegaSshSession &) = delete;
    OmegaSshSession &operator=(const OmegaSshSession &) = delete;

    // Starts a session. Returns false and emits errorOccurred when the
    // CONFIGURATION is wrong -- an unknown transport, a credential that names
    // nothing, an impossible parity -- in which case error() holds the reason
    // and nothing was opened.
    //
    // Returning true does NOT mean connected. It means the dial has started.
    // Watch stateChanged: Connecting -> Authenticating -> Connected, or
    // -> Failed, with the reason in errorOccurred and error(). A failed dial
    // also emits finished(-1), so a tab has one place to learn it is over
    // whether the session died at second one or an hour in.
    //
    // It does not block on the far end, so it is safe on the GUI thread --
    // which is the point: the connection overlay cannot render a dial that is
    // holding the thread that would paint it.
    bool start(const Config &config);

    // Overload for callers assembling the JSON themselves, or using a field
    // this wrapper has not caught up with yet.
    bool start(const QJsonObject &config);

    // Named running() and error() to match qtpyte::PtySession rather than
    // Qt's isRunning()/errorString() habits: the two classes are meant to be
    // read side by side.
    //
    // running() is true from a successful start() until the session ends,
    // which includes the time it spends connecting -- the same sense in which
    // a PtySession whose child has been spawned is running. Use state() to
    // ask whether it has actually reached the far end.
    bool running() const;
    QString error() const;

    // The session's lifecycle position, and the thing to render rather than
    // inferring one from running(). Disconnected before start and after the
    // session ends; Connecting or Authenticating while the dial is in flight;
    // Failed if it did not get there.
    State state() const;

    // Which transport this session is running over, and the target in short
    // human-readable form -- "10.0.0.1:23", "/dev/ttyUSB0 9600 8N1" -- for a
    // tab title. Both are empty-ish before start, and both are populated the
    // moment start() returns rather than when the dial lands: a tab is titled
    // while it is still connecting.
    Transport transport() const;
    QString summary() const;

    // The remote shell's exit status once it has ended, normalized the way
    // PtySession normalizes a local child: the program's own status, or
    // 128 + signal number. -1 while running, or if none was reported.
    //
    // Always -1 for telnet and serial: neither protocol carries an exit
    // status, and reporting 0 would be indistinguishable from a clean exit.
    int exitCode() const;

    // The serial ports the OS can see. Empty is a valid answer and means no
    // adapter, not a failure; check lastEnumerationError() to tell them apart.
    //
    // Static because it needs no session: the quick-connect form populates
    // its port list before anything is open. It comes from the library rather
    // than from QSerialPort so that neither the build nor windeployqt gains a
    // Qt module for the sake of a list of strings.
    static QVector<SerialPortInfo> serialPorts();
    static QString lastEnumerationError();

    // Library version, for logs and about boxes.
    static QString version();

public slots:
    // Sends keyboard input to the far end.
    void write(const QByteArray &data);

    // Tells the far end its new geometry. Call this from the terminal
    // widget's resize handler, not on every paint.
    //
    // A serial console has no window-change concept, so this succeeds and
    // does nothing there -- the caller does not branch on which transport it
    // holds.
    void resize(int cols, int rows);

    // Ends the session. Safe to call more than once, safe on a session that
    // never started, and safe on one that is still dialing -- it returns
    // straight away rather than waiting for the connect to give up.
    // finished() is emitted once, whether the session ended on its own, was
    // closed here, or never connected at all.
    void terminate();

    // Spelling kept for callers that reach for close(); identical behaviour.
    void close() { terminate(); }

signals:
    // Output from the far end, stderr included on ssh. Connect this to
    // TerminalWidget::feed.
    void dataReceived(const QByteArray &data);

    // The session moved. This is what a connection overlay listens to; it
    // renders the state rather than deciding one of its own.
    void stateChanged(omegassh::State state);

    // The session has ended and every remaining byte has been delivered.
    // exitCode is normalized as described on exitCode() above.
    void finished(int exitCode);

    // A connect or I/O failure. The session is not usable afterwards.
    void errorOccurred(const QString &message);

    // The far end put a challenge to the operator -- a keyboard-interactive
    // prompt, an MFA code, a security-key touch. echo false means the answer
    // is a secret and must not be shown as it is typed.
    //
    // Declared and not yet emitted. The dial is asynchronous now, so it can
    // park in State::Authenticating and publish a question; what is left is
    // the answer travelling back, which is one more call in the C surface.
    // The signal exists so the connect dialog can be written against the final
    // shape rather than retrofitted.
    void interactionRequired(const QString &question, bool echo);

private slots:
    void onNotifyActivated();

private:
    void drain();
    void drainEvents();
    void teardown();
    void setState(State s);
    QString takeLibraryError();

    long long m_handle = -1;
    QSocketNotifier *m_notifier = nullptr;
    QString m_error;
    QString m_summary;
    Transport m_transport = Transport::Ssh;
    State m_state = State::Disconnected;
    int m_exitCode = -1;
    bool m_finishedEmitted = false;
};

}  // namespace omegassh

// stateChanged carries a plain enum, and start() is documented as something to
// call from a worker thread -- so the connection that delivers its result back
// to the GUI is a queued one, and a queued connection needs the type
// registered or the signal is dropped at runtime with a warning rather than a
// compile error.
Q_DECLARE_METATYPE(omegassh::State)
Q_DECLARE_METATYPE(omegassh::Transport)

#endif  // OMEGASSH_QT_OMEGASSHSESSION_H
