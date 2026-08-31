#include "se_mission459_check.h"

#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgspolygon.h"
#include "qgslinestring.h"
#include "qgsmultipolygon.h"
#include "qgsmessagelog.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <cmath>

using namespace Mission459;

// ============================================================================
// 模式1: 多部件检查 — Must be single part
// ============================================================================
void Mission459::checkMultiPart(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double fuzzyTolerance)
{
    Q_UNUSED(fuzzyTolerance);
    if (!layer) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (!geom.isNull() && geom.isMultipart()) {
            errors.append(qMakePair(feat,
                QStringLiteral("多部件几何：要素由多个不连通的部分组成，应拆分为独立要素")));
        }
    }
}

// ============================================================================
// 模式2: 空图形检查 — 检查几何为NULL或EMPTY
// ============================================================================
void Mission459::checkEmptyGeometry(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull()) {
            errors.append(qMakePair(feat, QStringLiteral("空图形：要素几何为NULL")));
        } else if (geom.isEmpty()) {
            errors.append(qMakePair(feat, QStringLiteral("空图形：要素几何为EMPTY")));
        }
    }
}

// ============================================================================
// 模式4: 尖锐角检查 — 检查面要素是否存在过于尖锐的内角
// ============================================================================
void Mission459::checkAcuteAngle(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double acuteAngleThreshold)
{
    if (!layer) return;
    if (acuteAngleThreshold <= 0.0) acuteAngleThreshold = 10.0; // 默认10度

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        // 对于面几何，提取外环和内环的所有顶点
        QList<QgsPointXY> vertices;
        if (geom.type() == QgsWkbTypes::PolygonGeometry) {
            QgsMultiPolygonXY multiPoly = geom.asMultiPolygon();
            for (const QgsPolygonXY& poly : multiPoly) {
                for (const QgsPolylineXY& ring : poly) {
                    for (const QgsPointXY& pt : ring) {
                        vertices.append(pt);
                    }
                }
            }
        } else {
            // 对于线几何，直接取顶点
            QgsVertexIterator vIt = geom.vertices();
            while (vIt.hasNext()) {
                vertices.append(QgsPointXY(vIt.next()));
            }
        }

        if (vertices.size() < 3) continue;

        // 遍历每个顶点，计算角度
        QStringList acuteAngles;
        int n = vertices.size();
        for (int i = 0; i < n; i++) {
            const QgsPointXY& p0 = vertices[(i - 1 + n) % n];
            const QgsPointXY& p1 = vertices[i];
            const QgsPointXY& p2 = vertices[(i + 1) % n];

            // 向量 v1 = p0→p1, v2 = p2→p1
            double v1x = p1.x() - p0.x();
            double v1y = p1.y() - p0.y();
            double v2x = p1.x() - p2.x();
            double v2y = p1.y() - p2.y();

            double len1 = std::sqrt(v1x * v1x + v1y * v1y);
            double len2 = std::sqrt(v2x * v2x + v2y * v2y);
            if (len1 < 1e-15 || len2 < 1e-15) continue;

            double dot = v1x * v2x + v1y * v2y;
            double cosAngle = dot / (len1 * len2);
            // 限制范围防止浮点误差
            if (cosAngle > 1.0) cosAngle = 1.0;
            if (cosAngle < -1.0) cosAngle = -1.0;
            double angle = std::acos(cosAngle) * 180.0 / M_PI;

            if (angle < acuteAngleThreshold && angle > 0.01) {
                acuteAngles.append(QString("顶点%1: %2°").arg(i).arg(angle, 0, 'f', 2));
            }
        }

        if (!acuteAngles.isEmpty()) {
            errors.append(qMakePair(feat,
                QString("尖锐角：%1处角度<%2° — %3")
                    .arg(acuteAngles.size())
                    .arg(acuteAngleThreshold, 0, 'f', 1)
                    .arg(acuteAngles.mid(0, 3).join(", "))));
        }
    }
}

// ============================================================================
// 模式8: 碎面检查 — 检查面积过小的多边形（使用QGIS内置面积计算）
// ============================================================================
void Mission459::checkSliverPolygon(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double sliverArea, double fuzzyTolerance)
{
    Q_UNUSED(fuzzyTolerance);
    if (!layer) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        double area = geom.area();
        if (sliverArea > 0.0 && area < sliverArea && area > 0.0) {
            errors.append(qMakePair(feat,
                QStringLiteral("碎面：面积%1小于阈值%2").arg(area, 0, 'f', 4).arg(sliverArea, 0, 'f', 4)));
        }
    }
}

