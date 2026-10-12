#pragma once

#include <QPalette>
#include <QStringList>

class Vault;

struct RenderOptions {
    const Vault *vault = nullptr;
    QString notePath;
    int contentWidth = 720; // px, images are scaled down to fit
    QPalette palette;
    bool dark = false;
    int depth = 0;          // embed nesting
};

struct RenderResult {
    QString html;
    QStringList codeBlocks; // referenced by "copy:<index>" links
};

// Markdown (Obsidian flavour) -> HTML subset understood by QTextDocument.
// Link schemes produced: wiki:<target>[#heading], task:<utf8 offset>, copy:<n>,
// plus regular URLs.
RenderResult renderMarkdown(const QString &text, const RenderOptions &opt);
QString markdownStyleSheet(const QPalette &pal, double basePointSize);
QString headingAnchor(const QString &heading);
QColor mix(const QColor &a, const QColor &b, double t);
