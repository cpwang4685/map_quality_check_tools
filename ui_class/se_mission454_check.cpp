#include "se_mission454_check.h"

#include "../ui_task/wuji_engine_bridge.h"
#include "../ui_task/wuji_mission_runner.h"
#include "../ui_task/wuji_mission_xml.h"

#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsspatialindex.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>

// ============================================================
//  本地回退实现
//
//  检查逻辑正常由引擎子进程（MapBatchProcessing.exe）完成；该可执行
//  文件只存在于 Windows，麒麟上不存在，此时退回下面的本地 QGIS 算法，
//  保证 454 在无引擎环境下仍可执行（Windows 用引擎、麒麟用本地）。
//
//  算法本体与本工程综合前的实现逐字一致，唯一差别是参考图层由已合并
//  好的 SHP 路径加载而来（引擎路径下参考图层同样以合并 SHP 形式传入）。
//  模式位含义与引擎实现一致：1 重合 2 分离 4 线端点覆盖 8 线覆盖
//  16 在面内 32 在面边界。模式 1/2 需要参考点图层，两套实现均不提供
//  （454 的参考点 SHP 未用），故与改动前行为一致——不执行。
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

// ====== 模式1: 点必须重合 ======
void checkCoincident(QgsVectorLayer* layer, QgsVectorLayer* refLayer,
    QList<QPair<QgsFeature, QString>>& errors, double tolerance)
{
    if (!layer || !refLayer) return;
    QgsSpatialIndex refIndex(refLayer->getFeatures());
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull()) continue;
        QgsPointXY pt = geom.asPoint();
        QgsRectangle searchRect(pt.x()-tolerance, pt.y()-tolerance,
                                  pt.x()+tolerance, pt.y()+tolerance);
        QList<QgsFeatureId> neighbors = refIndex.intersects(searchRect);
        if (neighbors.isEmpty()) {
            errors.append(qMakePair(feat, QStringLiteral("点不重合：未在参考点图层的容差(%1)范围内找到重合点").arg(tolerance)));
        }
    }
}

// ====== 模式2: 点必须分离 ======
void checkDisjoint(QgsVectorLayer* layer, QgsVectorLayer* refLayer,
    QList<QPair<QgsFeature, QString>>& errors, double tolerance)
{
    if (!layer || !refLayer) return;
    QgsSpatialIndex refIndex(refLayer->getFeatures());
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull()) continue;
        QgsPointXY pt = geom.asPoint();
        QgsRectangle searchRect(pt.x()-tolerance, pt.y()-tolerance,
                                  pt.x()+tolerance, pt.y()+tolerance);
        QList<QgsFeatureId> neighbors = refIndex.intersects(searchRect);
        if (!neighbors.isEmpty()) {
            errors.append(qMakePair(feat, QStringLiteral("点未分离：在容差(%1)范围内存在参考点要素").arg(tolerance)));
        }
    }
}

// ====== 模式4: 点被线端点覆盖 ======
void checkCoveredByEndpoint(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
    QList<QPair<QgsFeature, QString>>& errors, double tolerance)
{
    if (!pointLayer || !lineLayer) return;
    // 收集所有线的端点
    QList<QgsPointXY> endpoints;
    QgsFeatureIterator lit = lineLayer->getFeatures();
    QgsFeature lfeat;
    while (lit.nextFeature(lfeat)) {
        QgsGeometry geom = lfeat.geometry();
        if (geom.isNull()) continue;
        QgsVertexIterator vIt = geom.vertices();
        if (vIt.hasNext()) { endpoints.append(QgsPointXY(vIt.next())); }
        QgsPointXY lastPt;
        while (vIt.hasNext()) { lastPt = QgsPointXY(vIt.next()); }
        if (!lastPt.isEmpty()) { endpoints.append(lastPt); }
    }
    // 建端点空间索引
    QgsSpatialIndex epIndex;
    for (int i = 0; i < endpoints.size(); i++) {
        QgsPointXY ep = endpoints[i];
        epIndex.addFeature(i, QgsRectangle(ep.x(), ep.y(), ep.x(), ep.y()));
    }
    // 检查点是否在线端点上
    QgsFeatureIterator pit = pointLayer->getFeatures();
    QgsFeature pfeat;
    while (pit.nextFeature(pfeat)) {
        QgsGeometry geom = pfeat.geometry();
        if (geom.isNull()) continue;
        QgsPointXY pt = geom.asPoint();
        QgsRectangle sr(pt.x()-tolerance, pt.y()-tolerance, pt.x()+tolerance, pt.y()+tolerance);
        if (epIndex.intersects(sr).isEmpty()) {
            errors.append(qMakePair(pfeat, QStringLiteral("点未被线端点覆盖")));
        }
    }
}

