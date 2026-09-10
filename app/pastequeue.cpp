// app/pastequeue.cpp

#include "app/pastequeue.h"

namespace omega::app {
namespace {

// 8N1: one start bit and one stop bit either side of the byte.
constexpr int kBitsPerChar = 10;

}  // namespace

int pasteDelayMs(int chars, int baud) {
    if (baud <= 0 || chars <= 0) {
        return 0;
    }
    // Rounded up, so a short line at a high rate still yields a whole
    // millisecond rather than a timer that fires immediately and paces
    // nothing.
    const long long bits = static_cast<long long>(chars) * kBitsPerChar;
    return static_cast<int>((bits * 1000 + baud - 1) / baud);
}

double pasteDurationSeconds(int chars, int baud) {
    if (baud <= 0 || chars <= 0) {
        return 0.0;
    }
    return static_cast<double>(chars) * kBitsPerChar / baud;
}

void PasteQueue::load(const QString &text) {
    clear();
    totalChars_ = static_cast<int>(text.size());
    if (text.isEmpty()) {
        return;
    }

    // Walk rather than split(): split() drops the terminators, and they have
    // to stay with the line they end. Trailing text with no newline is a
    // chunk of its own, which is what makes a paste with no final newline
    // behave the way typing it would.
    int start = 0;
    while (start < text.size()) {
        int end = text.indexOf(QLatin1Char('\n'), start);
        end = (end < 0) ? text.size() : end + 1;  // keep the newline

        // Long lines go out in pieces. The last piece keeps the terminator,
        // so the gap that matters still lands after it.
        while (end - start > kMaxChunkChars) {
            chunks_.append(text.mid(start, kMaxChunkChars));
            start += kMaxChunkChars;
        }
        chunks_.append(text.mid(start, end - start));
        start = end;
    }
}

QString PasteQueue::next() {
    if (!hasNext()) {
        return QString();
    }
    return chunks_.at(index_++);
}

int PasteQueue::remainingChars() const {
    int count = 0;
    for (int i = index_; i < chunks_.size(); ++i) {
        count += static_cast<int>(chunks_.at(i).size());
    }
    return count;
}

void PasteQueue::clear() {
    chunks_.clear();
    index_ = 0;
    totalChars_ = 0;
}

}  // namespace omega::app
