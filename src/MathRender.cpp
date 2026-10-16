#include "MathRender.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace MathRender {

namespace {

// Device the fonts are measured against: 96 dpi, so a point size of px * 0.75 is px pixels.
QImage &measureDevice()
{
    static QImage dev = [] {
        QImage img(1, 1, QImage::Format_ARGB32_Premultiplied);
        img.setDotsPerMeterX(3780);
        img.setDotsPerMeterY(3780);
        return img;
    }();
    return dev;
}

QString firstFamily(std::initializer_list<const char16_t *> names)
{
    const QStringList available = QFontDatabase::families();
    for (const char16_t *n : names)
        if (available.contains(QString::fromUtf16(n), Qt::CaseInsensitive))
            return QString::fromUtf16(n);
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

const QString &textFamily()
{
    static const QString f = firstFamily({u"STIX Two Text", u"Latin Modern Roman", u"Cambria", u"Libertinus Serif",
                                          u"DejaVu Serif", u"Times New Roman", u"Noto Serif", u"Liberation Serif"});
    return f;
}

const QString &mathFamily()
{
    static const QString f = firstFamily({u"STIX Two Math", u"Latin Modern Math", u"Cambria Math", u"XITS Math",
                                          u"Libertinus Math", u"DejaVu Math TeX Gyre", u"Noto Sans Math", u"DejaVu Serif"});
    return f;
}

enum class Shape { Italic, Roman, Bold, BoldItalic, Sans, Mono, Symbol };
enum class Atom { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

struct Style {
    int level = 0;       // 0 text, 1 script, 2 scriptscript
    bool display = false;
    QColor color;
    double base = 16;    // pixel size at level 0
    double px() const { return base * (level == 0 ? 1.0 : level == 1 ? 0.7 : 0.5); }
    Style script() const { return {qMin(level + 1, 2), false, color, base}; }
    Style inner() const { return {level, false, color, base}; }
};

QFont fontFor(Shape shape, double px)
{
    QFont f(shape == Shape::Symbol ? mathFamily() : textFamily());
    if (shape == Shape::Sans)
        f.setFamily(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
    if (shape == Shape::Mono)
        f.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    f.setPointSizeF(px * 0.75);
    f.setItalic(shape == Shape::Italic || shape == Shape::BoldItalic);
    f.setBold(shape == Shape::Bold || shape == Shape::BoldItalic);
    return f;
}

// The height of the fraction bar and of the middle of "+" above the baseline.
double axisOf(const Style &s)
{
    return QFontMetricsF(fontFor(Shape::Roman, s.px()), &measureDevice()).strikeOutPos();
}

struct Box {
    double w = 0, a = 0, d = 0; // width, ascent above and descent below the baseline
    Atom atom = Atom::Ord;
    virtual ~Box() = default;
    virtual void draw(QPainter &p, double x, double y) const = 0; // y is the baseline
};
using BoxP = std::unique_ptr<Box>;

struct Glyph : Box {
    QString text;
    QFont font;
    QColor color;
    double padLeft = 0;
    Glyph(const QString &t, Shape shape, const Style &s, QColor c = {})
        : text(t)
        , font(fontFor(shape, s.px()))
        , color(c.isValid() ? c : s.color)
    {
        const QFontMetricsF fm(font, &measureDevice());
        const QRectF r = fm.boundingRect(text);
        w = fm.horizontalAdvance(text);
        if (font.italic())
            w += fm.height() * 0.03; // italic correction, so the next glyph does not touch
        a = qMax(-r.top(), fm.xHeight());
        d = qMax(r.bottom(), 0.0);
    }
    void draw(QPainter &p, double x, double y) const override
    {
        p.setFont(font);
        p.setPen(color);
        p.drawText(QPointF(x + padLeft, y), text);
    }
};

struct Space : Box {
    explicit Space(double width) { w = width; }
    void draw(QPainter &, double, double) const override {}
};

struct Row : Box {
    std::vector<std::pair<double, BoxP>> items; // x offset, box
    void add(BoxP b, double gapBefore = 0)
    {
        const double x = w + gapBefore;
        w = x + b->w;
        a = qMax(a, b->a);
        d = qMax(d, b->d);
        items.emplace_back(x, std::move(b));
    }
    void draw(QPainter &p, double x, double y) const override
    {
        for (const auto &[dx, b] : items)
            b->draw(p, x + dx, y);
    }
};

struct Frac : Box {
    BoxP num, den;
    double bar = 1, axis = 0, gap = 0;
    bool rule = true;
    Frac(BoxP n, BoxP dn, const Style &s, bool withRule)
        : num(std::move(n))
        , den(std::move(dn))
        , rule(withRule)
    {
        bar = qMax(1.0, s.px() * 0.055);
        axis = axisOf(s);
        gap = qMax(2.0, s.px() * (s.display ? 0.18 : 0.12));
        w = qMax(num->w, den->w) + s.px() * 0.2;
        a = axis + bar / 2 + gap + num->d + num->a;
        d = den->a + den->d + gap + bar / 2 - axis;
        atom = Atom::Inner;
    }
    void draw(QPainter &p, double x, double y) const override
    {
        num->draw(p, x + (w - num->w) / 2, y - axis - bar / 2 - gap - num->d);
        den->draw(p, x + (w - den->w) / 2, y - axis + bar / 2 + gap + den->a);
        if (rule)
            p.fillRect(QRectF(x + 1, y - axis - bar / 2, w - 2, bar), p.pen().color());
    }
};

struct Scripts : Box {
    BoxP base, sup, sub;
    double supY = 0, subY = 0, scriptX = 0;
    Scripts(BoxP b, BoxP up, BoxP down, const Style &s)
        : base(std::move(b))
        , sup(std::move(up))
        , sub(std::move(down))
    {
        const double em = s.px();
        scriptX = base->w + em * 0.02;
        atom = base->atom;
        a = base->a;
        d = base->d;
        double sw = 0;
        if (sup) {
            supY = -qMax(base->a - sup->a * 0.6, em * 0.33);
            a = qMax(a, -supY + sup->a);
            sw = sup->w;
        }
        if (sub) {
            subY = qMax(base->d + sub->a * 0.25, em * 0.16);
            if (sup && subY - sub->a < supY + sup->d + em * 0.1)
                subY = supY + sup->d + em * 0.1 + sub->a;
            d = qMax(d, subY + sub->d);
            sw = qMax(sw, sub->w);
        }
        w = scriptX + sw + em * 0.05;
    }
    void draw(QPainter &p, double x, double y) const override
    {
        base->draw(p, x, y);
        if (sup)
            sup->draw(p, x + scriptX, y + supY);
        if (sub)
            sub->draw(p, x + scriptX, y + subY);
    }
};

// Limits stacked above and below (sums in display mode, \lim, \max…).
struct Limits : Box {
    BoxP base, over, under;
    double gap = 0;
    Limits(BoxP b, BoxP up, BoxP down, const Style &s)
        : base(std::move(b))
        , over(std::move(up))
        , under(std::move(down))
    {
        gap = s.px() * 0.15;
        atom = Atom::Op;
        w = base->w;
        a = base->a;
        d = base->d;
        if (over) {
            w = qMax(w, over->w);
            a += gap + over->d + over->a;
        }
        if (under) {
            w = qMax(w, under->w);
            d += gap + under->a + under->d;
        }
    }
    void draw(QPainter &p, double x, double y) const override
    {
        base->draw(p, x + (w - base->w) / 2, y);
        if (over)
            over->draw(p, x + (w - over->w) / 2, y - base->a - gap - over->d);
        if (under)
            under->draw(p, x + (w - under->w) / 2, y + base->d + gap + under->a);
    }
};

// A big operator symbol, centred on the axis.
struct BigOp : Box {
    Glyph glyph;
    double shift = 0;
    bool integral = false; // integrals keep their limits at the side, as in TeX
    BigOp(const QString &symbol, const Style &s)
        : glyph(symbol, Shape::Symbol, Style{s.level, s.display, s.color, s.base * (s.display ? 1.7 : 1.2)})
    {
        integral = symbol.contains(u'∫') || symbol.contains(u'∬') || symbol.contains(u'∭') || symbol.contains(u'∮');
        const double axis = axisOf(s);
        const double h = glyph.a + glyph.d;
        shift = (glyph.a - glyph.d) / 2 - axis; // moves the glyph's middle onto the axis
        a = h / 2 + axis;
        d = h / 2 - axis;
        w = glyph.w;
        atom = Atom::Op;
    }
    void draw(QPainter &p, double x, double y) const override { glyph.draw(p, x, y + shift); }
};

struct Sqrt : Box {
    BoxP body, index;
    double rule = 1, gap = 0, hook = 0;
    Sqrt(BoxP b, BoxP idx, const Style &s)
        : body(std::move(b))
        , index(std::move(idx))
    {
        const double em = s.px();
        rule = qMax(1.0, em * 0.055);
        gap = em * 0.12;
        hook = em * 0.55;
        a = body->a + gap + rule;
        d = body->d + em * 0.05;
        w = hook + body->w + em * 0.1;
        if (index)
            a = qMax(a, index->a + index->d + (a + d) * 0.3);
    }
    void draw(QPainter &p, double x, double y) const override
    {
        const double top = y - body->a - gap - rule / 2, bottom = y + d;
        QPainterPath path;
        path.moveTo(x, y - (a + d) * 0.32 + d * 0.3);
        path.lineTo(x + hook * 0.25, y - (a + d) * 0.4 + d * 0.4);
        path.lineTo(x + hook * 0.55, bottom);
        path.lineTo(x + hook, top);
        path.lineTo(x + w, top);
        p.save();
        p.setPen(QPen(p.pen().color(), rule, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.restore();
        body->draw(p, x + hook, y);
        if (index)
            index->draw(p, x, y - (a + d) * 0.38 - index->d);
    }
};

// Delimiters are drawn as paths so they grow to any height.
void drawDelimiter(QPainter &p, const QString &kind, double x, double top, double width, double height, double stroke)
{
    if (kind.isEmpty() || kind == u".")
        return;
    QPainterPath path;
    const double bottom = top + height, mid = top + height / 2;
    const double l = x + width * 0.25, r = x + width * 0.75;
    if (kind == u"(") {
        path.moveTo(r, top);
        path.cubicTo(l - width * 0.15, top + height * 0.2, l - width * 0.15, bottom - height * 0.2, r, bottom);
    } else if (kind == u")") {
        path.moveTo(l, top);
        path.cubicTo(r + width * 0.15, top + height * 0.2, r + width * 0.15, bottom - height * 0.2, l, bottom);
    } else if (kind == u"[" || kind == u"⌊" || kind == u"⌈") {
        path.moveTo(r, top);
        if (kind != u"⌊") {
            path.lineTo(l, top);
        } else {
            path.moveTo(l, top);
        }
        path.lineTo(l, bottom);
        if (kind != u"⌈")
            path.lineTo(r, bottom);
    } else if (kind == u"]" || kind == u"⌋" || kind == u"⌉") {
        path.moveTo(l, top);
        if (kind != u"⌋") {
            path.lineTo(r, top);
        } else {
            path.moveTo(r, top);
        }
        path.lineTo(r, bottom);
        if (kind != u"⌉")
            path.lineTo(l, bottom);
    } else if (kind == u"{" || kind == u"}") {
        const bool open = kind == u"{";
        const double out = open ? r : l, in = open ? l : r, tip = open ? x : x + width;
        path.moveTo(out, top);
        path.cubicTo(in, top, in, top + height * 0.05, in, top + height * 0.12);
        path.lineTo(in, mid - height * 0.08);
        path.cubicTo(in, mid - height * 0.02, tip, mid, tip, mid);
        path.cubicTo(tip, mid, in, mid + height * 0.02, in, mid + height * 0.08);
        path.lineTo(in, bottom - height * 0.12);
        path.cubicTo(in, bottom - height * 0.05, in, bottom, out, bottom);
    } else if (kind == u"|") {
        path.moveTo(x + width / 2, top);
        path.lineTo(x + width / 2, bottom);
    } else if (kind == u"‖") {
        path.moveTo(x + width * 0.35, top);
        path.lineTo(x + width * 0.35, bottom);
        path.moveTo(x + width * 0.65, top);
        path.lineTo(x + width * 0.65, bottom);
    } else if (kind == u"⟨") {
        path.moveTo(r, top);
        path.lineTo(l, mid);
        path.lineTo(r, bottom);
    } else if (kind == u"⟩") {
        path.moveTo(l, top);
        path.lineTo(r, mid);
        path.lineTo(l, bottom);
    }
    p.save();
    p.setPen(QPen(p.pen().color(), stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.restore();
}

struct Delimited : Box {
    BoxP body;
    QString left, right;
    double dw = 0, top = 0, height = 0, stroke = 1;
    Delimited(BoxP b, const QString &l, const QString &r, const Style &s)
        : body(std::move(b))
        , left(l)
        , right(r)
    {
        const double em = s.px(), axis = axisOf(s);
        const double half = qMax(qMax(body->a - axis, body->d + axis), em * 0.5) + em * 0.08;
        height = 2 * half;
        dw = qMax(em * 0.32, height * 0.12);
        stroke = qMax(1.0, em * 0.06);
        a = qMax(body->a, half + axis);
        d = qMax(body->d, half - axis);
        top = -(half + axis);
        const double lw = left.isEmpty() || left == u"." ? 0 : dw, rw = right.isEmpty() || right == u"." ? 0 : dw;
        w = lw + body->w + rw;
        atom = Atom::Inner;
    }
    void draw(QPainter &p, double x, double y) const override
    {
        const double lw = left.isEmpty() || left == u"." ? 0 : dw;
        drawDelimiter(p, left, x, y + top, dw, height, stroke);
        body->draw(p, x + lw, y);
        drawDelimiter(p, right, x + lw + body->w, y + top, dw, height, stroke);
    }
};

struct Accent : Box {
    BoxP body;
    QString kind;
    double em = 10, lift = 0;
    Accent(BoxP b, const QString &k, const Style &s)
        : body(std::move(b))
        , kind(k)
    {
        em = s.px();
        lift = em * 0.22;
        w = body->w;
        a = body->a + lift + em * 0.05;
        d = body->d;
        if (kind == u"underline")
            d += em * 0.15;
        atom = body->atom;
    }
    void draw(QPainter &p, double x, double y) const override
    {
        body->draw(p, x, y);
        const double top = y - body->a - em * 0.08, stroke = qMax(1.0, em * 0.055);
        const double cx = x + w / 2, half = kind.startsWith(u"wide"_s) || kind == u"overline" || kind == u"vec" ? w / 2 : qMin(w / 2, em * 0.22);
        p.save();
        p.setPen(QPen(p.pen().color(), stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        if (kind == u"hat" || kind == u"widehat") {
            path.moveTo(cx - half, top);
            path.lineTo(cx, top - lift * 0.8);
            path.lineTo(cx + half, top);
        } else if (kind == u"bar" || kind == u"overline") {
            path.moveTo(cx - half, top - lift * 0.4);
            path.lineTo(cx + half, top - lift * 0.4);
        } else if (kind == u"underline") {
            path.moveTo(x, y + body->d + em * 0.1);
            path.lineTo(x + w, y + body->d + em * 0.1);
        } else if (kind == u"vec") {
            const double yy = top - lift * 0.45;
            path.moveTo(cx - half, yy);
            path.lineTo(cx + half, yy);
            path.moveTo(cx + half - em * 0.15, yy - em * 0.1);
            path.lineTo(cx + half, yy);
            path.lineTo(cx + half - em * 0.15, yy + em * 0.1);
        } else if (kind == u"tilde" || kind == u"widetilde") {
            const double yy = top - lift * 0.45;
            path.moveTo(cx - half, yy + lift * 0.15);
            path.cubicTo(cx - half / 2, yy - lift * 0.45, cx + half / 2, yy + lift * 0.45, cx + half, yy - lift * 0.15);
        } else if (kind == u"dot" || kind == u"ddot") {
            p.setBrush(p.pen().color());
            p.setPen(Qt::NoPen);
            const double r = em * 0.06;
            if (kind == u"dot") {
                p.drawEllipse(QPointF(cx, top - lift * 0.45), r, r);
            } else {
                p.drawEllipse(QPointF(cx - em * 0.12, top - lift * 0.45), r, r);
                p.drawEllipse(QPointF(cx + em * 0.12, top - lift * 0.45), r, r);
            }
        }
        p.drawPath(path);
        p.restore();
    }
};

struct Matrix : Box {
    std::vector<std::vector<BoxP>> cells;
    std::vector<double> colX, colW, rowY;
    std::vector<int> align; // per column: -1 left, 0 centre, 1 right
    Matrix(std::vector<std::vector<BoxP>> c, std::vector<int> columnAlign, double colGap, const Style &s)
        : cells(std::move(c))
        , align(std::move(columnAlign))
    {
        const double em = s.px(), axis = axisOf(s), rowGap = em * 0.35;
        size_t cols = 0;
        for (const auto &row : cells)
            cols = qMax(cols, row.size());
        colW.assign(cols, 0);
        for (const auto &row : cells)
            for (size_t c = 0; c < row.size(); ++c)
                colW[c] = qMax(colW[c], row[c]->w);
        double x = 0;
        for (size_t c = 0; c < cols; ++c) {
            colX.push_back(x);
            x += colW[c] + (c + 1 < cols ? colGap : 0);
        }
        w = x;
        double y = 0;
        for (const auto &row : cells) {
            double ra = em * 0.7, rd = em * 0.25;
            for (const BoxP &b : row) {
                ra = qMax(ra, b->a);
                rd = qMax(rd, b->d);
            }
            rowY.push_back(y + ra);
            y += ra + rd + rowGap;
        }
        const double total = y - (cells.empty() ? 0 : rowGap);
        a = total / 2 + axis;
        d = total / 2 - axis;
        for (double &ry : rowY)
            ry -= a; // relative to the baseline
        atom = Atom::Inner;
    }
    void draw(QPainter &p, double x, double y) const override
    {
        for (size_t r = 0; r < cells.size(); ++r)
            for (size_t c = 0; c < cells[r].size(); ++c) {
                const Box &b = *cells[r][c];
                const int al = c < align.size() ? align[c] : 0;
                const double cx = al < 0 ? 0 : al > 0 ? colW[c] - b.w : (colW[c] - b.w) / 2;
                b.draw(p, x + colX[c] + cx, y + rowY[r]);
            }
    }
};

const QHash<QString, QString> &symbols()
{
    static const QHash<QString, QString> map = {
        {u"alpha"_s, u"α"_s}, {u"beta"_s, u"β"_s}, {u"gamma"_s, u"γ"_s}, {u"delta"_s, u"δ"_s}, {u"epsilon"_s, u"ϵ"_s},
        {u"varepsilon"_s, u"ε"_s}, {u"zeta"_s, u"ζ"_s}, {u"eta"_s, u"η"_s}, {u"theta"_s, u"θ"_s}, {u"vartheta"_s, u"ϑ"_s},
        {u"iota"_s, u"ι"_s}, {u"kappa"_s, u"κ"_s}, {u"lambda"_s, u"λ"_s}, {u"mu"_s, u"μ"_s}, {u"nu"_s, u"ν"_s}, {u"xi"_s, u"ξ"_s},
        {u"pi"_s, u"π"_s}, {u"varpi"_s, u"ϖ"_s}, {u"rho"_s, u"ρ"_s}, {u"varrho"_s, u"ϱ"_s}, {u"sigma"_s, u"σ"_s},
        {u"varsigma"_s, u"ς"_s}, {u"tau"_s, u"τ"_s}, {u"upsilon"_s, u"υ"_s}, {u"phi"_s, u"ϕ"_s}, {u"varphi"_s, u"φ"_s},
        {u"chi"_s, u"χ"_s}, {u"psi"_s, u"ψ"_s}, {u"omega"_s, u"ω"_s}, {u"Gamma"_s, u"Γ"_s}, {u"Delta"_s, u"Δ"_s},
        {u"Theta"_s, u"Θ"_s}, {u"Lambda"_s, u"Λ"_s}, {u"Xi"_s, u"Ξ"_s}, {u"Pi"_s, u"Π"_s}, {u"Sigma"_s, u"Σ"_s},
        {u"Upsilon"_s, u"Υ"_s}, {u"Phi"_s, u"Φ"_s}, {u"Psi"_s, u"Ψ"_s}, {u"Omega"_s, u"Ω"_s},
        {u"infty"_s, u"∞"_s}, {u"partial"_s, u"∂"_s}, {u"nabla"_s, u"∇"_s}, {u"forall"_s, u"∀"_s}, {u"exists"_s, u"∃"_s},
        {u"nexists"_s, u"∄"_s}, {u"neg"_s, u"¬"_s}, {u"lnot"_s, u"¬"_s}, {u"emptyset"_s, u"∅"_s}, {u"varnothing"_s, u"∅"_s},
        {u"angle"_s, u"∠"_s}, {u"hbar"_s, u"ℏ"_s}, {u"ell"_s, u"ℓ"_s}, {u"Re"_s, u"ℜ"_s}, {u"Im"_s, u"ℑ"_s}, {u"aleph"_s, u"ℵ"_s},
        {u"prime"_s, u"′"_s}, {u"ldots"_s, u"…"_s}, {u"dots"_s, u"…"_s}, {u"cdots"_s, u"⋯"_s}, {u"vdots"_s, u"⋮"_s},
        {u"ddots"_s, u"⋱"_s}, {u"top"_s, u"⊤"_s}, {u"bot"_s, u"⊥"_s}, {u"triangle"_s, u"△"_s}, {u"square"_s, u"□"_s},
        {u"checkmark"_s, u"✓"_s}, {u"degree"_s, u"°"_s}, {u"dagger"_s, u"†"_s}, {u"ddagger"_s, u"‡"_s}, {u"S"_s, u"§"_s},
        {u"%"_s, u"%"_s}, {u"$"_s, u"$"_s}, {u"#"_s, u"#"_s}, {u"&"_s, u"&"_s}, {u"_"_s, u"_"_s}, {u"{"_s, u"{"_s}, {u"}"_s, u"}"_s},
    };
    return map;
}

const QHash<QString, QString> &binaries()
{
    static const QHash<QString, QString> map = {
        {u"pm"_s, u"±"_s}, {u"mp"_s, u"∓"_s}, {u"times"_s, u"×"_s}, {u"div"_s, u"÷"_s}, {u"cdot"_s, u"⋅"_s}, {u"ast"_s, u"∗"_s},
        {u"star"_s, u"⋆"_s}, {u"circ"_s, u"∘"_s}, {u"bullet"_s, u"∙"_s}, {u"cup"_s, u"∪"_s}, {u"cap"_s, u"∩"_s},
        {u"setminus"_s, u"∖"_s}, {u"wedge"_s, u"∧"_s}, {u"land"_s, u"∧"_s}, {u"vee"_s, u"∨"_s}, {u"lor"_s, u"∨"_s},
        {u"oplus"_s, u"⊕"_s}, {u"ominus"_s, u"⊖"_s}, {u"otimes"_s, u"⊗"_s}, {u"odot"_s, u"⊙"_s},
    };
    return map;
}

const QHash<QString, QString> &relations()
{
    static const QHash<QString, QString> map = {
        {u"leq"_s, u"≤"_s}, {u"le"_s, u"≤"_s}, {u"geq"_s, u"≥"_s}, {u"ge"_s, u"≥"_s}, {u"neq"_s, u"≠"_s}, {u"ne"_s, u"≠"_s},
        {u"lt"_s, u"<"_s}, {u"gt"_s, u">"_s}, {u"approx"_s, u"≈"_s}, {u"equiv"_s, u"≡"_s}, {u"sim"_s, u"∼"_s}, {u"simeq"_s, u"≃"_s},
        {u"cong"_s, u"≅"_s}, {u"propto"_s, u"∝"_s}, {u"ll"_s, u"≪"_s}, {u"gg"_s, u"≫"_s}, {u"in"_s, u"∈"_s}, {u"notin"_s, u"∉"_s},
        {u"ni"_s, u"∋"_s}, {u"subset"_s, u"⊂"_s}, {u"supset"_s, u"⊃"_s}, {u"subseteq"_s, u"⊆"_s}, {u"supseteq"_s, u"⊇"_s},
        {u"to"_s, u"→"_s}, {u"rightarrow"_s, u"→"_s}, {u"leftarrow"_s, u"←"_s}, {u"gets"_s, u"←"_s}, {u"leftrightarrow"_s, u"↔"_s},
        {u"Rightarrow"_s, u"⇒"_s}, {u"Leftarrow"_s, u"⇐"_s}, {u"Leftrightarrow"_s, u"⇔"_s}, {u"implies"_s, u"⟹"_s},
        {u"impliedby"_s, u"⟸"_s}, {u"iff"_s, u"⟺"_s}, {u"mapsto"_s, u"↦"_s}, {u"longrightarrow"_s, u"⟶"_s},
        {u"longleftarrow"_s, u"⟵"_s}, {u"uparrow"_s, u"↑"_s}, {u"downarrow"_s, u"↓"_s}, {u"perp"_s, u"⊥"_s},
        {u"parallel"_s, u"∥"_s}, {u"mid"_s, u"∣"_s}, {u"vdash"_s, u"⊢"_s}, {u"models"_s, u"⊨"_s}, {u"colon"_s, u":"_s},
        {u"coloneqq"_s, u"≔"_s}, {u"prec"_s, u"≺"_s}, {u"succ"_s, u"≻"_s}, {u"preceq"_s, u"⪯"_s}, {u"succeq"_s, u"⪰"_s},
    };
    return map;
}

const QHash<QString, QString> &bigOperators()
{
    static const QHash<QString, QString> map = {
        {u"sum"_s, u"∑"_s}, {u"prod"_s, u"∏"_s}, {u"coprod"_s, u"∐"_s}, {u"int"_s, u"∫"_s}, {u"iint"_s, u"∬"_s},
        {u"iiint"_s, u"∭"_s}, {u"oint"_s, u"∮"_s}, {u"bigcup"_s, u"⋃"_s}, {u"bigcap"_s, u"⋂"_s}, {u"bigoplus"_s, u"⨁"_s},
        {u"bigotimes"_s, u"⨂"_s}, {u"bigvee"_s, u"⋁"_s}, {u"bigwedge"_s, u"⋀"_s},
    };
    return map;
}

// Function names set upright; those in `limitNames` take limits below in display mode.
const QStringList &functionNames()
{
    static const QStringList names = {u"sin"_s, u"cos"_s, u"tan"_s, u"cot"_s, u"sec"_s, u"csc"_s, u"arcsin"_s, u"arccos"_s,
                                      u"arctan"_s, u"sinh"_s, u"cosh"_s, u"tanh"_s, u"log"_s, u"ln"_s, u"lg"_s, u"exp"_s,
                                      u"deg"_s, u"dim"_s, u"ker"_s, u"arg"_s, u"hom"_s, u"gcd"_s, u"Pr"_s, u"det"_s, u"lim"_s,
                                      u"liminf"_s, u"limsup"_s, u"max"_s, u"min"_s, u"sup"_s, u"inf"_s, u"argmax"_s, u"argmin"_s};
    return names;
}

bool takesLimits(const QString &name)
{
    static const QStringList names = {u"lim"_s, u"liminf"_s, u"limsup"_s, u"max"_s, u"min"_s, u"sup"_s, u"inf"_s,
                                      u"det"_s, u"gcd"_s, u"Pr"_s, u"argmax"_s, u"argmin"_s};
    return names.contains(name);
}

QString delimiterFor(const QString &token)
{
    static const QHash<QString, QString> map = {
        {u"("_s, u"("_s}, {u")"_s, u")"_s}, {u"["_s, u"["_s}, {u"]"_s, u"]"_s}, {u"\\{"_s, u"{"_s}, {u"\\}"_s, u"}"_s},
        {u"\\lbrace"_s, u"{"_s}, {u"\\rbrace"_s, u"}"_s}, {u"|"_s, u"|"_s}, {u"\\vert"_s, u"|"_s}, {u"\\|"_s, u"‖"_s},
        {u"\\Vert"_s, u"‖"_s}, {u"\\langle"_s, u"⟨"_s}, {u"\\rangle"_s, u"⟩"_s}, {u"\\lfloor"_s, u"⌊"_s},
        {u"\\rfloor"_s, u"⌋"_s}, {u"\\lceil"_s, u"⌈"_s}, {u"\\rceil"_s, u"⌉"_s}, {u"."_s, u"."_s},
        {u"<"_s, u"⟨"_s}, {u">"_s, u"⟩"_s},
    };
    return map.value(token);
}

// Letters in another alphabet, for \mathbb, \mathcal and \mathfrak.
QString mapAlphabet(const QString &text, const QString &font)
{
    static const QHash<QChar, QString> bbHoles = {{u'C', u"ℂ"_s}, {u'H', u"ℍ"_s}, {u'N', u"ℕ"_s}, {u'P', u"ℙ"_s},
                                                  {u'Q', u"ℚ"_s}, {u'R', u"ℝ"_s}, {u'Z', u"ℤ"_s}};
    static const QHash<QChar, QString> calHoles = {{u'B', u"ℬ"_s}, {u'E', u"ℰ"_s}, {u'F', u"ℱ"_s}, {u'H', u"ℋ"_s},
                                                   {u'I', u"ℐ"_s}, {u'L', u"ℒ"_s}, {u'M', u"ℳ"_s}, {u'R', u"ℛ"_s},
                                                   {u'e', u"ℯ"_s}, {u'g', u"ℊ"_s}, {u'o', u"ℴ"_s}};
    static const QHash<QChar, QString> frakHoles = {{u'C', u"ℭ"_s}, {u'H', u"ℌ"_s}, {u'I', u"ℑ"_s}, {u'R', u"ℜ"_s}, {u'Z', u"ℨ"_s}};
    const char32_t upper = font == u"mathbb" ? 0x1D538 : font == u"mathcal" ? 0x1D49C : 0x1D504;
    const char32_t lower = font == u"mathbb" ? 0x1D552 : font == u"mathcal" ? 0x1D4B6 : 0x1D51E;
    const auto &holes = font == u"mathbb" ? bbHoles : font == u"mathcal" ? calHoles : frakHoles;
    QString out;
    for (const QChar c : text) {
        if (holes.contains(c)) {
            out += holes.value(c);
        } else if (c >= u'A' && c <= u'Z') {
            const char32_t cp = upper + (c.unicode() - u'A');
            out += QString::fromUcs4(&cp, 1);
        } else if (c >= u'a' && c <= u'z') {
            const char32_t cp = lower + (c.unicode() - u'a');
            out += QString::fromUcs4(&cp, 1);
        } else if (font == u"mathbb" && c >= u'0' && c <= u'9') {
            const char32_t cp = 0x1D7D8 + (c.unicode() - u'0');
            out += QString::fromUcs4(&cp, 1);
        } else {
            out += c;
        }
    }
    return out;
}

class Parser
{
public:
    Parser(const QString &tex, QStringList *errors)
        : s(tex)
        , m_errors(errors)
    {
    }

    BoxP parse(const Style &style)
    {
        BoxP b = row(style, {});
        return b;
    }

private:
    // Reads a row until one of `stops` ("}", "&", "\\\\", "\\right", "\\end") or the end.
    BoxP row(const Style &st, const QStringList &stops)
    {
        auto out = std::make_unique<Row>();
        Atom previous = Atom::Open;
        Shape shape = Shape::Italic;
        for (;;) {
            skipSpace();
            if (i >= s.size())
                break;
            const QString t = peek();
            if (stops.contains(t))
                break;
            if (t == u"}") { // stray brace
                ++i;
                continue;
            }
            BoxP atom = this->atom(st, shape);
            if (!atom)
                continue;
            atom = scripts(std::move(atom), st);
            // TeX's spacing: binary operators and relations get room on both sides, except in scripts.
            double gap = 0;
            Atom kind = atom->atom;
            if (kind == Atom::Bin && (previous == Atom::Open || previous == Atom::Bin || previous == Atom::Rel
                                      || previous == Atom::Op || previous == Atom::Punct))
                kind = Atom::Ord; // a sign, like the minus in "-x"
            const double em = st.px();
            if (st.level == 0) {
                if (kind == Atom::Rel || previous == Atom::Rel)
                    gap = (kind == Atom::Rel && previous == Atom::Rel) ? 0 : em * 5 / 18;
                else if (kind == Atom::Bin || previous == Atom::Bin)
                    gap = em * 4 / 18;
                else if (previous == Atom::Punct)
                    gap = em * 3 / 18;
                else if (kind == Atom::Op && previous != Atom::Open)
                    gap = em * 3 / 18;
                else if (previous == Atom::Op && kind != Atom::Close && kind != Atom::Punct)
                    gap = em * 3 / 18;
            }
            if (out->items.empty())
                gap = 0;
            atom->atom = kind;
            previous = kind;
            out->add(std::move(atom), gap);
        }
        if (out->items.empty()) {
            out->a = st.px() * 0.5;
            out->d = 0;
        }
        return out;
    }

    BoxP scripts(BoxP base, const Style &st)
    {
        BoxP sup, sub;
        bool forceLimits = false, noLimits = false;
        for (;;) {
            skipSpace();
            if (peek() == u"\\limits" || peek() == u"\\nolimits") {
                (peek() == u"\\limits" ? forceLimits : noLimits) = true;
                i += peek().size();
                continue;
            }
            if (i < s.size() && s[i] == u'\'') {
                QString primes;
                while (i < s.size() && s[i] == u'\'') {
                    primes += u"′"_s;
                    ++i;
                }
                sup = std::make_unique<Glyph>(primes, Shape::Roman, st.script());
                continue;
            }
            if (i < s.size() && (s[i] == u'^' || s[i] == u'_')) {
                const bool up = s[i] == u'^';
                ++i;
                BoxP arg = argument(st.script());
                (up ? sup : sub) = std::move(arg);
                continue;
            }
            break;
        }
        if (!sup && !sub)
            return base;
        const auto *op = dynamic_cast<const BigOp *>(base.get());
        const bool sideways = op && op->integral;
        const bool limits = base->atom == Atom::Op && !noLimits && (forceLimits || (!sideways && (st.display || m_lastLimits)));
        m_lastLimits = false;
        if (limits)
            return std::make_unique<Limits>(std::move(base), std::move(sup), std::move(sub), st);
        return std::make_unique<Scripts>(std::move(base), std::move(sup), std::move(sub), st);
    }

    // A {group} or a single token, laid out at style `st`.
    BoxP argument(const Style &st, Shape shape = Shape::Italic)
    {
        skipSpace();
        if (i < s.size() && s[i] == u'{') {
            ++i;
            BoxP b = groupRow(st, shape);
            if (i < s.size() && s[i] == u'}')
                ++i;
            return b;
        }
        if (i >= s.size())
            return std::make_unique<Space>(0);
        return atom(st, shape);
    }

    BoxP groupRow(const Style &st, Shape shape)
    {
        if (shape == Shape::Italic)
            return row(st, {u"}"_s});
        // Font commands apply to the whole group.
        const Shape saved = m_shape;
        m_shape = shape;
        BoxP b = row(st, {u"}"_s});
        m_shape = saved;
        return b;
    }

    QString rawGroup()
    {
        skipSpace();
        if (i >= s.size())
            return {};
        if (s[i] != u'{') {
            if (s[i] == u'\\') {
                const QString t = peek();
                i += t.size();
                return t;
            }
            return QString(s[i++]);
        }
        int depth = 0;
        const qsizetype start = i + 1;
        for (; i < s.size(); ++i) {
            if (s[i] == u'\\') {
                ++i;
                continue;
            }
            if (s[i] == u'{')
                ++depth;
            else if (s[i] == u'}' && --depth == 0)
                break;
        }
        const QString out = s.mid(start, i - start);
        if (i < s.size())
            ++i;
        return out;
    }

    BoxP atom(const Style &st, Shape shape)
    {
        const Shape effective = m_shape != Shape::Italic ? m_shape : shape;
        const QChar c = s[i];
        if (c == u'{') {
            ++i;
            BoxP b = groupRow(st, Shape::Italic);
            if (i < s.size() && s[i] == u'}')
                ++i;
            b->atom = Atom::Ord;
            return b;
        }
        if (c == u'\\')
            return command(st, effective);
        ++i;
        if (c.isLetter()) {
            const Shape letterShape = effective == Shape::Italic ? Shape::Italic : effective;
            return std::make_unique<Glyph>(QString(c), letterShape, st);
        }
        if (c.isDigit() || c == u'.') {
            QString number(c);
            while (i < s.size() && (s[i].isDigit() || (s[i] == u'.' && i + 1 < s.size() && s[i + 1].isDigit())))
                number += s[i++];
            return std::make_unique<Glyph>(number, effective == Shape::Bold ? Shape::Bold : Shape::Roman, st);
        }
        if (c.isHighSurrogate() && i < s.size()) {
            const QString pair = QString(c) + s[i++];
            return std::make_unique<Glyph>(pair, Shape::Symbol, st);
        }
        auto glyph = std::make_unique<Glyph>(QString(c == u'-' ? u'−' : c), Shape::Roman, st);
        if (c == u'+' || c == u'-' || c == u'*')
            glyph->atom = Atom::Bin;
        else if (c == u'=' || c == u'<' || c == u'>' || c == u':')
            glyph->atom = Atom::Rel;
        else if (c == u'(' || c == u'[')
            glyph->atom = Atom::Open;
        else if (c == u')' || c == u']')
            glyph->atom = Atom::Close;
        else if (c == u',' || c == u';')
            glyph->atom = Atom::Punct;
        else if (c == u'|')
            glyph->atom = Atom::Ord;
        return glyph;
    }

    BoxP command(const Style &st, Shape shape)
    {
        const QString t = peek(); // "\name" or "\x"
        i += t.size();
        const QString name = t.mid(1);
        const double em = st.px();

        if (name == u"," || name == u"thinspace")
            return std::make_unique<Space>(em * 3 / 18);
        if (name == u":" || name == u">" || name == u"medspace")
            return std::make_unique<Space>(em * 4 / 18);
        if (name == u";" || name == u"thickspace")
            return std::make_unique<Space>(em * 5 / 18);
        if (name == u"!")
            return std::make_unique<Space>(-em * 3 / 18);
        if (name == u" " || name == u"space")
            return std::make_unique<Space>(em * 0.25);
        if (name == u"quad")
            return std::make_unique<Space>(em);
        if (name == u"qquad")
            return std::make_unique<Space>(em * 2);
        if (name == u"|")
            return std::make_unique<Glyph>(u"‖"_s, Shape::Symbol, st);

        if (name == u"frac" || name == u"dfrac" || name == u"tfrac" || name == u"binom" || name == u"cfrac") {
            Style inner = name == u"dfrac" ? Style{st.level, true, st.color, st.base}
                        : name == u"tfrac" ? st.script() : st.display ? st.inner() : st.script();
            BoxP num = argument(inner);
            BoxP den = argument(inner);
            if (name == u"binom") {
                Style mid = st;
                mid.display = false;
                return std::make_unique<Delimited>(std::make_unique<Frac>(std::move(num), std::move(den), mid, false),
                                                   u"("_s, u")"_s, st);
            }
            return std::make_unique<Frac>(std::move(num), std::move(den), st, true);
        }
        if (name == u"sqrt") {
            BoxP index;
            skipSpace();
            if (i < s.size() && s[i] == u'[') {
                ++i;
                index = row(st.script().script(), {u"]"_s});
                if (i < s.size() && s[i] == u']')
                    ++i;
            }
            BoxP body = argument(st);
            return std::make_unique<Sqrt>(std::move(body), std::move(index), st);
        }
        static const QStringList accents = {u"hat"_s, u"widehat"_s, u"bar"_s, u"overline"_s, u"underline"_s, u"vec"_s,
                                            u"tilde"_s, u"widetilde"_s, u"dot"_s, u"ddot"_s, u"overrightarrow"_s};
        if (accents.contains(name)) {
            BoxP body = argument(st, shape);
            return std::make_unique<Accent>(std::move(body), name == u"overrightarrow" ? u"vec"_s : name, st);
        }
        if (name == u"text" || name == u"textrm" || name == u"mbox" || name == u"textit" || name == u"textbf"
            || name == u"mathrm" || name == u"operatorname" || name == u"textsf" || name == u"texttt") {
            const QString raw = rawGroup();
            const Shape textShape = name == u"textit" ? Shape::Italic : name == u"textbf" ? Shape::Bold
                                  : name == u"textsf" ? Shape::Sans : name == u"texttt" ? Shape::Mono : Shape::Roman;
            if (name == u"mathrm" || name == u"operatorname") {
                Parser sub(raw, m_errors);
                sub.m_shape = Shape::Roman;
                BoxP b = sub.row(st, {});
                if (name == u"operatorname")
                    b->atom = Atom::Op;
                return b;
            }
            return std::make_unique<Glyph>(raw, textShape, st);
        }
        if (name == u"mathbf" || name == u"boldsymbol" || name == u"bm" || name == u"mathit" || name == u"mathsf"
            || name == u"mathtt") {
            const Shape fontShape = name == u"mathbf" ? Shape::Bold : name == u"mathit" ? Shape::Italic
                                  : name == u"mathsf" ? Shape::Sans : name == u"mathtt" ? Shape::Mono : Shape::BoldItalic;
            const Shape saved = m_shape;
            m_shape = fontShape;
            BoxP b = argument(st, fontShape);
            m_shape = saved;
            return b;
        }
        if (name == u"mathbb" || name == u"mathcal" || name == u"mathfrak" || name == u"mathscr") {
            const QString raw = rawGroup();
            return std::make_unique<Glyph>(mapAlphabet(raw, name == u"mathscr" ? u"mathcal"_s : name), Shape::Symbol, st);
        }
        if (name == u"left") {
            skipSpace();
            const QString open = delimiterToken();
            BoxP body = row(st, {u"\\right"_s});
            QString close;
            if (peek() == u"\\right") {
                i += 6;
                skipSpace();
                close = delimiterToken();
            }
            return std::make_unique<Delimited>(std::move(body), delimiterFor(open), delimiterFor(close), st);
        }
        static const QStringList sized = {u"big"_s, u"Big"_s, u"bigg"_s, u"Bigg"_s, u"bigl"_s, u"bigr"_s, u"Bigl"_s, u"Bigr"_s,
                                          u"biggl"_s, u"biggr"_s, u"Biggl"_s, u"Biggr"_s};
        if (sized.contains(name)) {
            skipSpace();
            const QString d = delimiterFor(delimiterToken());
            auto filler = std::make_unique<Space>(0);
            const double scale = name.startsWith(u"Bigg"_s) ? 1.5 : name.startsWith(u"bigg"_s) ? 1.25 : name.startsWith(u"Big"_s) ? 1.0 : 0.75;
            filler->a = em * scale;
            filler->d = em * scale * 0.4;
            const bool closing = name.endsWith(u'r') || (!name.endsWith(u'l') && (d == u")" || d == u"]" || d == u"}" || d == u"⟩"));
            auto b = std::make_unique<Delimited>(std::move(filler), closing ? QString() : d, closing ? d : QString(), st);
            b->atom = closing ? Atom::Close : Atom::Open;
            return b;
        }
        if (name == u"begin")
            return environment(st);
        if (name == u"limits" || name == u"nolimits" || name == u"displaystyle" || name == u"textstyle" || name == u"label" || name == u"nonumber"
            || name == u"notag") {
            if (name == u"label")
                rawGroup();
            return nullptr;
        }
        if (name == u"color" || name == u"textcolor") {
            const QColor color(rawGroup());
            Style colored = st;
            if (color.isValid())
                colored.color = color;
            if (name == u"textcolor")
                return argument(colored, shape);
            BoxP rest = row(colored, {u"}"_s, u"&"_s, u"\\\\"_s, u"\\right"_s, u"\\end"_s});
            return rest;
        }
        if (bigOperators().contains(name))
            return std::make_unique<BigOp>(bigOperators().value(name), st);
        if (functionNames().contains(name)) {
            auto g = std::make_unique<Glyph>(name, Shape::Roman, st);
            g->atom = Atom::Op;
            m_lastLimits = takesLimits(name) && st.display;
            return g;
        }
        if (binaries().contains(name)) {
            auto g = std::make_unique<Glyph>(binaries().value(name), Shape::Symbol, st);
            g->atom = Atom::Bin;
            return g;
        }
        if (relations().contains(name)) {
            auto g = std::make_unique<Glyph>(relations().value(name), Shape::Symbol, st);
            g->atom = Atom::Rel;
            return g;
        }
        if (symbols().contains(name)) {
            const QString sym = symbols().value(name);
            // Lowercase Greek is italic like Latin letters; capitals and symbols stay upright.
            const bool greekLower = sym.size() == 1 && sym[0].unicode() >= 0x3B1 && sym[0].unicode() <= 0x3F5;
            return std::make_unique<Glyph>(sym, greekLower ? Shape::Italic : Shape::Symbol, st);
        }
        static const QHash<QString, QString> opens = {{u"langle"_s, u"⟨"_s}, {u"lfloor"_s, u"⌊"_s}, {u"lceil"_s, u"⌈"_s},
                                                      {u"lbrace"_s, u"{"_s}};
        static const QHash<QString, QString> closes = {{u"rangle"_s, u"⟩"_s}, {u"rfloor"_s, u"⌋"_s}, {u"rceil"_s, u"⌉"_s},
                                                       {u"rbrace"_s, u"}"_s}};
        if (opens.contains(name) || closes.contains(name)) {
            auto g = std::make_unique<Glyph>(opens.contains(name) ? opens.value(name) : closes.value(name), Shape::Symbol, st);
            g->atom = opens.contains(name) ? Atom::Open : Atom::Close;
            return g;
        }
        if (name == u"\\")
            return nullptr;
        m_errors->append(t);
        return std::make_unique<Glyph>(t, Shape::Mono, st, QColor(u"#d33"_s));
    }

    QString delimiterToken()
    {
        skipSpace();
        if (i >= s.size())
            return {};
        if (s[i] == u'\\') {
            const QString t = peek();
            i += t.size();
            return t;
        }
        return QString(s[i++]);
    }

    BoxP environment(const Style &st)
    {
        const QString env = rawGroup();
        if (env == u"array")
            rawGroup(); // column spec
        std::vector<std::vector<BoxP>> cells(1);
        const QString end = u"\\end"_s;
        Style cell = st;
        cell.display = env == u"aligned" || env == u"align" || env == u"align*" || env == u"gathered";
        for (;;) {
            skipSpace();
            if (i >= s.size())
                break;
            if (peek() == end) {
                i += 4;
                rawGroup();
                break;
            }
            cells.back().push_back(row(cell, {u"&"_s, u"\\\\"_s, end}));
            skipSpace();
            if (i < s.size() && s[i] == u'&') {
                ++i;
                continue;
            }
            if (peek() == u"\\\\") {
                i += 2;
                cells.emplace_back();
            }
        }
        if (!cells.empty() && cells.back().empty())
            cells.pop_back();
        size_t cols = 0;
        for (const auto &r : cells)
            cols = qMax(cols, r.size());
        std::vector<int> align(cols, 0);
        double gap = st.px();
        const bool aligned = env.startsWith(u"align"_s);
        if (aligned) {
            for (size_t c = 0; c < cols; ++c)
                align[c] = c % 2 == 0 ? 1 : -1;
            gap = 0;
        } else if (env == u"cases" || env == u"rcases") {
            std::fill(align.begin(), align.end(), -1);
        }
        BoxP body = std::make_unique<Matrix>(std::move(cells), align, gap, st);
        static const QHash<QString, QPair<QString, QString>> fences = {
            {u"pmatrix"_s, {u"("_s, u")"_s}}, {u"bmatrix"_s, {u"["_s, u"]"_s}}, {u"Bmatrix"_s, {u"{"_s, u"}"_s}},
            {u"vmatrix"_s, {u"|"_s, u"|"_s}}, {u"Vmatrix"_s, {u"‖"_s, u"‖"_s}}, {u"cases"_s, {u"{"_s, QString()}},
            {u"rcases"_s, {QString(), u"}"_s}}};
        if (fences.contains(env)) {
            const auto [l, r] = fences.value(env);
            return std::make_unique<Delimited>(std::move(body), l, r, st);
        }
        static const QStringList known = {u"matrix"_s, u"smallmatrix"_s, u"array"_s, u"aligned"_s, u"align"_s, u"align*"_s,
                                          u"gathered"_s, u"split"_s};
        if (!known.contains(env))
            m_errors->append(u"\\begin{%1}"_s.arg(env));
        return body;
    }

    QString peek() const
    {
        if (i >= s.size())
            return {};
        if (s[i] != u'\\')
            return QString(s[i]);
        qsizetype j = i + 1;
        if (j < s.size() && s[j].isLetter()) {
            while (j < s.size() && s[j].isLetter())
                ++j;
            if (j < s.size() && s[j] == u'*' && s.mid(i + 1, j - i - 1) == u"align")
                ++j;
            return s.mid(i, j - i);
        }
        return s.mid(i, 2);
    }

    void skipSpace()
    {
        while (i < s.size() && s[i].isSpace())
            ++i;
    }

    QString s;
    qsizetype i = 0;
    QStringList *m_errors;
    Shape m_shape = Shape::Italic;
    bool m_lastLimits = false;
};

} // namespace

Result render(const QString &tex, double pixelSize, const QColor &color, bool display, qreal dpr)
{
    Result out;
    const QString source = tex.trimmed();
    if (source.isEmpty())
        return out;
    Style style{0, display, color, pixelSize};
    Parser parser(source, &out.errors);
    BoxP box = parser.parse(style);
    const double pad = qCeil(pixelSize * 0.12);
    const QSize size(qCeil(box->w + 2 * pad), qCeil(box->a + box->d + 2 * pad));
    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.setDotsPerMeterX(3780);
    img.setDotsPerMeterY(3780);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    p.setPen(color);
    box->draw(p, pad, pad + box->a);
    p.end();
    out.image = img;
    out.baseline = pad + box->a;
    return out;
}

}
