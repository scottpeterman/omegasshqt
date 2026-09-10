// tests/compat/ttyaml_probe.cpp
//
// The TerminalTelemetry YAML reader, writer, importer and exporter, with no
// Qt and no display.
//
//   ttyaml_probe [db-path]
//
// Two kinds of check here. The parser ones are about the subset: what it
// accepts, what it refuses, and that a refusal names the right line. The
// import ones are about the rules ported from import_terminal_telemetry --
// duplicates by hostname, description built from DeviceType and Model, extras
// carrying three keys, empty folders creating nothing.
//
// tests/compat/ttyaml_differential.py asks the harder question: whether this
// reader and PyYAML agree about the same bytes. That one needs Python; this
// one runs anywhere the store builds.

#include "sessionio.h"
#include "store.h"
#include "ttyaml.h"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <optional>
#include <string>

using namespace omega::sessions;

namespace {

int failures = 0;

void ok(bool condition, const char *what, const std::string &detail = {}) {
    std::printf("  %s  %-48s %s\n", condition ? "ok  " : "FAIL", what,
                detail.c_str());
    if (!condition) ++failures;
}

void heading(const char *text) { std::printf("\n%s\n", text); }

TtDocument parseOk(const std::string &text, const char *what) {
    TtDocument doc;
    const TtError err = ttParse(text, &doc);
    ok(static_cast<bool>(err), what, err ? "" : err.message);
    return doc;
}

void refuses(const std::string &text, int expectLine, const char *what) {
    TtDocument doc;
    const TtError err = ttParse(text, &doc);
    if (err) {
        ok(false, what, "accepted, but should have refused");
        return;
    }
    ok(err.line == expectLine, what,
       "line " + std::to_string(err.line) + ": " + err.message);
}

const char *kSample = R"(# lab devices
- folder_name: Core
  sessions:
    - display_name: lab-core-1
      host: 10.10.0.1
      port: 22
      DeviceType: switch
      Model: C9300
      Vendor: cisco
    - display_name: lab-core-2
      host: 10.10.0.2
      DeviceType: switch
- folder_name: 'Edge / WAN'
  sessions:
    - display_name: "lab-edge-1"
      host: 10.10.1.1
      port: '2222'
)";

}  // namespace

// Canonical dumps, for tests/compat/ttyaml_differential.py to diff against
// the same thing produced by PyYAML and by import_terminal_telemetry. One
// line per field, tab separated, so a disagreement shows up as a diff on the
// line that disagrees rather than as two blobs.
namespace {

int dumpParse(const std::string &path) {
    std::string text;
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 2;
        }
        char buffer[4096];
        size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
            text.append(buffer, n);
        std::fclose(f);
    }

    TtDocument doc;
    const TtError err = ttParse(text, &doc);
    if (!err) {
        std::printf("ERROR\t%d\n", err.line);
        return 0;
    }
    for (const TtFolder &folder : doc.folders) {
        std::printf("folder\t%s\n", folder.name.c_str());
        for (const TtDevice &device : folder.devices) {
            std::printf("device\n");
            for (const auto &[key, value] : device.fields)
                std::printf("field\t%s\t%s\n", key.c_str(), value.c_str());
        }
    }
    return 0;
}

