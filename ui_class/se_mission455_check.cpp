#include "se_mission455_check.h"

#include "../ui_task/wuji_engine_bridge.h"
#include "../ui_task/wuji_mission_runner.h"
#include "../ui_task/wuji_mission_xml.h"

#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsspatialindex.h"
#include "qgsfeaturerequest.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <cmath>

// ============================================================
//  本地回退实现
//
//  检查逻辑正常由引擎子进程（MapBatchProcessing.exe）完成；该可执行
//  文件只存在于 Windows，麒麟上不存在，此时退回下面的本地 QGIS 算法，
//  保证 455 在无引擎环境下仍可执行（Windows 用引擎、麒麟用本地）。
//
//  算法本体与本工程综合前的实现逐字一致，唯一差别是参考面图层由已合并
//  好的 SHP 路径加载而来（引擎路径下参考图层同样以合并 SHP 形式传入）。
//  模式位含义与引擎实现一致：1 悬挂节点 2 伪节点 4 自重叠 8 自相交
//  16 多部件 32 相互重叠 64 相互相交 4096 线在面内部。
// ============================================================
namespace {

// 从 SHP 路径加载参考图层；失败返回 nullptr（调用方负责 delete）
QgsVectorLayer* openRefLayer(const QString& shpPath)
{
    if (shpPath.isEmpty() || !QFileInfo::exists(shpPath))
        return nullptr;
    QgsVectorLayer* lyr = new QgsVectorLayer(
        shpPath, QFileInfo(shpPath).completeBaseName(), QStringLiteral("ogr"));
    if (!lyr->isValid()) {
        delete lyr;
        return nullptr;
    }
    return lyr;
}

// ====== 模式1: 不能有悬挂节点 ======
void checkDangles(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
{
    if (!layer) return;
    // 统计每个端点出现的次数
    QHash<QString, int> endpointCount;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;
        QgsVertexIterator vIt = geom.vertices();
        QgsPointXY firstPt, lastPt;
        if (vIt.hasNext()) firstPt = QgsPointXY(vIt.next());
        lastPt = firstPt;
        while (vIt.hasNext()) lastPt = QgsPointXY(vIt.next());
        QString key1 = QString("%1,%2").arg(firstPt.x(),0,'f',4).arg(firstPt.y(),0,'f',4);
        QString key2 = QString("%1,%2").arg(lastPt.x(),0,'f',4).arg(lastPt.y(),0,'f',4);
        endpointCount[key1]++; endpointCount[key2]++;
    }
    // 悬挂节点：只出现一次的端点
    QgsFeatureIterator it2 = layer->getFeatures();
    QgsFeature feat2;
    while (it2.nextFeature(feat2)) {
        QgsGeometry geom = feat2.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;
        QgsVertexIterator vIt = geom.vertices();
        QgsPointXY firstPt, lastPt;
        if (vIt.hasNext()) firstPt = QgsPointXY(vIt.next());
        lastPt = firstPt;
        while (vIt.hasNext()) lastPt = QgsPointXY(vIt.next());
        QString key1 = QString("%1,%2").arg(firstPt.x(),0,'f',4).arg(firstPt.y(),0,'f',4);
        QString key2 = QString("%1,%2").arg(lastPt.x(),0,'f',4).arg(lastPt.y(),0,'f',4);
        if (endpointCount.value(key1) == 1 || endpointCount.value(key2) == 1) {
            errors.append(qMakePair(feat2, QStringLiteral("悬挂节点：线端点未连接到其他线")));
        }
    }
}

// ====== 模式2: 不能有伪节点 ======
void checkPseudos(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
{
    if (!layer) return;
    QHash<QString, QStringList> nodeLines;
    QHash<QString, QgsPointXY> nodePts;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;
        QgsVertexIterator vIt = geom.vertices();
        while (vIt.hasNext()) {
            QgsPointXY pt(vIt.next());
            QString key = QString("%1,%2").arg(pt.x(),0,'f',4).arg(pt.y(),0,'f',4);
            nodeLines[key].append(QString::number(feat.id()));
            nodePts[key] = pt;
        }
    }
    // 伪节点：恰好连接两条线的节点（非端点）
    QSet<QgsFeatureId> pseudoIds;
    for (auto it = nodeLines.begin(); it != nodeLines.end(); ++it) {
        QStringList lines = it.value();
        lines.removeDuplicates();
        if (lines.size() == 2) {
            pseudoIds.insert(lines[0].toLongLong());
            pseudoIds.insert(lines[1].toLongLong());
        }
    }
    QgsFeatureIterator it3 = layer->getFeatures();
    QgsFeature feat3;
    while (it3.nextFeature(feat3)) {
        if (pseudoIds.contains(feat3.id())) {
            errors.append(qMakePair(feat3, QStringLiteral("伪节点：线在非端点处只连接两条线，建议合并")));
        }
    }
}

// ====== 模式3: 不能自重叠 ======
void checkSelfOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;
        QVector<QgsGeometry::Error> gerrs;
        geom.validateGeometry(gerrs);
        for (const auto& e : gerrs) {
            if (e.what().contains("overlap", Qt::CaseInsensitive) ||
                e.what().contains("duplicate", Qt::CaseInsensitive)) {
                errors.append(qMakePair(feat, QStringLiteral("线自重叠：%1").arg(e.what())));
                break;
            }
        }
    }
}

