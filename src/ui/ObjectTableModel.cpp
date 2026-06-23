#include "ui/ObjectTableModel.h"

#include <QBrush>
#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QFileInfo>
#include <QStringList>

namespace {

QString badgeTextForElement(const QString &elementName)
{
    if (elementName.isEmpty()) {
        return QStringLiteral("?");
    }

    QString text;
    for (const QChar ch : elementName) {
        if (ch.isUpper() || ch.isDigit()) {
            text.append(ch);
        }
        if (text.size() == 2) {
            break;
        }
    }

    if (text.isEmpty()) {
        text = elementName.left(1).toUpper();
    }
    return text.left(2);
}

QPixmap buildTypeBadge(const QString &elementName)
{
    QPixmap badge(20, 20);
    badge.fill(Qt::transparent);

    QPainter painter(&badge);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor base = QColor(QStringLiteral("#4f83ff"));
    const QColor accent = QColor(QStringLiteral("#ffb15d"));
    QLinearGradient gradient(0, 0, 20, 20);
    gradient.setColorAt(0.0, base.lighter(130));
    gradient.setColorAt(1.0, accent.darker(110));
    painter.setPen(QPen(QColor(QStringLiteral("#85b4ff")), 1));
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(1.5, 1.5, 17.0, 17.0), 5.0, 5.0);

    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(7);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#f4f8ff")));
    painter.drawText(QRectF(1, 1, 18, 18), Qt::AlignCenter, badgeTextForElement(elementName));
    return badge;
}

QColor statusColor(const DataNode &node)
{
    if (node.candidateUnused) {
        return QColor(QStringLiteral("#5b1f25"));
    }
    if (node.duplicateId || node.duplicateContent) {
        return QColor(QStringLiteral("#7c651f"));
    }
    const QString trimmed = node.serializedXml.trimmed();
    const bool looksEmpty = trimmed.endsWith(QStringLiteral("/>")) || trimmed.contains(QStringLiteral("></"));
    if (looksEmpty) {
        return QColor(QStringLiteral("#6b4518"));
    }
    return QColor(QStringLiteral("#1d3a2a"));
}

QString statusTextForNode(const DataNode &node)
{
    if (node.candidateUnused) {
        return QStringLiteral("Unused candidate");
    }
    if (node.duplicateId && node.duplicateContent) {
        return QStringLiteral("ID collision + duplicate body");
    }
    if (node.duplicateId) {
        return QStringLiteral("ID collision");
    }
    if (node.duplicateContent) {
        return QStringLiteral("Identical body");
    }

    const QString trimmed = node.serializedXml.trimmed();
    if (trimmed.endsWith(QStringLiteral("/>")) || trimmed.contains(QStringLiteral("></"))) {
        return QStringLiteral("Suspicious empty");
    }
    return QStringLiteral("PASS");
}

}

ObjectTableModel::ObjectTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void ObjectTableModel::setNodes(QVector<DataNode> nodes)
{
    beginResetModel();
    m_nodes = std::move(nodes);
    m_duplicateRows.clear();
    endResetModel();
}

QVector<int> ObjectTableModel::selectedIndices() const
{
    QVector<int> indices;
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].selectedForRemoval) {
            indices.append(i);
        }
    }
    return indices;
}

void ObjectTableModel::setSelectedIndices(const QVector<int> &indices)
{
    QSet<int> selection;
    for (int index : indices) {
        selection.insert(index);
    }
    for (int i = 0; i < m_nodes.size(); ++i) {
        m_nodes[i].selectedForRemoval = selection.contains(i);
    }
    if (!m_nodes.isEmpty()) {
        emit dataChanged(index(0, IdColumn), index(m_nodes.size() - 1, StatusColumn));
    }
}

void ObjectTableModel::setDuplicateIndices(const QSet<int> &indices)
{
    m_duplicateRows = indices;
    if (!m_nodes.isEmpty()) {
        emit dataChanged(index(0, IdColumn), index(m_nodes.size() - 1, StatusColumn), {Qt::BackgroundRole, Qt::DisplayRole, Qt::ForegroundRole});
    }
}

void ObjectTableModel::clearSelection()
{
    for (DataNode &node : m_nodes) {
        node.selectedForRemoval = false;
    }
    if (!m_nodes.isEmpty()) {
        emit dataChanged(index(0, IdColumn), index(m_nodes.size() - 1, StatusColumn));
    }
}

QModelIndex ObjectTableModel::indexForRow(int row) const
{
    return index(row, 0);
}

int ObjectTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_nodes.size();
}

int ObjectTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ObjectTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_nodes.size()) {
        return {};
    }

    const DataNode &node = m_nodes[index.row()];
    switch (index.column()) {
    case IdColumn:
        if (role == Qt::DisplayRole) return node.id;
        if (role == Qt::DecorationRole) return buildTypeBadge(node.elementName);
        break;
    case ParentColumn:
        if (role == Qt::DisplayRole) return node.parentNode;
        break;
    case ElementColumn:
        if (role == Qt::DisplayRole) return node.elementName;
        break;
    case LocationColumn:
        if (role == Qt::DisplayRole) return node.originalLocation;
        break;
    case FileColumn:
        if (role == Qt::DisplayRole) return QFileInfo(node.sourceFile).fileName();
        if (role == SourceFileRole) return node.sourceFile;
        break;
    case HashColumn:
        if (role == Qt::DisplayRole) return node.contentHash.left(12);
        break;
    case StatusColumn:
        if (role == Qt::DisplayRole) return statusTextForNode(node);
        if (role == Qt::BackgroundRole) return statusColor(node);
        if (role == Qt::ForegroundRole) return QColor(QStringLiteral("#fff6d8"));
        if (role == Qt::ToolTipRole) {
            if (node.candidateUnused) {
                return QStringLiteral("No incoming XML references. Safe candidate only.");
            }
            if (node.duplicateId || node.duplicateContent) {
                return QStringLiteral("Identical object body or an ID collision was detected.");
            }
            return QStringLiteral("PASS. No strong warnings detected.");
        }
        break;
    default:
        break;
    }

    if (role == Qt::ForegroundRole && node.duplicateId) {
        return QColor(QStringLiteral("#ffb4b4"));
    }
    return {};
}

QVariant ObjectTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case IdColumn: return QStringLiteral("ID");
    case ParentColumn: return QStringLiteral("Parent");
    case ElementColumn: return QStringLiteral("Element");
    case LocationColumn: return QStringLiteral("XML Location");
    case FileColumn: return QStringLiteral("File");
    case HashColumn: return QStringLiteral("Hash");
    case StatusColumn: return QStringLiteral("Status");
    default: return {};
    }
}

Qt::ItemFlags ObjectTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool ObjectTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    Q_UNUSED(index);
    Q_UNUSED(value);
    Q_UNUSED(role);
    return false;
}
