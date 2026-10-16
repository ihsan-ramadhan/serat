#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>

// Mermaid diagrams drawn by mermaid-cli (mmdc) when it is installed, cached as SVG by their source.
class Diagrams : public QObject
{
    Q_OBJECT
public:
    static Diagrams &instance();

    // The command that draws diagrams, or an empty string when there is none.
    static QString renderer();
    // The drawn diagram, or a null image while it is being drawn, when drawing failed or without a renderer.
    QImage image(const QString &source, bool dark, qreal dpr = 1.0);
    // Why the last attempt for `source` failed, if it did.
    QString error(const QString &source, bool dark) const;
    bool pending(const QString &source, bool dark) const;

Q_SIGNALS:
    void ready();

private:
    Diagrams() = default;
    QString keyOf(const QString &source, bool dark) const;
    QString svgPath(const QString &key) const;
    void start(const QString &source, bool dark, const QString &key);

    QSet<QString> m_running;
    QHash<QString, QString> m_errors;
    QHash<QString, QImage> m_images;
};
