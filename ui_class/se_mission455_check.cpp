#include "se_mission455_check.h"
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsspatialindex.h"
#include "qgsfeaturerequest.h"
#include <QSet>
#include <cmath>

using namespace Mission455;

// ====== 模式1: 不能有悬挂节点 ======
void Mission455::checkDangles(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
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
void Mission455::checkPseudos(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
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
void Mission455::checkSelfOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
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
void Mission455::checkSelfIntersect(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
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
void Mission455::checkSinglePart(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
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
void Mission455::checkOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
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
void Mission455::checkIntersect(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors)
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
void Mission455::checkInsidePolygon(QgsVectorLayer* lineLayer, QgsVectorLayer* polyLayer,
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

// ====== 主入口 ======
void Mission455::execute(QgsVectorLayer* lineLayer, QgsVectorLayer* pointLayer,
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
