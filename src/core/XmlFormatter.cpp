#include "core/XmlFormatter.h"

#include <pugixml.hpp>

#include <sstream>

QString XmlFormatter::formatDocument(const QByteArray &xmlBytes, QString *errorMessage) const
{
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(xmlBytes.constData(), static_cast<size_t>(xmlBytes.size()));
    if (!result)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("XML parse error: %1").arg(QString::fromUtf8(result.description()));
        }
        return {};
    }

    std::ostringstream stream;
    document.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
    return QString::fromUtf8(stream.str().c_str());
}

QString XmlFormatter::formatNode(const pugi::xml_node &node, QString *errorMessage) const
{
    Q_UNUSED(errorMessage)
    std::ostringstream stream;
    node.print(stream, "  ", pugi::format_default, pugi::encoding_utf8);
    return QString::fromUtf8(stream.str().c_str());
}