// ====== 模式8: 点必须被线覆盖 ======
void checkCoveredByLine(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
    QList<QPair<QgsFeature, QString>>& errors, double tolerance)
{
    if (!pointLayer || !lineLayer) return;
    QgsSpatialIndex lineIndex(lineLayer->getFeatures());
    QgsFeatureIterator pit = pointLayer->getFeatures();
    QgsFeature pfeat;
    while (pit.nextFeature(pfeat)) {
        QgsGeometry ptGeom = pfeat.geometry();
        if (ptGeom.isNull()) continue;
        QgsPointXY pt = ptGeom.asPoint();
        QgsRectangle sr(pt.x()-tolerance, pt.y()-tolerance, pt.x()+tolerance, pt.y()+tolerance);
        QList<QgsFeatureId> candidates = lineIndex.intersects(sr);
        bool covered = false;
        for (QgsFeatureId fid : candidates) {
            QgsFeature lfeat;
            if (lineLayer->getFeatures(QgsFeatureRequest(fid)).nextFeature(lfeat)) {
                if (lfeat.geometry().distance(ptGeom) <= tolerance) { covered = true; break; }
            }
        }
        if (!covered) {
            errors.append(qMakePair(pfeat, QStringLiteral("点未被线覆盖")));
        }
    }
}

// ====== 模式16: 点必须在面内部 ======
void checkInsidePolygon(QgsVectorLayer* pointLayer, QgsVectorLayer* polyLayer,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!pointLayer || !polyLayer) return;
    QgsSpatialIndex polyIndex(polyLayer->getFeatures());
    QgsFeatureIterator pit = pointLayer->getFeatures();
    QgsFeature pfeat;
    while (pit.nextFeature(pfeat)) {
        QgsGeometry ptGeom = pfeat.geometry();
        if (ptGeom.isNull()) continue;
        QList<QgsFeatureId> candidates = polyIndex.intersects(ptGeom.boundingBox());
        bool inside = false;
        for (QgsFeatureId fid : candidates) {
            QgsFeature pfeat2;
            if (polyLayer->getFeatures(QgsFeatureRequest(fid)).nextFeature(pfeat2)) {
                if (pfeat2.geometry().contains(ptGeom)) { inside = true; break; }
            }
        }
        if (!inside) {
            errors.append(qMakePair(pfeat, QStringLiteral("点不在任何面内部")));
        }
    }
}

// ====== 模式32: 点必须在面边界上 ======
void checkOnBoundary(QgsVectorLayer* pointLayer, QgsVectorLayer* polyLayer,
    QList<QPair<QgsFeature, QString>>& errors, double tolerance)
{
    if (!pointLayer || !polyLayer) return;
    QgsSpatialIndex polyIndex(polyLayer->getFeatures());
    QgsFeatureIterator pit = pointLayer->getFeatures();
    QgsFeature pfeat;
    while (pit.nextFeature(pfeat)) {
        QgsGeometry ptGeom = pfeat.geometry();
        if (ptGeom.isNull()) continue;
        QgsPointXY pt = ptGeom.asPoint();
        QgsRectangle sr(pt.x()-tolerance, pt.y()-tolerance, pt.x()+tolerance, pt.y()+tolerance);
        QList<QgsFeatureId> candidates = polyIndex.intersects(sr);
        bool onBoundary = false;
        for (QgsFeatureId fid : candidates) {
            QgsFeature pfeat2;
            if (polyLayer->getFeatures(QgsFeatureRequest(fid)).nextFeature(pfeat2)) {
                QgsGeometry boundary = pfeat2.geometry().convertToType(QgsWkbTypes::LineGeometry, true);
                if (boundary.distance(ptGeom) <= tolerance) { onBoundary = true; break; }
            }
        }
        if (!onBoundary) {
            errors.append(qMakePair(pfeat, QStringLiteral("点不在面边界上")));
        }
    }
}

