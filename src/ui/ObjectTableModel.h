#pragma once

#include "core/DataNode.h"

#include <QAbstractTableModel>
#include <QSet>
#include <QVector>

class ObjectTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Role {
        SourceFileRole = Qt::UserRole + 1
    };

    enum Column {
        IdColumn = 0,
        ParentColumn,
        ElementColumn,
        LocationColumn,
        FileColumn,
        HashColumn,
        StatusColumn,
        ColumnCount
    };

    explicit ObjectTableModel(QObject *parent = nullptr);

    void setNodes(QVector<DataNode> nodes);
    const QVector<DataNode> &nodes() const { return m_nodes; }
    QVector<int> selectedIndices() const;
    void setSelectedIndices(const QVector<int> &indices);
    void setDuplicateIndices(const QSet<int> &indices);
    void clearSelection();

    QModelIndex indexForRow(int row) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

private:
    QVector<DataNode> m_nodes;
    QSet<int> m_duplicateRows;
};
