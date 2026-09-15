#include "se_mission459_check.h"

#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspoint.h"
#include "qgspointxy.h"
#include "qgswkbtypes.h"
#include "qgsgeometryutils.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>
#include <limits>

namespace {

// 2026-08-23 全部改为 QGIS 本地实现，不再调用引擎：
// 引擎 7-19/8-22 两个版本对 2/4/16/32/256/512 均"任务执行失败"，模式1在成果数据上
// 同样失败，且引擎对线图层喂面检查会段错误/挂死（靠子进程隔离）。本地口径按配置：
// 1多部件=几何含多个部件；8碎面=面积小于 SliverArea 阈值（检测面积小于阈值的碎面）。
// 模式64节点平均密度=每要素边界节点数/边界长度（个/米），阈值 AvgNodeDensityUpper/Lower；
// 模式128节点密度=每要素节点总数，阈值 NodeDensityUpper/Lower；阈值0表示该侧不启用。
const int kNativeModes = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512;
const int kEngineModes = 0;

const double kPi = std::acos(-1.0);

QString modeName(int mode)
{
    switch (mode) {
    case 1:   return QStringLiteral("多部件检查");
    case 2:   return QStringLiteral("空图形检查");
    case 4:   return QStringLiteral("尖锐角检查");
    case 8:   return QStringLiteral("碎面检查");
    case 16:  return QStringLiteral("狭长面检查");
    case 32:  return QStringLiteral("小面积检查");
    case 64:  return QStringLiteral("节点平均密度检查");
    case 128: return QStringLiteral("节点密度检查");
    case 256: return QStringLiteral("自相交检查");
    case 512: return QStringLiteral("节点最小距离检查");
    }
    return QString();
}

// ---- 尖锐角（顶点处两相邻边的夹角，度） ----

double angleAtVertexDeg(const QgsPointXY& a, const QgsPointXY& b, const QgsPointXY& c)
{
    const double vx1 = a.x() - b.x(), vy1 = a.y() - b.y();
    const double vx2 = c.x() - b.x(), vy2 = c.y() - b.y();
    const double len1 = std::sqrt(vx1 * vx1 + vy1 * vy1);
    const double len2 = std::sqrt(vx2 * vx2 + vy2 * vy2);
    if (len1 <= 0.0 || len2 <= 0.0)
        return 180.0; // 重复顶点，跳过
    const double cosv = qBound(-1.0, (vx1 * vx2 + vy1 * vy2) / (len1 * len2), 1.0);
    return std::acos(cosv) * 180.0 / kPi;
}

// line：中间顶点处夹角；ring：全部顶点处夹角（末点与首点闭合）
double minAngleDeg(const QgsPolylineXY& pts, bool isRing)
{
    const int n = pts.size();
    double worst = 180.0;
    if (isRing) {
        const int m = n - 1; // 去掉闭合点
        if (m < 3) return 180.0;
        for (int i = 0; i < m; ++i) {
            const QgsPointXY& a = pts[(i + m - 1) % m];
            const QgsPointXY& b = pts[i];
            const QgsPointXY& c = pts[(i + 1) % m];
            worst = qMin(worst, angleAtVertexDeg(a, b, c));
        }
    } else {
        for (int i = 1; i + 1 < n; ++i)
            worst = qMin(worst, angleAtVertexDeg(pts[i - 1], pts[i], pts[i + 1]));
    }
    return worst;
}

// ---- 自相交（线段对两两检测，跳过相邻段与环首尾闭合对） ----

bool segmentsSelfIntersect(const QgsPolylineXY& pts, bool isRing)
{
    const int segCount = pts.size() - 1;
    if (segCount < 4) return false; // 少于 4 段不存在不相邻段相交
    for (int i = 0; i < segCount; ++i) {
        for (int j = i + 1; j < segCount; ++j) {
            if (j == i + 1) continue;                              // 相邻段共享端点
            if (isRing && i == 0 && j == segCount - 1) continue;   // 环首尾段共享闭合点
            QgsPoint ip;
            bool isIntersection = false;
            const bool ok = QgsGeometryUtils::segmentIntersection(
                QgsPoint(pts[i].x(), pts[i].y()), QgsPoint(pts[i + 1].x(), pts[i + 1].y()),
                QgsPoint(pts[j].x(), pts[j].y()), QgsPoint(pts[j + 1].x(), pts[j + 1].y()),
                ip, isIntersection, 1e-8, false);
            if (ok && isIntersection)
                return true;
        }
    }
    return false;
}

// ---- 节点最小距离（不相邻顶点对；环排除闭合对） ----

double minVertexDistance(const QgsPolylineXY& pts, bool isRing)
{
    const int n = pts.size();
    const int m = isRing ? n - 1 : n;
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i < m; ++i) {
        for (int j = i + 1; j < m; ++j) {
            if (j == i + 1) continue;                           // 相邻顶点
            if (isRing && i == 0 && j == m - 1) continue;       // 环闭合对
            best = qMin(best, pts[i].distance(pts[j].x(), pts[j].y()));
        }
    }
    return best;
}

