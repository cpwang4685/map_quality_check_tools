#include "se_mission458_check.h"

#include "../ui_task/wuji_mission_runner.h"

#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgscurve.h"
#include "qgsgeometrycollection.h"
#include "qgsspatialindex.h"
#include "qgsvectorlayer.h"
#include "qgsproviderregistry.h"
#include "qgsprovidermetadata.h"
#include "qgsprovidersublayerdetails.h"
#include "qgswkbtypes.h"

#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSet>
#include <cmath>

// ---- Mission 458 本地实现说明 ----
// 引擎路线（DataBufferMatch/MapBatchProcessing.exe）在成果数据上稳定失败
// （引擎写中间文件丢 .shx 后重开失败），已改为 QGIS 本地缓冲匹配：
//   1. 按图层映射表（成果图层名→源图层代码）或同名规则配对原始/成果图层；
//   2. 成果要素按缓冲距离与原始要素求交（重叠面积/长度比），
//      无原始来源的成果要素记为"未匹配"，显著变形的记为"变形"；
//   3. 原始要素被综合掉（成果中找不到对应）属综合正常现象，只统计不报错。
// 匹配参数优先从 matchParameter.xml 读取（config 目录随包提供），
// 未找到时使用下列默认值（成果为投影坐标，单位米）。
namespace Mission458 {

static const double DEFAULT_BUFFER_DISTANCE = 10.0;   // 默认缓冲匹配距离（米）
static const double DEFAULT_MIN_OVERLAP_RATIO = 0.5;  // 重叠面积/长度比低于该值视为未匹配
static const double DEFAULT_MAX_CHANGE_RATIO = 0.3;   // 面积/长度变化率超过 30% 视为显著变形
static const int    DEFAULT_MAX_VERTEX_CHANGE = 10;   // 顶点数变化超过 10 个视为显著变形

// 匹配参数集（matchParameter.xml 提供，缺文件时取默认值）
struct MatchParams {
    double bufferDistance = DEFAULT_BUFFER_DISTANCE;
    double minOverlapRatio = DEFAULT_MIN_OVERLAP_RATIO;
    double maxChangeRatio = DEFAULT_MAX_CHANGE_RATIO;
    int maxVertexChange = DEFAULT_MAX_VERTEX_CHANGE;
    bool loadedFromFile = false;
};

// 解析 matchParameter.xml：
// <MapGeneBatchProcessing><Mission id="458"><Parameter>
//   <BufferDistance>10</BufferDistance><MinOverlapRatio>0.5</MinOverlapRatio>
//   <MaxChangeRatio>0.3</MaxChangeRatio><MaxVertexChange>10</MaxVertexChange>
// </Parameter></Mission></MapGeneBatchProcessing>
static MatchParams loadMatchParams(const QString& path)
{
    MatchParams p;
    if (path.isEmpty() || !QFileInfo::exists(path))
        return p;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return p;
    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        return p;
    }
    file.close();
    const QDomElement param = doc.documentElement()
        .firstChildElement(QStringLiteral("Mission"))
        .firstChildElement(QStringLiteral("Parameter"));
    if (param.isNull())
        return p;
    const auto readDouble = [&param](const QString& tag, double def) -> double {
        const QDomElement e = param.firstChildElement(tag);
        if (e.isNull())
            return def;
        bool ok = false;
        const double v = e.text().trimmed().toDouble(&ok);
        return ok ? v : def;
    };
    p.bufferDistance  = readDouble(QStringLiteral("BufferDistance"), DEFAULT_BUFFER_DISTANCE);
    p.minOverlapRatio = readDouble(QStringLiteral("MinOverlapRatio"), DEFAULT_MIN_OVERLAP_RATIO);
    p.maxChangeRatio  = readDouble(QStringLiteral("MaxChangeRatio"), DEFAULT_MAX_CHANGE_RATIO);
    p.maxVertexChange = static_cast<int>(readDouble(QStringLiteral("MaxVertexChange"),
                                                    DEFAULT_MAX_VERTEX_CHANGE));
    p.loadedFromFile  = true;
    return p;
}

