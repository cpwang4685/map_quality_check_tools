#include "wuji_mission_xml.h"

#include <QDir>

namespace {

// ---- 转义 ----

QString esc(const QString& s)
{
    QString r = s;
    r.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
    r.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
    r.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
    return r;
}

QString escAttr(const QString& s)
{
    QString r = esc(s);
    r.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
    return r;
}

// 双精度参数统一按定点输出（避免 1e-05 之类的科学计数法）
QString dbl(double v)
{
    return QString::number(v, 'f', 6);
}

QString bstr(bool v)
{
    return v ? QStringLiteral("true") : QStringLiteral("false");
}

// ---- XML 结构构造器（与质检测试.xml 结构一致） ----

class Builder
{
public:
    explicit Builder(const QString& dataPath, int id, const QString& note)
        : m_dataPath(dataPath), m_paraInClosed(false)
    {
        m << QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
        m << QStringLiteral("<MapGeneBatchProcessing>\n");
        m << QStringLiteral("\t<Mission id=\"") + QString::number(id)
          + QStringLiteral("\" note=\"") + escAttr(note) + QStringLiteral("\">\n");
        m << QStringLiteral("\t\t<ParaIn>\n");
    }

    void addLayers(const QString& note, const QString& note2 = QString())
    {
        m << QStringLiteral("\t\t\t<Layers note=\"") + escAttr(note) + QStringLiteral("\"");
        if (!note2.isEmpty())
            m << QStringLiteral(" note2=\"") + escAttr(note2) + QStringLiteral("\"");
        m << QStringLiteral(">\n");
    }

    // guidFieldName 传 null 表示不输出该属性；bufferDis 同理
    void addFile(const QString& absPath, const QString& guidFieldName = QString(),
                 bool isRP = false, const QString& bufferDis = QString())
    {
        QString line = QStringLiteral("\t\t\t\t<FilePath");
        if (!guidFieldName.isNull())
            line += QStringLiteral(" GUIDFieldName=\"") + escAttr(guidFieldName) + QStringLiteral("\"");
        if (isRP)
            line += QStringLiteral(" isRP=\"true\"");
        if (!bufferDis.isNull())
            line += QStringLiteral(" bufferDis=\"") + escAttr(bufferDis) + QStringLiteral("\"");
        line += QStringLiteral(">") + esc(relPath(absPath)) + QStringLiteral("</FilePath>\n");
        m << line;
    }

    void closeLayers()
    {
        m << QStringLiteral("\t\t\t</Layers>\n");
    }

    void startParams()
    {
        m << QStringLiteral("\t\t\t<Parameter>\n");
    }

    void param(const QString& name, const QString& value,
               const QString& note = QString())
    {
        m << QStringLiteral("\t\t\t\t<") + name;
        if (!note.isEmpty())
            m << QStringLiteral(" note=\"") + escAttr(note) + QStringLiteral("\"");
        m << QStringLiteral(">") + esc(value) + QStringLiteral("</") + name + QStringLiteral(">\n");
    }

    void endParams()
    {
        m << QStringLiteral("\t\t\t</Parameter>\n");
    }

    void startOut(const QString& note, const QString& note2 = QString())
    {
        closeParaIn();
        m << QStringLiteral("\t\t<ParaOut>\n");
        m << QStringLiteral("\t\t\t<Layers note=\"") + escAttr(note) + QStringLiteral("\"");
        if (!note2.isEmpty())
            m << QStringLiteral(" note2=\"") + escAttr(note2) + QStringLiteral("\"");
        m << QStringLiteral(">\n");
    }

    void addOutFile(const QString& absPath, bool editable = true)
    {
        m << QStringLiteral("\t\t\t\t<FilePath");
        if (editable)
            m << QStringLiteral(" editable=\"true\"");
        m << QStringLiteral(">") + esc(relPath(absPath)) + QStringLiteral("</FilePath>\n");
    }

    void endOut()
    {
        m << QStringLiteral("\t\t\t</Layers>\n");
        m << QStringLiteral("\t\t</ParaOut>\n");
    }

    QString build()
    {
        closeParaIn();
        m << QStringLiteral("\t</Mission>\n");
        m << QStringLiteral("</MapGeneBatchProcessing>\n");
        return m.join(QString());
    }

private:
    // 引擎要求 FilePath 为 dataPath 相对路径（绝对路径会静默失败）
    QString relPath(const QString& absPath) const
    {
        return QDir(m_dataPath).relativeFilePath(absPath);
    }

