#pragma once

#include "core/AnalysisModels.h"
#include "core/DecorationStreamingPlanner.h"
#include "core/MapPerformanceAnalyzer.h"
#include "core/MapPreviewData.h"

#include <QImage>
#include <QWidget>

class QLabel;
class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStandardItemModel;
class ScannedFileReader;
class QTableView;

class MapPerformancePage : public QWidget
{
    Q_OBJECT

public:
    explicit MapPerformancePage(QWidget *parent = nullptr);

    void setAnalysisResult(const AnalysisResult &result);

signals:
    void operationFinished(const OperationResult &result);

private:
    void rebuild();
    void populateTable();
    void updateDetails();
    void selectCellIndex(int cellIndex);
    void updateDecorPreview();
    void openUnitExclusions();
    void createDecorOptimizedMapCopy();
    void restoreDecorFromGalaxyCopy();
    void createMaximumCompressedCopy();
    static bool readObjectsFile(const AnalysisResult &result, QByteArray *objectsBytes, QString *sourceLabel,
                                const ScannedFileReader *sharedReader = nullptr);
    static bool readRegionsFile(const AnalysisResult &result, QByteArray *regionsBytes, QString *sourceLabel,
                                const ScannedFileReader *sharedReader = nullptr);
    static bool readMinimapImage(const AnalysisResult &result, QImage *image, QString *sourceLabel,
                                 const ScannedFileReader *sharedReader = nullptr);
    static bool readPreviewComponent(const AnalysisResult &result,
                                     const QStringList &fileNames,
                                     qint64 maxBytes,
                                     QByteArray *bytes,
                                     QString *sourceLabel,
                                     const ScannedFileReader *sharedReader = nullptr);
    static sc2dh::preview::MapPreviewData buildMapPreviewData(const AnalysisResult &result,
                                                              const ScannedFileReader *sharedReader = nullptr);
    void startMapPreviewLoad();
    void applyMapPreview(const sc2dh::preview::MapPreviewData &preview);
    QString sourceArchivePath() const;
    QString defaultDecorOutputPath() const;
    QVector<sc2dh::decor::DecorZone> zonesFromModel() const;
    QVector<sc2dh::decor::DecorZone> selectedRegionZones() const;
    void populateRegionSelector();
    void filterRegionList();
    void updateOptimizationScope();
    void toggleRegionFromMap(int regionIndex);
    sc2dh::decor::DecorationSafetyContext decorationSafetyContextFromDoodadTable() const;
    void populateDoodadTable(const QVector<sc2dh::decor::DoodadPlacement> &doodads,
                             const QVector<sc2dh::decor::UnitPlacement> &units = {});
    void updateDoodadTableState(const sc2dh::decor::DecorationVisibilityPlan &plan);
    void updateDoodadTableState(const sc2dh::decor::DecorationStreamingPlan &plan);
    QString detailTextForCell(const sc2dh::perf::MapPerformanceCell &cell) const;

    AnalysisResult m_result;
    QByteArray m_objectsBytes;
    QString m_objectsSourceLabel;
    QImage m_minimapImage;
    QString m_minimapSourceLabel;
    bool m_hasObjects = false;
    sc2dh::region::RegionReadResult m_regionReadResult;
    sc2dh::perf::MapPerformanceReport m_report;
    QVector<sc2dh::decor::DoodadPlacement> m_allDoodads;
    sc2dh::decor::DecorationSafetyContext m_archiveSafetyContext;
    QVector<sc2dh::decor::DecorZone> m_decorZones;
    sc2dh::decor::DecorationVisibilityArtifacts m_decorPreview;
    sc2dh::decor::DecorationOptimizedArtifacts m_recreatePreview;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_warningLabel = nullptr;
    QWidget *m_heatmap = nullptr;
    QStandardItemModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QLabel *m_decorSummaryLabel = nullptr;
    QLabel *m_regionStatusLabel = nullptr;
    QLabel *m_selectedRegionsLabel = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_unitExclusionsButton = nullptr;
    QListWidget *m_regionList = nullptr;
    QLineEdit *m_regionSearch = nullptr;
    QCheckBox *m_showSelectedOnly = nullptr;
    QCheckBox *m_restoreSidecars = nullptr;
    QCheckBox *m_createGuiAction = nullptr;
    QLineEdit *m_guiActionName = nullptr;
    QLineEdit *m_guiPresetName = nullptr;
    QCheckBox *m_keepReferencedTypes = nullptr;
    QCheckBox *m_keepAiTemplates = nullptr;
    QCheckBox *m_keepAiBuildings = nullptr;
    QLineEdit *m_keepUnitTypes = nullptr;
    QLineEdit *m_prefixEdit = nullptr;
    QStandardItemModel *m_doodadModel = nullptr;
    QTableView *m_doodadTable = nullptr;
    QPlainTextEdit *m_galaxyPreview = nullptr;
    QPushButton *m_createCopyButton = nullptr;
    QPushButton *m_restoreButton = nullptr;
    QPushButton *m_compressButton = nullptr;
    quint64 m_previewGeneration = 0;
};
