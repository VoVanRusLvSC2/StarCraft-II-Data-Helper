#include "ui/DataCollectionPage.h"

#include "core/UnitFamilyDetector.h"

#include <QAbstractItemView>
#include <QAbstractAnimation>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFileInfo>
#include <QEvent>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <pugixml.hpp>
#include <algorithm>

namespace {
class ComboPopupFade final : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget) {
                auto *effect = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
                if (!effect) {
                    effect = new QGraphicsOpacityEffect(widget);
                    widget->setGraphicsEffect(effect);
                }
                effect->setOpacity(0.72);
                auto *animation = new QPropertyAnimation(effect, "opacity", widget);
                animation->setDuration(150);
                animation->setStartValue(0.72);
                animation->setEndValue(1.0);
                animation->setEasingCurve(QEasingCurve::OutCubic);
                animation->start(QAbstractAnimation::DeleteWhenStopped);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

UnitFamilyRole genericRole(const QString &type)
{
    const QString value = type.toLower();
    if (value == QStringLiteral("cunit")) return UnitFamilyRole::Unit;
    if (value.startsWith(QStringLiteral("cactor"))) return UnitFamilyRole::Other;
    if (value.startsWith(QStringLiteral("cbutton"))) return UnitFamilyRole::Button;
    if (value.startsWith(QStringLiteral("cmodel"))) return UnitFamilyRole::Model;
    if (value.startsWith(QStringLiteral("csound"))) return UnitFamilyRole::Other;
    if (value.startsWith(QStringLiteral("cweapon"))) return UnitFamilyRole::Weapon;
    if (value.startsWith(QStringLiteral("cabil"))) return UnitFamilyRole::Ability;
    if (value.startsWith(QStringLiteral("ceffect"))) return UnitFamilyRole::Effect;
    if (value.startsWith(QStringLiteral("cbehavior"))) return UnitFamilyRole::Behavior;
    if (value.startsWith(QStringLiteral("cvalidator"))) return UnitFamilyRole::Validator;
    if (value.startsWith(QStringLiteral("crequirement"))) return UnitFamilyRole::Requirement;
    if (value.startsWith(QStringLiteral("cupgrade"))) return UnitFamilyRole::Upgrade;
    return UnitFamilyRole::ManualReview;
}
}

