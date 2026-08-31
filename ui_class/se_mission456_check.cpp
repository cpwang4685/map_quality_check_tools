#include "se_mission456_check.h"
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsspatialindex.h"
#include "qgsfeaturerequest.h"
#include <QSet>
#include <cmath>

using namespace Mission456;

// ====== 模式1: 面不能重叠 ======
void Mission456::checkOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
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
void Mission456::checkGaps(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
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
void Mission456::checkContainsPoint(QgsVectorLayer* polyLayer, QgsVectorLayer* pointLayer,
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
void Mission456::checkBoundaryCoveredByLine(QgsVectorLayer* polyLayer, QgsVectorLayer* lineLayer,
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
void Mission456::checkLargerThanTolerance(QgsVectorLayer* layer,
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

// ====== 主入口 ======
void Mission456::execute(QgsVectorLayer* polyLayer, QgsVectorLayer* lineLayer,
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
