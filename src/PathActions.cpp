#include "PathActions.h"

#include "Vault.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMenu>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace PathActions {

namespace {

// Both apps take the note path without ".md".
QUrl appUrl(const QString &scheme, const Vault &vault, const QString &path)
{
    QUrlQuery query;
    query.addQueryItem(u"vault"_s, vault.name());
    query.addQueryItem(u"file"_s, Vault::isNote(path) ? path.chopped(3) : path);
    QUrl url;
    url.setScheme(scheme);
    url.setHost(u"open"_s);
    url.setQuery(query);
    return url;
}

} // namespace

QUrl seratUrl(const Vault &vault, const QString &path)
{
    return appUrl(u"serat"_s, vault, path);
}

QUrl obsidianUrl(const Vault &vault, const QString &path)
{
    return appUrl(u"obsidian"_s, vault, path);
}

void addCopyMenu(QMenu *menu, const Vault &vault, const QString &path)
{
    QMenu *copy = menu->addMenu(QApplication::translate("PathActions", "Copy path"));
    auto add = [copy](const QString &label, const QString &text) {
        copy->addAction(label, copy, [text] { QApplication::clipboard()->setText(text); });
    };
    const bool dir = vault.isDir(path);
    if (!dir) {
        add(QApplication::translate("PathActions", "as Obsidian URL"), obsidianUrl(vault, path).toString(QUrl::FullyEncoded));
        add(QApplication::translate("PathActions", "as Serat link"), seratUrl(vault, path).toString(QUrl::FullyEncoded));
    }
    add(QApplication::translate("PathActions", "from vault folder"), path);
    add(QApplication::translate("PathActions", "from system root"), QDir::toNativeSeparators(vault.absolutePath(path)));
    if (dir)
        return;
    copy->addSeparator();
    const QString link = u"[["_s + vault.linkText(path) + u"]]"_s;
    if (Vault::isImage(path))
        add(QApplication::translate("PathActions", "as embed"), u'!' + link);
    else
        add(QApplication::translate("PathActions", "as wikilink"), link);
}

void addOpenActions(QMenu *menu, QWidget *context, const std::function<void(bool newTab)> &open)
{
    menu->addAction(QApplication::translate("PathActions", "Open"), context, [open] { open(false); });
    menu->addAction(QApplication::translate("PathActions", "Open in new tab"), context, [open] { open(true); });
}

QString hrefText(const QString &href)
{
    if (href.startsWith(u"wiki:"_s))
        return u"[["_s + QUrl::fromPercentEncoding(href.mid(5).toUtf8()) + u"]]"_s;
    return href;
}

} // namespace PathActions