DataCollectionPage::DataCollectionPage(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(8, 8, 8, 8);
    auto *header = new QFrame(this); header->setObjectName(QStringLiteral("bucketCard"));
    auto *headerLayout = new QVBoxLayout(header);
    auto *title = new QLabel(QStringLiteral("Data Collection Unit Builder"), header); title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);
    m_summary = new QLabel(QStringLiteral("Analyze data to detect standardized unit families."), header);
    m_summary->setObjectName(QStringLiteral("inspectorSubtitle")); m_summary->setWordWrap(true); headerLayout->addWidget(m_summary);
    auto *familyCard = new QFrame(header);
    familyCard->setObjectName(QStringLiteral("familySelectorCard"));
    auto *familyRow = new QHBoxLayout(familyCard); familyRow->setContentsMargins(14, 10, 14, 10);
    familyRow->addWidget(new QLabel(QStringLiteral("Unit family:"), familyCard));
    m_selector = new QComboBox(familyCard);
    m_selector->setObjectName(QStringLiteral("familySelector"));
    m_selector->view()->setObjectName(QStringLiteral("familySelectorPopup"));
    m_selector->view()->setMouseTracking(true);
    m_selector->view()->window()->installEventFilter(new ComboPopupFade(m_selector->view()->window()));
    familyRow->addWidget(m_selector, 1);
    m_root = new QLabel(QStringLiteral("Root ID: -"), familyCard); familyRow->addWidget(m_root);
    m_standard = new QLabel(QStringLiteral("Standard status: -"), familyCard); familyRow->addWidget(m_standard);
    m_existing = new QLabel(QStringLiteral("Existing collection: -"), familyCard); familyRow->addWidget(m_existing);
    headerLayout->addWidget(familyCard);
    m_fileStatus = new QLabel(QStringLiteral("Target XML: DataCollectionData.xml | (listfile): not checked"), header);
    m_fileStatus->setObjectName(QStringLiteral("inspectorSubtitle")); headerLayout->addWidget(m_fileStatus);
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(QStringLiteral("Current unit name:"), header));
    m_currentUnitName = new QLineEdit(header); m_currentUnitName->setReadOnly(true); nameRow->addWidget(m_currentUnitName);
    nameRow->addWidget(new QLabel(QStringLiteral("New unit name:"), header));
    m_newUnitName = new QLineEdit(header); nameRow->addWidget(m_newUnitName);
    auto *nameHint = new QLabel(QStringLiteral("Real ID changes are applied only through Rename To Standard."), header);
    nameHint->setObjectName(QStringLiteral("inspectorSubtitle")); nameRow->addWidget(nameHint, 1);
    headerLayout->addLayout(nameRow);
    auto *fields = new QHBoxLayout;
    fields->addWidget(new QLabel(QStringLiteral("Parent:"), header));
    m_parent = new QLineEdit(QStringLiteral("UnitGround"), header); fields->addWidget(m_parent);
    fields->addWidget(new QLabel(QStringLiteral("Editor categories:"), header));
    m_categories = new QLineEdit(QStringLiteral("DataFamily:Campaign,DataGroup:Unit,ObjectType:Hero,Race:Terran"), header);
    fields->addWidget(m_categories, 1);
    m_confirmNonStandard = new QCheckBox(QStringLiteral("Manually confirm non-standard family preview"), header);
    fields->addWidget(m_confirmNonStandard); headerLayout->addLayout(fields); layout->addWidget(header);

    m_entryTabs = new QTabWidget(this);
    m_familyTree = new QTreeWidget;
    m_familyTree->setObjectName(QStringLiteral("dataCollectionFamilyTree"));
    m_familyTree->setColumnCount(5);
    m_familyTree->setHeaderLabels({QStringLiteral("Collection / Object"), QStringLiteral("XML Type"),
                                    QStringLiteral("Role"), QStringLiteral("File"), QStringLiteral("Status")});
    m_familyTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_familyTree->setAlternatingRowColors(true);
    m_familyTree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_addableTable = createEntryTable();
    m_existingTable = createEntryTable();
    m_reviewTable = createEntryTable();
    m_entryTabs->addTab(m_familyTree, QStringLiteral("Unit Families"));
    m_entryTabs->addTab(m_addableTable, QStringLiteral("Can Add"));
    m_entryTabs->addTab(m_existingTable, QStringLiteral("Already Exists"));
    m_entryTabs->addTab(m_reviewTable, QStringLiteral("Recommendations / Review"));
    layout->addWidget(m_entryTabs, 1);
    auto *previewRow = new QHBoxLayout;
    m_xml = new QTextEdit(this); m_xml->setReadOnly(true); m_xml->setPlaceholderText(QStringLiteral("Generated XML preview"));
    m_warnings = new QTextEdit(this); m_warnings->setReadOnly(true); m_warnings->setPlaceholderText(QStringLiteral("Warnings and report"));
    previewRow->addWidget(m_xml, 1); previewRow->addWidget(m_warnings, 1); layout->addLayout(previewRow, 1);
    auto *buttons = new QHBoxLayout;
    auto *preview = new QPushButton(QStringLiteral("Preview Collection"), this);
    m_apply = new QPushButton(QStringLiteral("Apply Collection"), this); m_apply->setEnabled(false);
    auto *exportButton = new QPushButton(QStringLiteral("Export Collection Report"), this);
    buttons->addWidget(preview); buttons->addWidget(m_apply); buttons->addWidget(exportButton); buttons->addStretch(1); layout->addLayout(buttons);
    connect(m_selector, &QComboBox::currentIndexChanged, this, [this] { rebuildFamily(); });
    for (QTableWidget *table : {m_addableTable, m_existingTable, m_reviewTable})
        connect(table, &QTableWidget::itemChanged, this, [this] { m_apply->setEnabled(false); });
    connect(m_familyTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        QTreeWidgetItem *item = m_familyTree->currentItem();
        if (!item) return;
        QTreeWidgetItem *family = item->parent() ? item->parent() : item;
        const int familyIndex = family->data(0, Qt::UserRole).toInt();
        if (familyIndex >= 0 && familyIndex < m_families.size() && m_selector->currentIndex() != familyIndex)
            m_selector->setCurrentIndex(familyIndex);
    });
    connect(m_familyTree, &QTreeWidget::itemChanged, this, [this] { m_apply->setEnabled(false); });
    connect(m_parent, &QLineEdit::textChanged, this, [this] { m_apply->setEnabled(false); });
    connect(m_categories, &QLineEdit::textChanged, this, [this] { m_apply->setEnabled(false); });
    connect(m_newUnitName, &QLineEdit::textChanged, this, [this] { m_apply->setEnabled(false); });
    connect(m_confirmNonStandard, &QCheckBox::toggled, this, [this] { m_apply->setEnabled(false); });
    connect(preview, &QPushButton::clicked, this, [this] { emit previewRequested(currentRequest()); });
    connect(m_apply, &QPushButton::clicked, this, [this] { emit applyRequested(currentRequest()); });
    connect(exportButton, &QPushButton::clicked, this, [this] { emit exportRequested(m_previewReport.reportText); });
}

