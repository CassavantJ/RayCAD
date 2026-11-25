#include "directedit_logic.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLocale>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace ray::ui::directedit {
namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ray::ui::DirectEdit", text);
}

struct ToolInfo {
    Tool tool;
    const char* key;
    const char* type;
    const char* title;
};
constexpr ToolInfo kTools[] = {
    {Tool::Delete, "delete", "delete_face", QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "Delete Faces")},
    {Tool::PushPull, "pushPull", "offset_face", QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "Push/Pull")},
    {Tool::Resize, "resize", "resize_hole", QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "Resize Hole")},
};

const ToolInfo* infoOf(Tool tool)
{
    for (const ToolInfo& t : kTools) {
        if (t.tool == tool)
            return &t;
    }
    return nullptr;
}

QString capitalized(QString s)
{
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

} // namespace

QString toolKey(Tool tool)
{
    const ToolInfo* t = infoOf(tool);
    return t ? QLatin1String(t->key) : QString();
}

Tool toolOfKey(const QString& key)
{
    for (const ToolInfo& t : kTools) {
        if (key == QLatin1String(t.key))
            return t.tool;
    }
    return Tool::None;
}

QString featureType(Tool tool)
{
    const ToolInfo* t = infoOf(tool);
    return t ? QLatin1String(t->type) : QString();
}

Tool toolOfFeatureType(const QString& type)
{
    for (const ToolInfo& t : kTools) {
        if (type == QLatin1String(t.type))
            return t.tool;
    }
    return Tool::None;
}

QString toolTitle(Tool tool)
{
    const ToolInfo* t = infoOf(tool);
    return t ? tr(t->title) : QString();
}

// ---------- the flow ----------
void Flow::open()
{
    phase_ = Phase::Picking;
    ++serial_;
}

void Flow::close()
{
    phase_ = Phase::Closed;
    ++serial_;
}

void Flow::incomplete()
{
    if (!active())
        return;
    phase_ = Phase::Picking;
    ++serial_; // a preview still on its way is for picks or values that are gone
}

std::uint64_t Flow::request()
{
    if (active())
        phase_ = Phase::Building;
    return ++serial_;
}

bool Flow::result(std::uint64_t serial, Outcome outcome)
{
    if (serial != serial_ || phase_ != Phase::Building)
        return false;
    phase_ = outcome == Outcome::Ok ? Phase::Ready : outcome == Outcome::Warning ? Phase::Warning : Phase::Refused;
    return true;
}

void Flow::refuse()
{
    if (!active())
        return;
    phase_ = Phase::Refused;
    ++serial_;
}

void Flow::commitStarted()
{
    if (canCommit())
        phase_ = Phase::Committing;
}

void Flow::commitFailed()
{
    if (phase_ == Phase::Committing)
        phase_ = Phase::Refused;
}

void Flow::committed()
{
    close();
}

QString phaseKey(Phase phase)
{
    switch (phase) {
    case Phase::Closed:
    case Phase::Picking:
        return {};
    case Phase::Building:
        return QStringLiteral("building");
    case Phase::Ready:
        return QStringLiteral("ok");
    case Phase::Warning:
        return QStringLiteral("warning");
    case Phase::Refused:
        return QStringLiteral("failed");
    case Phase::Committing:
        return QStringLiteral("committing");
    }
    return {};
}

// ---------- lengths ----------
double snapStepMm(bool inch, bool fine)
{
    const double step = inch ? 0.005 * 25.4 : 0.1;
    return fine ? step / 10 : step;
}

double snapLength(double mm, bool inch, bool fine)
{
    const double step = snapStepMm(inch, fine);
    double steps = std::round(mm / step);
    if (steps == 0)
        steps = mm < 0 ? -1 : 1;
    return steps * step;
}

QString lengthText(double mm, bool inch, bool fine)
{
    // The step's decimals: 0.1 mm -> 1 (fine 2); 0.005 in -> 3 (fine 4).
    const int decimals = (inch ? 3 : 1) + (fine ? 1 : 0);
    const double value = inch ? mm / 25.4 : mm;
    QString text = QLocale::c().toString(value, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    if (text == QLatin1String("-0"))
        text = QStringLiteral("0");
    return text + (inch ? QStringLiteral(" in") : QStringLiteral(" mm"));
}

// ---------- the review list ----------
std::vector<ReviewGroup> groupItems(const std::vector<ReviewItem>& items)
{
    // Owner order first (features have the lower numbers), then the owner itself (two owners never share a number, but
    // the id keeps the order total), then the reference's place.
    std::vector<std::size_t> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const ReviewItem& x = items[a];
        const ReviewItem& y = items[b];
        return std::tie(x.ownerOrder, x.ownerId, x.index) < std::tie(y.ownerOrder, y.ownerId, y.index);
    });
    std::vector<ReviewGroup> groups;
    for (const std::size_t i : order) {
        const ReviewItem& item = items[i];
        if (groups.empty() || groups.back().ownerId != item.ownerId || groups.back().owner != item.owner) {
            ReviewGroup g;
            g.owner = item.owner;
            g.ownerId = item.ownerId;
            switch (item.owner) {
            case OwnerKind::Feature:
                g.title = item.ownerName;
                break;
            case OwnerKind::Operation:
                g.title =
                    item.setupName.isEmpty() ? item.ownerName : tr("%1 in %2").arg(item.ownerName, item.setupName);
                break;
            case OwnerKind::Setup:
                g.title = tr("%1: work offset").arg(item.ownerName);
                break;
            }
            groups.push_back(std::move(g));
        }
        groups.back().items.push_back(i);
    }
    return groups;
}