// ============================================================================
// 模式16: 狭长碎面检查 — 检查宽度过窄的狭长面
// 使用面积/周长比估算宽度（对于细长形状，宽度≈2*面积/周长）
// ============================================================================
void Mission459::checkNarrowPolygon(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double narrowWidth)
{
    if (!layer || narrowWidth <= 0.0) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        double area = geom.area();
        double perimeter = 0.0;

        // 计算周长
        if (geom.type() == QgsWkbTypes::PolygonGeometry) {
            QgsMultiPolygonXY multiPoly = geom.asMultiPolygon();
            for (const QgsPolygonXY& poly : multiPoly) {
                for (const QgsPolylineXY& ring : poly) {
                    for (int i = 0; i < ring.size() - 1; i++) {
                        double dx = ring[i + 1].x() - ring[i].x();
                        double dy = ring[i + 1].y() - ring[i].y();
                        perimeter += std::sqrt(dx * dx + dy * dy);
                    }
                }
            }
        }

        if (perimeter > 1e-15) {
            double estimatedWidth = 2.0 * area / perimeter;
            if (estimatedWidth < narrowWidth) {
                errors.append(qMakePair(feat,
                    QStringLiteral("狭长面：估算宽度%1小于阈值%2").arg(estimatedWidth, 0, 'f', 4).arg(narrowWidth, 0, 'f', 4)));
            }
        }
    }
}

// ============================================================================
// 模式32: 小面积碎面检查 — Must be larger than cluster tolerance
// ============================================================================
void Mission459::checkSmallArea(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double minArea)
{
    if (!layer || minArea <= 0.0) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        double area = geom.area();
        if (area > 0.0 && area < minArea) {
            errors.append(qMakePair(feat,
                QStringLiteral("小面积：面积%1小于阈值%2").arg(area, 0, 'f', 4).arg(minArea, 0, 'f', 4)));
        }
    }
}

// ============================================================================
// 模式64: 节点平均密度检查 — 检查面的节点平均密度是否在合理范围
// 密度 = 节点数 / 面积 或 节点数 / 周长
// ============================================================================
void Mission459::checkAvgNodeDensity(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double upperBound, double lowerBound)
{
    if (!layer) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        int nodeCount = 0;
        QgsVertexIterator vIt = geom.vertices();
        while (vIt.hasNext()) {
            vIt.next();
            nodeCount++;
        }

        double area = geom.area();
        if (area > 1e-15) {
            double density = nodeCount / area;
            if (upperBound > 0.0 && density > upperBound) {
                errors.append(qMakePair(feat,
                    QStringLiteral("节点平均密度偏高：%1节点/平方米(上界%2)").arg(density, 0, 'f', 6).arg(upperBound, 0, 'f', 6)));
            }
            if (lowerBound > 0.0 && density < lowerBound) {
                errors.append(qMakePair(feat,
                    QStringLiteral("节点平均密度偏低：%1节点/平方米(下界%2)").arg(density, 0, 'f', 6).arg(lowerBound, 0, 'f', 6)));
            }
        }
    }
}

// ============================================================================
// 模式128: 节点密度检查 — 检查面边界的节点间距密度
// ============================================================================
void Mission459::checkNodeDensity(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double upperBound, double lowerBound)
{
    if (!layer) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        // 计算周长上的平均节点间距
        int nodeCount = 0;
        double perimeter = 0.0;
        QgsPointXY prevPt;
        bool first = true;

        QgsVertexIterator vIt = geom.vertices();
        while (vIt.hasNext()) {
            QgsPointXY pt(vIt.next());
            if (!first) {
                double dx = pt.x() - prevPt.x();
                double dy = pt.y() - prevPt.y();
                perimeter += std::sqrt(dx * dx + dy * dy);
            }
            prevPt = pt;
            first = false;
            nodeCount++;
        }

        if (nodeCount > 1 && perimeter > 1e-15) {
            double avgSpacing = perimeter / (nodeCount - 1);
            if (upperBound > 0.0 && avgSpacing > upperBound) {
                errors.append(qMakePair(feat,
                    QStringLiteral("节点间距过大：平均%1(上界%2)").arg(avgSpacing, 0, 'f', 4).arg(upperBound, 0, 'f', 4)));
            }
            if (lowerBound > 0.0 && avgSpacing < lowerBound) {
                errors.append(qMakePair(feat,
                    QStringLiteral("节点间距过小：平均%1(下界%2)").arg(avgSpacing, 0, 'f', 4).arg(lowerBound, 0, 'f', 4)));
            }
        }
    }
}

