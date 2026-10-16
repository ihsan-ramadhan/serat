#pragma once

#include <QString>

namespace TextMerge {

// The one stretch of `base` that differs from `other`: base[from, to) became `text`.
struct Change {
    int from = 0;
    int to = 0;
    QString text;
    bool none() const { return from == to && text.isEmpty(); }
};
Change between(const QString &base, const QString &other);

// Both sides' line changes applied to `base`, or false when both changed the same text differently.
bool merge(const QString &base, const QString &ours, const QString &theirs, QString *merged);

}
