// app/sessiontransfer.h
//
// The File menu's Import and Export, and nothing else: file dialogs, the
// duplicate-policy question, and the summary. Every rule about what the
// formats mean lives in sessions/, which is why this file has no idea what a
// folder_name is.
//
// nterm-qt puts three dialog classes in manager/io.py -- ExportDialog,
// ImportDialog and ImportTerminalTelemetryDialog -- each with a preview tree
// and a set of checkboxes. Two functions here instead, because the preview is
// answering a question the tree already answers: after an import the tree
// refreshes and shows exactly what arrived, and an import that went wrong is
// undone by importing the file again in skip mode or by deleting a folder.
// A preview that has to be kept in step with the importer is a second
// implementation of the importer.

#ifndef OMEGA_APP_SESSIONTRANSFER_H
#define OMEGA_APP_SESSIONTRANSFER_H

#include <QString>

class QWidget;

namespace omega::sessions {
class SessionStore;
}

namespace omega::app {

// Prompts for a file, imports it, and reports what happened. Returns true when
// the store changed, which is the caller's cue to refresh the tree.
//
// A parse failure is reported with the line number and changes nothing: the
// document is fully parsed before a single row is written, so a malformed file
// cannot leave a half-imported tree behind.
bool importSessions(QWidget *parent, sessions::SessionStore *store);

// Prompts for a file and writes the whole store to it.
void exportSessions(QWidget *parent, sessions::SessionStore *store);

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONTRANSFER_H
