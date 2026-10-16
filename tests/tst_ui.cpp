// Drives the real main window offscreen, the way a person would: keys, clicks, menus and dialogs.
#include "CommandPalette.h"
#include "Editor.h"
#include "MainWindow.h"
#include "NoteView.h"
#include "Panels.h"
#include "Properties.h"
#include "Reader.h"
#include "RecoveryDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "TableEditor.h"
#include "Theme.h"
#include "Vault.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCompleter>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

using namespace Qt::StringLiterals;

class TestUi : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    MainWindow *m_window = nullptr;
    QString m_vault;
    QTimer m_dialogs;
    QMessageBox::StandardButton m_answer = QMessageBox::Yes;
    QString m_inputAnswer;
    QStringList m_dialogLog; // "title | text" of every message box that came up

    void makeVault(const QString &name, const QList<std::pair<QString, QString>> &files)
    {
        m_vault = m_dir.path() + u'/' + name;
        QDir(m_vault).removeRecursively();
        QDir().mkpath(m_vault);
        for (const auto &[path, text] : files)
            write(path, text);
        m_window->openVault(m_vault);
        QTest::qWait(300);
    }
    void write(const QString &rel, const QString &text) const
    {
        QFile f(m_vault + u'/' + rel);
        QDir().mkpath(QFileInfo(f).absolutePath());
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text.toUtf8());
    }
    QString read(const QString &rel) const
    {
        QFile f(m_vault + u'/' + rel);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }
    bool exists(const QString &rel) const { return QFile::exists(m_vault + u'/' + rel); }
    // The note view holding the keyboard focus, or else the visible one.
    NoteView *view() const
    {
        const auto all = m_window->findChildren<NoteView *>();
        for (NoteView *v : all)
            if (v->isVisible() && QApplication::focusWidget() && v->isAncestorOf(QApplication::focusWidget()))
                return v;
        for (NoteView *v : all)
            if (v->isVisible())
                return v;
        return nullptr;
    }
    Editor *editor() const { return view() ? view()->editor() : nullptr; }
    void open(const QString &path)
    {
        if (view())
            view()->open(path);
        else
            Q_EMIT m_window->findChild<FileTree *>()->openRequested(path, false);
        QTest::qWait(250);
    }
    void reactivate() const
    {
        if (!QApplication::activeWindow()) {
            m_window->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(m_window));
        }
    }
    QWidget *focus() const { return QApplication::focusWidget() ? QApplication::focusWidget() : m_window; }
    void key(Qt::Key k, Qt::KeyboardModifiers mods = {}) const
    {
        reactivate();
        QTest::keyClick(focus(), k, mods);
    }
    void type(const QString &text) const
    {
        reactivate();
        for (const QChar ch : text)
            QTest::keyClick(focus(), char(ch.unicode()));
    }
    QAction *action(const QString &text) const
    {
        for (QAction *a : m_window->actions())
            if (a->text() == text)
                return a;
        return nullptr;
    }
    QString line(int n) const { return editor()->document()->findBlockByNumber(n).text(); }
    QString cursorLine() const { return editor()->textCursor().block().text(); }
    QStringList tabs() const
    {
        QStringList out;
        for (auto *tw : m_window->findChildren<QTabWidget *>())
            if (tw->tabBar()->objectName() == u"notes")
                for (int i = 0; i < tw->count(); ++i)
                    out << tw->tabText(i);
        out.sort();
        return out;
    }
    // Opens a menu with `open`, then triggers the entries along `path`; returns the top level's labels.
    QStringList viaMenu(const std::function<void()> &open, const QStringList &path)
    {
        QStringList labels;
        QTimer::singleShot(150, this, [&labels, path] {
            QMenu *m = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QMenu *root = m;
            if (!m)
                return;
            for (QAction *a : m->actions())
                if (!a->isSeparator())
                    labels << a->text().remove(u'&');
            for (int i = 0; i < path.size() && m; ++i) {
                QAction *hit = nullptr;
                for (QAction *a : m->actions())
                    if (!hit && a->text().remove(u'&').startsWith(path[i]))
                        hit = a;
                if (!hit)
                    break;
                if (i + 1 < path.size()) {
                    m = hit->menu();
                    continue;
                }
                root->hide();
                hit->trigger();
                return;
            }
            root->hide();
        });
        open();
        QTest::qWait(400);
        return labels;
    }
    QLineEdit *propertyValue(PropertiesPanel *panel, const QString &key) const
    {
        const auto edits = panel->findChildren<QLineEdit *>();
        for (int i = 0; i < edits.size(); ++i)
            if (edits[i]->objectName() == u"propertyKey" && edits[i]->text() == key && edits[i]->isVisible())
                for (int j = i + 1; j < edits.size(); ++j)
                    if (edits[j]->isVisible() && edits[j]->objectName() != u"propertyKey")
                        return edits[j];
        return nullptr;
    }
    QStringList propertyKeys(PropertiesPanel *panel) const
    {
        QStringList out;
        for (auto *l : panel->findChildren<QLineEdit *>(u"propertyKey"_s))
            if (l->isVisible())
                out << l->text();
        return out;
    }
    QList<QWidget *> imageBlocks() const
    {
        QList<QWidget *> out;
        for (auto *w : editor()->viewport()->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
            if (w->isVisible() && QLatin1String(w->metaObject()->className()) == "QWidget")
                out << w;
        return out;
    }
    QStringList hiddenBlocks() const
    {
        QStringList out;
        for (QTextBlock b = editor()->document()->begin(); b.isValid(); b = b.next())
            if (!b.isVisible())
                out << QString::number(b.blockNumber());
        return out;
    }
    QModelIndex treeIndex(FileTree *tree, const QString &name) const
    {
        for (int r = 0; r < tree->model()->rowCount(tree->rootIndex()); ++r) {
            const QModelIndex top = tree->model()->index(r, 0, tree->rootIndex());
            if (top.data().toString().startsWith(name))
                return top;
            for (int c = 0; c < tree->model()->rowCount(top); ++c)
                if (tree->model()->index(c, 0, top).data().toString().startsWith(name))
                    return tree->model()->index(c, 0, top);
        }
        return {};
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
        QSettings().setValue(u"theme"_s, u"light"_s);
        Theme::install(*qApp);
        m_window = new MainWindow;
        m_window->resize(1280, 820);
        m_window->show();
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        // Message boxes and input dialogs are logged and answered so no test hangs on one.
        connect(&m_dialogs, &QTimer::timeout, this, [this] {
            QWidget *w = QApplication::activeModalWidget();
            if (auto *box = qobject_cast<QMessageBox *>(w)) {
                m_dialogLog << box->windowTitle() + u" | "_s + box->text();
                if (QAbstractButton *b = box->button(m_answer))
                    b->click();
                else
                    box->done(0);
            } else if (auto *input = qobject_cast<QInputDialog *>(w)) {
                if (m_inputAnswer.isNull()) {
                    input->reject();
                } else {
                    input->setTextValue(m_inputAnswer);
                    input->accept();
                }
            }
        });
        m_dialogs.start(100);
    }

    void cleanupTestCase()
    {
        m_dialogs.stop();
        delete m_window;
    }

    void init()
    {
        m_dialogLog.clear();
        m_answer = QMessageBox::Yes;
        m_inputAnswer = QString();
    }

    void emptyStateAndFirstNote()
    {
        makeVault(u"first"_s, {{u"Welcome.md"_s, u"# Welcome\nSee [[Project Alpha]].\n"_s},
                               {u"Projects/Project Alpha.md"_s, u"# Alpha\n"_s},
                               {u"Archive/Note 1.md"_s, u"old\n"_s}});
        QVERIFY(!view());
        QVERIFY(m_window->findChild<QWidget *>(u"emptyState"_s)->isVisible());

        key(Qt::Key_N, Qt::ControlModifier);
        auto *title = qobject_cast<QLineEdit *>(QApplication::focusWidget());
        QVERIFY(title);
        QCOMPARE(title->text(), u"Untitled"_s);
        type(u"My first note"_s);
        key(Qt::Key_Return);
        QTest::qWait(300);
        QCOMPARE(view()->path(), u"My first note.md"_s);
        QVERIFY(qobject_cast<Editor *>(QApplication::focusWidget()));

        type(u"# Ideas"_s);
        key(Qt::Key_Return);
        type(u"- apples"_s);
        key(Qt::Key_Return);
        type(u"bananas"_s);
        key(Qt::Key_Return);
        key(Qt::Key_Return);
        type(u"See [[Proj"_s);
        QTest::qWait(150);
        auto *completer = editor()->findChild<QCompleter *>();
        QVERIFY(completer->popup()->isVisible());
        QCOMPARE(completer->currentCompletion(), u"Project Alpha"_s);
        QTest::keyClick(completer->popup(), Qt::Key_Return);
        type(u" today."_s);
        QVERIFY(view()->save());
        QCOMPARE(read(u"My first note.md"_s), u"# Ideas\n- apples\n- bananas\n\nSee [[Project Alpha]] today."_s);

        while (view())
            key(Qt::Key_W, Qt::ControlModifier);
        QVERIFY(m_window->findChild<QWidget *>(u"emptyState"_s)->isVisible());
    }

    void linkToMissingNoteIsRemovedWhenLeftEmpty()
    {
        makeVault(u"missing"_s, {{u"Welcome.md"_s, u"# Welcome\nSee [[Missing note]] here.\n"_s}});
        open(u"Welcome.md"_s);
        Q_EMIT editor()->linkActivated(u"wiki:Missing note"_s, false);
        QTest::qWait(300);
        QCOMPARE(view()->path(), u"Missing note.md"_s);
        QVERIFY(exists(u"Missing note.md"_s));
        key(Qt::Key_Left, Qt::AltModifier);
        QTest::qWait(300);
        QCOMPARE(view()->path(), u"Welcome.md"_s);
        QVERIFY(!exists(u"Missing note.md"_s));
    }

    void commandsAndHotkeys()
    {
        makeVault(u"commands"_s, {{u"a.md"_s, u"intro\nkata penting\nlihat [[b]] juga\n"_s}, {u"b.md"_s, u"B\n"_s}});
        open(u"a.md"_s);
        editor()->setFocus();
        editor()->setTextCursor(QTextCursor(editor()->document()->begin()));
        int listed = 0;
        QString top;
        QTimer::singleShot(250, this, [&] {
            auto *d = qobject_cast<CommandPalette *>(QApplication::activeModalWidget());
            if (!d)
                return;
            auto *list = d->findChild<QListWidget *>();
            listed = list->count();
            QTest::keyClicks(d->findChild<QLineEdit *>(), u"heading 2"_s);
            top = list->item(0)->text();
            QTest::keyClick(d->findChild<QLineEdit *>(), Qt::Key_Return);
        });
        key(Qt::Key_P, Qt::ControlModifier);
        QTest::qWait(500);
        QVERIFY(listed > 80);
        QCOMPARE(top, u"Paragraph: Heading 2"_s);
        QCOMPARE(line(0), u"## intro"_s);

        editor()->setTextCursor(QTextCursor(editor()->document()->findBlockByNumber(1)));
        key(Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(line(1), u"- [ ] kata penting"_s);
        key(Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(line(1), u"- [x] kata penting"_s);
        key(Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(line(1), u"- [ ] kata penting"_s);
        QTextCursor c(editor()->document()->findBlockByNumber(1));
        c.movePosition(QTextCursor::EndOfBlock);
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 10);
        editor()->setTextCursor(c);
        key(Qt::Key_Slash, Qt::ControlModifier);
        QCOMPARE(line(1), u"- [ ] %%kata%% penting"_s);
        key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(line(1), u"- [ ] kata penting"_s);

        editor()->setTextCursor(QTextCursor(editor()->document()->findBlockByNumber(2)));
        key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(editor()->toPlainText(), u"## intro\n- [ ] kata penting\n"_s);
    }

    void propertiesPanel()
    {
        makeVault(u"props"_s, {{u"a.md"_s, u"---\ntitle: Hello\ntags:\n  - a\n  - b\ndone: false\ncount: 3\ndue: 2024-05-01\nnested:\n  x: 1\n---\nBody line\n"_s},
                               {u"b.md"_s, u"Plain\n"_s}});
        open(u"a.md"_s);
        auto *panel = editor()->findChild<PropertiesPanel *>();
        QVERIFY(panel && panel->isVisible());
        QCOMPARE(propertyKeys(panel), QStringList({u"title"_s, u"tags"_s, u"done"_s, u"count"_s, u"due"_s, u"nested"_s}));
        QCOMPARE(cursorLine(), u"Body line"_s);

        QLineEdit *title = propertyValue(panel, u"title"_s);
        title->setFocus();
        title->selectAll();
        type(u"World: two"_s);
        key(Qt::Key_Return);
        for (auto *box : panel->findChildren<QAbstractButton *>(u"propertyCheck"_s))
            if (box->isVisible())
                box->click();
        QLineEdit *tags = propertyValue(panel, u"tags"_s);
        tags->setFocus();
        type(u"#c"_s);
        key(Qt::Key_Return);
        for (auto *x : panel->findChildren<QToolButton *>(u"pillRemove"_s))
            if (x->isVisible()) {
                x->click();
                break;
            }
        QTest::qWait(200);
        QLineEdit *due = propertyValue(panel, u"due"_s);
        due->setFocus();
        due->selectAll();
        type(u"2024-13"_s);
        key(Qt::Key_Return);
        QCOMPARE(propertyValue(panel, u"due"_s)->text(), u"2024-05-01"_s);

        editor()->setFocus();
        key(Qt::Key_Semicolon, Qt::ControlModifier);
        auto *name = qobject_cast<QLineEdit *>(QApplication::focusWidget());
        QVERIFY(name);
        QCOMPARE(name->placeholderText(), u"Name"_s);
        type(u"status"_s);
        key(Qt::Key_Return);
        QTest::qWait(250);
        type(u"draft"_s);
        key(Qt::Key_Return);
        QTest::qWait(250);
        QVERIFY(view()->save());
        QCOMPARE(read(u"a.md"_s), u"---\ntitle: \"World: two\"\ntags:\n  - b\n  - c\ndone: true\ncount: 3\ndue: 2024-05-01\nnested:\n  x: 1\nstatus: draft\n---\nBody line\n"_s);

        key(Qt::Key_Escape);
        QVERIFY(qobject_cast<Editor *>(QApplication::focusWidget()));
        QCOMPARE(cursorLine(), u"Body line"_s);
        key(Qt::Key_Up);
        QTest::qWait(100);
        QVERIFY(panel->isAncestorOf(QApplication::focusWidget()));

        // Undo in the note puts the value back and keeps the panel.
        QLineEdit *status = propertyValue(panel, u"status"_s);
        status->setFocus();
        status->selectAll();
        type(u"paused"_s);
        key(Qt::Key_Return);
        QTest::qWait(250);
        editor()->setFocus();
        key(Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(250);
        QVERIFY(panel->isVisible());
        QCOMPARE(propertyValue(panel, u"status"_s)->text(), u"draft"_s);

        // Removing every property removes the frontmatter.
        m_answer = QMessageBox::Yes;
        for (int guard = 0; guard < 12; ++guard) {
            const auto keys = panel->findChildren<QLineEdit *>(u"propertyKey"_s);
            QToolButton *icon = nullptr;
            for (auto *b : panel->findChildren<QToolButton *>())
                if (b->isVisible() && !keys.isEmpty() && b->parentWidget() == keys.last()->parentWidget())
                    icon = b;
            if (!icon)
                break;
            viaMenu([icon] { icon->click(); }, {u"Remove"_s});
        }
        QVERIFY(view()->save());
        QCOMPARE(read(u"a.md"_s), u"Body line\n"_s);

        open(u"b.md"_s);
        editor()->setFocus();
        key(Qt::Key_Semicolon, Qt::ControlModifier);
        key(Qt::Key_Escape);
        QTest::qWait(300);
        QVERIFY(view()->save());
        QCOMPARE(read(u"b.md"_s), u"Plain\n"_s);
    }

    void imagesInEditor()
    {
        makeVault(u"images"_s, {{u"a.md"_s, u"Text\n![[pic.png]]\n\n![small|50](img/pic.png)\nend\n"_s}});
        QImage img(400, 200, QImage::Format_RGB32);
        img.fill(Qt::red);
        QDir().mkpath(m_vault + u"/img"_s);
        QVERIFY(img.save(m_vault + u"/img/pic.png"_s));
        QTest::qWait(600); // the vault picks the new file up on its own
        open(u"a.md"_s);
        QTRY_COMPARE(imageBlocks().size(), 2);
        QCOMPARE(imageBlocks()[0]->size(), QSize(400, 200));
        QCOMPARE(imageBlocks()[1]->size(), QSize(50, 25));

        QWidget *first = imageBlocks().first();
        const QStringList labels = viaMenu([first] {
            QContextMenuEvent ev(QContextMenuEvent::Mouse, QPoint(5, 5), first->mapToGlobal(QPoint(5, 5)));
            QApplication::sendEvent(first, &ev);
        }, {u"Copy path"_s, u"from vault folder"_s});
        QCOMPARE(labels.mid(0, 2), QStringList({u"Copy image"_s, u"Open in new tab"_s}));
        QVERIFY(labels.contains(u"Delete image"_s));
        QCOMPARE(QApplication::clipboard()->text(), u"img/pic.png"_s);

        QTest::mouseClick(first, Qt::LeftButton, {}, QPoint(5, 5));
        QTest::qWait(250);
        QCOMPARE(cursorLine(), u"![[pic.png]]"_s);
        QCOMPARE(imageBlocks().size(), 1);
        key(Qt::Key_Down);
        QTest::qWait(250);
        QCOMPARE(imageBlocks().size(), 2);
    }

    void keyboardThroughPanelsAndImages()
    {
        makeVault(u"keys"_s, {{u"a.md"_s, u"---\ntitle: T\n---\nIntro\n\n> [!tip] Tip\n> body\n\n![[logo.png|60]]\n\nafter\n"_s}});
        QImage img(120, 120, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QVERIFY(img.save(m_vault + u"/logo.png"_s));
        QTest::qWait(600);
        open(u"a.md"_s);
        view()->focusContent();
        auto tap = [this](Qt::Key k, Qt::KeyboardModifiers m = {}) {
            key(k, m);
            QTest::qWait(60);
        };
        QCOMPARE(cursorLine(), u"Intro"_s);
        tap(Qt::Key_Down);
        tap(Qt::Key_Down);
        QCOMPARE(cursorLine(), u"> [!tip] Tip"_s);
        tap(Qt::Key_End, Qt::ControlModifier);
        tap(Qt::Key_Up);
        tap(Qt::Key_Up);
        tap(Qt::Key_Up);
        QCOMPARE(cursorLine(), u"![[logo.png|60]]"_s);
        for (int i = 0; i < 8; ++i)
            tap(Qt::Key_Up);
        QVERIFY(editor()->findChild<PropertiesPanel *>()->isAncestorOf(QApplication::focusWidget()));
    }

    void typingHelpers()
    {
        makeVault(u"typing"_s, {{u"a.md"_s, u"start\n"_s}, {u"b.md"_s, u"---\naliases: [Bee Alias]\n---\nFirst paragraph here\n\nSecond para line\n"_s}});
        open(u"a.md"_s);
        editor()->setFocus();
        editor()->moveCursor(QTextCursor::End);
        type(u"(x)"_s);
        QCOMPARE(cursorLine(), u"(x)"_s);
        type(u" \"q\" `c`"_s);
        QCOMPARE(cursorLine(), u"(x) \"q\" `c`"_s);
        key(Qt::Key_Return);
        type(u"word"_s);
        QTextCursor c = editor()->textCursor();
        c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        editor()->setTextCursor(c);
        type(u"**"_s);
        QCOMPARE(cursorLine(), u"**word**"_s);
        QCOMPARE(editor()->textCursor().selectedText(), u"word"_s);
        type(u"[["_s);
        QCOMPARE(cursorLine(), u"**[[word]]**"_s);

        editor()->moveCursor(QTextCursor::End);
        key(Qt::Key_Return);
        type(u"site"_s);
        c = editor()->textCursor();
        c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        editor()->setTextCursor(c);
        QApplication::clipboard()->setText(u"https://example.com/a"_s);
        key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(cursorLine(), u"[site](https://example.com/a)"_s);

        // Aliases and blocks complete inside [[ ]].
        auto *completer = editor()->findChild<QCompleter *>();
        editor()->moveCursor(QTextCursor::End);
        key(Qt::Key_Return);
        type(u"[[Bee"_s);
        QTest::qWait(150);
        QCOMPARE(completer->currentCompletion(), u"Bee Alias"_s);
        QTest::keyClick(completer->popup(), Qt::Key_Return);
        QCOMPARE(cursorLine(), u"[[b|Bee Alias]]"_s);
        editor()->moveCursor(QTextCursor::End);
        key(Qt::Key_Return);
        type(u"[[b^Second"_s);
        QTest::qWait(150);
        QTest::keyClick(completer->popup(), Qt::Key_Return);
        const QString blockLink = cursorLine();
        QVERIFY2(blockLink.startsWith(u"[[b#^"_s), qPrintable(blockLink));
        const QString id = blockLink.mid(5, blockLink.size() - 7);
        QCOMPARE(read(u"b.md"_s).split(u'\n').value(5), u"Second para line ^"_s + id);

        // A file dragged from the tree is linked where it is dropped.
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(m_vault + u"/b.md"_s)});
        editor()->moveCursor(QTextCursor::End);
        const QPoint at = editor()->cursorRect().center();
        QDragEnterEvent enter(QPointF(at), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, {});
        QApplication::sendEvent(editor()->viewport(), &enter);
        QDragMoveEvent move(QPointF(at), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, {});
        QApplication::sendEvent(editor()->viewport(), &move);
        QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, {});
        QApplication::sendEvent(editor()->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::CopyAction);
        QVERIFY(cursorLine().endsWith(u"[[b]]"_s));

        // Ending a list leaves a blank line so the next paragraph stays out of it.
        editor()->moveCursor(QTextCursor::End);
        key(Qt::Key_Return);
        type(u"- one"_s);
        key(Qt::Key_Return);
        key(Qt::Key_Return);
        type(u"after"_s);
        QVERIFY(editor()->toPlainText().endsWith(u"- one\n\nafter"_s));
    }

    void foldingAndReading()
    {
        makeVault(u"fold"_s, {{u"f.md"_s, u"# One\nbody one\n\n## Sub\nsub body\n\n# Two\n- item\n\t- child\n\t- child2\n- next\n\n> [!note]- Hidden\n> secret\n\nend %%note to self%% visible ^blk1\n_penting_ ^abc\n"_s}});
        open(u"f.md"_s);
        QCOMPARE(hiddenBlocks(), QStringList{u"13"_s}); // the "-" callout starts folded
        editor()->goToLine(0);
        editor()->toggleFoldAtCursor();
        QCOMPARE(hiddenBlocks(), QStringList({u"1"_s, u"2"_s, u"3"_s, u"4"_s, u"13"_s}));
        editor()->goToLine(7);
        editor()->toggleFoldAtCursor();
        QCOMPARE(hiddenBlocks(), QStringList({u"1"_s, u"2"_s, u"3"_s, u"4"_s, u"8"_s, u"9"_s, u"13"_s}));
        editor()->goToLine(4);
        QTest::qWait(100);
        QCOMPARE(hiddenBlocks(), QStringList({u"8"_s, u"9"_s, u"13"_s}));

        // Folds stay on their heading when the file changes outside.
        QVERIFY(view()->save());
        write(u"f.md"_s, read(u"f.md"_s) + u"tail\n"_s);
        QTRY_VERIFY(editor()->toPlainText().endsWith(u"tail\n"_s));
        QCOMPARE(hiddenBlocks(), QStringList({u"8"_s, u"9"_s, u"13"_s}));
        editor()->foldAll(false);
        QVERIFY(hiddenBlocks().isEmpty());

        view()->setMode(NoteView::Read);
        QTest::qWait(250);
        auto *reader = view()->findChild<Reader *>();
        QString shown = reader->toPlainText();
        QVERIFY(!shown.contains(u"note to self"_s));
        QVERIFY(!shown.contains(u"blk1"_s));
        QVERIFY(shown.contains(u"visible"_s));
        QVERIFY(!shown.contains(u"secret"_s));
        QVERIFY(reader->toHtml().contains(u"penting"_s));
        Q_EMIT reader->anchorClicked(QUrl(u"callout:0"_s));
        QTest::qWait(250);
        QVERIFY(reader->toPlainText().contains(u"secret"_s));
        view()->setMode(NoteView::Edit);
    }

    void searchTagsAndBookmarks()
    {
        makeVault(u"search"_s, {{u"work/plan.md"_s, u"---\ntags: [project/alpha]\n---\n# Goals\nShip the Editor [[list]]\n- [ ] write docs\n- [x] fix bug #urgent\n"_s},
                                {u"home/list.md"_s, u"Buy milk\n- [ ] call mom #urgent\n"_s},
                                {u".obsidian/bookmarks.json"_s, u"{\"items\":[{\"type\":\"file\",\"path\":\"home/list.md\"},{\"type\":\"search\",\"query\":\"milk\"}]}"_s}});
        auto *search = m_window->findChild<SearchPanel *>();
        auto *results = search->findChild<QTreeWidget *>();
        auto files = [&](const QString &q) {
            search->search(q);
            QStringList out;
            for (int i = 0; i < results->topLevelItemCount(); ++i)
                out << results->topLevelItem(i)->text(0) + u'(' + QString::number(results->topLevelItem(i)->childCount()) + u')';
            return out.join(u", "_s);
        };
        QCOMPARE(files(u"task-todo:"_s), u"list(1), plan(1)"_s);
        QCOMPARE(files(u"tag:#urgent"_s), u"list(1), plan(1)"_s);
        QCOMPARE(files(u"line:(ship editor)"_s), u"plan(1)"_s);
        QCOMPARE(files(u"editor -milk"_s), u"plan(1)"_s);
        QCOMPARE(files(u"/c[ao]ll/"_s), u"list(1)"_s);
        QCOMPARE(files(u"/(/"_s), QString());
        QCOMPARE(files(u"((((("_s), QString());
        QToolButton *matchCase = nullptr;
        for (auto *b : search->findChildren<QToolButton *>())
            if (b->toolTip() == u"Match case")
                matchCase = b;
        search->search(u"editor"_s);
        matchCase->setChecked(true);
        QCOMPARE(results->topLevelItemCount(), 0);
        matchCase->setChecked(false);
        QCOMPARE(search->findChildren<QLabel *>().value(0)->text(), u"1 file"_s);

        auto *tags = m_window->findChild<TagsPanel *>();
        QStringList rows;
        tags->expandAll();
        for (QTreeWidgetItemIterator it(tags); *it; ++it)
            rows << (*it)->text(0) + u' ' + (*it)->text(1);
        QCOMPARE(rows, QStringList({u"#project 1"_s, u"#alpha 1"_s, u"#urgent 2"_s}));
        Q_EMIT tags->itemClicked(tags->topLevelItem(0), 0);
        QCOMPARE(search->query(), u"tag:#project"_s);

        auto *bookmarks = m_window->findChild<BookmarksPanel *>();
        auto marks = [bookmarks] {
            QStringList out;
            for (int i = 0; i < bookmarks->topLevelItemCount(); ++i)
                out << bookmarks->topLevelItem(i)->text(0);
            return out;
        };
        QCOMPARE(marks(), QStringList({u"list"_s, u"milk"_s}));
        auto *tree = m_window->findChild<FileTree *>();
        tree->setExpandedFolders({u"work"_s, u"home"_s});
        QTest::qWait(500);
        const QModelIndex plan = treeIndex(tree, u"plan"_s);
        QVERIFY(plan.isValid());
        const QPoint planPos = tree->visualRect(plan).center();
        viaMenu([tree, planPos] { Q_EMIT tree->customContextMenuRequested(planPos); }, {u"Bookmark"_s});
        QCOMPARE(marks(), QStringList({u"list"_s, u"milk"_s, u"plan"_s}));

        // Shift+click picks a second file; a right-click keeps both for the group menu.
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, planPos);
        const QString openBefore = view()->path();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ShiftModifier, tree->visualRect(treeIndex(tree, u"list"_s)).center());
        QCOMPARE(view()->path(), openBefore);
        QCOMPARE(tree->selectedPaths().size(), 2);
        QTest::mousePress(tree->viewport(), Qt::RightButton, {}, planPos);
        QTest::mouseRelease(tree->viewport(), Qt::RightButton, {}, planPos);
        QCOMPARE(tree->selectedPaths().size(), 2);
        const QStringList group = viaMenu([tree, planPos] { Q_EMIT tree->customContextMenuRequested(planPos); }, {});
        QCOMPARE(group, QStringList({u"Move 2 items to..."_s, u"Delete 2 items"_s}));
    }

    void splitPinPreviewAndSync()
    {
        makeVault(u"split"_s, {{u"n.md"_s, u"# H\nbody [[t]]\n\n# K\nmore\n"_s}, {u"t.md"_s, u"Target para\n"_s}, {u"a.md"_s, u"x\n"_s}});
        open(u"n.md"_s);
        action(u"Split right"_s)->trigger();
        QTest::qWait(250);
        QList<NoteView *> same;
        for (NoteView *v : m_window->findChildren<NoteView *>())
            if (v->path() == u"n.md")
                same << v;
        QCOMPARE(same.size(), 2);

        // Saving in one pane updates the other, so neither overwrites the other's edit.
        QTextCursor c(same[0]->editor()->document()->findBlockByNumber(1));
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertText(u" LEFT"_s);
        QVERIFY(same[0]->save());
        QCOMPARE(same[1]->editor()->document()->findBlockByNumber(1).text(), u"body [[t]] LEFT"_s);
        QTextCursor r(same[1]->editor()->document()->findBlockByNumber(4));
        r.movePosition(QTextCursor::EndOfBlock);
        r.insertText(u" RIGHT"_s);
        QVERIFY(same[1]->save());
        QCOMPARE(read(u"n.md"_s), u"# H\nbody [[t]] LEFT\n\n# K\nmore RIGHT\n"_s);

        // Links followed from a pinned tab open in a new one.
        NoteView *pinned = same[1];
        pinned->setPinned(true);
        pinned->editor()->setFocus(); // a click on the link puts the focus in that pane first
        QTest::qWait(50);
        Q_EMIT pinned->editor()->linkActivated(u"wiki:a"_s, false);
        QTest::qWait(250);
        QCOMPARE(pinned->path(), u"n.md"_s);
        QCOMPARE(same[0]->path(), u"n.md"_s);
        QVERIFY(tabs().contains(u"a"_s));

        // Ctrl over a link shows the note it points to.
        Editor *e = same[0]->editor();
        QTextCursor at(e->document()->findBlockByNumber(1));
        at.setPosition(at.block().position() + 7);
        const QPoint p = e->cursorRect(at).center();
        QMouseEvent hover(QEvent::MouseMove, p, e->viewport()->mapToGlobal(p), Qt::NoButton, Qt::NoButton, Qt::ControlModifier);
        QApplication::sendEvent(e->viewport(), &hover);
        auto *popup = static_cast<PreviewPopup *>(m_window->findChild<QFrame *>(u"preview"_s));
        QTRY_VERIFY(popup->isVisible());
        QVERIFY(popup->reader()->toPlainText().contains(u"Target para"_s));
        popup->hide();

        // A block id for a note that is open in a tab goes into that tab's text.
        open(u"t.md"_s);
        NoteView *target = view();
        Q_EMIT target->editor()->linkActivated(u"wiki:a"_s, true);
        QTest::qWait(250);
        editor()->setFocus();
        editor()->moveCursor(QTextCursor::End);
        type(u"[[t^Target"_s);
        QTest::qWait(150);
        QTest::keyClick(editor()->findChild<QCompleter *>()->popup(), Qt::Key_Return);
        QVERIFY(target->editor()->document()->firstBlock().text().startsWith(u"Target para ^"_s));
        QVERIFY(target->save());
        QVERIFY(read(u"t.md"_s).startsWith(u"Target para ^"_s));
    }

    void dailyNotesTemplatesAndSettings()
    {
        makeVault(u"daily"_s, {{u"Templates/Daily.md"_s, u"# {{title}}\nCreated {{date:YYYY}}\n"_s},
                               {u"Templates/Meeting.md"_s, u"## Meeting {{date:YYYY}}\n"_s},
                               {u".obsidian/daily-notes.json"_s, u"{\"folder\":\"Journal\",\"format\":\"YYYY/MM/YYYY-MM-DD\",\"template\":\"Templates/Daily\"}"_s},
                               {u".obsidian/templates.json"_s, u"{\"folder\":\"Templates\"}"_s},
                               {u"a.md"_s, u"start\n"_s}});
        const QDate today = QDate::currentDate();
        action(u"Open today's daily note"_s)->trigger();
        QTest::qWait(300);
        const QString daily = u"Journal/%1/%2.md"_s.arg(today.toString(u"yyyy/MM"_s), today.toString(Qt::ISODate));
        QCOMPARE(view()->path(), daily);
        QCOMPARE(read(daily), u"# %1\nCreated %2\n"_s.arg(today.toString(Qt::ISODate)).arg(today.year()));

        open(u"a.md"_s);
        editor()->setFocus();
        editor()->moveCursor(QTextCursor::End);
        QStringList offered;
        QTimer::singleShot(250, this, [&offered] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!d)
                return;
            auto *list = d->findChild<QListWidget *>();
            for (int i = 0; i < list->count(); ++i)
                offered << list->item(i)->text();
            QTest::keyClicks(d->findChild<QLineEdit *>(), u"meet"_s);
            QTest::keyClick(d->findChild<QLineEdit *>(), Qt::Key_Return);
        });
        action(u"Insert template…"_s)->trigger();
        QTest::qWait(300);
        QCOMPARE(offered, QStringList({u"Daily"_s, u"Meeting"_s}));
        QCOMPARE(editor()->toPlainText(), u"start\n## Meeting %1\n"_s.arg(today.year()));

        // A format with characters files cannot have still makes a usable name.
        Settings::setVault(*m_window->findChild<Vault *>(), u"dailyFormat"_s, u"YYYY:MM:DD"_s);
        action(u"Open today's daily note"_s)->trigger();
        QTest::qWait(300);
        QCOMPARE(view()->path(), u"Journal/%1.md"_s.arg(today.toString(u"yyyy-MM-dd"_s)));

        QStringList sections;
        QTimer::singleShot(250, this, [&sections] {
            auto *d = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            if (!d)
                return;
            auto *list = d->findChild<QListWidget *>();
            for (int i = 0; i < list->count(); ++i)
                sections << list->item(i)->text();
            for (auto *box : d->findChildren<QCheckBox *>())
                if (box->text() == u"Auto pair brackets")
                    box->setChecked(false);
            d->reject();
        });
        action(u"Settings…"_s)->trigger();
        QTest::qWait(300);
        QCOMPARE(sections, QStringList({u"Editor"_s, u"Files and links"_s, u"Appearance"_s, u"Daily notes"_s, u"Templates"_s}));
        open(u"a.md"_s);
        editor()->setFocus();
        editor()->moveCursor(QTextCursor::End);
        type(u"(z"_s);
        QCOMPARE(cursorLine(), u"(z"_s);
        QSettings().setValue(u"autoPairBrackets"_s, true);
    }

    void openFoldersComeBack()
    {
        makeVault(u"folders"_s, {{u"a/b/deep.md"_s, u"x"_s}, {u"a/top.md"_s, u"x"_s}, {u"c/n.md"_s, u"x"_s}});
        const QString vault = m_vault;
        auto *tree = m_window->findChild<FileTree *>();
        tree->setExpandedFolders({u"a"_s, u"a/b"_s, u"c"_s});
        QTest::qWait(600);
        tree->collapse(tree->model()->index(0, 0, tree->rootIndex()));
        QCOMPARE(tree->expandedFolders(), QStringList({u"a/b"_s, u"c"_s}));
        makeVault(u"elsewhere"_s, {{u"z.md"_s, u"z"_s}});
        m_window->openVault(vault);
        m_vault = vault;
        QTest::qWait(800);
        QCOMPARE(tree->expandedFolders(), QStringList({u"a/b"_s, u"c"_s}));
    }

    void tableCellsKeepTyping()
    {
        makeVault(u"table"_s, {{u"t.md"_s, u"x\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n"_s}, {u"o.md"_s, u"o\n"_s}});
        open(u"t.md"_s);
        TableOverlay *overlay = nullptr;
        for (auto *o : editor()->findChildren<TableOverlay *>())
            if (o->isVisible())
                overlay = o;
        QVERIFY(overlay);
        auto *grid = overlay->findChild<QTableWidget *>(u"mdTable"_s);
        grid->setFocus();
        grid->setCurrentCell(1, 0);
        key(Qt::Key_S);
        type(u"atu"_s);
        key(Qt::Key_B, Qt::ControlModifier);
        key(Qt::Key_W, Qt::ControlModifier);
        QTest::qWait(500);
        QCOMPARE(read(u"t.md"_s), u"x\n\n| A | B |\n| --- | --- |\n| satu | 2 |\n"_s);
    }

    void namesThatCannotBeFiles()
    {
        makeVault(u"names"_s, {{u"Welcome.md"_s, u"w\n"_s}, {u"Other.md"_s, u"o\n"_s}});
        open(u"Welcome.md"_s);
        auto rename = [this](const QString &name) {
            reactivate();
            view()->focusTitle();
            auto *title = view()->findChildren<QLineEdit *>().value(0);
            QVERIFY(title && title->hasFocus());
            title->setText(name);
            key(Qt::Key_Return);
            QTest::qWait(350);
        };
        const QStringList bad = {u"a/b"_s, u"<script>"_s, QString(300, u'x'), u".hidden"_s, u"trailing."_s, u"Other"_s};
        for (const QString &name : bad)
            rename(name);
        QCOMPARE(view()->path(), u"Welcome.md"_s);
        QCOMPARE(m_dialogLog.size(), bad.size());
        QVERIFY(m_dialogLog[0].contains(u"cannot contain"_s));
        QVERIFY(m_dialogLog[2].contains(u"too long"_s));
        QVERIFY(m_dialogLog[3].contains(u"start with a dot"_s));
        QVERIFY(m_dialogLog[4].contains(u"end with a dot"_s));
        QVERIFY(m_dialogLog[5].contains(u"already exists"_s));
        rename(u"Party 🎉 ünï"_s);
        QCOMPARE(view()->path(), u"Party 🎉 ünï.md"_s);
    }

    void outsideChangesKeepEveryonesWords()
    {
        makeVault(u"outside"_s, {{u"Other.md"_s, u"other\n"_s}, {u"Welcome.md"_s, u"# Welcome\n\nend\n"_s}, {u"c.md"_s, u"alpha\nbeta\ngamma\n"_s}});
        // Deleted outside while it has unsaved typing: written back, tab stays.
        open(u"Other.md"_s);
        editor()->setFocus();
        editor()->moveCursor(QTextCursor::End);
        type(u"unsaved words"_s);
        QFile::remove(m_vault + u"/Other.md"_s);
        QTRY_VERIFY_WITH_TIMEOUT(exists(u"Other.md"_s), 3000);
        QCOMPARE(view()->path(), u"Other.md"_s);
        QVERIFY(read(u"Other.md"_s).contains(u"unsaved words"_s));

        // Changed outside while typing elsewhere in the note: both kept.
        open(u"Welcome.md"_s);
        editor()->setFocus();
        editor()->setTextCursor(QTextCursor(editor()->document()->findBlockByNumber(1)));
        type(u"local edit"_s);
        write(u"Welcome.md"_s, u"# Welcome\n\nend\nremote edit\n"_s);
        QTRY_VERIFY_WITH_TIMEOUT(editor()->toPlainText().contains(u"remote edit"_s), 3000);
        QVERIFY(editor()->toPlainText().contains(u"local edit"_s));
        QVERIFY(view()->save());
        QCOMPARE(read(u"Welcome.md"_s), u"# Welcome\nlocal edit\nend\nremote edit\n"_s);

        // Both changed the same words: ours stays, theirs goes to a conflict copy.
        open(u"c.md"_s);
        editor()->setFocus();
        QTextCursor c(editor()->document()->findBlockByNumber(1));
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        editor()->setTextCursor(c);
        type(u"mine"_s);
        write(u"c.md"_s, u"alpha\ntheirs\ngamma\n"_s);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->statusBar()->currentMessage().contains(u"conflict"_s), 3000);
        QCOMPARE(line(1), u"mine"_s);
        const QStringList copies = QDir(m_vault).entryList({u"c (conflict*"_s}, QDir::Files);
        QCOMPARE(copies.size(), 1);
        QCOMPARE(read(copies.first()), u"alpha\ntheirs\ngamma\n"_s);
    }

    void fileRecovery()
    {
        makeVault(u"recovery"_s, {{u"a.md"_s, u"first version\n"_s}, {u"gone.md"_s, u"keep me\n"_s}});
        open(u"a.md"_s);
        editor()->setFocus();
        editor()->moveCursor(QTextCursor::End);
        type(u"second"_s);
        QVERIFY(view()->save());

        // Back to the version kept when the note was opened.
        QTimer::singleShot(300, this, [] {
            auto *d = qobject_cast<RecoveryDialog *>(QApplication::activeModalWidget());
            if (!d)
                return;
            auto *versions = d->findChildren<QListWidget *>().value(1);
            versions->setCurrentRow(versions->count() - 1);
            for (auto *b : d->findChildren<QPushButton *>())
                if (b->text() == u"Restore")
                    b->click();
        });
        action(u"File recovery…"_s)->trigger();
        QTest::qWait(400);
        QCOMPARE(read(u"a.md"_s), u"first version\n"_s);
        QCOMPARE(editor()->toPlainText(), u"first version\n"_s);

        // A deleted note can be brought back.
        m_answer = QMessageBox::Yes;
        open(u"gone.md"_s);
        action(u"Delete current file"_s)->trigger();
        QTest::qWait(400);
        QVERIFY(!exists(u"gone.md"_s));
        QStringList listed;
        QTimer::singleShot(300, this, [&listed] {
            auto *d = qobject_cast<RecoveryDialog *>(QApplication::activeModalWidget());
            if (!d)
                return;
            auto *notes = d->findChildren<QListWidget *>().value(0);
            for (int i = 0; i < notes->count(); ++i) {
                listed << notes->item(i)->text();
                if (notes->item(i)->text().startsWith(u"gone.md"_s))
                    notes->setCurrentRow(i);
            }
            for (auto *b : d->findChildren<QPushButton *>())
                if (b->text() == u"Restore")
                    b->click();
        });
        action(u"File recovery…"_s)->trigger();
        QTest::qWait(400);
        QVERIFY2(listed.contains(u"gone.md (deleted)"_s), qPrintable(listed.join(u", "_s)));
        QCOMPARE(read(u"gone.md"_s), u"keep me\n"_s);
    }
};

QTEST_MAIN(TestUi)
#include "tst_ui.moc"