// 递归收集目录下全部 .shp 绝对路径（原始数据按分类子目录存放，需要递归）
static QStringList shpPathsIn(const QString& dir)
{
    QStringList paths;
    QDirIterator it(dir, QStringList() << QStringLiteral("*.shp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) paths.append(it.next());
    return paths;
}

// 枚举 FileGDB 中全部矢量图层：返回 图层名(代码) → 图层URI列表
// URI 形如 "D:/xxx/永登县.gdb|layername=LRDL"，可直接交给 QgsVectorLayer 打开；
// 图层名即标准图层代码（LRDL/HYDL_HL/...），与图层映射表 sourceCode 列天然对应。
static QHash<QString, QStringList> gdbLayersByCode(const QString& gdbPath)
{
    QHash<QString, QStringList> byCode;
    QgsProviderMetadata* ogrMd =
        QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("ogr"));
    if (!ogrMd) return byCode;
    const QList<QgsProviderSublayerDetails> subs = ogrMd->querySublayers(gdbPath);
    for (const QgsProviderSublayerDetails& s : subs) {
        if (s.type() != QgsMapLayerType::VectorLayer) continue;   // 跳过非矢量子层
        if (s.wkbType() == QgsWkbTypes::Unknown
            || s.wkbType() == QgsWkbTypes::NoGeometry) continue;   // 跳过纯属性表
        const QString name = s.name();
        if (name.isEmpty()) continue;
        byCode[name].append(s.uri());
    }
    return byCode;
}

// 在图层字段中查找标识字段（候选名大小写无关，找不到返回空）
static QString findField(const QgsVectorLayer* layer, const QStringList& candidates)
{
    if (!layer) return QString();
    const QgsFields& fields = layer->fields();
    for (const QString& c : candidates) {
        if (fields.indexOf(c) >= 0) return c;
    }
    for (int i = 0; i < fields.count(); i++) {
        const QString fn = fields.at(i).name();
        for (const QString& c : candidates) {
            if (fn.compare(c, Qt::CaseInsensitive) == 0) return fn;
        }
    }
    return QString();
}

// 去掉常见综合后缀（_选取/_名称/_点/_方向点/_注记/_跳绘），用于图层配对回退
static QString stripSuffix(const QString& base)
{
    const QStringList suffixes = {
        QStringLiteral("_选取"), QStringLiteral("_名称"), QStringLiteral("_点"),
        QStringLiteral("_方向点"), QStringLiteral("_注记"), QStringLiteral("_跳绘")};
    for (const QString& sfx : suffixes) {
        if (base.endsWith(sfx))
            return base.left(base.length() - sfx.length());
    }
    return base;
}

// 成果要素与原始要素的重叠比：面→面积比、线→长度比（分母取原始要素，
// 综合后尺度更小、要素更大，原始要素大部分被覆盖即视为有来源）
static double overlapRatio(const QgsGeometry& resultGeom, const QgsGeometry& origGeom)
{
    if (resultGeom.isNull() || origGeom.isNull()) return 0.0;
    const QgsWkbTypes::GeometryType rt = resultGeom.type();
    const QgsWkbTypes::GeometryType ot = origGeom.type();
    if (rt == QgsWkbTypes::PolygonGeometry && ot == QgsWkbTypes::PolygonGeometry) {
        const double ob = origGeom.area();
        if (ob <= 0.0) return 0.0;
        return resultGeom.intersection(origGeom).area() / ob;
    }
    if (rt == QgsWkbTypes::LineGeometry && ot == QgsWkbTypes::LineGeometry) {
        const double lb = origGeom.length();
        if (lb <= 0.0) return 0.0;
        return resultGeom.intersection(origGeom).length() / lb;
    }
    return resultGeom.intersects(origGeom) ? 1.0 : 0.0;
}

// 统计顶点数（多部件递归）
static int countVertices(const QgsAbstractGeometry* g)
{
    if (!g) return 0;
    if (g->dimension() == 0) return 1;
    if (const QgsCurve* curve = dynamic_cast<const QgsCurve*>(g))
        return curve->numPoints();
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        int n = 0;
        for (int i = 0; i < gc->numGeometries(); i++)
            n += countVertices(gc->geometryN(i));
        return n;
    }
    return 0;
}

// 比较综合前后几何变化（移植自 7-27 集成版）
static void compareGeometry(const QgsFeature& origFeat, const QgsFeature& resultFeat,
                            double& areaChange, double& lengthChange, int& vertexChange)
{
    areaChange = 0.0;
    lengthChange = 0.0;
    vertexChange = 0;

    const QgsGeometry origGeom = origFeat.geometry();
    const QgsGeometry resultGeom = resultFeat.geometry();
    if (origGeom.isNull() || resultGeom.isNull()) return;

    const double origArea = origGeom.area();
    const double resultArea = resultGeom.area();
    if (origArea > 0.0 && resultArea > 0.0)
        areaChange = (resultArea - origArea) / origArea;

    const double origLen = origGeom.length();
    const double resultLen = resultGeom.length();
    if (origLen > 0.0 && resultLen > 0.0)
        lengthChange = (resultLen - origLen) / origLen;

    const int origVerts = countVertices(origGeom.constGet());
    const int resultVerts = countVertices(resultGeom.constGet());
    if (origVerts > 0 && resultVerts > 0)
        vertexChange = resultVerts - origVerts;
}

