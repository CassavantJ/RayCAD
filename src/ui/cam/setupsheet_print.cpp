#include "setupsheet_print.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QUrl>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPageLayout>
#include <QtGui/QPageSize>
#include <QtGui/QPainter>
#include <QtGui/QPdfWriter>
#include <QtGui/QTextDocument>

#include <algorithm>
#include <cmath>
#include <memory>

namespace ray::ui::camui {

namespace {

constexpr double kLayoutDpi = 96.0; // layout units per inch: CSS pixels
constexpr double kMarginMm = 12.7;  // half an inch all round: inside every printer's unprintable edge
constexpr double kFooterPx = 22.0;  // the band at the foot of each page for the page number
constexpr double kFooterFontPt = 7.5;

// A QTextDocument that loads only the images embedded as data: URLs (the sheet's picture). Nothing from a file, a
// resource or the network.
class SheetDocument final : public QTextDocument {
public:
    SheetDocument() = default;

protected:
    QVariant loadResource(int type, const QUrl& name) override
    {
        if (type != QTextDocument::ImageResource)
            return {};
        // Anything but an embedded image is answered with an empty (transparent) one: given nothing, Qt's image
        // handler would read the URL as a file path itself.
        QImage none(1, 1, QImage::Format_ARGB32);
        none.fill(Qt::transparent);
        if (name.scheme() != QLatin1String("data"))
            return none;
        const QString text = name.toString(QUrl::FullyEncoded);
        const qsizetype comma = text.indexOf(QLatin1Char(','));
        if (comma < 0 || !text.left(comma).endsWith(QLatin1String(";base64")))
            return none;
        const QByteArray bytes = QByteArray::fromBase64(text.mid(comma + 1).toLatin1());
        QImage image;
        if (!image.loadFromData(bytes))
            return none;
        return image;
    }
};

QPageLayout pageLayout(Paper paper)
{
    return QPageLayout(QPageSize(paper == Paper::Letter ? QPageSize::Letter : QPageSize::A4), QPageLayout::Portrait,
                       QMarginsF(kMarginMm, kMarginMm, kMarginMm, kMarginMm), QPageLayout::Millimeter);
}

// The printable area in layout units.
QSizeF bodySize(Paper paper)
{
    const QRectF points = pageLayout(paper).paintRect(QPageLayout::Point);
    return QSizeF(points.width() * kLayoutDpi / 72.0, points.height() * kLayoutDpi / 72.0);
}

struct Laid {
    std::unique_ptr<SheetDocument> doc;
    QSizeF body;       // the printable area
    double pageHeight; // the text's part of a page (the body less the footer band)
    int pages;
};

Laid layOut(const QString& html, Paper paper)
{
    Laid l;
    l.doc = std::make_unique<SheetDocument>();
    l.body = bodySize(paper);
    l.pageHeight = l.body.height() - kFooterPx;
    l.doc->setDocumentMargin(0);
    l.doc->setUseDesignMetrics(true); // the same line breaks on screen and in the PDF
    l.doc->setPageSize(QSizeF(l.body.width(), l.pageHeight));
    l.doc->setHtml(html);
    l.pages = std::max(1, l.doc->pageCount());
    return l;
}

// Page `index` of the laid-out sheet, in layout units, with the painter's origin at the top left of the printable
// area. Black on white whatever the app's theme: the sheet is paper.
void drawPage(QPainter& p, const Laid& l, int index, const QString& title)
{
    p.save();
    p.setClipRect(QRectF(QPointF(0, 0), QSizeF(l.body.width(), l.pageHeight)));
    p.translate(0, -index * l.pageHeight);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.clip = QRectF(0, index * l.pageHeight, l.body.width(), l.pageHeight);
    ctx.palette.setColor(QPalette::Text, Qt::black);
    l.doc->documentLayout()->draw(&p, ctx);
    p.restore();

    // The footer: what this is on the left, the page number on the right.
    p.save();
    QFont font = l.doc->defaultFont();
    font.setPointSizeF(kFooterFontPt);
    p.setFont(font);
    p.setPen(Qt::black);
    const QRectF band(0, l.body.height() - kFooterPx + 6, l.body.width(), kFooterPx - 6);
    const QString page =
        QCoreApplication::translate("ray::ui::SetupSheetModel", "Page %1 of %2").arg(index + 1).arg(l.pages);
    p.drawText(band, Qt::AlignRight | Qt::AlignVCenter, page);
    const QFontMetricsF fm(font);
    const QString left = fm.elidedText(title, Qt::ElideMiddle, band.width() - fm.horizontalAdvance(page) - 24);
    p.drawText(band, Qt::AlignLeft | Qt::AlignVCenter, left);
    p.restore();
}

} // namespace

Paper defaultPaper(const QLocale& locale)
{
    return locale.measurementSystem() == QLocale::ImperialUSSystem ? Paper::Letter : Paper::A4;
}

QString paperKey(Paper paper)
{
    return paper == Paper::Letter ? QStringLiteral("letter") : QStringLiteral("a4");
}

Paper paperFromKey(const QString& key)
{
    return key == QLatin1String("letter") ? Paper::Letter : Paper::A4;
}

int pageCount(const QString& html, Paper paper)
{
    return layOut(html, paper).pages;
}

QByteArray toPdf(const QString& html, Paper paper, const QString& title, QString* error)
{
    const Laid l = layOut(html, paper);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        writer.setPageLayout(pageLayout(paper));
        writer.setResolution(600);
        writer.setTitle(title);
        writer.setCreator(QStringLiteral("RayCAD"));
        QPainter p;
        if (!p.begin(&writer)) {
            if (error != nullptr)
                *error = QCoreApplication::translate("ray::ui::SetupSheetModel", "The PDF could not be started");
            return {};
        }
        const double scale = writer.resolution() / kLayoutDpi;
        for (int i = 0; i < l.pages; ++i) {
            if (i > 0 && !writer.newPage()) {
                if (error != nullptr)
                    *error =
                        QCoreApplication::translate("ray::ui::SetupSheetModel", "The PDF could not take a new page");
                p.end();
                return {};
            }
            p.save();
            p.scale(scale, scale);
            drawPage(p, l, i, title);
            p.restore();
        }
        p.end();
    }
    buffer.close();
    if (bytes.isEmpty() && error != nullptr)
        *error = QCoreApplication::translate("ray::ui::SetupSheetModel", "The PDF came out empty");
    return bytes;
}

std::vector<QImage> toImages(const QString& html, Paper paper, const QString& title, double pixelsPerInch)
{
    const Laid l = layOut(html, paper);
    const QRectF full = pageLayout(paper).fullRect(QPageLayout::Point);
    const QRectF body = pageLayout(paper).paintRect(QPageLayout::Point);
    const double scale = pixelsPerInch / kLayoutDpi;
    const double pxPerPt = pixelsPerInch / 72.0;
    const QSize size(static_cast<int>(std::lround(full.width() * pxPerPt)),
                     static_cast<int>(std::lround(full.height() * pxPerPt)));
    std::vector<QImage> out;
    for (int i = 0; i < l.pages; ++i) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(body.left() * pxPerPt, body.top() * pxPerPt);
        p.scale(scale, scale);
        drawPage(p, l, i, title);
        p.end();
        out.push_back(std::move(image));
    }
    return out;
}

} // namespace ray::ui::camui