QTableWidget *DataCollectionPage::createEntryTable() const
{
    auto *table = new QTableWidget;
    table->setColumnCount(8);
    table->setHorizontalHeaderLabels({QStringLiteral("Include"), QStringLiteral("Real Type"), QStringLiteral("Real Object ID"),
        QStringLiteral("Detected Role"), QStringLiteral("DataRecord Alias"), QStringLiteral("Found/Missing"),
        QStringLiteral("Confidence"), QStringLiteral("Recommendation / Action")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setTextElideMode(Qt::ElideNone);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    return table;
}

void DataCollectionPage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result; m_families = UnitFamilyDetector().detect(result);
    const QSignalBlocker blocker(m_selector); m_selector->clear();
    for (const UnitFamily &family : m_families) m_selector->addItem(QStringLiteral("%1 (%2 objects)").arg(family.rootId).arg(family.objects.size()));
    {
        const QSignalBlocker treeBlock(m_familyTree);
        m_familyTree->clear();
        for (int familyIndex = 0; familyIndex < m_families.size(); ++familyIndex) {
            const UnitFamily &family = m_families[familyIndex];
            auto *root = new QTreeWidgetItem(m_familyTree);
            root->setText(0, family.rootId);
            root->setText(1, QStringLiteral("CDataCollectionUnit"));
            root->setText(2, QStringLiteral("Main collection"));
            root->setText(4, QStringLiteral("%1 child objects").arg(family.objects.size()));
            root->setData(0, Qt::UserRole, familyIndex);
            QFont font = root->font(0); font.setBold(true); root->setFont(0, font);
            for (const UnitFamilyObject &object : family.objects) {
                if (object.nodeIndex < 0 || object.nodeIndex >= m_result.nodes.size()) continue;
                const DataNode &node = m_result.nodes[object.nodeIndex];
                auto *child = new QTreeWidgetItem(root);
                child->setText(0, node.id);
                child->setText(1, node.elementName);
                child->setText(2, unitFamilyRoleName(object.role));
                child->setText(3, QFileInfo(node.sourceFile).fileName());
                child->setText(4, object.confidence);
                child->setData(0, Qt::UserRole, object.nodeIndex);
                child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
                child->setCheckState(0, object.role == UnitFamilyRole::ManualReview ? Qt::Unchecked : Qt::Checked);
            }
            root->setExpanded(familyIndex == 0);
        }
        for (int column = 0; column < m_familyTree->columnCount(); ++column) m_familyTree->resizeColumnToContents(column);
    }
    if (!m_families.isEmpty()) m_selector->setCurrentIndex(0);
    m_summary->setText(QStringLiteral("%1 standardized-family candidates detected.").arg(m_families.size()));
    m_apply->setEnabled(false); m_xml->clear(); m_warnings->clear(); rebuildFamily();
}

void DataCollectionPage::rebuildFamily()
{
    for (QTableWidget *table : {m_addableTable, m_existingTable, m_reviewTable}) table->setRowCount(0);
    const int index = m_selector->currentIndex();
    if (index < 0 || index >= m_families.size()) { m_root->setText(QStringLiteral("Root ID: -")); return; }
    DataCollectionBuildRequest request; request.family = m_families[index]; request.confirmNonStandard = true;
    request.requestedUnitId = request.family.rootId;
    const DataCollectionPreviewReport view = DataCollectionUnitBuilder().preview(m_result, request);
    m_root->setText(QStringLiteral("Root ID: %1").arg(request.family.rootId));
    {
        const QSignalBlocker currentBlock(m_currentUnitName), newBlock(m_newUnitName);
        m_currentUnitName->setText(request.family.rootId);
        m_newUnitName->setText(request.family.rootId);
    }
    m_standard->setText(QStringLiteral("Standard status: %1").arg(view.familyStandardized ? QStringLiteral("standard") : QStringLiteral("non-standard")));
    m_existing->setText(QStringLiteral("Existing collection: %1").arg(view.existingCollection ? QStringLiteral("found") : QStringLiteral("not found")));
    m_fileStatus->setText(QStringLiteral("Target XML: %1 | Archive entry: %2 | (listfile): %3")
                              .arg(QFileInfo(view.targetFile).fileName(), view.archiveEntry,
                                   view.listfileNeedsUpdate ? QStringLiteral("will add entry") : QStringLiteral("entry found")));
    {
        const QSignalBlocker parentBlock(m_parent), categoryBlock(m_categories);
        m_parent->setText(QStringLiteral("UnitGround"));
        m_categories->setText(QStringLiteral("DataFamily:Campaign,DataGroup:Unit,ObjectType:Hero,Race:Terran"));
    }
    if (view.existingCollection) {
        for (const DataNode &node : m_result.nodes) if (node.elementName == QStringLiteral("CDataCollectionUnit") && node.id == request.family.rootId) {
            pugi::xml_document fragment;
            if (fragment.load_string(node.serializedXml.toUtf8().constData())) {
                const pugi::xml_node collection = fragment.first_child();
                const QSignalBlocker parentBlock(m_parent), categoryBlock(m_categories);
                if (collection.attribute("parent")) m_parent->setText(QString::fromUtf8(collection.attribute("parent").value()));
                if (collection.child("EditorCategories")) m_categories->setText(QString::fromUtf8(collection.child("EditorCategories").attribute("value").value()));
            }
        }
    }
    for (int top = 0; top < m_familyTree->topLevelItemCount(); ++top) {
        QTreeWidgetItem *item = m_familyTree->topLevelItem(top);
        item->setExpanded(top == index);
        if (top == index) m_familyTree->setCurrentItem(item);
    }
    m_entryTabs->setTabText(0, QStringLiteral("Unit Families (%1)").arg(m_families.size()));
    populateTables(view);
}

void DataCollectionPage::populateTables(const DataCollectionPreviewReport &report)
{
    for (QTableWidget *table : {m_addableTable, m_existingTable, m_reviewTable}) table->setRowCount(0);
    const auto addRow = [](QTableWidget *table, const DataCollectionEntryProposal &entry, bool checkable, bool checked) {
        const int row = table->rowCount(); table->insertRow(row);
        const QStringList values{QString(), entry.realType, entry.realId, unitFamilyRoleName(entry.role), entry.alias,
            entry.alias.isEmpty() ? QStringLiteral("Missing / unresolved") : QStringLiteral("Found"), entry.confidence, entry.status};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values[column]); item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            if (column == 0 && checkable) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
                item->setData(Qt::UserRole, entry.nodeIndex);
            }
            table->setItem(row, column, item);
        }
    };
    const QSignalBlocker addBlock(m_addableTable), existingBlock(m_existingTable), reviewBlock(m_reviewTable);
    for (const DataCollectionEntryProposal &entry : report.entries) {
        if (entry.status.startsWith(QStringLiteral("Already exists"))) addRow(m_existingTable, entry, false, false);
        else if (entry.status.startsWith(QStringLiteral("Will add"))) addRow(m_addableTable, entry, true, entry.included);
        else addRow(m_reviewTable, entry, !entry.alias.isEmpty(), entry.included && entry.status.contains(QStringLiteral("confirmed")));
    }
    DataCollectionAliasMapper mapper;
    for (const QString &missingId : report.missingExpectedObjects) {
        const QString root = report.request.family.rootId;
        const QString suffix = missingId.mid(root.size());
        DataCollectionEntryProposal missing; missing.realId = missingId; missing.confidence = QStringLiteral("Profile recommendation");
        if (suffix.isEmpty()) { missing.realType = QStringLiteral("CUnit"); missing.role = UnitFamilyRole::Unit; }
        else if (suffix == QStringLiteral("Actor")) { missing.realType = QStringLiteral("CActorUnit"); missing.role = UnitFamilyRole::Actor; }
        else if (suffix == QStringLiteral("Button")) { missing.realType = QStringLiteral("CButton"); missing.role = UnitFamilyRole::Button; }
        else if (suffix.contains(QStringLiteral("Model"))) { missing.realType = QStringLiteral("CModel"); missing.role = UnitFamilyRole::Model; }
        else { missing.realType = QStringLiteral("CSound"); missing.role = UnitFamilyRole::Other; }
        DataNode synthetic; synthetic.elementName = missing.realType; synthetic.id = missingId;
        missing.alias = mapper.aliasFor(synthetic, root, missing.role);
        missing.status = QStringLiteral("Recommended real object is missing; create/rename it before adding this alias");
        addRow(m_reviewTable, missing, false, false);
    }
    m_entryTabs->setTabText(1, QStringLiteral("Can Add (%1)").arg(m_addableTable->rowCount()));
    m_entryTabs->setTabText(2, QStringLiteral("Already Exists (%1)").arg(m_existingTable->rowCount()));
    m_entryTabs->setTabText(3, QStringLiteral("Recommendations / Review (%1)").arg(m_reviewTable->rowCount()));
    for (QTableWidget *table : {m_addableTable, m_existingTable, m_reviewTable}) table->resizeColumnsToContents();
    m_summary->setText(QStringLiteral("%1 can add | %2 already exist | %3 recommendations/review")
                           .arg(m_addableTable->rowCount()).arg(m_existingTable->rowCount()).arg(m_reviewTable->rowCount()));
}