// ---- 各模式检查（返回错误描述，空串 = 通过） ----

// 模式1：多部件检查
QString checkMultipart(const QgsGeometry& g)
{
    if (g.isMultipart())
        return QStringLiteral("几何为多部件（必须是单部件）");
    return QString();
}

// 模式2：空图形检查
QString checkEmpty(const QgsFeature& f)
{
    if (!f.hasGeometry() || f.geometry().isNull() || f.geometry().isEmpty())
        return QStringLiteral("要素无几何或几何为空");
    return QString();
}

// 模式4：尖锐角检查（阈值：AcuteAngle，度）
QString checkAcuteAngle(const QgsGeometry& g, double thr)
{
    double worst = 180.0;
    if (g.type() == QgsWkbTypes::LineGeometry) {
        const QgsMultiPolylineXY parts = g.asMultiPolyline();
        for (const QgsPolylineXY& p : parts)
            worst = qMin(worst, minAngleDeg(p, false));
    } else {
        const QgsMultiPolygonXY parts = g.asMultiPolygon();
        for (const QgsPolygonXY& poly : parts) {
            worst = qMin(worst, minAngleDeg(poly.at(0), true));
            for (int r = 1; r < poly.size(); ++r)
                worst = qMin(worst, minAngleDeg(poly.at(r), true));
        }
    }
    if (worst < thr)
        return QStringLiteral("最小夹角 %1° 小于阈值 %2°").arg(worst, 0, 'f', 2).arg(thr, 0, 'f', 2);
    return QString();
}

// 模式8：碎面检查（阈值：SliverArea；仅面图层适用，面积小于阈值即碎面）
QString checkSliverArea(const QgsGeometry& g, double thr)
{
    if (g.type() != QgsWkbTypes::PolygonGeometry)
        return QString();
    const double area = g.area();
    if (area > 0.0 && area < thr)
        return QStringLiteral("面积 %1 小于碎面阈值 %2").arg(area, 0, 'f', 3).arg(thr, 0, 'f', 3);
    return QString();
}

// 模式16：狭长面检查（阈值：NarrowWidth，最小外接矩形短边 < 阈值；仅面图层适用）
QString checkNarrowPolygon(const QgsGeometry& g, double thr)
{
    if (g.type() != QgsWkbTypes::PolygonGeometry)
        return QString();
    const QgsMultiPolygonXY parts = g.asMultiPolygon();
    double minWidth = std::numeric_limits<double>::max();
    for (const QgsPolygonXY& poly : parts) {
        const QgsGeometry partGeom = QgsGeometry::fromPolygonXY(poly);
        const QgsGeometry obb = partGeom.orientedMinimumBoundingBox();
        if (obb.isEmpty())
            continue;
        const QgsPolylineXY ring = obb.asPolygon().at(0);
        if (ring.size() < 4)
            continue;
        const double w = ring[0].distance(ring[1].x(), ring[1].y());
        const double h = ring[1].distance(ring[2].x(), ring[2].y());
        minWidth = qMin(minWidth, qMin(w, h));
    }
    if (minWidth < thr)
        return QStringLiteral("最小宽度 %1 小于阈值 %2").arg(minWidth, 0, 'f', 3).arg(thr, 0, 'f', 3);
    return QString();
}

