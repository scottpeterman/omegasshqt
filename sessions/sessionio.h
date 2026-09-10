// sessions/sessionio.h
//
// Import and export between the session store and the TerminalTelemetry
// sessions.yaml document in ttyaml.h. No Qt: the file dialogs and the summary
// live in app/, and this half is what the probe and the differential drive.
//
// Import is a port of manager/io.py's import_terminal_telemetry, including the
// parts that are arguably wrong, because a differential test is only worth
// having if the two implementations are asked the same question. Two of those
// are worth naming at the point of use rather than in a commit message:
//
//   - Duplicates are decided by HOSTNAME, not by name or by folder. Two
//     devices that share an address are the same session to this importer even
//     if they sit in different folders and are named differently.
//   - On merge, extras is REPLACED by the three keys built from the YAML, not
//     merged into. Anything else a session was carrying in extras is dropped.
//     See applyImport().
//
// Export has no counterpart in nterm-qt at all -- io.py exports JSON and only
// reads this format -- so its rules are set here:
//
//   - The format has no folder nesting. A session in access/rack2 exports with
//     folder_name "access/rack2", and importing that back produces one flat
//     folder of that literal name. The hierarchy is lost because the format
//     cannot carry it, and joining the path is the version of that loss that
//     keeps the sessions distinguishable.
//   - Sessions outside any folder export under "Ungrouped".
//   - credential_name is not representable and is dropped. That is a feature:
//     the file is an interchange artifact, and a vault reference in it would
//     name a credential the receiving machine has no way to resolve.

#ifndef OMEGA_SESSIONS_SESSIONIO_H
#define OMEGA_SESSIONS_SESSIONIO_H

#include <string>

#include "store.h"
#include "ttyaml.h"

namespace omega::sessions {

// What to do when an imported host is already in the store.
enum class ImportMode {
    Merge,  // update the existing row in place, keeping its id
    Skip,   // leave the existing row alone and count it
};

struct ImportResult {
    int foldersCreated = 0;
    int sessionsImported = 0;
    int sessionsSkipped = 0;
};

// Applies a parsed document to the store. Folders are matched by name at the
// ROOT level only and created if absent, which is what the Python does -- a
// folder of the same name nested inside another is not found and a second
// root-level one is made.
//
// A folder entry with no devices is skipped entirely, so an empty folder in
// the YAML does not create an empty folder here.
ImportResult applyImport(SessionStore &store, const TtDocument &doc,
                         ImportMode mode, Status *status = nullptr);

// Builds a document from everything in the store.
TtDocument buildExport(SessionStore &store, Status *status = nullptr);

// The extras blob is opaque to the store by contract. These are the smallest
// readers that let this file put three known keys in and take them back out
// without pretending to be a JSON library: flat object, string values only.
// Anything nested or non-string reads as absent.
std::string extrasGet(const std::string &extras, const std::string &key);
std::string extrasBuild(
    const std::vector<std::pair<std::string, std::string>> &fields);

}  // namespace omega::sessions

#endif  // OMEGA_SESSIONS_SESSIONIO_H
