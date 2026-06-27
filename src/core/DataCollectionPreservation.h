#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

struct DataCollectionPreservationReport
{
    int baselineCollections = 0;
    int stagedCollections = 0;
    int baselineRecords = 0;
    int stagedRecords = 0;
    int missingBeforeRestore = 0;
    int missingAfterRestore = 0;
    int restoredRecords = 0;
    int addedRecords = 0;
    QStringList restoredSamples;
    QStringList missingSamples;
    QStringList addedSamples;

    QString summaryText() const;
};

bool restoreMissingDataCollectionRecords(const QByteArray &baselineBytes, QByteArray *stagedBytes,
                                         DataCollectionPreservationReport *report, QString *error);
