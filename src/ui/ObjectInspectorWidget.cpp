#include "ui/ObjectInspectorWidget.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextDocument>
#include <QTextOption>
#include <QSyntaxHighlighter>
#include <QVBoxLayout>
#include <QStringList>

namespace {

class XmlHighlighter : public QSyntaxHighlighter
{
public:
    explicit XmlHighlighter(QTextDocument *parent)
        : QSyntaxHighlighter(parent)
    {
    }

protected:
    void highlightBlock(const QString &text) override
    {
        const QTextCharFormat tagFormat = formatFor(QStringLiteral("#8bc5ff"), true);
        const QTextCharFormat attrNameFormat = formatFor(QStringLiteral("#ffd47a"), false);
        const QTextCharFormat attrValueFormat = formatFor(QStringLiteral("#9ef7b6"), false);
        const QTextCharFormat commentFormat = formatFor(QStringLiteral("#8292a6"), false);

        applyRegex(text, QRegularExpression(QStringLiteral("</?[^>\\s/]+")), tagFormat);
        applyRegex(text, QRegularExpression(QStringLiteral("\\b[A-Za-z_:-]+(?=\\=)")), attrNameFormat);
        applyRegex(text, QRegularExpression(QStringLiteral("\"[^\"]*\"")), attrValueFormat);
        applyRegex(text, QRegularExpression(QStringLiteral("<!--.*-->")), commentFormat);
    }

private:
    static QTextCharFormat formatFor(const QString &color, bool bold)
    {
        QTextCharFormat format;
        format.setForeground(QColor(color));
        format.setFontWeight(bold ? QFont::Bold : QFont::DemiBold);
        return format;
    }

    void applyRegex(const QString &text, const QRegularExpression &regex, const QTextCharFormat &format)
    {
        auto it = regex.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};

QString safeText(const QString &text, const QString &fallback = QStringLiteral("N/A"))
{
    return text.isEmpty() ? fallback : text;
}

} // namespace

ObjectInspectorWidget::ObjectInspectorWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("inspectorHeader"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(12, 10, 12, 10);
    headerLayout->setSpacing(4);

    m_titleLabel = new QLabel(QStringLiteral("No object selected"), header);
    m_titleLabel->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel(QStringLiteral("Select a row in the table to inspect XML and properties."), header);
    m_subtitleLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_subtitleLabel->setWordWrap(true);
    headerLayout->addWidget(m_subtitleLabel);
    rootLayout->addWidget(header);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName(QStringLiteral("inspectorSplitter"));

    auto *detailsFrame = new QFrame(splitter);
    detailsFrame->setObjectName(QStringLiteral("inspectorCard"));
    auto *detailsLayout = new QVBoxLayout(detailsFrame);
    detailsLayout->setContentsMargins(12, 12, 12, 12);
    detailsLayout->setSpacing(10);

    m_statusLabel = new QLabel(QStringLiteral("Waiting for selection"), detailsFrame);
    m_statusLabel->setObjectName(QStringLiteral("statusBadge"));
    detailsLayout->addWidget(m_statusLabel, 0, Qt::AlignLeft);

    auto *formFrame = new QFrame(detailsFrame);
    auto *formLayout = new QFormLayout(formFrame);
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->setHorizontalSpacing(12);
    formLayout->setVerticalSpacing(6);

    auto createValueLabel = [formFrame](const QString &objectName) {
        auto *label = new QLabel(formFrame);
        label->setObjectName(objectName);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        return label;
    };

    m_fileValue = createValueLabel(QStringLiteral("inspectorValue"));
    m_typeValue = createValueLabel(QStringLiteral("inspectorValue"));
    m_parentValue = createValueLabel(QStringLiteral("inspectorValue"));
    m_idValue = createValueLabel(QStringLiteral("inspectorValue"));
    m_lineValue = createValueLabel(QStringLiteral("inspectorValue"));
    m_locationValue = createValueLabel(QStringLiteral("inspectorValue"));
    m_hashValue = createValueLabel(QStringLiteral("inspectorValue"));

    formLayout->addRow(QStringLiteral("File"), m_fileValue);
    formLayout->addRow(QStringLiteral("Type"), m_typeValue);
    formLayout->addRow(QStringLiteral("Parent"), m_parentValue);
    formLayout->addRow(QStringLiteral("ID"), m_idValue);
    formLayout->addRow(QStringLiteral("Line"), m_lineValue);
    formLayout->addRow(QStringLiteral("Location"), m_locationValue);
    formLayout->addRow(QStringLiteral("Hash"), m_hashValue);
    detailsLayout->addWidget(formFrame);

    m_attributesTable = new QTableWidget(detailsFrame);
    m_attributesTable->setObjectName(QStringLiteral("inspectorAttributes"));
    m_attributesTable->setColumnCount(2);
    m_attributesTable->setHorizontalHeaderLabels({QStringLiteral("Attribute"), QStringLiteral("Value")});
    m_attributesTable->verticalHeader()->hide();
    m_attributesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attributesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_attributesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_attributesTable->horizontalHeader()->setStretchLastSection(true);
    m_attributesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_attributesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_attributesTable->setAlternatingRowColors(true);
    detailsLayout->addWidget(m_attributesTable, 1);

    auto *xmlFrame = new QFrame(splitter);
    xmlFrame->setObjectName(QStringLiteral("inspectorCard"));
    auto *xmlLayout = new QVBoxLayout(xmlFrame);
    xmlLayout->setContentsMargins(12, 12, 12, 12);
    xmlLayout->setSpacing(8);

    auto *xmlTitle = new QLabel(QStringLiteral("XML Source"), xmlFrame);
    xmlTitle->setObjectName(QStringLiteral("panelTitle"));
    xmlLayout->addWidget(xmlTitle);

    m_xmlView = new QPlainTextEdit(xmlFrame);
    m_xmlView->setObjectName(QStringLiteral("xmlSourceView"));
    m_xmlView->setReadOnly(true);
    m_xmlView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_xmlView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_xmlView->document()->setDefaultFont(QFont(QStringLiteral("Consolas")));
    m_xmlView->setPlaceholderText(QStringLiteral("No XML source available."));
    xmlLayout->addWidget(m_xmlView, 1);
    new XmlHighlighter(m_xmlView->document());

    splitter->addWidget(detailsFrame);
    splitter->addWidget(xmlFrame);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({320, 520});
    rootLayout->addWidget(splitter, 1);

    populateEmptyState();
}

void ObjectInspectorWidget::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    if (m_currentRow >= m_result.nodes.size()) {
        m_currentRow = -1;
    }
    refreshCurrentNode();
}

