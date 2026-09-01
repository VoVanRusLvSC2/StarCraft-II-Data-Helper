#pragma once

#include "core/AnalysisModels.h"
#include "core/DecorationStreamingPlanner.h"
#include "core/MapPerformanceAnalyzer.h"

#include <QImage>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStandardItemModel;
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
    void createDecorOptimizedMapCopy();
    bool readObjectsFile(QByteArray *objectsBytes, QString *sourceLabel) const;
    bool readRegionsFile(QByteArray *regionsBytes, QString *sourceLabel) const;
    bool readMinimapImage(QImage *image, QString *sourceLabel) const;
    QString sourceArchivePath() const;
    QString defaultDecorOutputPath() const;
    QVector<sc2dh::decor::DecorZone> zonesFromModel() const;
    QVector<sc2dh::decor::DecorZone> selectedRegionZones() const;
    void populateRegionSelector();
    void updateOptimizationScope();
    void toggleRegionFromMap(int regionIndex);
    sc2dh::decor::DecorationSafetyContext decorationSafetyContextFromDoodadTable() const;
    void populateZoneTable(const QVector<sc2dh::decor::DecorZone> &zones);
    void populateDoodadTable(const QVector<sc2dh::decor::DoodadPlacement> &doodads);
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
    QVector<sc2dh::decor::DecorZone> m_decorZones;
    sc2dh::decor::DecorationOptimizedArtifacts m_decorPreview;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_warningLabel = nullptr;
    QWidget *m_heatmap = nullptr;
    QStandardItemModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QLabel *m_decorSummaryLabel = nullptr;
    QLabel *m_regionStatusLabel = nullptr;
    QPushButton *m_chooseRegionsButton = nullptr;
    QPushButton *m_previewButton = nullptr;
    QListWidget *m_regionList = nullptr;
    QLineEdit *m_prefixEdit = nullptr;
    QSpinBox *m_batchSpin = nullptr;
    QStandardItemModel *m_zoneModel = nullptr;
    QTableView *m_zoneTable = nullptr;
    QStandardItemModel *m_doodadModel = nullptr;
    QTableView *m_doodadTable = nullptr;
    QPlainTextEdit *m_galaxyPreview = nullptr;
    QPushButton *m_createCopyButton = nullptr;
};
