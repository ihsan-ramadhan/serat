#pragma once

#include <QList>
#include <QString>

#include <memory>

struct Note;

// Words (all must match), "phrases", /regex/, -not, OR, ( ) and file: path: tag: content: line: section: task:.
class SearchQuery
{
public:
    explicit SearchQuery(const QString &query, bool matchCase = false);
    ~SearchQuery();

    bool isEmpty() const;
    bool matches(const Note &note) const;
    // Lines to list under a matching file; empty when only file-level terms matched.
    QList<int> hitLines(const Note &note) const;

    struct Node;

private:
    std::shared_ptr<Node> m_root;
};
