#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QStandardPaths>
#include <QTranslator>

using namespace Qt::StringLiterals;

namespace {

// macOS delivers serat:// links as a FileOpen event instead of an argument.
class UrlOpener : public QObject
{
public:
    explicit UrlOpener(MainWindow *window)
        : m_window(window)
    {
    }

protected:
    bool eventFilter(QObject *obj, QEvent *e) override
    {
        if (e->type() == QEvent::FileOpen) {
            m_window->handleArgument(static_cast<QFileOpenEvent *>(e)->url().toString());
            return true;
        }
        return QObject::eventFilter(obj, e);
    }

private:
    MainWindow *m_window;
};

void registerUrlScheme()
{
#ifdef Q_OS_WIN
    QSettings classes(u"HKEY_CURRENT_USER\\Software\\Classes\\serat"_s, QSettings::NativeFormat);
    classes.setValue(u"Default"_s, u"URL:Serat link"_s);
    classes.setValue(u"URL Protocol"_s, QString());
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    classes.setValue(u"shell/open/command/Default"_s, u"\"%1\" \"%2\""_s.arg(exe, u"%1"_s));
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(u"Serat"_s);
    QApplication::setOrganizationName(u"Serat"_s);
    QApplication::setApplicationVersion(QStringLiteral(SERAT_VERSION));
    QApplication::setDesktopFileName(u"id.icqn.Serat"_s);
    QApplication::setWindowIcon(QIcon(u":/serat.svg"_s));

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Native Markdown notes, compatible with Obsidian vaults."_s);
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(u"vault"_s, u"Folder to open as a vault, or a serat:// link."_s);
    parser.process(app);
    const QString arg = parser.positionalArguments().value(0);

    // One window per settings profile: a second launch hands its argument over and exits.
    const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString server = u"serat-"_s + QString::fromLatin1(QCryptographicHash::hash(config.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    QLocalSocket running;
    running.connectToServer(server);
    if (running.waitForConnected(500)) {
        running.write(arg.toUtf8() + '\n');
        running.waitForBytesWritten(1000);
        return 0;
    }

    // The interface language: "system" follows the desktop, otherwise "en" or "id".
    const QString language = QSettings().value(u"language"_s, u"system"_s).toString();
    const QLocale locale = language == u"system" ? QLocale::system() : QLocale(language);
    QTranslator serat, qt;
    if (serat.load(locale, u"serat"_s, u"_"_s, u":/i18n"_s))
        app.installTranslator(&serat);
    if (qt.load(locale, u"qt"_s, u"_"_s, u":/i18n"_s))
        app.installTranslator(&qt);

    Theme::install(app);
    registerUrlScheme();

    MainWindow window;
    QLocalServer::removeServer(server);
    QLocalServer listener;
    listener.listen(server);
    QObject::connect(&listener, &QLocalServer::newConnection, &window, [&listener, &window] {
        QLocalSocket *client = listener.nextPendingConnection();
        QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
        QObject::connect(client, &QLocalSocket::readyRead, &window, [client, &window] {
            if (client->canReadLine())
                window.handleArgument(QString::fromUtf8(client->readLine()).trimmed());
        });
    });
    UrlOpener opener(&window);
    app.installEventFilter(&opener);

    const bool link = arg.startsWith(u"serat:"_s, Qt::CaseInsensitive);
    QString vault = link || arg.isEmpty() ? QSettings().value(u"lastVault"_s).toString() : arg;
    if (vault.startsWith(u"file:"_s))
        vault = QUrl(vault).toLocalFile();
    if (!vault.isEmpty() && QFileInfo(vault).isDir())
        window.openVault(QFileInfo(vault).absoluteFilePath());
    else
        window.showWelcome();
    window.show();
    if (link)
        window.openUrl(QUrl(arg));
    return app.exec();
}
