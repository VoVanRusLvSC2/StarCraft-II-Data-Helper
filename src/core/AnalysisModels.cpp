#include "core/AnalysisModels.h"

int AnalysisResult::totalXmlFiles() const
{
    int count = 0;
    for (const ScannedFileInfo &file : scannedFiles)
    {
        if (file.isXml)
        {
            ++count;
        }
    }
    return count;
}
