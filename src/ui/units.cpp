#include "units.h"

#include "session.h"

#include <ray/api.h>

#include <QtCore/QVariantList>

#include <cmath>
#include <limits>

namespace ray::ui {
namespace {
constexpr const char* kEngineKey = "_ray_ui_units";
constexpr double kMmPerInch = 25.4;
} // namespace

UnitsObject::UnitsObject(Session* session, QObject* parent) : QObject(parent), session_(session)
{
    if (session_ != nullptr) {
        connect(session_, &Session::documentChanged, this, &UnitsObject::refreshDocument);
    }
}

UnitsObject::~UnitsObject() = default;

UnitsObject* UnitsObject::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* units = qobject_cast<UnitsObject*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(units, QJSEngine::CppOwnership);
            return units;
        }
    }
    return new UnitsObject(nullptr, nullptr); // owned by the engine
}

void UnitsObject::provideFor(QQmlEngine* engine, UnitsObject* units)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(units));
}

QString UnitsObject::systemName() const
{
    return system_ == UnitSystem::Inch ? QStringLiteral("inch") : QStringLiteral("metric");
}

QString UnitsObject::unitLabel() const
{
    return system_ == UnitSystem::Inch ? tr("in") : tr("mm");
}

QString UnitsObject::documentSystemName() const
{
    if (docId_.empty()) {
        return {};
    }
    return docSystem_ == UnitSystem::Inch ? QStringLiteral("inch") : QStringLiteral("metric");
}

bool UnitsObject::overridden() const
{
    return !docId_.empty() && system_ != docSystem_;
}

void UnitsObject::setDecimals(int inch, int metric)
{
    if (inch == decimalsInch_ && metric == decimalsMetric_) {
        return;
    }
    decimalsInch_ = inch;
    decimalsMetric_ = metric;
    ++revision_;
    emit changed();
}

void UnitsObject::setNewDocumentSystem(UnitSystem system)
{
    if (system == newDocSystem_) {
        return;
    }
    newDocSystem_ = system;
    update();
}

void UnitsObject::refreshDocument()
{
    std::string id;
    UnitSystem units = docSystem_;
    if (session_ != nullptr) {
        if (api::OpenDocument* od = session_->activeDocument(); od != nullptr && od->document) {
            id = od->id;
            units = od->document->state()->settings.units;
        }
    }
    if (id == docId_ && units == docSystem_) {
        return;
    }
    docId_ = std::move(id);
    docSystem_ = units;
    update();
}

void UnitsObject::update()
{
    UnitSystem next = newDocSystem_;
    if (!docId_.empty()) {
        auto it = toggled_.find(docId_);
        next = it != toggled_.end() ? it->second : docSystem_;
    }
    // Revision moves on every call that may change what a read-out shows (a document switch, too).
    system_ = next;
    ++revision_;
    emit changed();
}

void UnitsObject::toggle()
{
    const UnitSystem next = system_ == UnitSystem::Inch ? UnitSystem::Metric : UnitSystem::Inch;
    if (docId_.empty()) {
        // Nothing open: the toggle sets what new documents get (the settings object stores it).
        emit newDocumentSystemRequested(next);
        setNewDocumentSystem(next);
        return;
    }
    if (next == docSystem_) {
        toggled_.erase(docId_);
    } else {
        toggled_[docId_] = next;
    }
    update();
}

void UnitsObject::setDisplaySystem(const QString& system)
{
    if (docId_.empty()) {
        return;
    }
    if (system.isEmpty()) {
        toggled_.erase(docId_);
    } else if (system == QLatin1String("inch") || system == QLatin1String("metric")) {
        const UnitSystem s = system == QLatin1String("inch") ? UnitSystem::Inch : UnitSystem::Metric;
        if (s == docSystem_) {
            toggled_.erase(docId_);
        } else {
            toggled_[docId_] = s;
        }
    } else {
        return;
    }
    update();
}

QString UnitsObject::number(double value) const
{
    // formatLength in mm leaves the number as it is: its fixed-point, locale-free form without the suffix.
    std::string text = ray::formatLength(value, UnitFormat{UnitSystem::Metric, decimals()});
    if (text.size() > 3 && text.compare(text.size() - 3, 3, " mm") == 0) {
        text.resize(text.size() - 3);
    }
    return QString::fromStdString(text);
}

QString UnitsObject::formatLength(double mm) const
{
    return QString::fromStdString(ray::formatLength(mm, format()));
}

QString UnitsObject::formatArea(double mm2) const
{
    if (system_ == UnitSystem::Inch) {
        return tr("%1 in²").arg(number(mm2 / (kMmPerInch * kMmPerInch)));
    }
    return tr("%1 mm²").arg(number(mm2));
}

QString UnitsObject::formatVolume(double mm3) const
{
    if (system_ == UnitSystem::Inch) {
        return tr("%1 in³").arg(number(mm3 / (kMmPerInch * kMmPerInch * kMmPerInch)));
    }
    return tr("%1 mm³").arg(number(mm3));
}

QString UnitsObject::formatPoint(double x, double y, double z) const
{
    const double scale = system_ == UnitSystem::Inch ? 1.0 / kMmPerInch : 1.0;
    return QStringLiteral("%1, %2, %3 %4").arg(number(x * scale), number(y * scale), number(z * scale), unitLabel());
}

QString UnitsObject::formatRow(const QVariantMap& row) const
{
    const QString quantity = row.value(QStringLiteral("quantity")).toString();
    const QVariant mm = row.value(QStringLiteral("mm"));
    if (quantity == QLatin1String("length")) {
        return formatLength(mm.toDouble());
    }
    if (quantity == QLatin1String("area")) {
        return formatArea(mm.toDouble());
    }
    if (quantity == QLatin1String("volume")) {
        return formatVolume(mm.toDouble());
    }
    if (quantity == QLatin1String("point")) {
        const QVariantList p = mm.toList();
        if (p.size() == 3) {
            return formatPoint(p[0].toDouble(), p[1].toDouble(), p[2].toDouble());
        }
    }
    return row.value(QStringLiteral("value")).toString();
}

double UnitsObject::parseLength(const QString& text) const
{
    const QByteArray utf8 = text.toUtf8();
    auto mm = ray::parseLength(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())), system_);
    return mm ? *mm : std::numeric_limits<double>::quiet_NaN();
}

} // namespace ray::ui
