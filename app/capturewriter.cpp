// app/capturewriter.cpp

#include "app/capturewriter.h"

namespace omega::app {
namespace {

constexpr char kEsc = '\x1b';
constexpr char kBel = '\x07';

// CSI ends on a final byte in 0x40..0x7e. Everything before it -- parameter
// bytes 0x30..0x3f and intermediate bytes 0x20..0x2f -- is consumed.
bool isCsiFinal(unsigned char c) { return c >= 0x40 && c <= 0x7e; }

// ESC ] P X ^ _ all open a string sequence terminated by ST (or BEL, for OSC
// as xterm allows). They are grouped because the terminator is what matters
// here, not what the string means.
bool opensStringSequence(unsigned char c) {
    return c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_';
}

}  // namespace

CaptureWriter::~CaptureWriter() { stop(); }

bool CaptureWriter::start(const QString &path) {
    stop();
    error_.clear();

    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error_ = file_.errorString();
        file_.setFileName(QString());
        return false;
    }

    path_ = path;
    // A new file starts outside any sequence. Carrying state across a
    // stop/start would drop the head of the new log if the old one happened
    // to end mid-escape.
    state_ = State::Text;
    return true;
}

void CaptureWriter::stop() {
    if (file_.isOpen()) {
        file_.close();
    }
    path_.clear();
    state_ = State::Text;
}

void CaptureWriter::write(const QByteArray &data) {
    if (!file_.isOpen()) {
        return;
    }
    const QByteArray text = strip(data);
    if (!text.isEmpty()) {
        file_.write(text);
        // Flushed per chunk on purpose. A capture is normally being tailed or
        // read straight after something went wrong, and a buffered log that
        // is empty until the session ends is the wrong answer both times.
        file_.flush();
    }
}

QByteArray CaptureWriter::strip(const QByteArray &data) {
    QByteArray out;
    out.reserve(data.size());

    for (char raw : data) {
        const auto c = static_cast<unsigned char>(raw);

        switch (state_) {
            case State::Text:
                if (raw == kEsc) {
                    state_ = State::Escape;
                } else {
                    out.append(raw);
                }
                break;

            case State::Escape:
                if (raw == '[') {
                    state_ = State::Csi;
                } else if (opensStringSequence(c)) {
                    state_ = State::StringSeq;
                } else if (c >= 0x20 && c <= 0x2f) {
                    // An intermediate byte: still inside the escape, the
                    // final has not arrived. Stay put.
                } else {
                    // A two-byte escape -- ESC 7, ESC M, ESC =. Done.
                    state_ = State::Text;
                }
                break;

            case State::Csi:
                if (isCsiFinal(c)) {
                    state_ = State::Text;
                }
                break;

            case State::StringSeq:
                if (raw == kBel) {
                    state_ = State::Text;
                } else if (raw == kEsc) {
                    state_ = State::StringEsc;
                }
                break;

            case State::StringEsc:
                if (raw == '\\') {
                    state_ = State::Text;  // ST
                } else if (raw == kEsc) {
                    // Another ESC: still waiting for the backslash.
                } else {
                    state_ = State::StringSeq;
                }
                break;
        }
    }

    return out;
}

}  // namespace omega::app