DataCollectionBuildRequest DataCollectionPage::currentRequest() const
{
    DataCollectionBuildRequest request;
    const int index = m_selector->currentIndex(); if (index < 0 || index >= m_families.size()) return request;
    request.family = m_families[index]; request.parent = m_parent->text().trimmed(); request.editorCategories = m_categories->text().trimmed();
    request.requestedUnitId = m_newUnitName->text().trimmed();
    QVector<UnitFamilyObject> selectedObjects;
    QHash<int, UnitFamilyObject> detected;
    for (const UnitFamilyObject &object : request.family.objects) detected.insert(object.nodeIndex, object);
    QTreeWidgetItem *familyItem = index < m_familyTree->topLevelItemCount() ? m_familyTree->topLevelItem(index) : nullptr;
    if (familyItem) for (int childIndex = 0; childIndex < familyItem->childCount(); ++childIndex) {
        QTreeWidgetItem *child = familyItem->child(childIndex);
        if (child->checkState(0) != Qt::Checked) continue;
        const int nodeIndex = child->data(0, Qt::UserRole).toInt();
        if (detected.contains(nodeIndex)) selectedObjects << detected.value(nodeIndex);
    }
    if (!detected.contains(request.family.rootNodeIndex)
        || std::none_of(selectedObjects.cbegin(), selectedObjects.cend(), [&](const UnitFamilyObject &object) { return object.nodeIndex == request.family.rootNodeIndex; }))
        selectedObjects << UnitFamilyObject{request.family.rootNodeIndex, UnitFamilyRole::Unit, QStringLiteral("High"), QStringLiteral("Root CUnit")};
    request.family.objects = selectedObjects;
    request.confirmNonStandard = m_confirmNonStandard->isChecked();
    for (QTableWidget *table : {m_addableTable, m_reviewTable})
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem *item = table->item(row, 0);
            if (item && item->flags().testFlag(Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked)
                request.includedNodeIndices.insert(item->data(Qt::UserRole).toInt());
        }
    if (request.includedNodeIndices.isEmpty()) request.includedNodeIndices.insert(-1);
    return request;
}

void DataCollectionPage::setPreviewReport(const DataCollectionPreviewReport &report)
{
    m_previewReport = report; m_xml->setPlainText(report.generatedXml); m_warnings->setPlainText(report.reportText);
    populateTables(report);
    m_standard->setText(QStringLiteral("Standard status: %1").arg(report.familyStandardized ? QStringLiteral("standard") : QStringLiteral("non-standard")));
    m_existing->setText(QStringLiteral("Existing collection: %1").arg(report.existingCollection ? QStringLiteral("found") : QStringLiteral("not found")));
    m_fileStatus->setText(QStringLiteral("Target XML: %1 | Archive entry: %2 | (listfile): %3")
                              .arg(QFileInfo(report.targetFile).fileName(), report.archiveEntry,
                                   report.listfileNeedsUpdate ? QStringLiteral("will add entry") : QStringLiteral("entry found")));
    m_apply->setEnabled(report.valid);
}

void DataCollectionPage::setApplyAvailable(bool available) { if (!available) m_apply->setEnabled(false); }
