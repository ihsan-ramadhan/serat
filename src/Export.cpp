#include "Export.h"

#include "Markdown.h"
#include "Reader.h"
#include "Theme.h"
#include "Vault.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeDatabase>
#include <QPageLayout>
#include <QPixmap>
#include <QPrinter>
#include <QRegularExpression>
#include <QTextDocument>

#include <memory>

using namespace Qt::StringLiterals;

namespace {

constexpr int pageWidth = 680; // px of text, close to an A4 page with its margins

QString unescape(QString s)
{
    return s.replace(u"&quot;"_s, u"\""_s).replace(u"&#39;"_s, u"'"_s).replace(u"&lt;"_s, u"<"_s).replace(u"&gt;"_s, u">"_s)
        .replace(u"&amp;"_s, u"&"_s);
}

// Links into the vault, to tasks or to code copy buttons go nowhere outside the app.
QString withoutAppLinks(const QString &html)
{
    static const QRegularExpression copyButton(u"<p align=\"right\"[^>]*><a href=\"copy:\\d+\"[^>]*>.*?</a></p>"_s);
    static const QRegularExpression appLink(u"<a href=\"(?!https?:|mailto:|#)[^\"]*\""_s);
    return QString(html).remove(copyButton).replace(appLink, u"<a"_s);
}

struct Page {
    std::unique_ptr<Reader> reader;
    QString html;
};

Page layOut(const Vault &vault, const QString &path, const QFont &font)
{
    Page page{std::make_unique<Reader>(), {}};
    Reader *r = page.reader.get();
    const QPalette pal = Theme::lightPalette();
    r->setPalette(pal);
    r->setResourceScale(3);
    r->document()->setDefaultFont(font);
    r->document()->setDefaultStyleSheet(markdownStyleSheet(pal, font.pointSizeF()));
    RenderOptions opt;
    opt.vault = &vault;
    opt.notePath = path;
    opt.contentWidth = pageWidth;
    opt.palette = pal;
    const Note *note = vault.note(path);
    page.html = withoutAppLinks(renderMarkdown(note ? note->text : QString(), opt).html);
    r->setHtml(page.html);
    return page;
}

QString dataUri(const QTextDocument *doc, const QString &src, QSize *logical)
{
    const QUrl url(src);
    if (url.isLocalFile()) {
        QFile f(url.toLocalFile());
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QString mime = QMimeDatabase().mimeTypeForFile(url.toLocalFile()).name();
        return u"data:%1;base64,%2"_s.arg(mime, QString::fromLatin1(f.readAll().toBase64()));
    }
    const QVariant v = doc->resource(QTextDocument::ImageResource, url);
    QImage img = v.typeId() == QMetaType::QPixmap ? v.value<QPixmap>().toImage() : v.value<QImage>();
    if (img.isNull())
        return {};
    *logical = (QSizeF(img.size()) / img.devicePixelRatio()).toSize();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return u"data:image/png;base64,"_s + QString::fromLatin1(png.toBase64());
}

}

namespace Export {

QString html(const Vault &vault, const QString &path, const QFont &font)
{
    const Page page = layOut(vault, path, font);
    static const QRegularExpression img(u"<img\\b[^>]*>"_s);
    static const QRegularExpression srcAttr(u"src=\"([^\"]*)\""_s);
    QString body;
    qsizetype from = 0;
    for (auto it = img.globalMatch(page.html); it.hasNext();) {
        const auto m = it.next();
        QString tag = m.captured();
        if (const auto s = srcAttr.match(tag); s.hasMatch()) {
            QSize logical;
            const QString src = unescape(s.captured(1));
            const QString uri = dataUri(page.reader->document(), src, &logical);
            if (uri.isEmpty() && !src.startsWith(u"http"_s))
                tag.clear(); // an app-only image that could not be drawn would show as broken
            if (!uri.isEmpty()) {
                tag.replace(s.capturedStart(), s.capturedLength(), u"src=\"%1\""_s.arg(uri));
                if (logical.isValid() && !tag.contains(u" width="_s))
                    tag.insert(4, u" width=\"%1\" height=\"%2\""_s.arg(logical.width()).arg(logical.height()));
            }
        }
        body += QStringView(page.html).sliced(from, m.capturedStart() - from) + tag;
        from = m.capturedEnd();
    }
    body += QStringView(page.html).sliced(from);

    const QPalette pal = Theme::lightPalette();
    const QString css = markdownStyleSheet(pal, font.pointSizeF())
        + u"body { max-width: %1px; margin: 40px auto; padding: 0 16px; font-family: '%2', sans-serif; font-size: %3pt;"
          " line-height: 1.55; color: %4; background: %5; } img { max-width: 100%; height: auto; }"_s
              .arg(pageWidth)
              .arg(font.family(), QString::number(font.pointSizeF()), pal.color(QPalette::Text).name(),
                   pal.color(QPalette::Base).name());
    return u"<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>%1</title>\n"
           "<style>\n%2\n</style>\n</head>\n<body>\n%3\n</body>\n</html>\n"_s
        .arg(Vault::baseName(path).toHtmlEscaped(), css, body);
}

bool writeHtml(const Vault &vault, const QString &path, const QFont &font, const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(html(vault, path, font).toUtf8()) >= 0;
}

void print(const Vault &vault, const QString &path, const QFont &font, QPrinter *printer)
{
    const Page page = layOut(vault, path, font);
    printer->setDocName(Vault::baseName(path));
    page.reader->document()->print(printer);
}

bool writePdf(const Vault &vault, const QString &path, const QFont &font, const QString &file)
{
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(file);
    printer.setCreator(u"Serat"_s);
    printer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter); // the document keeps its own 2 cm
    print(vault, path, font, &printer);
    return QFileInfo(file).size() > 0;
}

}
