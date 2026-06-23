#pragma once

#include <QSortFilterProxyModel>
#include <QString>

class ObjectFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit ObjectFilterProxyModel(QObject *parent = nullptr);
    void setFilterText(const QString &text);
    void setSourceFileFilter(const QString &sourceFile);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_filterText;
    QString m_sourceFileFilter;
};