int dumpImport(const std::string &dbPath, const std::string &yamlPath,
               const std::string &mode) {
    std::string text;
    {
        FILE *f = std::fopen(yamlPath.c_str(), "rb");
        if (!f) return 2;
        char buffer[4096];
        size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
            text.append(buffer, n);
        std::fclose(f);
    }

    TtDocument doc;
    const TtError err = ttParse(text, &doc);
    if (!err) {
        std::printf("ERROR\t%d\n", err.line);
        return 0;
    }

    auto store = SessionStore::open(dbPath, nullptr);
    if (!store) return 2;

    const ImportResult r = applyImport(
        *store, doc, mode == "skip" ? ImportMode::Skip : ImportMode::Merge);
    std::printf("folders_created\t%d\n", r.foldersCreated);
    std::printf("sessions_imported\t%d\n", r.sessionsImported);
    std::printf("sessions_skipped\t%d\n", r.sessionsSkipped);

    for (const Folder &f : store->tree().folders)
        std::printf("folder\t%s\t%lld\n", f.name.c_str(),
                    static_cast<long long>(f.parent_id.value_or(-1)));

    // Ordered by hostname so the two implementations' insertion order cannot
    // be mistaken for a disagreement about content.
    std::vector<Session> sessions = store->listAllSessions();
    std::sort(sessions.begin(), sessions.end(),
              [](const Session &a, const Session &b) {
                  return a.hostname < b.hostname;
              });
    for (const Session &s : sessions)
        std::printf("session\t%s\t%s\t%s\t%d\t%s\t%s\t%s\n",
                    s.hostname.c_str(), s.name.c_str(), s.description.c_str(),
                    s.port,
                    extrasGet(s.extras, "vendor").c_str(),
                    extrasGet(s.extras, "device_type").c_str(),
                    extrasGet(s.extras, "model").c_str());
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 2 && std::string(argv[1]) == "parse")
        return dumpParse(argv[2]);
    if (argc > 4 && std::string(argv[1]) == "import")
        return dumpImport(argv[2], argv[3], argv[4]);

    const std::string dbPath =
        argc > 1 ? argv[1] : std::string("/tmp/omega-ttyaml-probe.db");
    std::remove(dbPath.c_str());

    // --- parsing ----------------------------------------------------------
    heading("parsing");

    TtDocument doc = parseOk(kSample, "the sample parses");
    ok(doc.folders.size() == 2, "two folders",
       std::to_string(doc.folders.size()));
    ok(doc.folders[0].name == "Core", "plain folder name");
    ok(doc.folders[1].name == "Edge / WAN",
       "single-quoted name keeps its spaces and slash");
    ok(doc.folders[0].devices.size() == 2, "two devices in the first folder");
    ok(doc.folders[1].devices[0].find("display_name") &&
           *doc.folders[1].devices[0].find("display_name") == "lab-edge-1",
       "double-quoted scalar");

    {
        const TtDevice &d = doc.folders[0].devices[0];
        ok(d.find("host") && *d.find("host") == "10.10.0.1", "host");
        ok(d.find("Vendor") && *d.find("Vendor") == "cisco", "vendor");
        ok(d.fields.size() == 6, "every key is kept, in file order",
           std::to_string(d.fields.size()));
        ok(d.fields.front().first == "display_name", "including the one on the dash line");
    }

    // A comment after a plain scalar is a comment; a '#' inside one is not.
    doc = parseOk("- folder_name: Lab  # trailing\n"
                  "  sessions:\n"
                  "    - host: 10.0.0.1\n"
                  "      Model: C9300#R\n",
                  "comments and embedded hashes");
    ok(doc.folders[0].name == "Lab", "trailing comment stripped",
       doc.folders.empty() ? "" : doc.folders[0].name);
    ok(doc.folders[0].devices[0].find("Model") &&
           *doc.folders[0].devices[0].find("Model") == "C9300#R",
       "a hash with no leading space stays in the value");

    // --- refusals ---------------------------------------------------------
    heading("refusals, with the line that caused them");

    refuses("folder_name: Core\n", 1, "a mapping at the top level");
    refuses("- folder_name: Core\n  sessions:\n    - host: &anchor x\n", 3,
            "an anchor");
    refuses("- folder_name: Core\n  sessions:\n    - host: |\n        text\n", 3,
            "a block scalar");
    refuses("- folder_name: Core\n  sessions:\n    - host: [a, b]\n", 3,
            "a flow sequence");
    refuses("- folder_name: 'unterminated\n", 1, "an unterminated quote");
    refuses("- folder_name: Core\n\tsessions:\n", 2, "tab indentation");
    refuses("- folder_name: A\n---\n- folder_name: B\n", 2,
            "a second document");

    // --- emitting ---------------------------------------------------------
    heading("emitting");

    doc = parseOk(kSample, "reparse for the round trip");
    const std::string emitted = ttEmit(doc);
    TtDocument again;
    const TtError err = ttParse(emitted, &again);
    ok(static_cast<bool>(err), "emitted text parses", err ? "" : err.message);
    ok(again.folders.size() == doc.folders.size(), "same folder count");
    ok(again.folders[1].name == "Edge / WAN", "a name needing quotes survives");
    ok(again.folders[0].devices[0].fields ==
           doc.folders[0].devices[0].fields,
       "every field survives the round trip");

    {
        // A port must come back as a string, not as something a YAML reader
        // would resolve to an int and then a different string.
        TtDocument one;
        TtFolder f;
        f.name = "N";
        TtDevice d;
        d.set("host", "10.0.0.1");
        d.set("port", "22");
        d.set("Model", "NO");  // would resolve to false unquoted
        f.devices.push_back(d);
        one.folders.push_back(f);
        TtDocument back;
        ok(static_cast<bool>(ttParse(ttEmit(one), &back)), "synthetic emits and parses");
        ok(back.folders[0].devices[0].find("Model") &&
               *back.folders[0].devices[0].find("Model") == "NO",
           "a value that looks boolean is quoted");
    }

    // --- import -----------------------------------------------------------
    heading("import");

    Status status;
    auto store = SessionStore::open(dbPath, &status);
    if (!store) {
        std::fprintf(stderr, "cannot open %s: %s\n", dbPath.c_str(),
                     status.message.c_str());
        return 2;
    }

    doc = parseOk(kSample, "reparse for import");
    ImportResult r = applyImport(*store, doc, ImportMode::Merge);
    ok(r.foldersCreated == 2, "two folders created",
       std::to_string(r.foldersCreated));
    ok(r.sessionsImported == 3, "three sessions imported",
       std::to_string(r.sessionsImported));
    ok(r.sessionsSkipped == 0, "nothing skipped");

    {
        bool found = false;
        for (const Session &s : store->listAllSessions()) {
            if (s.hostname != "10.10.0.1") continue;
            found = true;
            ok(s.name == "lab-core-1", "display_name became the name");
            ok(s.description == "switch - C9300",
               "description joins DeviceType and Model", s.description);
            ok(extrasGet(s.extras, "vendor") == "cisco", "extras carries vendor");
            ok(extrasGet(s.extras, "device_type") == "switch",
               "extras carries device_type");
            ok(!s.credential_name.has_value(), "no credential reference");
            ok(s.port == 22, "default port");
        }
        ok(found, "the imported session is in the store");
    }

    for (const Session &s : store->listAllSessions()) {
        if (s.hostname != "10.10.0.2") continue;
        ok(s.description == "switch",
           "description with no Model has no separator", s.description);
    }
    for (const Session &s : store->listAllSessions()) {
        if (s.hostname != "10.10.1.1") continue;
        ok(s.port == 2222, "a quoted port is still a number",
           std::to_string(s.port));
    }

    // Re-import: duplicates are decided by hostname.
    r = applyImport(*store, doc, ImportMode::Skip);
    ok(r.sessionsSkipped == 3, "skip mode skips every existing host",
       std::to_string(r.sessionsSkipped));
    ok(r.sessionsImported == 0, "and imports nothing");
    ok(r.foldersCreated == 0, "and reuses the folders it made before");

    r = applyImport(*store, doc, ImportMode::Merge);
    ok(r.sessionsImported == 3, "merge mode updates them in place");
    ok(store->listAllSessions().size() == 3, "still three sessions",
       std::to_string(store->listAllSessions().size()));

    // An empty folder makes nothing at all.
    {
        TtDocument empty;
        TtFolder f;
        f.name = "Nothing Here";
        empty.folders.push_back(f);
        const ImportResult e = applyImport(*store, empty, ImportMode::Merge);
        ok(e.foldersCreated == 0 && e.sessionsImported == 0,
           "an empty folder entry creates nothing");
    }

    // A device with no host is not a session.
    {
        TtDocument noHost;
        TtFolder f;
        f.name = "Core";
        TtDevice d;
        d.set("display_name", "no address");
        f.devices.push_back(d);
        noHost.folders.push_back(f);
        const ImportResult e = applyImport(*store, noHost, ImportMode::Merge);
        ok(e.sessionsImported == 0, "a device with no host is skipped");
    }

    // --- export -----------------------------------------------------------
    heading("export");

    TtDocument out = buildExport(*store);
    ok(out.folders.size() == 2, "two folders exported",
       std::to_string(out.folders.size()));

    {
        const TtDevice *core1 = nullptr;
        for (const TtFolder &f : out.folders)
            for (const TtDevice &d : f.devices)
                if (d.find("host") && *d.find("host") == "10.10.0.1") core1 = &d;
        ok(core1 != nullptr, "the core session is in the export");
        if (core1) {
            ok(core1->find("Vendor") && *core1->find("Vendor") == "cisco",
               "vendor came back out of extras");
            ok(core1->find("Model") && *core1->find("Model") == "C9300",
               "model came back out of extras");
            ok(core1->find("port") && *core1->find("port") == "22", "port");
        }
    }

    // Nesting has nowhere to go, so the path is the name.
    {
        const auto parent = store->addFolder("access", std::nullopt);
        const auto child = store->addFolder("rack2", parent);
        Session s;
        s.name = "lab-leaf-1";
        s.hostname = "10.10.2.1";
        s.folder_id = child;
        store->addSession(s);

        const TtDocument nested = buildExport(*store);
        bool found = false;
        for (const TtFolder &f : nested.folders)
            if (f.name == "access/rack2") found = true;
        ok(found, "a nested folder exports as a joined path");
    }

    // A session outside any folder.
    {
        Session s;
        s.name = "lab-jump-1";
        s.hostname = "10.10.9.9";
        store->addSession(s);

        const TtDocument loose = buildExport(*store);
        bool found = false;
        for (const TtFolder &f : loose.folders)
            if (f.name == "Ungrouped") found = true;
        ok(found, "a session with no folder exports under Ungrouped");
    }

    // Export, reimport into a fresh store, export again: the second document
    // must equal the first. This is the claim that matters for a user moving a
    // tree between machines.
    {
        const TtDocument first = buildExport(*store);
        const std::string text = ttEmit(first);

        const std::string second = dbPath + ".round";
        std::remove(second.c_str());
        auto other = SessionStore::open(second, nullptr);

        TtDocument reparsed;
        const TtError e = ttParse(text, &reparsed);
        ok(static_cast<bool>(e), "the exported file parses", e ? "" : e.message);
        applyImport(*other, reparsed, ImportMode::Merge);

        const TtDocument third = buildExport(*other);
        bool same = third.folders.size() == first.folders.size();
        if (same) {
            for (size_t i = 0; i < first.folders.size() && same; ++i) {
                same = first.folders[i].name == third.folders[i].name &&
                       first.folders[i].devices.size() ==
                           third.folders[i].devices.size();
                for (size_t j = 0;
                     j < first.folders[i].devices.size() && same; ++j)
                    same = first.folders[i].devices[j].fields ==
                           third.folders[i].devices[j].fields;
            }
        }
        ok(same, "export -> import -> export is stable");
        std::remove(second.c_str());
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
