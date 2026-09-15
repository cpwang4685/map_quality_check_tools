#include "se_mission456_check.h"

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
//  保证 456 在无引擎环境下仍可执行（Windows 用引擎、麒麟用本地）。
//
//  算法本体与本工程综合前的实现逐字一致，唯一差别是参考线/点图层由已
//  合并好的 SHP 路径加载而来（引擎路径下参考图层同样以合并 SHP 形式传入）。
//  模式位含义与引擎实现一致：1 面重叠 2 面缝隙 4 面包含点
//  8 面包含唯一一点 16 面被要素覆盖 32 面边界被线覆盖 1024 面大于容差。
//  其中 8/16 已从配置中移除，32 默认关闭，按配置不会进入。
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

// ====== 模式1: 面不能重叠 ======
void checkOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QgsSpatialIndex index(layer->getFeatures());
    QSet<QString> reported;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g1 = feat.geometry();
        if (g1.isNull() || g1.isEmpty()) continue;
        QList<QgsFeatureId> cands = index.intersects(g1.boundingBox());
        for (QgsFeatureId fid : cands) {
            if (fid <= feat.id()) continue;
            QgsFeature feat2;
            if (layer->getFeatures(QgsFeatureRequest(fid)).nextFeature(feat2)) {
                QgsGeometry g2 = feat2.geometry();
                if (!g2.isNull() && g1.overlaps(g2)) {
                    QString key = QString("%1-%2").arg(qMin(feat.id(),fid)).arg(qMax(feat.id(),fid));
                    if (!reported.contains(key)) {
                        reported.insert(key);
                        errors.append(qMakePair(feat,
                            QStringLiteral("面重叠：要素%1与%2相互重叠").arg(feat.id()).arg(fid)));
                    }
                }
            }
        }
    }
}

// ====== 模式2: 面不能有缝隙 ======
// 通过检查相邻面之间是否存在未被覆盖的狭长区域来检测
void checkGaps(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
{
    if (!layer) return;
    QgsSpatialIndex index(layer->getFeatures());
    QSet<QString> reported;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g1 = feat.geometry();
        if (g1.isNull() || g1.isEmpty()) continue;
        // 对外环做缓冲，检查相邻面
        QgsGeometry g1Boundary = g1.convertToType(QgsWkbTypes::LineGeometry, true);
        QgsGeometry g1Buffered = g1Boundary.buffer(tol * 2, 5);
        QList<QgsFeatureId> cands = index.intersects(g1Buffered.boundingBox());
        for (QgsFeatureId fid : cands) {
            if (fid <= feat.id()) continue;
            QgsFeature feat2;
            if (layer->getFeatures(QgsFeatureRequest(fid)).nextFeature(feat2)) {
                QgsGeometry g2 = feat2.geometry();
                if (g2.isNull()) continue;
                // 计算两个面之间的空隙
                if (g1.touches(g2) && !g1.intersects(g2)) {
                    // 两个面刚好接触但不重叠，检查间隙
                    QgsGeometry gap = g1.combine(g2).convexHull();
                    double gapArea = gap.area() - g1.area() - g2.area();
                    if (gapArea > tol * tol && gapArea < g1.area() * 0.1) {
                        QString key = QString("%1-%2").arg(qMin(feat.id(),fid)).arg(qMax(feat.id(),fid));
                        if (!reported.contains(key)) {
                            reported.insert(key);
                            errors.append(qMakePair(feat,
                                QStringLiteral("面缝隙：要素%1与%2之间存在约%3的缝隙")
                                    .arg(feat.id()).arg(fid).arg(gapArea, 0, 'f', 4)));
                        }
                    }
                }
            }
        }
    }
}

// ====== 模式3: 面必须包含点 ======
void checkContainsPoint(QgsVectorLayer* polyLayer, QgsVectorLayer* pointLayer,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!polyLayer || !pointLayer) return;
    QgsSpatialIndex ptIndex(pointLayer->getFeatures());
    QgsFeatureIterator it = polyLayer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g = feat.geometry();
        if (g.isNull() || g.isEmpty()) continue;
        QList<QgsFeatureId> cands = ptIndex.intersects(g.boundingBox());
        bool hasPoint = false;
        for (QgsFeatureId fid : cands) {
            QgsFeature pf;
            if (pointLayer->getFeatures(QgsFeatureRequest(fid)).nextFeature(pf)) {
                if (g.contains(pf.geometry())) { hasPoint = true; break; }
            }
        }
        if (!hasPoint) {
            errors.append(qMakePair(feat, QStringLiteral("面内无点：面要素%1不包含任何点").arg(feat.id())));
        }
    }
}

