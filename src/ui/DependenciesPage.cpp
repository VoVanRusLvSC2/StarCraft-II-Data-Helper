#include "ui/DependenciesPage.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QSet>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QIcon buildDotIcon(const QColor &color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color.lighter(140), 1));
    painter.setBrush(color);
    painter.drawEllipse(QRectF(1.5, 1.5, 11.0, 11.0));
    return QIcon(pixmap);
}

QWidget *createBucketCard(const QString &title, QLabel **countLabel, QListWidget **listWidget, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("bucketCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout();
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("panelTitle"));
    *countLabel = new QLabel(QStringLiteral("0"), card);
    (*countLabel)->setObjectName(QStringLiteral("bucketCount"));
    header->addWidget(titleLabel);
    header->addStretch(1);
    header->addWidget(*countLabel);
    layout->addLayout(header);

    auto *list = new QListWidget(card);
    list->setObjectName(QStringLiteral("dependencyList"));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    *listWidget = list;
    layout->addWidget(list, 1);
    return card;
}

QString joinNodeLabel(const DataNode &node)
{
    QStringList parts;
    if (!node.id.isEmpty()) {
        parts.append(node.id);
    }
    if (!node.elementName.isEmpty()) {
        parts.append(node.elementName);
    }
    if (!node.sourceFile.isEmpty()) {
        parts.append(node.sourceFile);
    }
    return parts.isEmpty() ? QStringLiteral("Unknown node") : parts.join(QStringLiteral(" | "));
}

} // namespace

DependenciesPage::DependenciesPage(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("dependenciesHeader"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(12, 10, 12, 10);
    headerLayout->setSpacing(4);

    m_titleLabel = new QLabel(QStringLiteral("Dependencies"), header);
    m_titleLabel->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel(QStringLiteral("Select an object to inspect incoming, outgoing and missing references."), header);
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    headerLayout->addWidget(m_subtitleLabel);
    rootLayout->addWidget(header);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("dependenciesSplitter"));

    auto *leftCard = createBucketCard(QStringLiteral("Incoming"), &m_incomingCount, &m_incomingList, splitter);
    auto *centerCard = createBucketCard(QStringLiteral("Outgoing"), &m_outgoingCount, &m_outgoingList, splitter);
    auto *rightCard = createBucketCard(QStringLiteral("Missing"), &m_missingCount, &m_missingList, splitter);

    splitter->addWidget(leftCard);
    splitter->addWidget(centerCard);
    splitter->addWidget(rightCard);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({420, 420, 420});
    rootLayout->addWidget(splitter, 1);

    populateBucket(m_incomingList, {}, QColor(QStringLiteral("#7fd0ff")), QStringLiteral("No incoming references"));
    populateBucket(m_outgoingList, {}, QColor(QStringLiteral("#ffce78")), QStringLiteral("No outgoing references"));
    populateBucket(m_missingList, {}, QColor(QStringLiteral("#ff8a8a")), QStringLiteral("No missing references"));
}

void DependenciesPage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    m_graph.build(m_result.nodes);
    if (m_currentRow >= m_result.nodes.size()) {
        m_currentRow = -1;
    }
    refreshView();
}

void DependenciesPage::setCurrentRow(int row)
{
    m_currentRow = row;
    refreshView();
}

QStringList DependenciesPage::uniqueSorted(const QStringList &values) const
{
    QSet<QString> set;
    for (const QString &value : values) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            set.insert(trimmed);
        }
    }
    QStringList sorted = set.values();
    std::sort(sorted.begin(), sorted.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return sorted;
}

QString DependenciesPage::describeNodeById(const QString &id) const
{
    for (const DataNode &node : m_result.nodes) {
        if (node.id == id) {
            return joinNodeLabel(node);
        }
    }
    return QStringLiteral("%1 | not found in registry").arg(id);
}

