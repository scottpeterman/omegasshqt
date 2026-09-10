// app/pastequeue.h
//
// The chunking half of a rate-limited paste. No timer and no Qt event loop:
// it holds the pending text and hands out one chunk at a time, so the
// splitting is testable without waiting for anything.
//
// WHY THIS EXISTS. A console server, or a switch reached over a 9600 baud
// console cable, has no flow control worth the name. Paste forty lines of
// configuration into one and some of them arrive with characters missing --
// not an error anywhere, just a line that silently became a different
// command. The answer every terminal that talks to this gear has settled on
// is the same: send it slowly, a line at a time, and give the far end the
// gap it needs to echo and process.
//
// A chunk is one line INCLUDING its terminator, because the line is the unit
// the far end processes: the gap belongs after the newline, where the device
// is parsing what it just received. Splitting mid-line would put the delay
// somewhere the device is not doing any work.
//
// A very long line is split anyway, at kMaxChunkChars. A single 8 KB line
// pasted at 9600 baud overruns an input buffer whatever the line delay is,
// and the cap costs nothing on the normal case where no line is near it.

#ifndef OMEGA_APP_PASTEQUEUE_H
#define OMEGA_APP_PASTEQUEUE_H

#include <QString>
#include <QStringList>

namespace omega::app {

// The pause owed after sending `chars` characters if the paste is to be paced
// at `baud`, in milliseconds. 0 baud means unlimited and returns 0.
//
// Ten bits per character, not eight: 8N1 spends a start bit and a stop bit on
// every byte, so 9600 baud carries 960 characters a second and not 1200.
// Getting this wrong by 25% is the difference between pacing to a console and
// overrunning it slowly.
//
// Pacing by character rather than by a flat per-line delay is the point of
// expressing this in baud at all. A 4-character line and an 80-character line
// do not cost the far end the same, and a flat delay is either too slow for
// the first or too fast for the second.
int pasteDelayMs(int chars, int baud);

// What a paste of `chars` characters will take at `baud`, in seconds. For
// telling someone what they are about to start.
double pasteDurationSeconds(int chars, int baud);

class PasteQueue {
public:
    // Above this, a single line is sent in pieces. Chosen to sit under the
    // input buffer of the gear this exists for, not tuned to any one device.
    static constexpr int kMaxChunkChars = 256;

    // Replaces whatever was pending. Splitting happens here, once.
    void load(const QString &text);

    bool hasNext() const { return index_ < chunks_.size(); }

    // The next chunk, or an empty string when there is none. Advances.
    QString next();

    int sent() const { return index_; }
    int total() const { return static_cast<int>(chunks_.size()); }

    void clear();

    // Characters still to send, for a progress or duration estimate.
    int remainingChars() const;

    // Every character loaded, sent or not.
    int totalChars() const { return totalChars_; }

private:
    QStringList chunks_;
    int index_ = 0;
    int totalChars_ = 0;
};

}  // namespace omega::app

#endif  // OMEGA_APP_PASTEQUEUE_H
