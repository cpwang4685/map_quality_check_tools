#include "se_mission455_check.h"

#include "../ui_task/wuji_engine_bridge.h"
#include "../ui_task/wuji_mission_runner.h"
#include "../ui_task/wuji_mission_xml.h"

#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgsfeaturerequest.h"
#include "qgsgeometryutils.h"
#include "qgspointxy.h"
#include "qgsspatialindex.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>

#include <cmath>

namespace {

// 本地复核线要素的真自相交/自重叠，并返回准确位置：
//   1) 非相邻线段的真交叉；
//   2) 非相邻线段的共线重叠（返回重叠段中点）；
//   3) 顶点落在非相邻线段内部（T形自接触）。
// 不含相邻线段、不含闭合线首尾段对，也不把“近邻折返/贴边”算作错误——
// 引擎的 self-overlap/self-intersect 对闭合线和细微折返也会报，这里用于
// 过滤误报并给出准确位置。
bool firstLineSelfIntersection(const QgsGeometry& g, QgsPointXY& loc)
{
    if (g.isNull() || g.type() != QgsWkbTypes::LineGeometry)
        return false;
    const QgsMultiPolylineXY parts = g.asMultiPolyline();
    for (const QgsPolylineXY& part : parts) {
        const int n = part.size();
        if (n < 4) continue;
        const int segCount = n - 1;
        const bool closed = part.first().x() == part.last().x()
            && part.first().y() == part.last().y();
        auto skipPair = [&](int i, int j) {
            return j == i + 1 || (closed && i == 0 && j == segCount - 1);
        };
        // 1) 真交叉
        for (int i = 0; i < segCount; ++i) {
            for (int j = i + 1; j < segCount; ++j) {
                if (skipPair(i, j)) continue;
                QgsPoint ip;
                bool isIntersection = false;
                const bool ok = QgsGeometryUtils::segmentIntersection(
                    QgsPoint(part[i].x(), part[i].y()),
                    QgsPoint(part[i + 1].x(), part[i + 1].y()),
                    QgsPoint(part[j].x(), part[j].y()),
                    QgsPoint(part[j + 1].x(), part[j + 1].y()),
                    ip, isIntersection, 1e-8, false);
                if (ok && isIntersection) {
                    loc = QgsPointXY(ip.x(), ip.y());
                    return true;
                }
            }
        }
        // 2) 共线重叠
        for (int i = 0; i < segCount; ++i) {
            const QgsPointXY& p1 = part[i];
            const QgsPointXY& p2 = part[i + 1];
            const double dx = p2.x() - p1.x(), dy = p2.y() - p1.y();
            const double len2 = dx * dx + dy * dy;
            if (len2 <= 0.0) continue;
            const double len = std::sqrt(len2);
            for (int j = i + 1; j < segCount; ++j) {
                if (skipPair(i, j)) continue;
                const QgsPointXY& q1 = part[j];
                const QgsPointXY& q2 = part[j + 1];
                const double c1 = std::fabs(dx * (q1.y() - p1.y())
                                          - dy * (q1.x() - p1.x())) / len;
                const double c2 = std::fabs(dx * (q2.y() - p1.y())
                                          - dy * (q2.x() - p1.x())) / len;
                if (c1 > 1e-6 || c2 > 1e-6) continue;   // 不共线
                const double t1 = ((q1.x() - p1.x()) * dx + (q1.y() - p1.y()) * dy) / len2;
                const double t2 = ((q2.x() - p1.x()) * dx + (q2.y() - p1.y()) * dy) / len2;
                const double lo = qMax(0.0, qMin(t1, t2));
                const double hi = qMin(1.0, qMax(t1, t2));
                if (hi - lo > 1e-9) {                   // 投影区间有交集=重叠
                    const double t = (lo + hi) / 2.0;
                    loc = QgsPointXY(p1.x() + dx * t, p1.y() + dy * t);
                    return true;
                }
            }
        }
        // 3) 顶点落在非相邻线段内部
        for (int j = 0; j < segCount; ++j) {
            const QgsPointXY& a = part[j];
            const QgsPointXY& b = part[j + 1];
            const double abx = b.x() - a.x(), aby = b.y() - a.y();
            const double ab2 = abx * abx + aby * aby;
            if (ab2 <= 0.0) continue;
            for (int k = 0; k < n; ++k) {
                if (k == j || k == j + 1) continue;     // 线段自身的端点
                const double t = ((part[k].x() - a.x()) * abx
                                + (part[k].y() - a.y()) * aby) / ab2;
                if (t <= 0.0 || t >= 1.0) continue;     // 只认落在线段内部
                const double px = a.x() + abx * t, py = a.y() + aby * t;
                if (std::hypot(part[k].x() - px, part[k].y() - py) <= 1e-6) {
                    loc = part[k];
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- 本地悬挂节点判定（2026-10-28 按甲方反馈）----
// 一处线端点在本图层内没有其他端点与之重合（距离≤nodeTol）即为悬挂；
// 但若该端点距任一居民点 ≤ residentTol，视为接到居民点，不算悬挂。
// 引擎的悬挂位（ProcessMode bit 1）不再执行，统一由本函数输出，
// 便于在结果中附准确端点坐标并做居民点豁免。
struct DanglingEndpoint {
    QgsPointXY pt;
    QgsFeatureId fid = -1;
};

void appendDanglingErrors(QgsVectorLayer* lineLayer, const QString& residentShp,
                          double nodeTol, double residentTol,
                          QList<QPair<QgsFeature, QString>>& allErrors,
                          QStringList& executedChecks)
{
    if (!lineLayer) return;

    QVector<DanglingEndpoint> eps;
    QgsSpatialIndex epIndex;
    qint64 gid = 1;   // 端点内部 id，从 1 开始，便于 id-1 直接索引 eps
    QgsFeatureIterator fit = lineLayer->getFeatures();
    QgsFeature f;
    while (fit.nextFeature(f)) {
        const QgsGeometry g = f.geometry();
        if (g.isNull() || g.type() != QgsWkbTypes::LineGeometry) continue;
        const QgsMultiPolylineXY parts = g.asMultiPolyline();
        for (const QgsPolylineXY& part : parts) {
            if (part.size() < 2) continue;
            for (int e = 0; e < 2; ++e) {
                DanglingEndpoint ep;
                ep.pt = (e == 0) ? part.first() : part.last();
                ep.fid = f.id();
                eps.append(ep);
                QgsFeature pf;
                pf.setId(gid++);
                pf.setGeometry(QgsGeometry::fromPointXY(ep.pt));
                epIndex.addFeature(pf);
            }
        }
    }

    // 端点重合判定：最近的两个端点中，除自身外有任意一个在容差内即已连接
    const auto connected = [&](int i) -> bool {
        const QList<QgsFeatureId> near = epIndex.nearestNeighbor(eps[i].pt, 2);
        for (QgsFeatureId id : near) {
            const int j = static_cast<int>(id - 1);
            if (j == i || j < 0 || j >= eps.size()) continue;
            if (eps[j].pt.distance(eps[i].pt) <= nodeTol + 1e-9)
                return true;
        }
        return false;
    };

    // 居民点索引（点图层；面取质心）
    QgsSpatialIndex resIndex;
    QHash<QgsFeatureId, QgsPointXY> resPts;
    if (!residentShp.isEmpty()) {
        QgsVectorLayer res(residentShp, QStringLiteral("m455_resident"),
                           QStringLiteral("ogr"));
        if (res.isValid()) {
            QgsFeature rf;
            QgsFeatureIterator rit = res.getFeatures();
            while (rit.nextFeature(rf)) {
                const QgsGeometry rg = rf.geometry();
                if (rg.isNull()) continue;
                const QgsPointXY rp = (rg.type() == QgsWkbTypes::PointGeometry)
                    ? rg.asPoint() : rg.centroid().asPoint();
                QgsFeature pf;
                pf.setId(rf.id());
                pf.setGeometry(QgsGeometry::fromPointXY(rp));
                resIndex.addFeature(pf);
                resPts.insert(rf.id(), rp);
            }
        }
    }
    const auto residentCovered = [&](const QgsPointXY& p) -> bool {
        if (resPts.isEmpty()) return false;
        const QList<QgsFeatureId> ids = resIndex.nearestNeighbor(p, 1);
        if (ids.isEmpty()) return false;
        const auto it = resPts.constFind(ids.first());
        return it != resPts.constEnd()
            && it.value().distance(p) <= residentTol + 1e-9;
    };

    int dangling = 0, covered = 0, reported = 0;
    const QString base = QFileInfo(lineLayer->source()).completeBaseName();
    for (int i = 0; i < eps.size(); ++i) {
        if (connected(i)) continue;
        ++dangling;
        if (residentCovered(eps[i].pt)) {
            ++covered;
            continue;
        }
        QgsFeature lf = lineLayer->getFeature(eps[i].fid);
        if (!lf.isValid()) continue;
        const QString msg = QStringLiteral("线拓扑检查(%1)：悬挂节点（定位点 %2,%3）")
            .arg(base)
            .arg(eps[i].pt.x(), 0, 'f', 3)
            .arg(eps[i].pt.y(), 0, 'f', 3);
        allErrors.append(qMakePair(lf, msg));
        ++reported;
    }
    executedChecks.append(QStringLiteral(
        "线拓扑检查(%1)：端点%2个，悬挂%3处，居民点压盖豁免%4处，报出%5处"
        "（节点容差%6米/居民点容差%7米）")
        .arg(base).arg(eps.size()).arg(dangling).arg(covered).arg(reported)
        .arg(nodeTol, 0, 'f', 3).arg(residentTol, 0, 'f', 1));
}


// ============================================================
//  本地回退实现（麒麟等无引擎环境）
//
//  检查逻辑正常由引擎子进程（MapBatchProcessing.exe）完成；该可执行文件
//  只存在于 Windows，麒麟上不存在，此时整体退回下面的本地 QGIS 算法，
//  保证 455 在无引擎环境下仍可执行（Windows 用引擎、麒麟用本地）。
//
//  模式位含义与引擎实现一致：1 悬挂节点 2 伪节点 4 自重叠 8 自相交
//  16 多部件 32 相互重叠 64 相互相交 4096 线在面内部。
//  模式 1 复用上面的 appendDanglingErrors，两条路径口径一致（含居民点豁免）。
// ============================================================

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

// ====== 模式2: 不能有伪节点 ======
void checkPseudos(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol)
{
    Q_UNUSED(tol);
    if (!layer) return;
    QHash<QString, QStringList> nodeLines;
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
        }
    }
    // 伪节点：恰好连接两条线的节点（非端点）
    QSet<QgsFeatureId> pseudoIds;
    for (auto it2 = nodeLines.begin(); it2 != nodeLines.end(); ++it2) {
        QStringList lines = it2.value();
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

// ====== 模式4: 不能自重叠 ======
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

// ====== 模式8: 不能自相交 ======
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

// ====== 模式16: 必须是单部件 ======
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

// ====== 模式32: 线不能相互重叠 ======
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

// ====== 模式64: 线不能相互相交 ======
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

// ====== 模式4096: 线必须在面内部 ======
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
void executeLocal455(QgsVectorLayer* lineLayer, const QString& refResidentShp,
    QgsVectorLayer* polyLayer, int processMode,
    const QHash<QString, double>& thresholds,
    QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!lineLayer) return;
    const bool all = (processMode == 0);
    auto en = [&](int m) { return all || (processMode & m); };
    const double tol = thresholds.value(QStringLiteral("FuzzyTolerance"), 0.001);
    const double residentTol =
        thresholds.value(QStringLiteral("ResidentCoverTolerance"), 10.0);

    auto run = [&](int mode, const QString& name, auto fn) {
        if (!en(mode)) return;
        QList<QPair<QgsFeature, QString>> errs;
        fn(errs);
        allErrors.append(errs);
        executedChecks.append(QString("%1(%2个错误)").arg(name).arg(errs.size()));
    };

    // 模式1：与引擎路径共用同一套本地判定（含居民点压盖豁免）
    if (en(1))
        appendDanglingErrors(lineLayer, refResidentShp, tol, residentTol,
                             allErrors, executedChecks);

    run(2,  QStringLiteral("伪节点"),     [&](auto& e){ checkPseudos(lineLayer, e, tol); });
    run(4,  QStringLiteral("线自重叠"),   [&](auto& e){ checkSelfOverlap(lineLayer, e); });
    run(8,  QStringLiteral("线自相交"),   [&](auto& e){ checkSelfIntersect(lineLayer, e); });
    run(16, QStringLiteral("多部件线"),   [&](auto& e){ checkSinglePart(lineLayer, e); });
    run(32, QStringLiteral("线相互重叠"), [&](auto& e){ checkOverlap(lineLayer, e); });
    run(64, QStringLiteral("线相互相交"), [&](auto& e){ checkIntersect(lineLayer, e); });
    if (polyLayer)
        run(4096, QStringLiteral("线在面内部"), [&](auto& e){ checkInsidePolygon(lineLayer, polyLayer, e); });
}

} // namespace

namespace Mission455 {

void execute(QgsVectorLayer* lineLayer, const QString& refPointShp,
             const QString& refPolyShp, const QString& refResidentShp,
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
        executeLocal455(lineLayer, refResidentShp, refPoly, processMode,
                        thresholds, allErrors, executedChecks);
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
    // 悬挂位（bit 1）由本地 appendDanglingErrors 处理，引擎只跑其余位：
    // 引擎不知道“居民点连接点不算悬挂”，且其 point_out 位置不可靠；
    // 屏蔽后既避免重复，也保证豁免规则生效。
    const int engineMode = processMode & ~1;
    const double nodeTol = thresholds.value(QStringLiteral("FuzzyTolerance"), 0.001);
    const double residentTol = thresholds.value(QStringLiteral("ResidentCoverTolerance"), 10.0);
    const QString outLineShp = QDir(dir).filePath(QStringLiteral("polyline_out.shp"));
    const QString outPointShp = QDir(dir).filePath(QStringLiteral("point_out.shp"));

    int errBefore = allErrors.size();
    if (engineMode == 0) {
        executedChecks.append(QStringLiteral(
            "线拓扑检查(%1)：仅勾选悬挂节点，引擎未执行其余拓扑位")
            .arg(QFileInfo(lineLayer->source()).completeBaseName()));
    }
    if (engineMode != 0) {
    const QString xml = WujiMissionXml::buildMission455(
        dir, lineShp, pointShp, polyShp, QString() /*refLineShp 未用*/,
        engineMode, nodeTol,
        thresholds.value(QStringLiteral("BufferDistance"), 0.0),
        outLineShp, outPointShp);

    if (WujiMissionRunner::runMissionXml(xml, dir, QStringLiteral("线拓扑检查"),
                                         engineMode, executedChecks)) {
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

        // ---- 自重叠/自相交结果复核（2026-10-27）----
        // 引擎对“闭合线首尾相接”“近邻折返/贴边（间距<0.35m）”也报自重叠/自相交，
        // 实测 6 条记录全部为误报；point_out 的点又是要素几何质心、不是错误位置。
        // 处理：用 GEOS isSimple() 复核线记录（简单线=误报，丢弃）；自重叠/自相交
        // 的点记录一律丢弃（线记录已带要素，且下面会补准确位置）；真自相交用
        // 本地线段扫描定位，消息里附“（定位点 x,y）”供 *_pt.shp 使用。
        QList<QPair<QgsFeature, QString>> kept;
        int droppedSelf = 0;
        for (int i = errBefore; i < allErrors.size(); ++i) {
            const QgsFeature& f = allErrors.at(i).first;
            const QString msg = allErrors.at(i).second;
            const bool selfMsg =
                msg.contains(QStringLiteral("self-overlap"), Qt::CaseInsensitive)
                || msg.contains(QStringLiteral("self-intersect"), Qt::CaseInsensitive);
            if (!selfMsg) {
                kept.append(allErrors.at(i));
                continue;
            }
            // 点记录：位置是质心，丢弃（对应的线记录会保留并给出定位点）
            if (f.geometry().type() != QgsWkbTypes::LineGeometry) {
                ++droppedSelf;
                continue;
            }
            // 线记录：简单线没有自重叠/自相交，判为引擎误报
            if (f.geometry().isNull() || f.geometry().isSimple()) {
                ++droppedSelf;
                continue;
            }
            const bool overlap =
                msg.contains(QStringLiteral("self-overlap"), Qt::CaseInsensitive);
            const QString what = overlap ? QStringLiteral("线自重叠")
                                         : QStringLiteral("线自相交");
            QgsPointXY loc;
            QString detail;
            if (firstLineSelfIntersection(f.geometry(), loc)) {
                detail = QStringLiteral("（定位点 %1,%2）")
                    .arg(loc.x(), 0, 'f', 3).arg(loc.y(), 0, 'f', 3);
            }
            kept.append(qMakePair(f, QStringLiteral("线拓扑检查(%1)：%2%3")
                .arg(baseName, what, detail)));
        }
        allErrors.erase(allErrors.begin() + errBefore, allErrors.end());
        allErrors += kept;
        if (droppedSelf > 0) {
            executedChecks.append(QStringLiteral(
                "线拓扑检查(%1)：复核剔除自重叠/自相交重复或误报记录%2处（GEOS isSimple 判定）")
                .arg(baseName).arg(droppedSelf));
        }
    }
    }   // engineMode != 0

    // ---- 本地悬挂节点（含居民点豁免）----
    if (processMode & 1)
        appendDanglingErrors(lineLayer, refResidentShp, nodeTol, residentTol,
                             allErrors, executedChecks);

    int added = allErrors.size() - errBefore;
    executedChecks.append(QStringLiteral("线拓扑检查(%1)：%2")
        .arg(QFileInfo(lineLayer->source()).completeBaseName())
        .arg(added == 0 ? QStringLiteral("未检出异常") : QStringLiteral("检出异常%1处").arg(added)));
}

} // namespace Mission455
