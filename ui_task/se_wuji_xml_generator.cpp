#include "se_wuji_xml_generator.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>

QString SeWujiXmlGenerator::resolveTemplatePath(const QString& templateName)
{
    QStringList candidates;
    // 1) 运行时 xml 目录：LTZK.exe 位于 <LTZK_HOME>/bin/，../xml 即
    //    Windows: D:\02_Runtime\LTZK\xml，麒麟: /opt/ltzk/xml（双平台通用）
    candidates << QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("../xml/") + templateName);
    // 2) Windows 部署目录（garmap_release 插件目录）
    candidates << QStringLiteral("D:/GarMap/garmap_release/starmap/plugins/xml/") + templateName;
    // 3) Windows 开发源码树（插件源码 xml/ 目录，无运行时树时开发期兜底）
    candidates << QStringLiteral("D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/xml/") + templateName;

    for (const QString& path : candidates)
    {
        if (QFileInfo::exists(path))
            return path;
    }
    // 全部不存在时返回第一个候选（运行时 xml），便于调用方在错误提示里展示期望位置
    return candidates.first();
}

QString SeWujiXmlGenerator::applyTemplate(const TemplateConfig& config)
{
    QFile file(config.templatePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QString xml = QString::fromUtf8(file.readAll());
    file.close();

    // Use absolute paths. Bare filenames relative to <RelativePath> don't
    // resolve correctly when called via DoXMLFile() (they work from the EXE
    // because the EXE sets up additional path context).
    xml = substituteFilePaths(xml, QStringLiteral("SourceDataStore"), config.inputFiles, false);

    xml = substituteFilePaths(xml, QStringLiteral("DstDataStore"), config.outputFiles, false);
    if (xml.contains(QStringLiteral("DstDataStores")))
        xml = substituteFilePaths(xml, QStringLiteral("DstDataStores"), config.outputFiles, false);

    // Override Field (ID175) or AdminField (ID327) if user specified
    if (!config.entityField.isEmpty()) {
        xml = substituteParam(xml, QStringLiteral("Field"), config.entityField);
        xml = substituteParam(xml, QStringLiteral("AdminField"), config.entityField);
    }

    // Override tunable template params (non-zero = user adjusted from default)
    if (config.bufferDistance > 0)
        xml = substituteParam(xml, QStringLiteral("BufferDistance"), QString::number(config.bufferDistance, 'f', 6));
    if (config.angleEpsilon > 0)
        xml = substituteParam(xml, QStringLiteral("AngleEpsilon"), QString::number(config.angleEpsilon, 'f', 1));
    if (config.neighborStyle > 0)
        xml = substituteParam(xml, QStringLiteral("NeighborStyle"), QString::number(config.neighborStyle));
    if (config.fuzzyTolerance > 0)
        xml = substituteParam(xml, QStringLiteral("FuzzyTolerance"), QString::number(config.fuzzyTolerance, 'f', 6));
    if (config.linkMode > 0)
        xml = substituteParam(xml, QStringLiteral("LinkMode"), QString::number(config.linkMode));

    // Fill RelativePath so the SDK knows where to find input files.
    // Also passed as a separate parameter to DoXMLFile() for redundancy.
    if (!config.dataPath.isEmpty()) {
        QString relativeTag = QStringLiteral("<RelativePath>%1</RelativePath>").arg(config.dataPath);
        xml.replace(QStringLiteral("<RelativePath></RelativePath>"), relativeTag);
    }

    // Write to temp file
    QString tempPath = QDir::tempPath() + QStringLiteral("/wuji_mission_")
                       + QString::number(QCoreApplication::applicationPid())
                       + QStringLiteral(".xml");
    QFile out(tempPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    out.write(xml.toUtf8());
    out.close();

    return tempPath;
}

QString SeWujiXmlGenerator::substituteFilePaths(const QString& xml, const QString& layerNote, const QStringList& files, bool filenamesOnly)
{
    // Build the new FilePath block
    QStringList entries;
    for (const QString& f : files) {
        QString path = filenamesOnly ? QFileInfo(f).fileName() : QDir::toNativeSeparators(f);
        entries.append(QStringLiteral("\t\t<FilePath>%1</FilePath>").arg(path));
    }

    // Match <Layers note="layerNote"> ... </Layers> and replace inner FilePath elements
    QRegularExpression re(
        QStringLiteral("(<Layers\\s+note=\"%1\">)(.*?)(</Layers>)").arg(layerNote),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch m = re.match(xml);
    if (!m.hasMatch())
        return xml;

    return xml.left(m.capturedStart(1)) + m.captured(1) + QStringLiteral("\n")
           + entries.join(QStringLiteral("\n")) + QStringLiteral("\n\t")
           + m.captured(3) + xml.mid(m.capturedEnd(3));
}

QString SeWujiXmlGenerator::substituteParam(const QString& xml, const QString& paramName, const QString& newValue)
{
    QRegularExpression re(
        QStringLiteral("<%1[^>]*>([^<]*)</%1>").arg(paramName));
    QRegularExpressionMatch m = re.match(xml);
    if (!m.hasMatch())
        return xml;

    // Preserve any attributes on the opening tag
    QString fullMatch = m.captured(0);
    QString oldContent = m.captured(1);
    QString replacement = fullMatch;
    replacement.replace(oldContent, newValue);
    return QString(xml).replace(fullMatch, replacement);
}
