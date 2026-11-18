// Crash recovery as the start screen sees it (task T-019; PLAN.md 4.2).
//
// Between saves the app session journals every change of a document (IApp, task T-012). When RayCAD ends without
// saving or closing a document, its journal stays; at the next launch the shell asks the source below for those
// journals and offers to recover or discard each one (dialogs/RecoveryDialog.qml). Recovering reopens the document
// with the journalled changes replayed (it becomes the active document); discarding deletes the journal.
//
// The real source (makeAppRecoverySource) is IApp::pendingRecoveries / recover / discardRecovery. The interface is
// kept small so a test double can stand in for it.
#pragma once

#include <ray/api.h>

#include <QtCore/QString>

#include <memory>
#include <vector>

namespace ray::ui {

// One document with unsaved work found at start-up (api::RecoveryInfo in Qt types).
struct PendingRecovery {
    QString id;               // the journal (RecoveryInfo::id)
    QString filePath;         // the .raycad document it belongs to (for `unsaved`, its backing file)
    QString title;            // what the dialog calls it ("bracket", "Untitled")
    qint64 lastWriteMs = 0;   // when the journal was last written (Unix ms)
    int steps = 0;            // journalled changes
    bool unsaved = false;     // never saved by the user: recovers as an untitled document
    bool damagedTail = false; // the last change was torn; the ones before it recover
};

class IRecoverySource {
public:
    virtual ~IRecoverySource() = default;
    // On the UI thread: at start-up (after the first frame), and when an open is refused because the file has
    // changes to recover first.
    virtual std::vector<PendingRecovery> pending() = 0;
    // Reopens the document with its changes (it becomes the active document). Unknown ids: NotFound.
    virtual JobHandle<api::OpenDocument*> recover(const PendingRecovery&) = 0;
    // Deletes the journal: the user declined.
    virtual Status discard(const PendingRecovery&) = 0;
};

// The app session's own journals: IApp::pendingRecoveries, IApp::recover and IApp::discardRecovery.
std::unique_ptr<IRecoverySource> makeAppRecoverySource(api::IApp& app);

} // namespace ray::ui
