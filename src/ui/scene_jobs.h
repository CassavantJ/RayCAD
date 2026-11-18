// Jobs for the viewer (task T-020): the Qt implementation of IUiDispatcher, and where the scene, the selection and
// the developer file opener get their job system. Until T-017's application session exists AppHost owns one job
// system (api::viewer::makeViewerJobs); with a session it is IApp::jobs().
#pragma once

#include <ray/api.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>

#include <functional>
#include <memory>
#include <utility>

namespace ray::ui {

// Posts to the Qt event loop of the thread that created it (the UI thread). Queued, never run inline.
class QtUiDispatcher final : public IUiDispatcher {
public:
    QtUiDispatcher() : context_(std::make_unique<QObject>()) {}
    void post(std::function<void()> fn) override
    {
        QMetaObject::invokeMethod(context_.get(), std::move(fn), Qt::QueuedConnection);
    }

private:
    std::unique_ptr<QObject> context_; // queued calls are dropped when the dispatcher goes
};

// Returns the job system to use now; never null while the viewer runs.
using JobsProvider = std::function<IJobSystem*()>;

} // namespace ray::ui
