#include "TextMerge.h"

#include <QStringList>

#include <vector>

namespace TextMerge {

namespace {

// Lines base[from, to) became `lines`.
struct Hunk {
    int from = 0;
    int to = 0;
    QStringList lines;
};

// Where `other` differs from `base`, line by line (longest common subsequence).
QList<Hunk> hunks(const QStringList &base, const QStringList &other)
{
    const int b = int(base.size()), o = int(other.size());
    int p = 0;
    while (p < b && p < o && base[p] == other[p])
        ++p;
    int s = 0;
    while (s < b - p && s < o - p && base[b - 1 - s] == other[o - 1 - s])
        ++s;
    const int n = b - p - s, m = o - p - s;
    if (n == 0 && m == 0)
        return {};
    // Past this size the table gets big; one coarse hunk still merges when the other side is elsewhere.
    if (qint64(n) * m > 4'000'000 || qMin(n, m) >= 65535)
        return {{p, b - s, other.mid(p, m)}};
    std::vector<quint16> lcs(size_t(n + 1) * size_t(m + 1), 0);
    auto at = [&](int i, int j) -> quint16 & { return lcs[size_t(i) * size_t(m + 1) + size_t(j)]; };
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            at(i, j) = base[p + i] == other[p + j] ? quint16(at(i + 1, j + 1) + 1) : qMax(at(i + 1, j), at(i, j + 1));
    QList<Hunk> out;
    int i = 0, j = 0, hi = -1, hj = -1;
    auto flush = [&] {
        if (hi >= 0)
            out.append({p + hi, p + i, other.mid(p + hj, j - hj)});
        hi = hj = -1;
    };
    while (i < n || j < m) {
        if (i < n && j < m && base[p + i] == other[p + j]) {
            flush();
            ++i;
            ++j;
            continue;
        }
        if (hi < 0) {
            hi = i;
            hj = j;
        }
        if (j >= m || (i < n && at(i + 1, j) >= at(i, j + 1)))
            ++i;
        else
            ++j;
    }
    flush();
    return out;
}

// base[from, to) with `side`'s hunks inside that range applied.
QStringList apply(const QStringList &base, int from, int to, const QList<Hunk> &side)
{
    QStringList out;
    int pos = from;
    for (const Hunk &h : side) {
        out += base.mid(pos, h.from - pos);
        out += h.lines;
        pos = h.to;
    }
    out += base.mid(pos, to - pos);
    return out;
}

// The same stretch changed on both sides: merge it character by character, or give up.
bool mergeChars(const QString &base, const QString &ours, const QString &theirs, QString *merged)
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

} // namespace

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
    if (ours == theirs || theirs == base) {
        *merged = ours;
        return true;
    }
    if (ours == base) {
        *merged = theirs;
        return true;
    }
    const QStringList b = base.split(u'\n');
    const QList<Hunk> mine = hunks(b, ours.split(u'\n'));
    const QList<Hunk> other = hunks(b, theirs.split(u'\n'));
    QStringList out;
    int pos = 0, i = 0, j = 0;
    // Hunks start at or after `from`; they join a group they overlap or meet at an insertion point.
    auto touches = [](const Hunk &h, int from, int to) { return h.from < to || (h.from == to && (h.from == h.to || from == to)); };
    while (i < mine.size() || j < other.size()) {
        const bool mineFirst = j >= other.size() || (i < mine.size() && mine[i].from <= other[j].from);
        QList<Hunk> ourGroup, theirGroup;
        if (mineFirst)
            ourGroup << mine[i++];
        else
            theirGroup << other[j++];
        const int from = mineFirst ? ourGroup.first().from : theirGroup.first().from;
        int to = mineFirst ? ourGroup.first().to : theirGroup.first().to;
        bool grew = true;
        while (grew) {
            grew = false;
            while (i < mine.size() && touches(mine[i], from, to)) {
                to = qMax(to, mine[i].to);
                ourGroup << mine[i++];
                grew = true;
            }
            while (j < other.size() && touches(other[j], from, to)) {
                to = qMax(to, other[j].to);
                theirGroup << other[j++];
                grew = true;
            }
        }
        out += b.mid(pos, from - pos);
        pos = to;
        if (theirGroup.isEmpty() || ourGroup.isEmpty()) {
            out += apply(b, from, to, ourGroup.isEmpty() ? theirGroup : ourGroup);
            continue;
        }
        const QStringList ourLines = apply(b, from, to, ourGroup);
        const QStringList theirLines = apply(b, from, to, theirGroup);
        if (ourLines == theirLines) {
            out += ourLines;
        } else if (from == to) {
            out += theirLines + ourLines; // both added lines at the same place: keep both, theirs first
        } else {
            QString both;
            if (!mergeChars(b.mid(from, to - from).join(u'\n'), ourLines.join(u'\n'), theirLines.join(u'\n'), &both))
                return false;
            out += both.split(u'\n');
        }
    }
    out += b.mid(pos);
    *merged = out.join(u'\n');
    return true;
}

}