void DependenciesPage::populateBucket(QListWidget *listWidget,
                                      const QStringList &values,
                                      const QColor &accent,
                                      const QString &emptyText)
{
    listWidget->clear();
    if (values.isEmpty()) {
        auto *emptyItem = new QListWidgetItem(emptyText, listWidget);
        emptyItem->setFlags(Qt::ItemIsEnabled);
        emptyItem->setForeground(QColor(QStringLiteral("#91a3b7")));
        emptyItem->setIcon(buildDotIcon(QColor(QStringLiteral("#445266"))));
        return;
    }

    for (const QString &value : values) {
        auto *item = new QListWidgetItem(buildDotIcon(accent), value, listWidget);
        item->setToolTip(value);
        item->setForeground(accent.lighter(120));
    }
}

void DependenciesPage::refreshView()
{
    if (m_currentRow < 0 || m_currentRow >= m_result.nodes.size()) {
        m_titleLabel->setText(QStringLiteral("Dependencies"));
        m_subtitleLabel->setText(QStringLiteral("No object selected"));
        m_incomingCount->setText(QStringLiteral("0"));
        m_outgoingCount->setText(QStringLiteral("0"));
        m_missingCount->setText(QStringLiteral("0"));
        populateBucket(m_incomingList, {}, QColor(QStringLiteral("#7fd0ff")), QStringLiteral("No object selected"));
        populateBucket(m_outgoingList, {}, QColor(QStringLiteral("#ffce78")), QStringLiteral("No object selected"));
        populateBucket(m_missingList, {}, QColor(QStringLiteral("#ff8a8a")), QStringLiteral("No object selected"));
        return;
    }

    const DataNode &node = m_result.nodes[m_currentRow];
    const QString nodeLabel = node.id.isEmpty() ? node.elementName : node.id;
    m_titleLabel->setText(nodeLabel.isEmpty() ? QStringLiteral("Dependencies") : nodeLabel);
    m_subtitleLabel->setText(QStringLiteral("%1 | %2").arg(node.elementName.isEmpty() ? QStringLiteral("Unknown type") : node.elementName,
                                                          node.sourceFile.isEmpty() ? QStringLiteral("Unknown source") : node.sourceFile));

    const QStringList incomingIds = uniqueSorted(m_graph.inboundReferencesFor(node.id));
    const QStringList outgoingIds = uniqueSorted(node.referencedIds);
    QSet<QString> knownIds;
    for (const DataNode &candidate : m_result.nodes) {
        if (!candidate.id.isEmpty()) {
            knownIds.insert(candidate.id);
        }
    }

    QStringList missingIds;
    for (const QString &reference : outgoingIds) {
        if (!knownIds.contains(reference) && reference != node.id) {
            missingIds.append(reference);
        }
    }
    missingIds.removeDuplicates();
    std::sort(missingIds.begin(), missingIds.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });

    m_incomingCount->setText(QString::number(incomingIds.size()));
    m_outgoingCount->setText(QString::number(outgoingIds.size()));
    m_missingCount->setText(QString::number(missingIds.size()));

    QStringList incomingEntries;
    for (const QString &id : incomingIds) {
        incomingEntries.append(describeNodeById(id));
    }

    QStringList outgoingEntries;
    for (const QString &id : outgoingIds) {
        outgoingEntries.append(describeNodeById(id));
    }

    QStringList missingEntries;
    for (const QString &id : missingIds) {
        missingEntries.append(QStringLiteral("%1 | missing reference").arg(id));
    }

    populateBucket(m_incomingList, incomingEntries, QColor(QStringLiteral("#7fd0ff")), QStringLiteral("No incoming references"));
    populateBucket(m_outgoingList, outgoingEntries, QColor(QStringLiteral("#ffce78")), QStringLiteral("No outgoing references"));
    populateBucket(m_missingList, missingEntries, QColor(QStringLiteral("#ff8a8a")), QStringLiteral("No missing references"));
}
