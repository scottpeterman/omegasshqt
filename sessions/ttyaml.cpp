// sessions/ttyaml.cpp

#include "ttyaml.h"

#include <algorithm>
#include <cctype>

namespace omega::sessions {
namespace {

struct Line {
    int number = 0;      // 1-based, for errors
    int indent = 0;      // leading spaces
    bool dash = false;   // starts a sequence entry
    std::string text;    // content after indent and any "- "
};

bool isBlankOrComment(const std::string &raw) {
    for (const char c : raw) {
        if (c == ' ') continue;
        return c == '#' || c == '\r';
    }
    return true;
}

// Splits "key: value" once. A colon inside a quoted key is not something this
// format produces, so the first colon wins and a key that needs quoting is a
// refusal rather than a guess.
bool splitKey(const std::string &text, std::string *key, std::string *value) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return false;

    *key = text.substr(0, colon);
    // Trim the key.
    while (!key->empty() && key->back() == ' ') key->pop_back();

    std::string rest = text.substr(colon + 1);
    size_t start = 0;
    while (start < rest.size() && rest[start] == ' ') ++start;
    *value = rest.substr(start);
    while (!value->empty() &&
           (value->back() == ' ' || value->back() == '\r'))
        value->pop_back();
    return !key->empty();
}

// Unquotes a scalar and strips a trailing comment from a plain one, which is
// what YAML does. Returns false when the scalar is outside the subset.
bool scalar(const std::string &raw, std::string *out, std::string *why) {
    if (raw.empty()) {
        out->clear();
        return true;
    }

    const char first = raw.front();

    if (first == '&' || first == '*') {
        *why = "anchors and aliases are not supported";
        return false;
    }
    if (first == '|' || first == '>') {
        *why = "block scalars are not supported";
        return false;
    }
    if (first == '[' || first == '{') {
        *why = "flow collections are not supported";
        return false;
    }

    if (first == '\'' || first == '"') {
        const char quote = first;
        std::string value;
        size_t i = 1;
        bool closed = false;
        for (; i < raw.size(); ++i) {
            const char c = raw[i];
            if (quote == '\'' ) {
                // '' is an escaped single quote; a lone one ends the scalar.
                if (c == '\'') {
                    if (i + 1 < raw.size() && raw[i + 1] == '\'') {
                        value.push_back('\'');
                        ++i;
                        continue;
                    }
                    closed = true;
                    ++i;
                    break;
                }
                value.push_back(c);
            } else {
                if (c == '\\' && i + 1 < raw.size()) {
                    const char next = raw[i + 1];
                    switch (next) {
                        case 'n': value.push_back('\n'); break;
                        case 't': value.push_back('\t'); break;
                        case '"': value.push_back('"'); break;
                        case '\\': value.push_back('\\'); break;
                        default:
                            *why = "unsupported escape in a double-quoted scalar";
                            return false;
                    }
                    ++i;
                    continue;
                }
                if (c == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                value.push_back(c);
            }
        }
        if (!closed) {
            *why = "unterminated quoted scalar";
            return false;
        }
        // Only a comment may follow a closing quote.
        while (i < raw.size() && raw[i] == ' ') ++i;
        if (i < raw.size() && raw[i] != '#') {
            *why = "unexpected text after a quoted scalar";
            return false;
        }
        *out = value;
        return true;
    }

    // Plain scalar. A " #" begins a comment; a bare '#' does not.
    std::string value = raw;
    const size_t hash = value.find(" #");
    if (hash != std::string::npos) value = value.substr(0, hash);
    while (!value.empty() && value.back() == ' ') value.pop_back();

    // The null spellings all mean an absent value here, which for a format of
    // strings is the empty string.
    if (value == "null" || value == "~" || value == "Null" || value == "NULL")
        value.clear();

    *out = value;
    return true;
}

bool needsQuoting(const std::string &s) {
    if (s.empty()) return true;
    if (s.front() == ' ' || s.back() == ' ') return true;
    // A leading indicator, or anything that could read as structure or as a
    // non-string scalar once it is parsed back.
    static const std::string indicators = "-?:,[]{}#&*!|>'\"%@`";
    if (indicators.find(s.front()) != std::string::npos) return true;
    if (s.find(": ") != std::string::npos) return true;
    if (s.find(" #") != std::string::npos) return true;
    if (s.find('\n') != std::string::npos) return true;
    if (s.find('\t') != std::string::npos) return true;
    if (!s.empty() && s.back() == ':') return true;

    // Values PyYAML would resolve to a bool, null or number rather than a
    // string. Quoting them keeps a port of "22" a string on the way back in,
    // and keeps a model named "NO" from becoming false.
    static const char *const specials[] = {
        "true", "false", "yes", "no", "on", "off", "null", "~", "y", "n"};
    std::string lower;
    lower.reserve(s.size());
    for (const char c : s)
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    for (const char *sp : specials)
        if (lower == sp) return true;

    bool numeric = true;
    bool anyDigit = false;
    for (const char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            anyDigit = true;
            continue;
        }
        if (c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') continue;
        numeric = false;
        break;
    }
    if (numeric && anyDigit) return true;

    return false;
}

std::string quote(const std::string &s) {
    // Single quotes, with '' for an embedded one. No escape sequences to get
    // wrong, and it is what PyYAML emits for the same strings.
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string emitScalar(const std::string &s) {
    return needsQuoting(s) ? quote(s) : s;
}

}  // namespace

const std::string *TtDevice::find(const std::string &key) const {
    for (const auto &[k, v] : fields)
        if (k == key) return &v;
    return nullptr;
}

void TtDevice::set(const std::string &key, const std::string &value) {
    for (auto &[k, v] : fields) {
        if (k == key) {
            v = value;
            return;
        }
    }
    fields.emplace_back(key, value);
}

TtError ttParse(const std::string &text, TtDocument *out) {
    out->folders.clear();

    // --- tokenise into indent / dash / content -----------------------------
    std::vector<Line> lines;
    {
        int number = 0;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string raw = text.substr(pos, end - pos);
            ++number;

            if (!isBlankOrComment(raw)) {
                size_t i = 0;
                while (i < raw.size() && raw[i] == ' ') ++i;
                if (i < raw.size() && raw[i] == '\t')
                    return TtError::fail(
                        number, "tab indentation is not supported; use spaces");

                std::string content = raw.substr(i);
                while (!content.empty() && content.back() == '\r')
                    content.pop_back();

                if (content == "---" || content == "...") {
                    // A single document is fine; a second one is not, and
                    // silently reading only the first would lose the rest.
                    if (!lines.empty())
                        return TtError::fail(
                            number, "multiple YAML documents are not supported");
                    pos = end + 1;
                    if (end == text.size()) break;
                    continue;
                }

                Line line;
                line.number = number;
                line.indent = static_cast<int>(i);
                if (content.rfind("- ", 0) == 0) {
                    line.dash = true;
                    line.text = content.substr(2);
                    while (!line.text.empty() && line.text.front() == ' ')
                        line.text.erase(line.text.begin());
                } else if (content == "-") {
                    line.dash = true;
                } else {
                    line.text = content;
                }
                lines.push_back(std::move(line));
            }

            if (end == text.size()) break;
            pos = end + 1;
        }
    }

    if (lines.empty()) return TtError::good();

    // --- walk it -----------------------------------------------------------
    const int topIndent = lines.front().indent;
    size_t i = 0;

    if (!lines.front().dash)
        return TtError::fail(lines.front().number,
                             "expected a sequence of folders at the top level "
                             "(a line beginning with \"- \")");

    while (i < lines.size()) {
        const Line &head = lines[i];
        if (head.indent != topIndent || !head.dash)
            return TtError::fail(head.number,
                                 "expected a folder entry at the top level");

        TtFolder folder;
        // Keys of this folder sit at the column the dash's content starts in.
        const int keyIndent = head.indent + 2;
        bool sawSessions = false;

        // The dash line itself carries the first key.
        std::vector<const Line *> folderKeys;
        if (!head.text.empty()) folderKeys.push_back(&head);
        ++i;
        while (i < lines.size() && !lines[i].dash &&
               lines[i].indent == keyIndent) {
            folderKeys.push_back(&lines[i]);
            ++i;
        }

        for (const Line *line : folderKeys) {
            std::string key, rawValue;
            if (!splitKey(line->text, &key, &rawValue))
                return TtError::fail(line->number,
                                     "expected \"key: value\" in a folder entry");

            if (key == "sessions") {
                if (!rawValue.empty() && rawValue != "[]")
                    return TtError::fail(
                        line->number,
                        "\"sessions\" must be a block sequence or empty");
                sawSessions = true;
                continue;
            }

            std::string value, why;
            if (!scalar(rawValue, &value, &why))
                return TtError::fail(line->number, why);
            if (key == "folder_name") folder.name = value;
            // Other folder-level keys are not part of the format; keeping the
            // name is all a folder needs and inventing storage for the rest
            // would be storage nothing reads.
        }

        // Devices: entries deeper than the folder's keys.
        if (sawSessions) {
            while (i < lines.size() && lines[i].indent > keyIndent &&
                   lines[i].dash) {
                const int deviceIndent = lines[i].indent;
                const int deviceKeyIndent = deviceIndent + 2;

                TtDevice device;
                std::vector<const Line *> deviceKeys;
                if (!lines[i].text.empty()) deviceKeys.push_back(&lines[i]);
                const int startLine = lines[i].number;
                ++i;
                while (i < lines.size() && !lines[i].dash &&
                       lines[i].indent == deviceKeyIndent) {
                    deviceKeys.push_back(&lines[i]);
                    ++i;
                }

                if (deviceKeys.empty())
                    return TtError::fail(startLine, "empty session entry");

                for (const Line *line : deviceKeys) {
                    std::string key, rawValue;
                    if (!splitKey(line->text, &key, &rawValue))
                        return TtError::fail(
                            line->number,
                            "expected \"key: value\" in a session entry");

                    std::string value, why;
                    if (!scalar(rawValue, &value, &why))
                        return TtError::fail(line->number, why);
                    device.set(key, value);
                }
                folder.devices.push_back(std::move(device));
            }
        }

        out->folders.push_back(std::move(folder));
    }

    return TtError::good();
}

std::string ttEmit(const TtDocument &doc) {
    std::string out;
    for (const TtFolder &folder : doc.folders) {
        out += "- folder_name: " + emitScalar(folder.name) + "\n";
        out += "  sessions:\n";
        for (const TtDevice &device : folder.devices) {
            bool first = true;
            for (const auto &[key, value] : device.fields) {
                out += first ? "    - " : "      ";
                out += key + ": " + emitScalar(value) + "\n";
                first = false;
            }
            // A device with no fields would emit a bare "- ", which parses back
            // as an empty entry and is refused on the way in. Nothing produces
            // one, and skipping it is better than writing a file this reader
            // would reject.
        }
    }
    return out;
}

}  // namespace omega::sessions