// ====== 本地回退主入口 ======
void executeLocal(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
    QgsVectorLayer* polyLayer, QgsVectorLayer* refPointLayer,
    int processMode, const QHash<QString, double>& thresholds,
    QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!pointLayer) return;
    bool all = (processMode == 0);
    auto en = [&](int m) { return all || (processMode & m); };
    auto gd = [&](const QString& k, double d) { return thresholds.value(k, d); };
    double tol = gd("FuzzyTolerance", 0.001);

    auto run = [&](int mode, const QString& name, auto fn) {
        if (!en(mode)) return;
        QList<QPair<QgsFeature, QString>> errs;
        fn(errs);
        allErrors.append(errs);
        executedChecks.append(QString("%1(%2个错误)").arg(name).arg(errs.size()));
    };

    if (refPointLayer) {
        run(1, "点必须重合",   [&](auto& e){ checkCoincident(pointLayer, refPointLayer, e, tol); });
        run(2, "点必须分离",   [&](auto& e){ checkDisjoint(pointLayer, refPointLayer, e, tol); });
    }
    if (lineLayer) {
        run(4, "点被线端点覆盖", [&](auto& e){ checkCoveredByEndpoint(pointLayer, lineLayer, e, tol); });
        run(8, "点必须被线覆盖", [&](auto& e){ checkCoveredByLine(pointLayer, lineLayer, e, tol); });
    }
    if (polyLayer) {
        run(16, "点必须在面内部", [&](auto& e){ checkInsidePolygon(pointLayer, polyLayer, e); });
        run(32, "点必须在面边界", [&](auto& e){ checkOnBoundary(pointLayer, polyLayer, e, tol); });
    }
}

} // namespace

namespace Mission454 {

void execute(QgsVectorLayer* pointLayer, const QString& refLineShp,
             const QString& refPolyShp,
             int processMode, const QHash<QString, double>& thresholds,
             QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!pointLayer) {
        executedChecks.append(QStringLiteral("点拓扑检查：无点要素图层，本次不涉及"));
        return;
    }

    // ---- 引擎不可用（如麒麟无 MapBatchProcessing.exe）→ 本地 QGIS 实现 ----
    if (!WujiEngineBridge::engineAvailable()) {
        QgsVectorLayer* refLine = openRefLayer(refLineShp);
        QgsVectorLayer* refPoly = openRefLayer(refPolyShp);
        executeLocal(pointLayer, refLine, refPoly, nullptr /*refPointShp 未用*/,
                     processMode, thresholds, allErrors, executedChecks);
        delete refPoly;
        delete refLine;
        return;
    }

    // 每次执行使用独立临时工作目录（QTemporaryDir 析构自动清理）
    QTemporaryDir work;
    if (!work.isValid()) {
        executedChecks.append(QStringLiteral("点拓扑检查：无法创建工作目录，本次未执行"));
        return;
    }
    const QString dir = work.path();

    // ---- 输入图层导出为 GBK 编码 SHP（无极引擎只读 GBK 的 DBF） ----
    QString errOut;
    const QString pointShp = WujiMissionRunner::exportLayerShp(
        pointLayer, dir, QStringLiteral("point"), &errOut);
    if (pointShp.isEmpty()) {
        executedChecks.append(QStringLiteral("点拓扑检查：无法导出点图层（%1），本次未执行").arg(errOut));
        return;
    }
    // 参考图层（其余类型的全部图层合并件）复制进任务目录：
    // XML 的 FilePath 只能是 dataPath 相对路径，不能指向目录外
    const QString lineShp = refLineShp.isEmpty() ? QString()
        : WujiMissionRunner::stageShapefiles(QStringList() << refLineShp, dir).value(0);
    const QString polyShp = refPolyShp.isEmpty() ? QString()
        : WujiMissionRunner::stageShapefiles(QStringList() << refPolyShp, dir).value(0);

    // ---- 构造任务 XML 并执行（官方 DoXMLFile 入口） ----
    const QString outShp = QDir(dir).filePath(QStringLiteral("point_out.shp"));
    const QString xml = WujiMissionXml::buildMission454(
        dir, pointShp, lineShp, polyShp, QString() /*refPointShp 未用*/,
        processMode, thresholds.value(QStringLiteral("FuzzyTolerance"), 0.001), outShp);

    int errBefore = allErrors.size();
    if (WujiMissionRunner::runMissionXml(xml, dir, QStringLiteral("点拓扑检查"),
                                         processMode, executedChecks)) {
        // 结果层：源点要素副本 + info_NM 字段（非空即错误）
        // 错误消息带图层名，如"点拓扑检查(一级河流)"，供合并输出时提取
        WujiMissionRunner::readResultErrors(outShp, pointLayer,
            QStringLiteral("点拓扑检查(%1)").arg(QFileInfo(pointLayer->source()).completeBaseName()),
            WujiMissionRunner::ReadInfoField,
            QStringLiteral("info_NM"), allErrors);
    }
    int added = allErrors.size() - errBefore;
    executedChecks.append(QStringLiteral("点拓扑检查(%1)：%2")
        .arg(QFileInfo(pointLayer->source()).completeBaseName())
        .arg(added == 0 ? QStringLiteral("未检出异常") : QStringLiteral("检出异常%1处").arg(added)));
}

} // namespace Mission454