// 模式32：小面积检查（阈值：MinArea；仅面图层适用）
QString checkSmallArea(const QgsGeometry& g, double thr)
{
    if (g.type() != QgsWkbTypes::PolygonGeometry)
        return QString();
    const double area = g.area();
    if (area > 0.0 && area < thr)
        return QStringLiteral("面积 %1 小于阈值 %2").arg(area, 0, 'f', 3).arg(thr, 0, 'f', 3);
    return QString();
}

// 模式256：自相交检查（线：线段自相交；面：边界环自相交）
QString checkSelfIntersect(const QgsGeometry& g)
{
    if (g.type() == QgsWkbTypes::LineGeometry) {
        const QgsMultiPolylineXY parts = g.asMultiPolyline();
        for (const QgsPolylineXY& p : parts)
            if (segmentsSelfIntersect(p, false))
                return QStringLiteral("几何存在自相交");
    } else {
        const QgsMultiPolygonXY parts = g.asMultiPolygon();
        for (const QgsPolygonXY& poly : parts) {
            if (segmentsSelfIntersect(poly.at(0), true))
                return QStringLiteral("边界存在自相交");
            for (int r = 1; r < poly.size(); ++r)
                if (segmentsSelfIntersect(poly.at(r), true))
                    return QStringLiteral("边界存在自相交");
        }
    }
    return QString();
}

// 模式512：节点最小距离检查（阈值：MinNodeDistance，节点绝对距离）
QString checkMinNodeDistance(const QgsGeometry& g, double thr)
{
    double best = std::numeric_limits<double>::max();
    if (g.type() == QgsWkbTypes::LineGeometry) {
        const QgsMultiPolylineXY parts = g.asMultiPolyline();
        for (const QgsPolylineXY& p : parts)
            best = qMin(best, minVertexDistance(p, false));
    } else {
        const QgsMultiPolygonXY parts = g.asMultiPolygon();
        for (const QgsPolygonXY& poly : parts) {
            best = qMin(best, minVertexDistance(poly.at(0), true));
            for (int r = 1; r < poly.size(); ++r)
                best = qMin(best, minVertexDistance(poly.at(r), true));
        }
    }
    if (best < thr)
        return QStringLiteral("节点最小距离 %1 小于阈值 %2").arg(best, 0, 'f', 4).arg(thr, 0, 'f', 4);
    return QString();
}

// ---- 节点密度相关（模式64/128） ----

// 统计几何顶点数（多部件递归；环去掉闭合重复点，闭合线同样去重）
int vertexCount(const QgsGeometry& g)
{
    int n = 0;
    if (g.type() == QgsWkbTypes::LineGeometry) {
        const QgsMultiPolylineXY parts = g.asMultiPolyline();
        for (const QgsPolylineXY& p : parts) {
            int m = p.size();
            if (m >= 2 && p[0].x() == p[m - 1].x() && p[0].y() == p[m - 1].y())
                m--;
            n += qMax(0, m);
        }
    } else if (g.type() == QgsWkbTypes::PolygonGeometry) {
        const QgsMultiPolygonXY parts = g.asMultiPolygon();
        for (const QgsPolygonXY& poly : parts)
            for (const QgsPolylineXY& r : poly)
                n += qMax(0, r.size() - 1); // 环首尾闭合点重复
    }
    return n;
}

// 模式64：节点平均密度检查（边界节点数/边界长度，个/米；仅面图层适用。
// 阈值 AvgNodeDensityUpper/Lower 为绝对上下界，0 表示该侧不启用）
QString checkAvgNodeDensity(const QgsGeometry& g, double upper, double lower)
{
    if (g.type() != QgsWkbTypes::PolygonGeometry)
        return QString();
    const double len = g.length();
    if (len <= 0.0)
        return QString();
    const double density = static_cast<double>(vertexCount(g)) / len;
    if (upper > 0.0 && density > upper)
        return QStringLiteral("节点平均密度 %1 个/米 超过上界 %2")
            .arg(density, 0, 'f', 4).arg(upper, 0, 'f', 4);
    if (lower > 0.0 && density < lower)
        return QStringLiteral("节点平均密度 %1 个/米 低于下界 %2")
            .arg(density, 0, 'f', 4).arg(lower, 0, 'f', 4);
    return QString();
}