// ====== 模式4: 不能自相交 ======
void checkSelfIntersect(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;
        if (!geom.isSimple()) {
            errors.append(qMakePair(feat, QStringLiteral("线自相交：几何不是简单几何")));
        }
    }
}

// ====== 模式5: 必须是单部件 ======
void checkSinglePart(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (!geom.isNull() && geom.isMultipart()) {
            errors.append(qMakePair(feat, QStringLiteral("多部件线：要素包含多个不连通部分")));
        }
    }
}

// ====== 模式6: 线不能相互重叠 ======
void checkOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QgsSpatialIndex index(layer->getFeatures());
    QSet<QString> reported;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g1 = feat.geometry();
        if (g1.isNull()) continue;
        QList<QgsFeatureId> cands = index.intersects(g1.boundingBox());
        for (QgsFeatureId fid : cands) {
            if (fid <= feat.id()) continue;
            QgsFeature feat2;
            if (layer->getFeatures(QgsFeatureRequest(fid)).nextFeature(feat2)) {
                QgsGeometry g2 = feat2.geometry();
                if (g1.overlaps(g2) || (g1.intersects(g2) && g1.length()>0 && g2.length()>0)) {
                    QString key = QString("%1-%2").arg(qMin(feat.id(),fid)).arg(qMax(feat.id(),fid));
                    if (!reported.contains(key)) {
                        reported.insert(key);
                        errors.append(qMakePair(feat,
                            QStringLiteral("线重叠：要素%1与%2相互重叠").arg(feat.id()).arg(fid)));
                    }
                }
            }
        }
    }
}

// ====== 模式7: 线不能相互相交 ======
void checkIntersect(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QgsSpatialIndex index(layer->getFeatures());
    QSet<QString> reported;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g1 = feat.geometry();
        if (g1.isNull()) continue;
        QList<QgsFeatureId> cands = index.intersects(g1.boundingBox());
        for (QgsFeatureId fid : cands) {
            if (fid <= feat.id()) continue;
            QgsFeature feat2;
            if (layer->getFeatures(QgsFeatureRequest(fid)).nextFeature(feat2)) {
                QgsGeometry g2 = feat2.geometry();
                if (g1.crosses(g2)) {
                    QString key = QString("%1-%2").arg(qMin(feat.id(),fid)).arg(qMax(feat.id(),fid));
                    if (!reported.contains(key)) {
                        reported.insert(key);
                        errors.append(qMakePair(feat,
                            QStringLiteral("线相交：要素%1与%2空间交叉").arg(feat.id()).arg(fid)));
                    }
                }
            }
        }
    }
}

// ====== 线必须在面内部 ======
void checkInsidePolygon(QgsVectorLayer* lineLayer, QgsVectorLayer* polyLayer,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!lineLayer || !polyLayer) return;
    QgsSpatialIndex polyIndex(polyLayer->getFeatures());
    QgsFeatureIterator it = lineLayer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g = feat.geometry();
        if (g.isNull()) continue;
        QList<QgsFeatureId> cands = polyIndex.intersects(g.boundingBox());
        bool inside = false;
        for (QgsFeatureId fid : cands) {
            QgsFeature pf;
            if (polyLayer->getFeatures(QgsFeatureRequest(fid)).nextFeature(pf)) {
                if (pf.geometry().contains(g)) { inside = true; break; }
            }
        }
        if (!inside) errors.append(qMakePair(feat, QStringLiteral("线不完全在面内部")));
    }
}