// ============================================================================
// 模式256: 线自相交检查 — Must not self-intersect
// 对多边形边界和线要素都适用
// ============================================================================
void Mission459::checkSelfIntersect(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        // 使用QGIS内置的几何验证
        QVector<QgsGeometry::Error> geomErrors;
        geom.validateGeometry(geomErrors);

        bool hasSelfIntersect = false;
        QStringList details;
        for (const QgsGeometry::Error& err : geomErrors) {
            if (err.what().contains("self", Qt::CaseInsensitive) ||
                err.what().contains("intersect", Qt::CaseInsensitive)) {
                hasSelfIntersect = true;
                details.append(err.what());
            }
        }

        // 额外检查：使用isSimple判断
        if (!hasSelfIntersect && !geom.isSimple()) {
            hasSelfIntersect = true;
            details.append(QStringLiteral("几何不是简单几何（可能自相交）"));
        }

        if (hasSelfIntersect) {
            errors.append(qMakePair(feat,
                QStringLiteral("自相交：%1").arg(details.join("; "))));
        }
    }
}

// ============================================================================
// 模式512: 节点最小距离检查 — 检查相邻节点间距是否过小
// ============================================================================
void Mission459::checkMinNodeDistance(QgsVectorLayer* layer,
    QList<QPair<QgsFeature, QString>>& errors,
    double minDistance)
{
    if (!layer || minDistance <= 0.0) return;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || geom.isEmpty()) continue;

        QStringList closeNodes;
        QgsPointXY prevPt;
        bool first = true;
        int nodeIdx = 0;

        QgsVertexIterator vIt = geom.vertices();
        while (vIt.hasNext()) {
            QgsPointXY pt(vIt.next());
            if (!first) {
                double dx = pt.x() - prevPt.x();
                double dy = pt.y() - prevPt.y();
                double dist = std::sqrt(dx * dx + dy * dy);
                if (dist < minDistance && dist > 1e-20) {
                    closeNodes.append(QString("节点%1-%2: 距离%3")
                        .arg(nodeIdx - 1).arg(nodeIdx).arg(dist, 0, 'f', 6));
                }
            }
            prevPt = pt;
            first = false;
            nodeIdx++;
        }

        if (!closeNodes.isEmpty()) {
            errors.append(qMakePair(feat,
                QStringLiteral("节点过近(<%1)：%2")
                    .arg(minDistance, 0, 'f', 4)
                    .arg(closeNodes.mid(0, 5).join(", "))));
        }
    }
}

