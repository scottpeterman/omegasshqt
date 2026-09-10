// sessions/ttyaml.h
//
// The TerminalTelemetry sessions.yaml format: read and written, no Qt.
//
// The shape, which is all of it:
//
//     - folder_name: Core
//       sessions:
//         - display_name: lab-core-1
//           host: 10.10.0.1
//           port: 22
//           DeviceType: switch
//           Model: C9300
//           Vendor: cisco
//
// A sequence of folders at the top level; each holds a name and a sequence of
// devices; every device value is a scalar. There is no nesting below that and
// no other node type in the format.
//
// THIS IS NOT A YAML PARSER. It reads the subset above and REFUSES anything
// else, with the line number and the reason. That is the deliberate choice
// here: the alternative is a real YAML library, which for one narrow
// interchange file would be a dependency larger than the session store it
// feeds, in a tree whose only third-party C is a SQLite amalgamation. The
// danger of a hand-written subset reader is not that it rejects too much --
// that is visible and fixable -- but that it silently mis-reads something it
// should have rejected, so anything outside the subset is an error rather
// than a guess. Anchors, aliases, flow collections, block scalars, multiple
// documents and tab indentation are all named refusals.
//
// The writer emits the same subset and quotes conservatively, so its output
// parses both here and in PyYAML. tests/compat/ttyaml_differential.py checks
// that second claim against PyYAML itself rather than asserting it.

#ifndef OMEGA_SESSIONS_TTYAML_H
#define OMEGA_SESSIONS_TTYAML_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace omega::sessions {

// A device entry. Fields are kept in file order and as written, including keys
// this application has no use for: an import that drops them would make a
// round trip through Omega a way to lose data the other tool relies on.
struct TtDevice {
    std::vector<std::pair<std::string, std::string>> fields;

    const std::string *find(const std::string &key) const;
    void set(const std::string &key, const std::string &value);
};

struct TtFolder {
    std::string name;
    std::vector<TtDevice> devices;
};

struct TtDocument {
    std::vector<TtFolder> folders;
};

// Carries the line number because the whole point of refusing is that the
// person can go and look. Lines are 1-based.
struct TtError {
    bool ok = true;
    int line = 0;
    std::string message;

    explicit operator bool() const { return ok; }
    static TtError good() { return {}; }
    static TtError fail(int line, std::string m) {
        return {false, line, std::move(m)};
    }
};

// Parses the subset. On failure the document is left empty and the error says
// which line stopped it.
TtError ttParse(const std::string &text, TtDocument *out);

// Emits the subset. Round-trips ttParse for any document ttParse produced.
std::string ttEmit(const TtDocument &doc);

}  // namespace omega::sessions

#endif  // OMEGA_SESSIONS_TTYAML_H