    // ParaIn 必须在 <ParaOut> 之前关闭，且只关一次
    // （455/456/459 会多次 startOut）
    void closeParaIn()
    {
        if (!m_paraInClosed) {
            m << QStringLiteral("\t\t</ParaIn>\n");
            m_paraInClosed = true;
        }
    }

    QString m_dataPath;
    bool m_paraInClosed;
    QStringList m;
};

} // namespace

namespace WujiMissionXml {

QString buildMission454(const QString& dataPath, const QString& pointShp,
                        const QString& lineShp, const QString& polygonShp,
                        const QString& refPointShp, int processMode,
                        double fuzzyTolerance, const QString& outPointShp)
{
    Builder b(dataPath, 454, QStringLiteral("点拓扑规则"));
    b.addLayers(QStringLiteral("PointDataStore"));
    b.addFile(pointShp);
    b.closeLayers();
    if (!lineShp.isEmpty()) {
        b.addLayers(QStringLiteral("LineDataStore"));
        b.addFile(lineShp);
        b.closeLayers();
    }
    if (!polygonShp.isEmpty()) {
        b.addLayers(QStringLiteral("PolygonDataStore"));
        b.addFile(polygonShp);
        b.closeLayers();
    }
    if (!refPointShp.isEmpty()) {
        b.addLayers(QStringLiteral("RefPointDataStore"));
        b.addFile(refPointShp);
        b.closeLayers();
    }
    b.startParams();
    b.param(QStringLiteral("ProcessMode"), QString::number(processMode));
    b.param(QStringLiteral("FuzzyTolerance"), dbl(fuzzyTolerance));
    b.param(QStringLiteral("IsGeographic"), QStringLiteral("false"));
    b.param(QStringLiteral("LogInfoField"), QStringLiteral("info_NM"));
    b.endParams();
    b.startOut(QStringLiteral("PointDataStore"));
    b.addOutFile(outPointShp);
    b.endOut();
    return b.build();
}

QString buildMission455(const QString& dataPath, const QString& lineShp,
                        const QString& pointShp, const QString& polygonShp,
                        const QString& refLineShp, int processMode,
                        double fuzzyTolerance, double bufferDistance,
                        const QString& outLineShp, const QString& outPointShp)
{
    Builder b(dataPath, 455, QStringLiteral("线拓扑规则"));
    b.addLayers(QStringLiteral("LineDataStore"));
    b.addFile(lineShp);
    b.closeLayers();
    if (!pointShp.isEmpty()) {
        b.addLayers(QStringLiteral("PointDataStore"));
        b.addFile(pointShp);
        b.closeLayers();
    }
    if (!polygonShp.isEmpty()) {
        b.addLayers(QStringLiteral("PolygonDataStore"));
        b.addFile(polygonShp);
        b.closeLayers();
    }
    if (!refLineShp.isEmpty()) {
        b.addLayers(QStringLiteral("RefLineDataStore"));
        b.addFile(refLineShp);
        b.closeLayers();
    }
    b.startParams();
    b.param(QStringLiteral("ProcessMode"), QString::number(processMode));
    b.param(QStringLiteral("FuzzyTolerance"), dbl(fuzzyTolerance));
    b.param(QStringLiteral("IsGeographic"), QStringLiteral("false"));
    b.param(QStringLiteral("Scale"), QStringLiteral("0"));
    b.param(QStringLiteral("BufferDistance"), dbl(bufferDistance));
    b.param(QStringLiteral("LogInfoField"), QStringLiteral("info_NM"));
    b.endParams();
    b.startOut(QStringLiteral("LineDataStore"));
    b.addOutFile(outLineShp);
    b.endOut();
    b.startOut(QStringLiteral("PointDataStore"));
    b.addOutFile(outPointShp);
    b.endOut();
    return b.build();
}

QString buildMission456(const QString& dataPath, const QString& polygonShp,
                        const QString& lineShp, const QString& pointShp,
                        const QString& refPolygonShp, int processMode,
                        double fuzzyTolerance, double bufferDistance,
                        const QString& outPolygonShp, const QString& outLineShp,
                        const QString& outPointShp)
{
    Builder b(dataPath, 456, QStringLiteral("面拓扑规则"));
    b.addLayers(QStringLiteral("PolygonDataStore"));
    b.addFile(polygonShp);
    b.closeLayers();
    if (!lineShp.isEmpty()) {
        b.addLayers(QStringLiteral("LineDataStore"));
        b.addFile(lineShp);
        b.closeLayers();
    }
    if (!pointShp.isEmpty()) {
        b.addLayers(QStringLiteral("PointDataStore"));
        b.addFile(pointShp);
        b.closeLayers();
    }
    if (!refPolygonShp.isEmpty()) {
        b.addLayers(QStringLiteral("RefPolygonDataStore"));
        b.addFile(refPolygonShp);
        b.closeLayers();
    }
    b.startParams();
    b.param(QStringLiteral("ProcessMode"), QString::number(processMode));
    b.param(QStringLiteral("FuzzyTolerance"), dbl(fuzzyTolerance));
    b.param(QStringLiteral("IsGeographic"), QStringLiteral("false"));
    b.param(QStringLiteral("Scale"), QStringLiteral("0"));
    b.param(QStringLiteral("BufferDistance"), dbl(bufferDistance));
    b.param(QStringLiteral("LogInfoField"), QStringLiteral("info_NM"));
    b.endParams();
    b.startOut(QStringLiteral("PolygonDataStore"));
    b.addOutFile(outPolygonShp);
    b.endOut();
    b.startOut(QStringLiteral("LineDataStore"));
    b.addOutFile(outLineShp);
    b.endOut();
    b.startOut(QStringLiteral("PointDataStore"));
    b.addOutFile(outPointShp);
    b.endOut();
    return b.build();
}

QString buildMission457(const QString& dataPath, const QStringList& srsShps,
                        const QStringList& resShps, const QString& relationDb,
                        int processMode, const QString& outShp)
{
    Builder b(dataPath, 457, QStringLiteral("关联表检查"));
    b.addLayers(QStringLiteral("SourceDataStore"), QStringLiteral("原始1w数据"));
    for (const QString& p : srsShps) {
        b.addFile(p, QStringLiteral(""));
        b.addFile(p, QStringLiteral(""), true);
    }
    b.closeLayers();
    b.addLayers(QStringLiteral("ResDataStore"), QStringLiteral("结果5w数据"));
    for (const QString& p : resShps) {
        b.addFile(p, QStringLiteral(""));
        b.addFile(p, QStringLiteral(""), true);
    }
    b.closeLayers();
    b.startParams();
    b.param(QStringLiteral("RelationTablePath"),
            relationDb + QStringLiteral(";Relation_1w_5w"));
    b.param(QStringLiteral("ProcessMode"), QString::number(processMode));
    b.param(QStringLiteral("GUIDFieldName"), QStringLiteral("ELEMID"));
    b.param(QStringLiteral("InfoFieldName"), QStringLiteral("info_NM"));
    b.endParams();
    b.startOut(QStringLiteral("DstDataStores"),
               QStringLiteral("结果图层自动添加后缀生成点线面三个层，固定属性结构"));
    b.addOutFile(outShp);
    b.endOut();
    return b.build();
}

QString buildMission458(const QString& dataPath,
                        const QStringList& beforeShps,
                        const QStringList& afterShps, int processMode,
                        const QString& relationDb, const QString& matchParamXml,
                        const QStringList& outShps)
{
    Builder b(dataPath, 458, QStringLiteral("综合前后缓冲匹配检查"));
    b.addLayers(QStringLiteral("AfterDataStores"));
    for (const QString& p : afterShps)
        b.addFile(p, QString(), false, QStringLiteral(""));
    b.closeLayers();
    b.addLayers(QStringLiteral("BeforeDataStores"));
    for (const QString& p : beforeShps)
        b.addFile(p);
    b.closeLayers();
    b.startParams();
    b.param(QStringLiteral("IsGeographic"), QStringLiteral("false"));
    b.param(QStringLiteral("Scale"), QStringLiteral("0"));
    b.param(QStringLiteral("BufferDis"), QStringLiteral("0"));
    b.param(QStringLiteral("IdentityField"), QStringLiteral("ELEMID"));
    b.param(QStringLiteral("ProcessMode"), QString::number(processMode));
    b.param(QStringLiteral("MatchStyle"), QStringLiteral("0"));
    b.param(QStringLiteral("AreaRatio"), dbl(0.5));
    b.param(QStringLiteral("LengthRatio"), dbl(0.5));
    b.param(QStringLiteral("SelfAreaRatio"), dbl(0.0));
    b.param(QStringLiteral("SelfLengthRatio"), dbl(0.0));
    b.param(QStringLiteral("AssociationType"), QStringLiteral("3"));
    b.param(QStringLiteral("AbsoluteValue"), QStringLiteral("false"));
    b.param(QStringLiteral("BufferGeoProcessSelfIntersect"), QStringLiteral("true"));
    b.param(QStringLiteral("MultiGeoToSingle"), QStringLiteral("true"));
    b.param(QStringLiteral("BufferType"), QStringLiteral("0"));
    b.param(QStringLiteral("LineOffSetStyle"), QStringLiteral("0"));
    b.param(QStringLiteral("FuzzyTolerance"), dbl(0.001));
    b.param(QStringLiteral("IntersectionEpsilon"), dbl(0.00001));
    b.param(QStringLiteral("RedundancyVertexTolerance"), dbl(0.001));
    if (!relationDb.isEmpty())
        b.param(QStringLiteral("RelationTablePath"),
                relationDb + QStringLiteral(";Relation_1w_5w"));
    if (!matchParamXml.isEmpty())
        b.param(QStringLiteral("MatchParameterXMLFile"), matchParamXml);
    b.endParams();
    b.startOut(QStringLiteral("AfterDataStores"));
    for (const QString& p : outShps)
        b.addOutFile(p);
    b.endOut();
    return b.build();
}

QString buildMission459(const QString& dataPath, const QStringList& srcShps,
                        int processMode,
                        const QHash<QString, double>& thresholds,
                        const QString& outPolygonShp,
                        const QString& outPointShp)
{
    Builder b(dataPath, 459, QStringLiteral("图形规范性检查"));
    b.addLayers(QStringLiteral("SourceDataStore"));
    for (const QString& p : srcShps)
        b.addFile(p);
    b.closeLayers();
    b.startParams();
    b.param(QStringLiteral("IsGeographic"), QStringLiteral("false"));
    b.param(QStringLiteral("Scale"), QStringLiteral("0"));
    b.param(QStringLiteral("ProcessMode"), QString::number(processMode));
    b.param(QStringLiteral("FuzzyTolerance"),
            dbl(thresholds.value(QStringLiteral("FuzzyTolerance"), 0.001)));
    b.param(QStringLiteral("AcuteAngle"),
            dbl(thresholds.value(QStringLiteral("AcuteAngle"), 10.0)));
    b.param(QStringLiteral("LongNarrowWidth"),
            dbl(thresholds.value(QStringLiteral("NarrowWidth"), 0.5)));
    b.param(QStringLiteral("SliverArea"),
            dbl(thresholds.value(QStringLiteral("SliverArea"), 1.0)));
    b.param(QStringLiteral("SliverLength"), dbl(0.0));
    b.param(QStringLiteral("AverageNodeDensityLength_Upper"), dbl(0.0));
    b.param(QStringLiteral("AverageNodeDensityLength_Lower"), dbl(0.0));
    b.param(QStringLiteral("NodeDensityLength_Upper"), dbl(0.0));
    b.param(QStringLiteral("NodeDensityLength_Lower"), dbl(0.0));
    b.param(QStringLiteral("AverageNodeDensityArea_Upper"), dbl(0.0));
    b.param(QStringLiteral("AverageNodeDensityArea_Lower"), dbl(0.0));
    b.param(QStringLiteral("NodeDensityArea_Upper"), dbl(0.0));
    b.param(QStringLiteral("NodeDensityArea_Lower"), dbl(0.0));
    b.param(QStringLiteral("MinNodeLength"),
            dbl(thresholds.value(QStringLiteral("MinNodeDistance"), 0.001)));
    b.param(QStringLiteral("FlagField"), QStringLiteral("error"));
    b.endParams();
    b.startOut(QStringLiteral("DstDataStores"));
    b.addOutFile(outPolygonShp);
    b.endOut();
    b.startOut(QStringLiteral("PointDataStore"));
    b.addOutFile(outPointShp);
    b.endOut();
    return b.build();
}

} // namespace WujiMissionXml
