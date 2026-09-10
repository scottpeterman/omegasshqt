// app/sessiontransfer.cpp

#include "app/sessiontransfer.h"

#include <QDialogButtonBox>
#include "app/dialogbuttons.h"
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include "sessionio.h"
#include "store.h"
#include "ttyaml.h"

namespace omega::app {
namespace {

const char *const kFilter =
    "TerminalTelemetry sessions (*.yaml *.yml);;All files (*)";

// The duplicate question, asked once rather than per row. Merge and skip are
// the two the importer implements, and there is no third option hiding behind
// a checkbox.
bool askMode(QWidget *parent, sessions::ImportMode *mode) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Import Sessions"));

    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(QObject::tr(
        "A session already in the tree with the same hostname:"));
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *merge = new QRadioButton(
        QObject::tr("Update it from the file (merge)"));
    auto *skip = new QRadioButton(
        QObject::tr("Leave it alone (skip)"));
    merge->setChecked(true);
    layout->addWidget(merge);
    layout->addWidget(skip);

    // Said once, here, rather than discovered afterwards: merge replaces the
    // vendor, model and device-type an existing session was carrying.
    auto *note = new QLabel(QObject::tr(
        "Merging replaces the name, description, port and device details of "
        "the existing session. Its folder and connection history are kept."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Import"));
    layout->addWidget(buttons);
    setReturnActivates(buttons, buttons->button(QDialogButtonBox::Ok));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return false;
    *mode = merge->isChecked() ? sessions::ImportMode::Merge
                               : sessions::ImportMode::Skip;
    return true;
}

}  // namespace

bool importSessions(QWidget *parent, sessions::SessionStore *store) {
    if (!store) {
        QMessageBox::warning(parent, QObject::tr("Import Sessions"),
                             QObject::tr("The session store is not open."));
        return false;
    }

    const QString path = QFileDialog::getOpenFileName(
        parent, QObject::tr("Import TerminalTelemetry sessions"), QString(),
        QObject::tr(kFilter));
    if (path.isEmpty()) return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(parent, QObject::tr("Import Sessions"),
                             QObject::tr("Could not read %1:\n%2")
                                 .arg(QFileInfo(path).fileName(),
                                      file.errorString()));
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // Parsed in full before anything is written, so a file that is malformed
    // halfway down leaves the tree exactly as it was.
    sessions::TtDocument doc;
    const sessions::TtError parsed =
        sessions::ttParse(bytes.toStdString(), &doc);
    if (!parsed) {
        QMessageBox::warning(
            parent, QObject::tr("Import Sessions"),
            QObject::tr("%1 could not be read as a TerminalTelemetry sessions "
                        "file.\n\nLine %2: %3")
                .arg(QFileInfo(path).fileName())
                .arg(parsed.line)
                .arg(QString::fromStdString(parsed.message)));
        return false;
    }

    if (doc.folders.empty()) {
        QMessageBox::information(parent, QObject::tr("Import Sessions"),
                                 QObject::tr("%1 contains no sessions.")
                                     .arg(QFileInfo(path).fileName()));
        return false;
    }

    sessions::ImportMode mode = sessions::ImportMode::Merge;
    if (!askMode(parent, &mode)) return false;

    sessions::Status status;
    const sessions::ImportResult result =
        sessions::applyImport(*store, doc, mode, &status);

    if (!status) {
        // Partial by definition: rows before the failure are committed. Saying
        // so is better than a bare "failed" over a tree that has changed.
        QMessageBox::warning(
            parent, QObject::tr("Import Sessions"),
            QObject::tr("The import stopped after %1 session(s):\n\n%2")
                .arg(result.sessionsImported)
                .arg(QString::fromStdString(status.message)));
        return result.sessionsImported > 0 || result.foldersCreated > 0;
    }

    QString summary = QObject::tr("Imported %1 session(s) into %2 new folder(s).")
                          .arg(result.sessionsImported)
                          .arg(result.foldersCreated);
    if (result.sessionsSkipped > 0)
        summary += QObject::tr("\n\n%1 already in the tree were left alone.")
                       .arg(result.sessionsSkipped);
    QMessageBox::information(parent, QObject::tr("Import Sessions"), summary);

    return result.sessionsImported > 0 || result.foldersCreated > 0;
}

void exportSessions(QWidget *parent, sessions::SessionStore *store) {
    if (!store) {
        QMessageBox::warning(parent, QObject::tr("Export Sessions"),
                             QObject::tr("The session store is not open."));
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        parent, QObject::tr("Export sessions"),
        QStringLiteral("sessions.yaml"), QObject::tr(kFilter));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".yaml");

    sessions::Status status;
    const sessions::TtDocument doc = sessions::buildExport(*store, &status);
    if (!status) {
        QMessageBox::warning(parent, QObject::tr("Export Sessions"),
                             QString::fromStdString(status.message));
        return;
    }

    int sessionCount = 0;
    for (const sessions::TtFolder &folder : doc.folders)
        sessionCount += static_cast<int>(folder.devices.size());

    if (sessionCount == 0) {
        QMessageBox::information(parent, QObject::tr("Export Sessions"),
                                 QObject::tr("There are no sessions to export."));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(parent, QObject::tr("Export Sessions"),
                             QObject::tr("Could not write %1:\n%2")
                                 .arg(QFileInfo(path).fileName(),
                                      file.errorString()));
        return;
    }
    {
        QTextStream out(&file);
        out << QString::fromStdString(sessions::ttEmit(doc));
    }
    file.close();

    QString summary =
        QObject::tr("Exported %1 session(s) in %2 folder(s) to %3.")
            .arg(sessionCount)
            .arg(doc.folders.size())
            .arg(QFileInfo(path).fileName());
    // Two losses worth stating at the moment they happen, rather than in a
    // document nobody reads before clicking Export.
    summary += QObject::tr(
        "\n\nThe format has no nested folders, so a folder inside another is "
        "written as a single name with its path. Credential references are "
        "not included.");
    QMessageBox::information(parent, QObject::tr("Export Sessions"), summary);
}

}  // namespace omega::app