QString stateLabel(RefState state)
{
    switch (state) {
    case RefState::Exact:
        return {};
    case RefState::Split:
        return tr("Split");
    case RefState::Merged:
        return tr("Merged");
    case RefState::Repaired:
        return tr("Check match");
    case RefState::Broken:
        return tr("Missing");
    }
    return {};
}

QString stateKey(RefState state)
{
    switch (state) {
    case RefState::Exact:
        return QStringLiteral("exact");
    case RefState::Split:
        return QStringLiteral("split");
    case RefState::Merged:
        return QStringLiteral("merged");
    case RefState::Repaired:
        return QStringLiteral("repaired");
    case RefState::Broken:
        return QStringLiteral("broken");
    }
    return {};
}

QString countWord(std::size_t n)
{
    static const char* const words[] = {
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "no"),     QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "one"),
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "two"),    QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "three"),
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "four"),   QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "five"),
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "six"),    QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "seven"),
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "eight"),  QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "nine"),
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "ten"),    QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "eleven"),
        QT_TRANSLATE_NOOP("ray::ui::DirectEdit", "twelve"),
    };
    if (n < std::size(words))
        return tr(words[n]);
    return QString::number(n);
}

QString elementWord(ElementKind kind, std::size_t n)
{
    const bool one = n == 1;
    switch (kind) {
    case ElementKind::Face:
        return one ? tr("face") : tr("faces");
    case ElementKind::Edge:
        return one ? tr("edge") : tr("edges");
    case ElementKind::Vertex:
        return one ? tr("point") : tr("points");
    }
    return {};
}

QString stateWords(const ReviewItem& item)
{
    const QString one = elementWord(item.kind);
    switch (item.state) {
    case RefState::Exact:
        return {};
    case RefState::Split:
        if (item.elements < 2)
            return tr("This %1 was split").arg(one);
        return tr("This reference now points at %1 %2").arg(countWord(item.elements), elementWord(item.kind, 2));
    case RefState::Merged:
        if (item.kind == ElementKind::Edge)
            return tr("This edge is now part of a longer edge");
        if (item.kind == ElementKind::Vertex)
            return tr("This point is now part of a larger element");
        return tr("This face is now part of a larger face");
    case RefState::Repaired:
        return tr("This %1 changed; a likely match was found").arg(one);
    case RefState::Broken:
        return tr("This %1 no longer exists").arg(one);
    }
    return {};
}

QString consequence(const ReviewItem& item)
{
    const QString one = elementWord(item.kind);
    if (item.owner == OwnerKind::Feature) {
        switch (item.state) {
        case RefState::Exact:
            return {};
        // Split and Merged: the feature goes on with what the reference points at now (with a warning); nothing is
        // needed unless that is not what the user wants.
        case RefState::Split:
            return item.elements == 2 ? tr("The feature uses both pieces. Re-pick only if that is not what you want.")
                                      : tr("The feature uses all %1 pieces. Re-pick only if that is not what you want.")
                                            .arg(countWord(item.elements));
        case RefState::Merged:
            return tr("The feature uses the whole %1. Re-pick only if that is not what you want.").arg(one);
        case RefState::Repaired:
            return tr("The feature does not build until you confirm the match or pick the %1 again.").arg(one);
        case RefState::Broken:
            return tr("The feature does not build until the %1 is picked again.").arg(one);
        }
        return {};
    }
    const bool setup = item.owner == OwnerKind::Setup;
    switch (item.state) {
    case RefState::Exact:
        return {};
    case RefState::Split:
    case RefState::Merged:
        return setup ? tr("The setup's toolpaths cannot be posted until the %1 is picked again.").arg(one)
                     : tr("The toolpath cannot be posted until the %1 is picked again.").arg(one);
    case RefState::Repaired:
        return setup ? tr("The setup's toolpaths cannot be posted until you confirm the match or pick again.")
                     : tr("The toolpath cannot be posted until you confirm the match or pick again.");
    case RefState::Broken:
        return setup ? tr("The setup's toolpaths cannot be posted until the %1 is picked again.").arg(one)
                     : tr("The toolpath cannot be posted until the %1 is picked again.").arg(one);
    }
    return {};
}

QString placeWords(const ReviewItem& item)
{
    const QString one = elementWord(item.kind);
    if (item.owner == OwnerKind::Setup) {
        if (item.path == QLatin1String("wcs/zFace"))
            return tr("Z face of the work offset");
        if (item.path == QLatin1String("wcs/xEdge"))
            return tr("X edge of the work offset");
        if (item.path == QLatin1String("wcs/originVertex"))
            return tr("Origin point of the work offset");
        return tr("A reference of the work offset");
    }
    if (item.of <= 1)
        return item.owner == OwnerKind::Operation ? tr("The %1 it machines").arg(one)
                                                  : capitalized(tr("the %1").arg(one));
    const QString nth = tr("%1 %2 of %3").arg(capitalized(one)).arg(item.index + 1).arg(item.of);
    return item.owner == OwnerKind::Operation ? tr("%1 it machines").arg(nth) : nth;
}

} // namespace ray::ui::directedit
