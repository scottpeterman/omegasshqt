// app/capturewriter.h
//
// Session capture: received bytes written to a file with the escape sequences
// taken out, so the log reads as the text that was on screen rather than as
// the wire.
//
// The stripping is a byte-level state machine rather than nterm-qt's regex,
// and the difference matters for one reason: capture is fed a chunk at a time
// as data arrives, and a regex applied per chunk cannot see a sequence that
// straddles a read. A prompt redraw split across two packets -- and on a slow
// link they are, routinely -- lands in the log as visible garbage with the
// regex and as nothing with this. The state carries across write() calls,
// which is the whole point of it being a member.
//
// Working on bytes also keeps multi-byte UTF-8 intact. Running the same regex
// over a QString decoded from a partial chunk would corrupt any character
// unlucky enough to land on the boundary.
//
// What it strips: CSI (ESC [ ... final), OSC and the other string sequences
// (ESC ] / P / X / ^ / _, terminated by BEL or ST), and the short two-byte
// escapes. What it keeps: everything else, verbatim -- CR and LF included, so
// the file has the line structure the session had.

#ifndef OMEGA_APP_CAPTUREWRITER_H
#define OMEGA_APP_CAPTUREWRITER_H

#include <QByteArray>
#include <QFile>
#include <QString>

namespace omega::app {

class CaptureWriter {
public:
    CaptureWriter() = default;
    ~CaptureWriter();

    CaptureWriter(const CaptureWriter &) = delete;
    CaptureWriter &operator=(const CaptureWriter &) = delete;

    // Truncates. Returns false with error() set when the path cannot be
    // opened -- a read-only directory, a name the filesystem refuses.
    // Starting a capture closes any capture already running.
    bool start(const QString &path);
    void stop();

    bool isCapturing() const { return file_.isOpen(); }
    QString path() const { return path_; }
    QString error() const { return error_; }

    // Strips and appends. A no-op when no capture is running, so the caller
    // can wire it to the data signal once and leave it there.
    void write(const QByteArray &data);

    // The stripper on its own, for a caller that wants the text without a
    // file -- and for the probe, which is how the state machine is tested
    // without touching disk. Stateful: feed it the chunks in order.
    QByteArray strip(const QByteArray &data);

private:
    enum class State {
        Text,        // ordinary bytes
        Escape,      // saw ESC
        Csi,         // inside ESC [ ... final
        StringSeq,   // inside OSC/DCS/SOS/PM/APC, awaiting BEL or ST
        StringEsc,   // saw ESC inside a string sequence: ST if next is '\'
    };

    QFile file_;
    QString path_;
    QString error_;
    State state_ = State::Text;
};

}  // namespace omega::app

#endif  // OMEGA_APP_CAPTUREWRITER_H
