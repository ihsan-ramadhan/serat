#include "Diagrams.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>

using namespace Qt::StringLiterals;

Diagrams &Diagrams::instance()
{
    static Diagrams diagrams;
    return diagrams;
}

QString Diagrams::renderer()
{
    const QString chosen = QSettings().value(u"mermaidCommand"_s).toString().trimmed();
    if (!chosen.isEmpty())
        return QFileInfo(chosen).isExecutable() ? chosen : QStandardPaths::findExecutable(chosen);
    return QStandardPaths::findExecutable(u"mmdc"_s);
}

QString Diagrams::keyOf(const QString &source, bool dark) const
{
    return QString::fromLatin1(QCryptographicHash::hash((source + (dark ? u"\ndark"_s : u"\nlight"_s)).toUtf8(),
                                                        QCryptographicHash::Sha1).toHex().left(20));
}

QString Diagrams::svgPath(const QString &key) const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + u"/diagrams/"_s + key + u".svg"_s;
}

QImage Diagrams::image(const QString &source, bool dark, qreal dpr)
{
    const QString key = keyOf(source, dark);
    const QString cacheKey = key + u'@' + QString::number(dpr);
    if (const auto it = m_images.constFind(cacheKey); it != m_images.constEnd())
        return *it;
    if (QFile::exists(svgPath(key))) {
        QSvgRenderer svg(svgPath(key));
        if (svg.isValid()) {
            const QSize size = svg.defaultSize();
            QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::transparent);
            QPainter p(&img);
            svg.render(&p, QRectF(QPointF(), size));
            p.end();
            m_images.insert(cacheKey, img);
            return img;
        }
    }
    if (!m_running.contains(key) && !m_errors.contains(key) && !renderer().isEmpty())
        start(source, dark, key);
    return {};
}

QString Diagrams::error(const QString &source, bool dark) const
{
    return m_errors.value(keyOf(source, dark));
}

bool Diagrams::pending(const QString &source, bool dark) const
{
    return m_running.contains(keyOf(source, dark));
}

void Diagrams::start(const QString &source, bool dark, const QString &key)
{
    auto *dir = new QTemporaryDir;
    QFile in(dir->filePath(u"diagram.mmd"_s));
    if (!dir->isValid() || !in.open(QIODevice::WriteOnly)) {
        delete dir;
        return;
    }
    in.write(source.toUtf8());
    in.close();
    QDir().mkpath(QFileInfo(svgPath(key)).absolutePath());
    m_running.insert(key);
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, dir, key](int code, QProcess::ExitStatus status) {
        m_running.remove(key);
        const QString out = dir->filePath(u"diagram.svg"_s);
        if (status == QProcess::NormalExit && code == 0 && QFile::exists(out)) {
            QFile::remove(svgPath(key));
            QFile::copy(out, svgPath(key));
        } else {
            const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
            m_errors.insert(key, err.isEmpty() ? tr("mermaid-cli could not draw this diagram.") : err.section(u'\n', 0, 2));
        }
        process->deleteLater();
        delete dir;
        Q_EMIT ready();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, key](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_running.remove(key);
        m_errors.insert(key, tr("Could not start mermaid-cli (%1).").arg(process->program()));
        Q_EMIT ready();
    });
    process->start(renderer(), {u"-i"_s, dir->filePath(u"diagram.mmd"_s), u"-o"_s, dir->filePath(u"diagram.svg"_s),
                                u"-b"_s, u"transparent"_s, u"-t"_s, dark ? u"dark"_s : u"default"_s});
}