// ============================================================================
// 主入口：根据processMode位掩码执行启用的检查项
// ============================================================================
void Mission459::execute(QgsVectorLayer* layer,
    int processMode,
    const QHash<QString, double>& thresholds,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks)
{
    if (!layer || !layer->isValid()) return;

    // processMode == 0 表示全部启用
    bool allEnabled = (processMode == 0);

    auto enabled = [&](int mode) -> bool {
        return allEnabled || (processMode & mode);
    };

    auto getThreshold = [&](const QString& key, double defaultVal) -> double {
        return thresholds.value(key, defaultVal);
    };

    // 1: 多部件检查
    if (enabled(1)) {
        QList<QPair<QgsFeature, QString>> errs;
        checkMultiPart(layer, errs);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("多部件检查(%1个错误)").arg(errs.size()));
    }

    // 2: 空图形检查
    if (enabled(2)) {
        QList<QPair<QgsFeature, QString>> errs;
        checkEmptyGeometry(layer, errs);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("空图形检查(%1个错误)").arg(errs.size()));
    }

    // 4: 尖锐角检查
    if (enabled(4)) {
        QList<QPair<QgsFeature, QString>> errs;
        double threshold = getThreshold("acuteAngle", 10.0);
        checkAcuteAngle(layer, errs, threshold);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("尖锐角检查(%1个错误,阈值%2°)").arg(errs.size()).arg(threshold));
    }

    // 8: 碎面检查
    if (enabled(8)) {
        QList<QPair<QgsFeature, QString>> errs;
        double threshold = getThreshold("sliverArea", 0.0);
        checkSliverPolygon(layer, errs, threshold);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("碎面检查(%1个错误,阈值%2)").arg(errs.size()).arg(threshold));
    }

    // 16: 狭长碎面检查
    if (enabled(16)) {
        QList<QPair<QgsFeature, QString>> errs;
        double threshold = getThreshold("narrowWidth", 0.0);
        checkNarrowPolygon(layer, errs, threshold);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("狭长碎面检查(%1个错误,阈值%2)").arg(errs.size()).arg(threshold));
    }

    // 32: 小面积碎面检查
    if (enabled(32)) {
        QList<QPair<QgsFeature, QString>> errs;
        double threshold = getThreshold("minArea", 0.0);
        checkSmallArea(layer, errs, threshold);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("小面积检查(%1个错误,阈值%2)").arg(errs.size()).arg(threshold));
    }

    // 64: 节点平均密度检查
    if (enabled(64)) {
        QList<QPair<QgsFeature, QString>> errs;
        double upper = getThreshold("avgNodeDensityUpper", 0.0);
        double lower = getThreshold("avgNodeDensityLower", 0.0);
        checkAvgNodeDensity(layer, errs, upper, lower);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("节点平均密度检查(%1个错误)").arg(errs.size()));
    }

    // 128: 节点密度检查
    if (enabled(128)) {
        QList<QPair<QgsFeature, QString>> errs;
        double upper = getThreshold("nodeDensityUpper", 0.0);
        double lower = getThreshold("nodeDensityLower", 0.0);
        checkNodeDensity(layer, errs, upper, lower);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("节点密度检查(%1个错误)").arg(errs.size()));
    }

    // 256: 线自相交检查
    if (enabled(256)) {
        QList<QPair<QgsFeature, QString>> errs;
        checkSelfIntersect(layer, errs);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("自相交检查(%1个错误)").arg(errs.size()));
    }

    // 512: 节点最小距离检查
    if (enabled(512)) {
        QList<QPair<QgsFeature, QString>> errs;
        double threshold = getThreshold("minNodeDistance", 0.0);
        checkMinNodeDistance(layer, errs, threshold);
        allErrors.append(errs);
        executedChecks.append(QStringLiteral("节点最小距离检查(%1个错误,阈值%2)").arg(errs.size()).arg(threshold));
    }
}

// ============================================================================
// 日志输出工具 — 突破SHP 254字符限制
// ============================================================================
QString QualityCheckLogger::timestamp()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
}

bool QualityCheckLogger::writeCheckLog(const QString& logFilePath,
    const QString& checkItemName,
    const QString& layerName,
    int totalFeatures,
    const QList<QPair<QgsFeature, QString>>& errors,
    const QStringList& executedChecks)
{
    QJsonObject root;
    root["checkItem"] = checkItemName;
    root["layerName"] = layerName;
    root["timestamp"] = timestamp();
    root["totalFeatures"] = totalFeatures;
    root["errorCount"] = errors.size();
    root["passRate"] = totalFeatures > 0
        ? QString("%1%").arg(100.0 * (totalFeatures - errors.size()) / totalFeatures, 0, 'f', 1)
        : "N/A";

    // 执行摘要
    QJsonArray checksArray;
    for (const QString& check : executedChecks) {
        checksArray.append(check);
    }
    root["executedChecks"] = checksArray;

    // 详细错误列表
    QJsonArray errorsArray;
    for (const auto& pair : errors) {
        QJsonObject errObj;
        errObj["featureId"] = (qint64)pair.first.id();
        errObj["errorMessage"] = pair.second;

        // 附加几何信息（WKT格式，用于定位）
        QgsGeometry geom = pair.first.geometry();
        if (!geom.isNull()) {
            errObj["geometryType"] = QgsWkbTypes::displayString(geom.wkbType());
            errObj["centroidX"] = geom.centroid().asPoint().x();
            errObj["centroidY"] = geom.centroid().asPoint().y();
        }

        // 附加属性信息（前5个字段）
        QgsFields fields = pair.first.fields();
        QJsonObject attrs;
        for (int i = 0; i < qMin(fields.size(), 5); i++) {
            attrs[fields.at(i).name()] = pair.first.attribute(i).toString();
        }
        errObj["attributes"] = attrs;

        errorsArray.append(errObj);
    }
    root["errors"] = errorsArray;

    // 写入文件
    QFile file(logFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}