// ====== 面边界必须被线覆盖 ======
void checkBoundaryCoveredByLine(QgsVectorLayer* polyLayer, QgsVectorLayer* lineLayer,
    QList<QPair<QgsFeature, QString>>& errors, double tol)
{
    if (!polyLayer || !lineLayer) return;
    QgsSpatialIndex lineIndex(lineLayer->getFeatures());
    QgsFeatureIterator it = polyLayer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g = feat.geometry();
        if (g.isNull() || g.isEmpty()) continue;
        QgsGeometry boundary = g.convertToType(QgsWkbTypes::LineGeometry, true);
        // 采样边界点检查
        QgsVertexIterator vIt = boundary.vertices();
        bool allCovered = true;
        while (vIt.hasNext()) {
            QgsPointXY pt(vIt.next());
            QgsGeometry ptGeom = QgsGeometry::fromPointXY(pt);
            QList<QgsFeatureId> cands = lineIndex.intersects(
                QgsRectangle(pt.x()-tol, pt.y()-tol, pt.x()+tol, pt.y()+tol));
            bool covered = false;
            for (QgsFeatureId fid : cands) {
                QgsFeature lf;
                if (lineLayer->getFeatures(QgsFeatureRequest(fid)).nextFeature(lf)) {
                    if (lf.geometry().distance(ptGeom) <= tol) { covered = true; break; }
                }
            }
            if (!covered) { allCovered = false; break; }
        }
        if (!allCovered) {
            errors.append(qMakePair(feat, QStringLiteral("边界未覆盖：面边界未被线要素完全覆盖")));
        }
    }
}

// ====== 面必须大于聚类容差 ======
void checkLargerThanTolerance(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors, double minArea)
{
    if (!layer || minArea <= 0.0) return;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry g = feat.geometry();
        if (g.isNull() || g.isEmpty()) continue;
        double area = g.area();
        if (area > 0 && area < minArea) {
            errors.append(qMakePair(feat,
                QStringLiteral("面过小：面积%1小于聚类容差%2").arg(area, 0, 'f', 4).arg(minArea, 0, 'f', 4)));
        }
    }
}

// ====== 本地回退主入口 ======
void executeLocal(QgsVectorLayer* polyLayer, QgsVectorLayer* lineLayer,
    QgsVectorLayer* pointLayer, QgsVectorLayer* refPolyLayer,
    int processMode, const QHash<QString, double>& thresholds,
    QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!polyLayer) return;
    bool all = (processMode == 0);
    auto en = [&](int m) { return all || (processMode & m); };
    auto gd = [&](const QString& k, double d) { return thresholds.value(k, d); };
    double tol = gd("FuzzyTolerance", 0.001);
    Q_UNUSED(refPolyLayer);

    auto run = [&](int mode, const QString& name, auto fn) {
        if (!en(mode)) return;
        QList<QPair<QgsFeature, QString>> errs;
        fn(errs);
        allErrors.append(errs);
        executedChecks.append(QString("%1(%2个错误)").arg(name).arg(errs.size()));
    };

    run(1,    "面重叠",           [&](auto& e){ checkOverlap(polyLayer, e); });
    run(2,    "面缝隙",           [&](auto& e){ checkGaps(polyLayer, e, tol); });
    if (pointLayer) {
        run(4,  "面包含点",       [&](auto& e){ checkContainsPoint(polyLayer, pointLayer, e); });
    }
    if (lineLayer) {
        run(32, "面边界被线覆盖",  [&](auto& e){ checkBoundaryCoveredByLine(polyLayer, lineLayer, e, tol); });
    }
    double minArea = gd("minArea", 0.0);
    run(1024, "面大于容差",       [&](auto& e){ checkLargerThanTolerance(polyLayer, e, minArea); });
}

} // namespace

