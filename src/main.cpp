#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>

using namespace Qt::StringLiterals;

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
    parser.addPositionalArgument(u"vault"_s, u"Folder to open as a vault."_s);
    parser.process(app);

    MainWindow window;
    QString vault = parser.positionalArguments().value(0);
    if (vault.isEmpty())
        vault = QSettings().value(u"lastVault"_s).toString();
    if (!vault.isEmpty() && QFileInfo(vault).isDir())
        window.openVault(QFileInfo(vault).absoluteFilePath());
    else
        window.showWelcome();
    window.show();
    return app.exec();
}
