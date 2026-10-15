#include "TextMerge.h"

namespace TextMerge {

Change between(const QString &base, const QString &other)
{
    const int b = int(base.size()), o = int(other.size());
    int prefix = 0;
    while (prefix < b && prefix < o && base[prefix] == other[prefix])
        ++prefix;
    int suffix = 0;
    while (suffix < b - prefix && suffix < o - prefix && base[b - 1 - suffix] == other[o - 1 - suffix])
        ++suffix;
    return {prefix, b - suffix, other.mid(prefix, o - suffix - prefix)};
}

bool merge(const QString &base, const QString &ours, const QString &theirs, QString *merged)
{
    const Change a = between(base, ours);
    const Change b = between(base, theirs);
    if (a.none() || b.none() || ours == theirs) {
        *merged = a.none() ? theirs : ours;
        return true;
    }
    // Both typed at the same spot: keep both, theirs first.
    if (a.from == a.to && b.from == b.to && a.from == b.from) {
        *merged = base.left(a.from) + b.text + a.text + base.mid(a.from);
        return true;
    }
    if (!(a.to <= b.from || b.to <= a.from))
        return false;
    // The later change goes in first so the earlier one's offsets still hold.
    const Change &later = b.from >= a.to ? b : a;
    const Change &earlier = &later == &b ? a : b;
    QString out = base;
    out.replace(later.from, later.to - later.from, later.text);
    out.replace(earlier.from, earlier.to - earlier.from, earlier.text);
    *merged = out;
    return true;
}

}