// basename → 路径列表（同名冲突时保留全部，选择时按确定性规则取）
static QHash<QString, QStringList> groupByBase(const QStringList& shps)
{
    QHash<QString, QStringList> byBase;
    for (const QString& p : shps)
        byBase[QFileInfo(p).completeBaseName()].append(p);
    return byBase;
}

// 同名 basename 冲突时选目录最浅者（更接近数据根目录，通常是主数据），
// 深度相同则按路径字典序取首个，保证每次运行结果一致；冲突时经 note 记入日志
static QString pickPath(const QStringList& paths, QString* ambiguousNote = nullptr)
{
    if (paths.isEmpty()) return QString();
    if (paths.size() > 1 && ambiguousNote) {
        QStringList others;
        for (int i = 1; i < paths.size(); ++i) others.append(paths.at(i));
        *ambiguousNote = QStringLiteral("（同名%1个，另见：%2）")
            .arg(paths.size()).arg(others.join(QStringLiteral("; ")));
    }
    QString best = paths.first();
    const auto depthOf = [](const QString& p) {
        return p.count(QLatin1Char('/')) + p.count(QLatin1Char('\\'));
    };
    int bestDepth = depthOf(best);
    for (const QString& p : paths) {
        const int d = depthOf(p);
        if (d < bestDepth || (d == bestDepth && p < best)) {
            bestDepth = d;
            best = p;
        }
    }
    return best;
}

// 成果图层名 → 原始 SHP 路径配对。
// 优先用图层映射表（成果图层名→源图层代码），回退同名/去后缀同名；
// 同名 basename 冲突按"目录最浅优先"确定性规则选取，不再后写覆盖先写。
// origByBase: 原始数据条目（basename 或 GDB 图层代码 → 路径/URI 列表）
static QMap<QString, QString> pairLayers(const QHash<QString, QStringList>& origByBase,
                                         const QStringList& resultShps,
                                         const QHash<QString, QString>* layerMapping,
                                         QStringList* ambiguousNotes)
{
    QStringList resultNames;
    for (const QString& p : resultShps)
        resultNames.append(QFileInfo(p).completeBaseName());

    QMap<QString, QString> pairs; // 成果 basename → 原始路径
    for (const QString& r : resultNames) {
        QString origPath;
        // 1) 图层映射表（支持去后缀回退）
        if (layerMapping) {
            const QString stripped = stripSuffix(r);
            const auto it = layerMapping->constFind(r);
            const auto it2 = layerMapping->constFind(stripped);
            const QString code = (it != layerMapping->constEnd()) ? it.value()
                               : (it2 != layerMapping->constEnd()) ? it2.value() : QString();
            if (!code.isEmpty()) {
                const auto oit = origByBase.constFind(code);
                if (oit != origByBase.constEnd()) {
                    QString note;
                    origPath = pickPath(oit.value(), &note);
                    if (!note.isEmpty() && ambiguousNotes)
                        ambiguousNotes->append(QStringLiteral("[%1] 源图层代码%2存在同名SHP%3")
                                                   .arg(r, code, note));
                }
            }
        }
        // 2) 同名 / 去后缀同名
        if (origPath.isEmpty()) {
            const QStringList& same = origByBase.value(r);
            const QStringList& cand = same.isEmpty()
                ? origByBase.value(stripSuffix(r)) : same;
            if (!cand.isEmpty()) {
                QString note;
                origPath = pickPath(cand, &note);
                if (!note.isEmpty() && ambiguousNotes)
                    ambiguousNotes->append(QStringLiteral("[%1] 原始数据存在同名SHP%2").arg(r, note));
            }
        }
        if (!origPath.isEmpty()) pairs[r] = origPath;
    }
    return pairs;
}

void executeWithMapping(const QString& origDir, const QString& resultDir,
                        int processMode,
                        const QHash<QString, QString>& layerMapping,
                        QList<QPair<QgsFeature, QString>>& allErrors,
                        QStringList& executedChecks,
                        const QString& matchParamPath)
{
    execute(origDir, resultDir, processMode, allErrors, executedChecks,
            &layerMapping, matchParamPath);
}

