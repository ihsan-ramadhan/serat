#pragma once

#include <QStringList>

namespace Frontmatter {

enum class Type { Text, List, Number, Checkbox, Date, DateTime, Raw };

struct Property {
    QString key;   // empty for comment and blank lines kept between properties
    Type type = Type::Text;
    QString value;
    QStringList items;
    QStringList lines; // source lines, written back unchanged until the property is edited
};

// Understands scalars, flow and block lists; anything else becomes Raw and is kept verbatim.
QList<Property> parse(const QStringList &lines);
QStringList serialize(const Property &p);
bool fixedList(const QString &key); // tags, aliases, cssclasses
bool isDate(const QString &v);
bool isDateTime(const QString &v);
bool isNumber(const QString &v);

}