void ObjectInspectorWidget::setCurrentRow(int row)
{
    m_currentRow = row;
    refreshCurrentNode();
}

void ObjectInspectorWidget::clearSelection()
{
    m_currentRow = -1;
    populateEmptyState();
}

void ObjectInspectorWidget::refreshCurrentNode()
{
    if (m_currentRow < 0 || m_currentRow >= m_result.nodes.size()) {
        populateEmptyState();
        return;
    }

    populateNodeState(m_result.nodes[m_currentRow]);
}

void ObjectInspectorWidget::populateEmptyState()
{
    m_titleLabel->setText(QStringLiteral("No object selected"));
    m_subtitleLabel->setText(QStringLiteral("Select a row in the table to inspect XML, properties and references."));
    m_statusLabel->setText(QStringLiteral("Waiting for selection"));

    m_fileValue->setText(QStringLiteral("N/A"));
    m_typeValue->setText(QStringLiteral("N/A"));
    m_parentValue->setText(QStringLiteral("N/A"));
    m_idValue->setText(QStringLiteral("N/A"));
    m_lineValue->setText(QStringLiteral("N/A"));
    m_locationValue->setText(QStringLiteral("N/A"));
    m_hashValue->setText(QStringLiteral("N/A"));

    m_attributesTable->clearContents();
    m_attributesTable->setRowCount(1);
    m_attributesTable->setItem(0, 0, new QTableWidgetItem(QStringLiteral("No object selected")));
    m_attributesTable->setItem(0, 1, new QTableWidgetItem(QStringLiteral("Choose a data row to see properties.")));

    m_xmlView->setPlainText(QStringLiteral("No object selected.\n\nClick a row in the Objects table to inspect the XML source and properties."));
}

QString ObjectInspectorWidget::statusTextForNode(const DataNode &node) const
{
    QStringList tags;
    if (node.duplicateId) {
        tags.append(QStringLiteral("Duplicate ID"));
    }
    if (node.duplicateContent) {
        tags.append(QStringLiteral("Duplicate XML"));
    }
    if (node.candidateUnused) {
        tags.append(QStringLiteral("Cleanup candidate"));
    }
    if (tags.isEmpty()) {
        tags.append(QStringLiteral("Clean"));
    }
    return tags.join(QStringLiteral(" | "));
}

void ObjectInspectorWidget::populateNodeState(const DataNode &node)
{
    m_titleLabel->setText(node.id.isEmpty() ? node.elementName : node.id);
    m_subtitleLabel->setText(QStringLiteral("%1 | %2").arg(safeText(node.elementName), safeText(node.sourceFile)));
    m_statusLabel->setText(statusTextForNode(node));

    m_fileValue->setText(safeText(node.sourceFile));
    m_typeValue->setText(safeText(node.elementName));
    m_parentValue->setText(safeText(node.parentNode));
    m_idValue->setText(safeText(node.id));
    m_lineValue->setText(node.lineNumber > 0 ? QString::number(node.lineNumber) : QStringLiteral("N/A"));
    m_locationValue->setText(safeText(node.originalLocation));
    m_hashValue->setText(node.contentHash.isEmpty() ? QStringLiteral("N/A") : node.contentHash.left(32));

    m_attributesTable->clearContents();
    const QStringList keys = node.attributes.keys();
    m_attributesTable->setRowCount(qMax(1, keys.size()));
    if (keys.isEmpty()) {
        m_attributesTable->setItem(0, 0, new QTableWidgetItem(QStringLiteral("No attributes")));
        m_attributesTable->setItem(0, 1, new QTableWidgetItem(QStringLiteral("This object does not expose custom XML attributes.")));
    } else {
        int row = 0;
        for (const QString &key : keys) {
            m_attributesTable->setItem(row, 0, new QTableWidgetItem(key));
            m_attributesTable->setItem(row, 1, new QTableWidgetItem(node.attributes.value(key)));
            ++row;
        }
    }

    m_xmlView->setPlainText(node.serializedXml.isEmpty()
                                ? QStringLiteral("No XML source available.")
                                : node.serializedXml);
}