void execute(const QString& origDir, const QString& resultDir,
             int processMode,
             QList<QPair<QgsFeature, QString>>& allErrors,
             QStringList& executedChecks,
             const QHash<QString, QString>* layerMapping,
             const QString& matchParamPath)
{
    Q_UNUSED(processMode) // 458 只有模式1（综合前后要素匹配检查），全部执行

    if (origDir.isEmpty() || resultDir.isEmpty()) {
        executedChecks.append(QStringLiteral("综合前后匹配：需同时提供原始数据与成果数据，本次未执行"));
        return;
    }

    // 匹配参数：优先 matchParameter.xml，缺失时取默认值并在日志中注明
    const MatchParams mp = loadMatchParams(matchParamPath);
    if (!mp.loadedFromFile)
        executedChecks.append(QStringLiteral("综合前后匹配(未找到matchParameter.xml，使用默认参数："
                                             "缓冲%1米/重叠比%2/变化率%3/顶点差%4)")
            .arg(mp.bufferDistance).arg(mp.minOverlapRatio)
            .arg(mp.maxChangeRatio).arg(mp.maxVertexChange));

    // 原始数据支持两种形态：SHP 目录（递归收集）或 FileGDB（枚举图层，层名=图层代码）
    const bool origIsGdb = WujiMissionRunner::isFileGdbSource(origDir);
    const QHash<QString, QStringList> origByBase = origIsGdb
        ? gdbLayersByCode(origDir) : groupByBase(shpPathsIn(origDir));
    const QStringList resultShps = shpPathsIn(resultDir);
    if (origByBase.isEmpty() || resultShps.isEmpty()) {
        executedChecks.append(QStringLiteral("综合前后匹配：无可参与匹配的图层（原始数据%1个图层，成果数据%2个SHP），本次未执行")
                                  .arg(origByBase.size()).arg(resultShps.size()));
        return;
    }

    executedChecks.append((origIsGdb
        ? QStringLiteral("原始数据(FileGDB): %1个图层, 成果数据: %2个图层")
        : QStringLiteral("原始数据: %1个图层, 成果数据: %2个图层"))
        .arg(origByBase.size()).arg(resultShps.size()));

    const QHash<QString, QStringList> resultByBaseGroup = groupByBase(resultShps);
    QMap<QString, QString> resultByBase; // 成果 basename → 路径（同名冲突取目录最浅）
    for (auto git = resultByBaseGroup.constBegin(); git != resultByBaseGroup.constEnd(); ++git)
        resultByBase[git.key()] = pickPath(git.value());

    QStringList ambiguousNotes;
    const QMap<QString, QString> pairs =
        pairLayers(origByBase, resultShps, layerMapping, &ambiguousNotes);
    for (const QString& n : ambiguousNotes)
        executedChecks.append(QStringLiteral("综合前后匹配图层配对提示：%1").arg(n));
    if (pairs.isEmpty()) {
        executedChecks.append(QStringLiteral("综合前后匹配：原始数据与成果数据中未找到可配对图层，本次未执行"));
        return;
    }

    int totalMatched = 0, totalUnmatched = 0, totalChanged = 0, totalDropped = 0;

    for (auto pit = pairs.constBegin(); pit != pairs.constEnd(); ++pit) {
        const QString resultName = pit.key();
        const QString origPath = pit.value();
        const QString resultPath = resultByBase.value(resultName);
        if (resultPath.isEmpty()) continue;

        QgsVectorLayer origLayer(origPath, QStringLiteral("m458_orig"), QStringLiteral("ogr"));
        QgsVectorLayer resultLayer(resultPath, QStringLiteral("m458_result"), QStringLiteral("ogr"));
        if (!origLayer.isValid() || !resultLayer.isValid()) {
            executedChecks.append(QStringLiteral("[%1] 图层打开失败，本图层未参与匹配").arg(resultName));
            continue;
        }

        // 原始要素索引（一次性装入内存做空间查询）
        QgsSpatialIndex origIndex;
        QVector<QgsFeature> origFeats;
        QHash<QgsFeatureId, int> origPos; // fid → origFeats 下标
        {
            QgsFeatureIterator fit = origLayer.getFeatures();
            QgsFeature f;
            while (fit.nextFeature(f)) {
                origIndex.addFeature(f);
                origPos.insert(f.id(), origFeats.size());
                origFeats.append(f);
            }
        }
        if (origFeats.isEmpty()) {
            executedChecks.append(QStringLiteral("[%1] 原始图层无要素，本图层未参与匹配").arg(resultName));
            continue;
        }

        // 成果标识字段（ELEMID 等），用于错误消息定位
        const QString eidField = findField(&resultLayer, QStringList()
            << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
            << QStringLiteral("FormerID"));
        // 原始图层标识字段：ELEMID 一致的候选即为来源要素（优先匹配）
        const QString origEidField = findField(&origLayer, QStringList()
            << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
            << QStringLiteral("FormerID"));
        const int origEidIdx = origEidField.isEmpty() ? -1
            : origLayer.fields().indexOf(origEidField);

        QSet<QgsFeatureId> usedOrigIds;
        int layerMatched = 0, layerUnmatched = 0, layerChanged = 0;

        QgsFeatureIterator rit = resultLayer.getFeatures();
        QgsFeature rf;
        while (rit.nextFeature(rf)) {
            const QgsGeometry rGeom = rf.geometry();
            if (rGeom.isNull()) continue;

            const QString eid = eidField.isEmpty() ? QString()
                : rf.attribute(rf.fields().indexOf(eidField)).toString().trimmed();
            const QString eidText = eid.isEmpty() ? QString() : QStringLiteral(" ELEMID='%1'").arg(eid);

            // 缓冲后按包围盒查询候选原始要素
            const QgsGeometry bufGeom = rGeom.buffer(mp.bufferDistance, 5);
            const QList<QgsFeatureId> candIds =
                origIndex.intersects(bufGeom.boundingBox());

            // 候选选取：ELEMID 一致者优先（综合会移动/简化几何，重叠比最高的
            // 候选未必是原要素）；无同标识候选时取重叠比最大者。
            double bestRatio = 0.0;
            const QgsFeature* bestOrig = nullptr;
            bool eidMatched = false;
            for (const QgsFeatureId oid : candIds) {
                const auto posIt = origPos.constFind(oid);
                if (posIt == origPos.constEnd()) continue;
                const QgsFeature& of = origFeats.at(posIt.value());
                if (origEidIdx >= 0 && !eid.isEmpty()
                    && of.attribute(origEidIdx).toString().trimmed() == eid) {
                    bestOrig = &of;
                    eidMatched = true;
                    break;
                }
                const double ratio = overlapRatio(bufGeom, of.geometry());
                if (ratio > bestRatio) {
                    bestRatio = ratio;
                    bestOrig = &of;
                }
            }

            if (!bestOrig || (!eidMatched && bestRatio < mp.minOverlapRatio)) {
                // 成果要素在原始数据中找不到来源 → 异常
                totalUnmatched++; layerUnmatched++;
                allErrors.append(qMakePair(rf, QStringLiteral("[%1] 综合前后未匹配：%2缓冲范围内无原始要素来源")
                                                  .arg(resultName, eidText)));
                continue;
            }

            usedOrigIds.insert(bestOrig->id());
            totalMatched++; layerMatched++;

            // 显著变形检测
            double areaChange = 0.0, lengthChange = 0.0;
            int vertexChange = 0;
            compareGeometry(*bestOrig, rf, areaChange, lengthChange, vertexChange);
            QStringList warnings;
            if (std::abs(areaChange) > mp.maxChangeRatio)
                warnings.append(QStringLiteral("面积变化%1%").arg(static_cast<int>(areaChange * 100)));
            if (std::abs(lengthChange) > mp.maxChangeRatio)
                warnings.append(QStringLiteral("长度变化%1%").arg(static_cast<int>(lengthChange * 100)));
            if (std::abs(vertexChange) > mp.maxVertexChange)
                warnings.append(QStringLiteral("顶点数变化%1").arg(vertexChange));
            if (!warnings.isEmpty()) {
                totalChanged++; layerChanged++;
                allErrors.append(qMakePair(rf, QStringLiteral("[%1] 综合前后变形：%2%3")
                                                  .arg(resultName, warnings.join(QStringLiteral("; ")), eidText)));
            }
        }

        // 原始要素被综合掉（无成果要素覆盖）只统计，不报错——综合会按选取指标舍弃要素
        totalDropped += origFeats.size() - usedOrigIds.size();

        executedChecks.append(QStringLiteral("[%1] 匹配%2/未匹配%3/显著变形%4 (缓冲%5米)")
                                  .arg(resultName)
                                  .arg(layerMatched).arg(layerUnmatched).arg(layerChanged)
                                  .arg(mp.bufferDistance));
    }

    executedChecks.append(QStringLiteral("综合前后匹配汇总: %1个可比图层, %2条匹配, %3条未匹配, %4条显著变形, %5条原始要素被综合")
                              .arg(pairs.size())
                              .arg(totalMatched).arg(totalUnmatched).arg(totalChanged).arg(totalDropped));
}

} // namespace Mission458
