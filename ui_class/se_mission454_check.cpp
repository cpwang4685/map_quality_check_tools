#include "se_mission454_check.h"
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsspatialindex.h"
#include <cmath>

using namespace Mission454;

// ====== 模式1: 点必须重合 ======
void Mission454::checkCoincident(QgsVectorLayer* layer, QgsVectorLayer* refLayer,
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
void Mission454::checkDisjoint(QgsVectorLayer* layer, QgsVectorLayer* refLayer,
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
void Mission454::checkCoveredByEndpoint(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
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
void Mission454::checkCoveredByLine(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
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
void Mission454::checkInsidePolygon(QgsVectorLayer* pointLayer, QgsVectorLayer* polyLayer,
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
void Mission454::checkOnBoundary(QgsVectorLayer* pointLayer, QgsVectorLayer* polyLayer,
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

// ====== 主入口 ======
void Mission454::execute(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
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
