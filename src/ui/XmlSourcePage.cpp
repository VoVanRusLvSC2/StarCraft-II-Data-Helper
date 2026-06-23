#include "ui/XmlSourcePage.h"

#include <QColor>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>

namespace {
class XmlFileHighlighter final : public QSyntaxHighlighter
{
public:
    explicit XmlFileHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}
protected:
    void highlightBlock(const QString &text) override
    {
        apply(text, QRegularExpression(QStringLiteral("</?[^>\\s/]+")), QStringLiteral("#8bc5ff"), true);
        apply(text, QRegularExpression(QStringLiteral("\\b[A-Za-z_:-]+(?=\\=)")), QStringLiteral("#ffd47a"), false);
        apply(text, QRegularExpression(QStringLiteral("\"[^\"]*\"")), QStringLiteral("#9ef7b6"), false);
        apply(text, QRegularExpression(QStringLiteral("<!--.*-->")), QStringLiteral("#8292a6"), false);
    }
private:
    void apply(const QString &text, const QRegularExpression &regex, const QString &color, bool bold)
    {
        QTextCharFormat format; format.setForeground(QColor(color)); format.setFontWeight(bold ? QFont::Bold : QFont::Normal);
        auto matches = regex.globalMatch(text);
        while (matches.hasNext()) { const auto match = matches.next(); setFormat(match.capturedStart(), match.capturedLength(), format); }
    }
};
}

XmlSourcePage::XmlSourcePage(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(8, 8, 8, 8);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("XML file:"), this));
    m_files = new QComboBox(this); top->addWidget(m_files, 1);
    m_location = new QLabel(QStringLiteral("No XML loaded"), this); m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(m_location); layout->addLayout(top);
    m_editor = new QPlainTextEdit(this); m_editor->setReadOnly(true); m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setObjectName(QStringLiteral("fullXmlSourceView")); m_editor->document()->setDefaultFont(QFont(QStringLiteral("Consolas"), 10));
    layout->addWidget(m_editor, 1); new XmlFileHighlighter(m_editor->document());
    connect(m_files, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) showFile(m_files->itemData(index).toString());
    });
}

void XmlSourcePage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result; m_files->clear(); m_editor->clear();
    QStringList files = m_result.sourceXmlByFile.keys(); std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    for (const QString &file : files) m_files->addItem(QFileInfo(file).fileName(), file);
    if (files.isEmpty()) m_location->setText(QStringLiteral("Analyze a source to view full XML."));
}

void XmlSourcePage::showFile(const QString &filePath, int line)
{
    QString text = m_result.sourceXmlByFile.value(filePath);
    if (text.isEmpty()) { QFile file(filePath); if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.readAll()); }
    m_editor->setPlainText(text); m_location->setText(QStringLiteral("%1 | line %2").arg(filePath).arg(line > 0 ? QString::number(line) : QStringLiteral("-")));
    QList<QTextEdit::ExtraSelection> selections;
    if (line > 0) {
        QTextBlock block = m_editor->document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextCursor cursor(block); QTextEdit::ExtraSelection selection; selection.cursor = cursor;
            selection.format.setBackground(QColor(39, 105, 84, 145)); selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selections << selection; m_editor->setTextCursor(cursor); m_editor->centerCursor();
        }
    }
    m_editor->setExtraSelections(selections);
}

void XmlSourcePage::showNode(int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= m_result.nodes.size()) return;
    const DataNode &node = m_result.nodes[nodeIndex];
    const int comboIndex = m_files->findData(node.sourceFile);
    if (comboIndex >= 0) {
        m_files->blockSignals(true); m_files->setCurrentIndex(comboIndex); m_files->blockSignals(false);
    }
    showFile(node.sourceFile, node.lineNumber);
}
