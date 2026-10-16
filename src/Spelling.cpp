#include "Spelling.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QStringEncoder>

#ifdef SERAT_HAVE_HUNSPELL
#include <hunspell.hxx>
#endif

using namespace Qt::StringLiterals;

struct Spelling::Dictionary {
#ifdef SERAT_HAVE_HUNSPELL
    std::unique_ptr<Hunspell> hunspell;
#endif
    QStringConverter::Encoding encoding = QStringConverter::Utf8;
};

Spelling &Spelling::instance()
{
    static Spelling spelling;
    return spelling;
}

Spelling::Spelling()
{
    QFile f(personalFile());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        for (const QString &w : QString::fromUtf8(f.readAll()).split(u'\n', Qt::SkipEmptyParts))
            m_personal.insert(w.trimmed());
}

Spelling::~Spelling() = default;

bool Spelling::supported()
{
#ifdef SERAT_HAVE_HUNSPELL
    return true;
#else
    return false;
#endif
}

QString Spelling::personalFile() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/dictionary.txt"_s;
}

QHash<QString, QString> Spelling::found() const
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList dirs = {userDictionaryDir(), app + u"/dictionaries"_s, app + u"/../share/serat/dictionaries"_s, app + u"/../Resources/dictionaries"_s};
    dirs += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, u"serat/dictionaries"_s, QStandardPaths::LocateDirectory);
    dirs += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, u"hunspell"_s, QStandardPaths::LocateDirectory);
    dirs += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, u"myspell/dicts"_s, QStandardPaths::LocateDirectory);
    QHash<QString, QString> out;
    for (const QString &dir : std::as_const(dirs)) {
        for (const QFileInfo &aff : QDir(dir).entryInfoList({u"*.aff"_s}, QDir::Files)) {
            const QString base = aff.absolutePath() + u'/' + aff.completeBaseName();
            // Hyphenation and thesaurus files share the folder; only names like en_US or id are dictionaries.
            if (!out.contains(aff.completeBaseName()) && QFile::exists(base + u".dic"_s) && !aff.completeBaseName().contains(u'-'))
                out.insert(aff.completeBaseName(), base);
        }
    }
    return out;
}

QStringList Spelling::available() const
{
    QStringList names = found().keys();
    names.sort();
    return names;
}

QString Spelling::displayName(const QString &dictionary)
{
    const QLocale locale(dictionary);
    if (locale.language() == QLocale::C)
        return dictionary;
    QString name = QLocale::languageToString(locale.language());
    if (dictionary.contains(u'_'))
        name += u" (%1)"_s.arg(QLocale::territoryToString(locale.territory()));
    return name;
}

QString Spelling::userDictionaryDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/dictionaries"_s;
}

QStringList Spelling::languages() const
{
    const QStringList all = available();
    QStringList fallback;
    for (const QString &name : {u"en_US"_s, QLocale::system().name()})
        if (all.contains(name) && !fallback.contains(name))
            fallback << name;
    return Settings::app(u"spellLanguages"_s, fallback).toStringList();
}

void Spelling::reload()
{
    QStringList wanted;
    if (supported() && Settings::app(u"spellcheck"_s, true).toBool())
        wanted = languages();
    if (wanted == m_loadedNames)
        return;
    m_loaded.clear();
    m_cache.clear();
    m_loadedNames = wanted;
#ifdef SERAT_HAVE_HUNSPELL
    const QHash<QString, QString> paths = found();
    for (const QString &name : std::as_const(wanted)) {
        const QString base = paths.value(name);
        if (base.isEmpty())
            continue;
        auto d = std::make_unique<Dictionary>();
        d->hunspell = std::make_unique<Hunspell>(QFile::encodeName(base + u".aff"_s).constData(),
                                                 QFile::encodeName(base + u".dic"_s).constData());
        const QString encoding = QString::fromStdString(d->hunspell->get_dict_encoding()).toUpper();
        if (encoding.startsWith(u"ISO8859-1"_s) || encoding.startsWith(u"ISO-8859-1"_s))
            d->encoding = QStringConverter::Latin1;
        else if (const auto e = QStringConverter::encodingForName(encoding.toLatin1().constData()))
            d->encoding = *e;
        else
            continue;
        m_loaded.push_back(std::move(d));
    }
#endif
    Q_EMIT changed();
}

bool Spelling::active() const
{
    return !m_loaded.empty();
}

bool Spelling::check(const QString &word)
{
#ifdef SERAT_HAVE_HUNSPELL
    if (m_loaded.empty() || m_personal.contains(word) || m_personal.contains(word.toLower()))
        return true;
    if (const auto it = m_cache.constFind(word); it != m_cache.constEnd())
        return *it;
    bool ok = false;
    for (const auto &d : m_loaded) {
        QStringEncoder encode(d->encoding);
        const QByteArray bytes = encode(word);
        if (!encode.hasError() && d->hunspell->spell(bytes.toStdString())) {
            ok = true;
            break;
        }
    }
    if (m_cache.size() > 50000)
        m_cache.clear();
    m_cache.insert(word, ok);
    return ok;
#else
    Q_UNUSED(word)
    return true;
#endif
}

QStringList Spelling::suggestions(const QString &word)
{
    QStringList out;
#ifdef SERAT_HAVE_HUNSPELL
    for (const auto &d : m_loaded) {
        QStringEncoder encode(d->encoding);
        const QByteArray bytes = encode(word);
        if (encode.hasError())
            continue;
        for (const std::string &s : d->hunspell->suggest(bytes.toStdString())) {
            QStringDecoder decode(d->encoding);
            const QString suggestion = decode(QByteArrayView(s.data(), qsizetype(s.size())));
            if (!out.contains(suggestion))
                out << suggestion;
        }
    }
#else
    Q_UNUSED(word)
#endif
    return out.mid(0, 7);
}

void Spelling::addWord(const QString &word)
{
    if (word.isEmpty() || m_personal.contains(word))
        return;
    m_personal.insert(word);
    m_cache.remove(word);
    QDir().mkpath(QFileInfo(personalFile()).absolutePath());
    QFile f(personalFile());
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((word + u'\n').toUtf8());
    Q_EMIT changed();
}
