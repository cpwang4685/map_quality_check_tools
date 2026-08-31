#ifndef SE_WUJI_XML_GENERATOR_H
#define SE_WUJI_XML_GENERATOR_H

#include <QString>
#include <QStringList>

class SeWujiXmlGenerator
{
public:
    struct TemplateConfig {
        QString templatePath;          // path to teacher's XML template
        QStringList inputFiles;        // source data file paths (absolute)
        QStringList outputFiles;       // destination file paths (absolute)
        QString entityField;           // override Field/AdminField value (empty = keep template default)
        QString dataPath;              // relativePath base dir (directory of input data)

        // Overridable template params (zero = don't override, keep template default)
        double bufferDistance = 0;     // merge: BufferDistance
        double angleEpsilon = 0;       // merge: AngleEpsilon
        int neighborStyle = 0;         // merge: NeighborStyle
        double fuzzyTolerance = 0;     // edge match: FuzzyTolerance
        int linkMode = 0;              // edge match: LinkMode
    };

    // Apply template substitution, write to a temp file.
    // Returns the path to the generated XML, or empty QString on failure.
    static QString applyTemplate(const TemplateConfig& config);

    // Resolve the runtime path of a template XML file (e.g. "merge_template.xml").
    // Priority: runtime xml dir (Windows <LTZK_HOME>/xml, Kylin /opt/ltzk/xml) →
    // Windows deployed dir → Windows dev source tree. Returns the first existing
    // candidate, or the first candidate (runtime xml) if none exist.
    static QString resolveTemplatePath(const QString& templateName);

private:
    static QString substituteFilePaths(const QString& xml, const QString& layerNote, const QStringList& files, bool filenamesOnly = false);
    static QString substituteParam(const QString& xml, const QString& paramName, const QString& newValue);
};

#endif
