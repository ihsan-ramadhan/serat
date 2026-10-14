#pragma once

#include <QUrl>

#include <functional>

class QMenu;
class QWidget;
class Vault;

namespace PathActions {

QUrl seratUrl(const Vault &vault, const QString &path);
QUrl obsidianUrl(const Vault &vault, const QString &path);
// Wikilink (or embed, for images), vault path, full path and app links.
void addCopyMenu(QMenu *menu, const Vault &vault, const QString &path);
void addOpenActions(QMenu *menu, QWidget *context, const std::function<void(bool newTab)> &open);
// What "Copy link" puts on the clipboard for a rendered href: [[target]] or the URL.
QString hrefText(const QString &href);

}