// 模式128：节点密度检查（每要素节点总数；阈值 NodeDensityUpper/Lower 为绝对上下界，
// 0 表示该侧不启用）
QString checkNodeDensity(const QgsGeometry& g, double upper, double lower)
{
    const int n = vertexCount(g);
    if (n == 0)
        return QString();
    if (upper > 0.0 && n > upper)
        return QStringLiteral("节点总数 %1 超过上界 %2").arg(n).arg(static_cast<int>(upper));
    if (lower > 0.0 && n < lower)
        return QStringLiteral("节点总数 %1 低于下界 %2").arg(n).arg(static_cast<int>(lower));
    return QString();
}

} // namespace

namespace Mission459 {

void execute(QgsVectorLayer* layer, int processMode,
             const QHash<QString, double>& thresholds,
             QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!layer) {
        executedChecks.append(QStringLiteral("图形规范性检查：无图层，本次不涉及"));
        return;
    }

    // 错误消息带图层名，如"图形规范性检查(A级景区/尖锐角检查)：..."，供合并输出时提取
    const QString base = QFileInfo(layer->source()).completeBaseName();

    const int nativeMask = processMode & kNativeModes;

    // ---- 本地 QGIS 检查 ----
    if (nativeMask) {
        const double acuteThr = thresholds.value(QStringLiteral("AcuteAngle"), 10.0);
        const double sliverThr = thresholds.value(QStringLiteral("SliverArea"), 1.0);
        const double narrowThr = thresholds.value(QStringLiteral("NarrowWidth"), 0.5);
        const double minAreaThr = thresholds.value(QStringLiteral("MinArea"), 1.0);
        const double minNodeThr = thresholds.value(QStringLiteral("MinNodeDistance"), 0.001);
        const double avgDensUpper = thresholds.value(QStringLiteral("AvgNodeDensityUpper"), 0.0);
        const double avgDensLower = thresholds.value(QStringLiteral("AvgNodeDensityLower"), 0.0);
        const double nodeDensUpper = thresholds.value(QStringLiteral("NodeDensityUpper"), 0.0);
        const double nodeDensLower = thresholds.value(QStringLiteral("NodeDensityLower"), 0.0);

        const int modes[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
        QHash<int, int> counts;
        QgsFeature f;
        QgsFeatureIterator it = layer->getFeatures();
        while (it.nextFeature(f)) {
            for (const int m : modes) {
                if (!(nativeMask & m)) continue;
                QString detail;
                switch (m) {
                case 1:   detail = checkMultipart(f.geometry()); break;
                case 2:   detail = checkEmpty(f); break;
                case 4:   detail = checkAcuteAngle(f.geometry(), acuteThr); break;
                case 8:   detail = checkSliverArea(f.geometry(), sliverThr); break;
                case 16:  detail = checkNarrowPolygon(f.geometry(), narrowThr); break;
                case 32:  detail = checkSmallArea(f.geometry(), minAreaThr); break;
                case 64:  detail = checkAvgNodeDensity(f.geometry(), avgDensUpper, avgDensLower); break;
                case 128: detail = checkNodeDensity(f.geometry(), nodeDensUpper, nodeDensLower); break;
                case 256: detail = checkSelfIntersect(f.geometry()); break;
                case 512: detail = checkMinNodeDistance(f.geometry(), minNodeThr); break;
                }
                if (!detail.isEmpty()) {
                    allErrors.append(qMakePair(f,
                        QStringLiteral("图形规范性检查(%1/%2)：%3").arg(base, modeName(m), detail)));
                    counts[m]++;
                }
            }
        }
        // 汇总：检出异常的模式逐条列出，最后按图层给一条总结果
        int total = 0;
        for (const int m : modes) {
            if (!(nativeMask & m) || counts.value(m, 0) == 0) continue;
            executedChecks.append(QStringLiteral("图形规范性检查(%1/%2)：检出异常%3处")
                .arg(base, modeName(m)).arg(counts.value(m, 0)));
            total += counts.value(m, 0);
        }
        executedChecks.append(QStringLiteral("图形规范性检查(%1)：%2")
            .arg(base)
            .arg(total == 0 ? QStringLiteral("未检出异常")
                            : QStringLiteral("检出异常%1处").arg(total)));
    }
}

} // namespace Mission459
