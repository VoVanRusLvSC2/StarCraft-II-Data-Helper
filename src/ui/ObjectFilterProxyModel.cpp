#include "ui/ObjectFilterProxyModel.h"
#include "ui/ObjectTableModel.h"

#include <QAbstractItemModel>

ObjectFilterProxyModel::ObjectFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setDynamicSortFilter(true);
}

void ObjectFilterProxyModel::setFilterText(const QString &text)
{
    m_filterText = text;
    invalidateFilter();
}

void ObjectFilterProxyModel::setSourceFileFilter(const QString &sourceFile)
{
    m_sourceFileFilter = sourceFile.trimmed();
    invalidateFilter();
}

bool ObjectFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *model = sourceModel();
    if (!model) {
        return true;
    }

    if (!m_sourceFileFilter.isEmpty()) {
        const QModelIndex fileIndex = model->index(sourceRow, ObjectTableModel::FileColumn, sourceParent);
        const QString fileValue = model->data(fileIndex, ObjectTableModel::SourceFileRole).toString();
        if (fileValue != m_sourceFileFilter) {
            return false;
        }
    }

    if (m_filterText.isEmpty()) {
        return true;
    }

    for (int column = 0; column < model->columnCount(); ++column) {
        const QModelIndex index = model->index(sourceRow, column, sourceParent);
        const QVariant value = model->data(index, Qt::DisplayRole);
        if (value.toString().contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