// ====== 本地回退主入口 ======
void executeLocal(QgsVectorLayer* lineLayer, QgsVectorLayer* pointLayer,
    QgsVectorLayer* polyLayer, QgsVectorLayer* refLineLayer,
    int processMode, const QHash<QString, double>& thresholds,
    QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!lineLayer) return;
    bool all = (processMode == 0);
    auto en = [&](int m) { return all || (processMode & m); };
    auto gd = [&](const QString& k, double d) { return thresholds.value(k, d); };
    double tol = gd("FuzzyTolerance", 0.001);
    Q_UNUSED(pointLayer); Q_UNUSED(refLineLayer);

    auto run = [&](int mode, const QString& name, auto fn) {
        if (!en(mode)) return;
        QList<QPair<QgsFeature, QString>> errs;
        fn(errs);
        allErrors.append(errs);
        executedChecks.append(QString("%1(%2个错误)").arg(name).arg(errs.size()));
    };

    run(1,  "悬挂节点",     [&](auto& e){ checkDangles(lineLayer, e, tol); });
    run(2,  "伪节点",       [&](auto& e){ checkPseudos(lineLayer, e, tol); });
    run(4,  "线自重叠",     [&](auto& e){ checkSelfOverlap(lineLayer, e); });
    run(8,  "线自相交",     [&](auto& e){ checkSelfIntersect(lineLayer, e); });
    run(16, "多部件线",     [&](auto& e){ checkSinglePart(lineLayer, e); });
    run(32, "线相互重叠",   [&](auto& e){ checkOverlap(lineLayer, e); });
    run(64, "线相互相交",   [&](auto& e){ checkIntersect(lineLayer, e); });
    if (polyLayer) {
        run(4096, "线在面内部", [&](auto& e){ checkInsidePolygon(lineLayer, polyLayer, e); });
    }
}

} // namespace

namespace Mission455 {

void execute(QgsVectorLayer* lineLayer, const QString& refPointShp,
             const QString& refPolyShp,
             int processMode, const QHash<QString, double>& thresholds,
             QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!lineLayer) {
        executedChecks.append(QStringLiteral("线拓扑检查：无线要素图层，本次不涉及"));
        return;
    }

    // ---- 引擎不可用（如麒麟无 MapBatchProcessing.exe）→ 本地 QGIS 实现 ----
    if (!WujiEngineBridge::engineAvailable()) {
        QgsVectorLayer* refPoly = openRefLayer(refPolyShp);
        executeLocal(lineLayer, nullptr /*参考点图层未用*/,
                     refPoly, nullptr /*参考线图层未用*/,
                     processMode, thresholds, allErrors, executedChecks);
        delete refPoly;
        return;
    }

    QTemporaryDir work;
    if (!work.isValid()) {
        executedChecks.append(QStringLiteral("线拓扑检查：无法创建工作目录，本次未执行"));
        return;
    }
    const QString dir = work.path();

    // ---- 输入图层导出为 GBK 编码 SHP ----
    QString errOut;
    const QString lineShp = WujiMissionRunner::exportLayerShp(
        lineLayer, dir, QStringLiteral("polyline"), &errOut);
    if (lineShp.isEmpty()) {
        executedChecks.append(QStringLiteral("线拓扑检查：无法导出线图层（%1），本次未执行").arg(errOut));
        return;
    }
    // 参考图层（其余类型的全部图层合并件）复制进任务目录（FilePath 必须相对路径）
    const QString pointShp = refPointShp.isEmpty() ? QString()
        : WujiMissionRunner::stageShapefiles(QStringList() << refPointShp, dir).value(0);
    const QString polyShp = refPolyShp.isEmpty() ? QString()
        : WujiMissionRunner::stageShapefiles(QStringList() << refPolyShp, dir).value(0);

    // ---- 构造任务 XML 并执行 ----
    const QString outLineShp = QDir(dir).filePath(QStringLiteral("polyline_out.shp"));
    const QString outPointShp = QDir(dir).filePath(QStringLiteral("point_out.shp"));
    const QString xml = WujiMissionXml::buildMission455(
        dir, lineShp, pointShp, polyShp, QString() /*refLineShp 未用*/,
        processMode, thresholds.value(QStringLiteral("FuzzyTolerance"), 0.001),
        thresholds.value(QStringLiteral("BufferDistance"), 0.0),
        outLineShp, outPointShp);

    int errBefore = allErrors.size();
    if (WujiMissionRunner::runMissionXml(xml, dir, QStringLiteral("线拓扑检查"),
                                         processMode, executedChecks)) {
        // 线结果层：源线要素副本 + info_NM 字段（非空即错误）
        // 错误消息带图层名，如"线拓扑检查(一级河流)"，供合并输出时提取
        const QString baseName = QFileInfo(lineLayer->source()).completeBaseName();
        WujiMissionRunner::readResultErrors(outLineShp, lineLayer,
            QStringLiteral("线拓扑检查(%1)").arg(baseName), WujiMissionRunner::ReadInfoField,
            QStringLiteral("info_NM"), allErrors);
        // 错误位置点层：每个要素即一处错误
        WujiMissionRunner::readResultErrors(outPointShp, lineLayer,
            QStringLiteral("线拓扑检查错误位置(%1)").arg(baseName), WujiMissionRunner::ReadAll,
            QStringLiteral("info_NM"), allErrors);
    }
    int added = allErrors.size() - errBefore;
    executedChecks.append(QStringLiteral("线拓扑检查(%1)：%2")
        .arg(QFileInfo(lineLayer->source()).completeBaseName())
        .arg(added == 0 ? QStringLiteral("未检出异常") : QStringLiteral("检出异常%1处").arg(added)));
}

} // namespace Mission455