namespace Mission456 {

void execute(QgsVectorLayer* polyLayer, const QString& refLineShp,
             const QString& refPointShp,
             int processMode, const QHash<QString, double>& thresholds,
             QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!polyLayer) {
        executedChecks.append(QStringLiteral("面拓扑检查：无面要素图层，本次不涉及"));
        return;
    }

    // ---- 引擎不可用（如麒麟无 MapBatchProcessing.exe）→ 本地 QGIS 实现 ----
    if (!WujiEngineBridge::engineAvailable()) {
        QgsVectorLayer* refLine = openRefLayer(refLineShp);
        QgsVectorLayer* refPoint = openRefLayer(refPointShp);
        executeLocal(polyLayer, refLine, refPoint, nullptr /*refPolygonShp 未用*/,
                     processMode, thresholds, allErrors, executedChecks);
        delete refPoint;
        delete refLine;
        return;
    }

    QTemporaryDir work;
    if (!work.isValid()) {
        executedChecks.append(QStringLiteral("面拓扑检查：无法创建工作目录，本次未执行"));
        return;
    }
    const QString dir = work.path();

    // ---- 输入图层导出为 GBK 编码 SHP ----
    QString errOut;
    const QString polyShp = WujiMissionRunner::exportLayerShp(
        polyLayer, dir, QStringLiteral("polygon"), &errOut);
    if (polyShp.isEmpty()) {
        executedChecks.append(QStringLiteral("面拓扑检查：无法导出面图层（%1），本次未执行").arg(errOut));
        return;
    }
    // 参考图层（其余类型的全部图层合并件）复制进任务目录（FilePath 必须相对路径）
    const QString lineShp = refLineShp.isEmpty() ? QString()
        : WujiMissionRunner::stageShapefiles(QStringList() << refLineShp, dir).value(0);
    const QString pointShp = refPointShp.isEmpty() ? QString()
        : WujiMissionRunner::stageShapefiles(QStringList() << refPointShp, dir).value(0);

    // ---- 构造任务 XML 并执行 ----
    const QString outPolyShp = QDir(dir).filePath(QStringLiteral("polygon_out.shp"));
    const QString outLineShp = QDir(dir).filePath(QStringLiteral("polyline_out.shp"));
    const QString outPointShp = QDir(dir).filePath(QStringLiteral("point_out.shp"));
    const QString xml = WujiMissionXml::buildMission456(
        dir, polyShp, lineShp, pointShp, QString() /*refPolygonShp 未用*/,
        processMode, thresholds.value(QStringLiteral("FuzzyTolerance"), 0.001),
        thresholds.value(QStringLiteral("BufferDistance"), 0.0),
        outPolyShp, outLineShp, outPointShp);

    int errBefore = allErrors.size();
    if (WujiMissionRunner::runMissionXml(xml, dir, QStringLiteral("面拓扑检查"),
                                         processMode, executedChecks)) {
        // 面结果层：源面要素副本 + info_NM 字段（非空即错误）
        // 错误消息带图层名，如"面拓扑检查(A级景区)"，供合并输出时提取
        const QString baseName = QFileInfo(polyLayer->source()).completeBaseName();
        WujiMissionRunner::readResultErrors(outPolyShp, polyLayer,
            QStringLiteral("面拓扑检查(%1)").arg(baseName), WujiMissionRunner::ReadInfoField,
            QStringLiteral("info_NM"), allErrors);
        // 线结果层 / 错误位置点层：每个要素即一处错误
        WujiMissionRunner::readResultErrors(outLineShp, polyLayer,
            QStringLiteral("面拓扑检查线结果(%1)").arg(baseName), WujiMissionRunner::ReadAll,
            QStringLiteral("info_NM"), allErrors);
        WujiMissionRunner::readResultErrors(outPointShp, polyLayer,
            QStringLiteral("面拓扑检查错误位置(%1)").arg(baseName), WujiMissionRunner::ReadAll,
            QStringLiteral("info_NM"), allErrors);
    }
    int added = allErrors.size() - errBefore;
    executedChecks.append(QStringLiteral("面拓扑检查(%1)：%2")
        .arg(QFileInfo(polyLayer->source()).completeBaseName())
        .arg(added == 0 ? QStringLiteral("未检出异常") : QStringLiteral("检出异常%1处").arg(added)));
}

} // namespace Mission456
