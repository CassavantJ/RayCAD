// The setup sheet on paper (task T-076): the PDF and the preview pages, both made from the sheet's HTML
// (post::renderSetupSheetsHtml through ray_api/cam_setupsheet.h), so the HTML stays the one source.
//
// How. Qt has no HTML engine short of Qt WebEngine (not a dependency of RayCAD, and not added for this). The sheet's
// HTML is therefore written in the part of HTML and CSS that Qt's own rich text (QTextDocument) renders as a browser
// does (resources/setupsheet/setupsheet.css says which), and the pages are laid out by QTextDocument: at 96 layout
// units per inch, on Letter or A4 with 12.7 mm margins, the paginated layout cut into pages (each later setup on a new
// page: page-break-before), and a footer drawn on each (the sheet's name, "Page n of N"). The PDF is QPdfWriter
// (QtGui) painting the same pages, vector text and the picture at its full resolution; the preview is the same pages
// painted into images. Only the picture's data: URL is loaded: nothing from a file or the network.
//
// Limits (QTextDocument's, not a browser's): no CSS layout beyond tables (no floats, flex or positioning), borders on
// tables and cells only, no hyphenation; a table row taller than a page is cut at the page end; fonts are the system's
// (Arial where it is, else Qt's sans-serif fallback). Pure functions of their arguments, safe on a worker thread.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QLocale>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <vector>

namespace ray::ui::camui {

enum class Paper { Letter, A4 };
// The paper a locale prints on: Letter where US letter is the norm (the US, Canada, Mexico and the other countries
// whose QLocale measurement system is imperial US), A4 elsewhere.
Paper defaultPaper(const QLocale& locale);
QString paperKey(Paper paper);          // "letter", "a4"
Paper paperFromKey(const QString& key); // A4 for anything but "letter"

// The number of pages the sheet takes on `paper`.
int pageCount(const QString& html, Paper paper);
// The PDF; empty with *error set when it cannot be made. `title` goes into the PDF's information and the footer.
QByteArray toPdf(const QString& html, Paper paper, const QString& title, QString* error = nullptr);
// The pages as images, white paper with the margins, `pixelsPerInch` (96: the layout's own size).
std::vector<QImage> toImages(const QString& html, Paper paper, const QString& title, double pixelsPerInch);

} // namespace ray::ui::camui
