#pragma once

#include <QFont>
#include <QString>

class QPrinter;
class Vault;

// A note as a page of its own: standalone HTML, PDF or paper, always in the light colours.
namespace Export {

// HTML with its images, math and diagrams inlined, so the file works anywhere.
QString html(const Vault &vault, const QString &path, const QFont &font);
bool writeHtml(const Vault &vault, const QString &path, const QFont &font, const QString &file);
bool writePdf(const Vault &vault, const QString &path, const QFont &font, const QString &file);
void print(const Vault &vault, const QString &path, const QFont &font, QPrinter *printer);

}
