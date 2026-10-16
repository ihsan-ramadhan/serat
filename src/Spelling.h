#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <memory>
#include <vector>

// Hunspell spellchecking with the system's dictionaries, Serat's own and any in the app data folder; without Hunspell every word passes.
class Spelling : public QObject
{
    Q_OBJECT
public:
    static Spelling &instance();
    static bool supported();
    // Dictionary names such as "en_US", each with a .aff and .dic file.
    QStringList available() const;
    static QString displayName(const QString &dictionary);
    // From the settings: on or off, and which dictionaries.
    void reload();
    bool active() const;
    // The dictionaries in use when spellchecking is on: the chosen ones, or English and the system's language.
    QStringList languages() const;
    static QString userDictionaryDir();

    bool check(const QString &word);
    QStringList suggestions(const QString &word);
    void addWord(const QString &word);

Q_SIGNALS:
    void changed();

private:
    Spelling();
    ~Spelling() override;
    QHash<QString, QString> found() const; // name -> path without extension
    QString personalFile() const;

    struct Dictionary;
    std::vector<std::unique_ptr<Dictionary>> m_loaded;
    QStringList m_loadedNames;
    QSet<QString> m_personal;
    QHash<QString, bool> m_cache;
};
