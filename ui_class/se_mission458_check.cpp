#include "se_mission458_check.h"
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsfields.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgslinestring.h"
#include "qgspolygon.h"
#include "qgscurve.h"
#include "qgsgeometrycollection.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QMap>
#include <cmath>
#include <functional>

using namespace Mission458;

// ====== 比较几何变化 ======
void Mission458::compareGeometry(const QgsFeature& origFeat, const QgsFeature& resultFeat,
    double& areaChange, double& lengthChange, int& vertexChange)
{
    areaChange = 0.0;
    lengthChange = 0.0;
    vertexChange = 0;

    QgsGeometry origGeom = origFeat.geometry();
    QgsGeometry resultGeom = resultFeat.geometry();
    if (origGeom.isNull() || resultGeom.isNull()) return;

    // 面积变化（面要素）
    double origArea = origGeom.area();
    double resultArea = resultGeom.area();
    if (origArea > 0.0 && resultArea > 0.0) {
        areaChange = (resultArea - origArea) / origArea;
    }

    // 长度变化（线要素）
    double origLen = origGeom.length();
    double resultLen = resultGeom.length();
    if (origLen > 0.0 && resultLen > 0.0) {
        lengthChange = (resultLen - origLen) / origLen;
    }

    // 顶点数变化
    int origVerts = 0, resultVerts = 0;
    std::function<int(const QgsAbstractGeometry*)> countVerts;
    countVerts = [&](const QgsAbstractGeometry* g) -> int {
        if (!g) return 0;
        if (g->dimension() == 0) return 1;
        const QgsCurve* curve = dynamic_cast<const QgsCurve*>(g);
        if (curve) return curve->numPoints();
        const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g);
        if (gc) {
            int n = 0;
            for (int i = 0; i < gc->numGeometries(); i++)
                n += countVerts(gc->geometryN(i));
            return n;
        }
        return 0;
    };
    if (origGeom.constGet())
        origVerts = countVerts(origGeom.constGet());
    if (resultGeom.constGet())
        resultVerts = countVerts(resultGeom.constGet());
    if (origVerts > 0 && resultVerts > 0)
        vertexChange = resultVerts - origVerts;
}

// ====== 比较属性变化 ======
QStringList Mission458::compareAttributes(const QgsFeature& origFeat, const QgsFeature& resultFeat,
    const QStringList& ignoreFields)
{
    QStringList changes;
    const QgsFields& oFields = origFeat.fields();
    const QgsFields& rFields = resultFeat.fields();

    for (int i = 0; i < oFields.count(); i++) {
        QString fn = oFields.at(i).name();
        if (ignoreFields.contains(fn, Qt::CaseInsensitive)) continue;
        // 跳过FormerID/综合前ID字段（综合前后必然不同）
        if (fn.compare("FormerID", Qt::CaseInsensitive) == 0) continue;
        if (fn.compare("FORMER_ID", Qt::CaseInsensitive) == 0) continue;
        if (fn.compare("OldID", Qt::CaseInsensitive) == 0) continue;
        if (fn.compare("SrcID", Qt::CaseInsensitive) == 0) continue;

        int ri = rFields.indexOf(fn);
        if (ri < 0) continue; // 字段仅存在于原始数据

        QVariant oVal = origFeat.attribute(i);
        QVariant rVal = resultFeat.attribute(ri);

        if (oVal.isNull() && rVal.isNull()) continue;
        if (oVal.isNull() != rVal.isNull() || oVal.toString() != rVal.toString()) {
            changes.append(QString("%1: '%2' → '%3'")
                .arg(fn, oVal.isNull() ? "(null)" : oVal.toString(),
                     rVal.isNull() ? "(null)" : rVal.toString()));
        }
    }
    return changes;
}

// ====== 匹配两个图层 ======
QList<MatchResult> Mission458::matchLayers(QgsVectorLayer* origLayer, QgsVectorLayer* resultLayer,
    const QString& eidField, const QString& formerIdField)
{
    QList<MatchResult> results;
    if (!origLayer || !resultLayer) return results;

    // 构建原始数据EntityID索引
    QMap<QString, QgsFeature> origIndex;
    {
        int eidIdx = origLayer->fields().indexOf(eidField);
        if (eidIdx < 0) return results;

        QgsFeatureIterator it = origLayer->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            QString eid = feat.attribute(eidIdx).toString().trimmed();
            if (!eid.isEmpty())
                origIndex[eid] = feat;
        }
    }

    // 构建成果数据索引（通过EntityID 和 FormerID 两种方式）
    QMap<QString, QgsFeature> resultByEid, resultByFormerId;
    {
        int eidIdx = resultLayer->fields().indexOf(eidField);
        int fidIdx = formerIdField.isEmpty() ? -1 : resultLayer->fields().indexOf(formerIdField);

        QgsFeatureIterator it = resultLayer->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            if (eidIdx >= 0) {
                QString eid = feat.attribute(eidIdx).toString().trimmed();
                if (!eid.isEmpty()) resultByEid[eid] = feat;
            }
            if (fidIdx >= 0) {
                QString fid = feat.attribute(fidIdx).toString().trimmed();
                if (!fid.isEmpty()) resultByFormerId[fid] = feat;
            }
        }
    }

    QSet<QString> matchedResultIds;
    QStringList ignoreFields = {"Shape_Area", "Shape_Leng", "BSM", "LoadTime", "UpdateTime",
                                 "BornTime", "EndTime", "UpdateSts"};

    // 匹配原始数据 → 成果数据
    for (auto it = origIndex.begin(); it != origIndex.end(); ++it) {
        const QString& origEid = it.key();
        const QgsFeature& origFeat = it.value();

        MatchResult mr;
        mr.entityId = origEid;
        mr.isNew = false;
        mr.isRemoved = false;

        // 尝试通过EntityID匹配
        QgsFeature matchedFeat;
        bool found = false;

        if (resultByEid.contains(origEid)) {
            matchedFeat = resultByEid[origEid];
            found = true;
            matchedResultIds.insert(origEid);
        }
        // 尝试通过FormerID匹配（成果数据的FormerID指向原始数据的EntityID）
        else if (resultByFormerId.contains(origEid)) {
            matchedFeat = resultByFormerId[origEid];
            found = true;
            // 也记录成果数据的EntityID
            int eidIdx = resultLayer->fields().indexOf(eidField);
            if (eidIdx >= 0) {
                QString reid = matchedFeat.attribute(eidIdx).toString().trimmed();
                if (!reid.isEmpty()) matchedResultIds.insert(reid);
            }
        }

        if (found) {
            mr.matched = true;
            compareGeometry(origFeat, matchedFeat, mr.areaChange, mr.lengthChange, mr.vertexChange);
            mr.attrChanges = compareAttributes(origFeat, matchedFeat, ignoreFields);
        } else {
            mr.matched = false;
            mr.isRemoved = true; // 原始数据中的要素在成果数据中找不到了
        }

        results.append(mr);
    }

    // 查找新增要素（在成果数据中存在但原始数据中没有）
    for (auto it = resultByEid.begin(); it != resultByEid.end(); ++it) {
        if (matchedResultIds.contains(it.key())) continue;

        // 同时检查FormerID是否指向原始数据
        const QgsFeature& feat = it.value();
        int fidIdx = formerIdField.isEmpty() ? -1 : resultLayer->fields().indexOf(formerIdField);
        bool hasFormerMatch = false;
        if (fidIdx >= 0) {
            QString fid = feat.attribute(fidIdx).toString().trimmed();
            if (!fid.isEmpty() && origIndex.contains(fid))
                hasFormerMatch = true;
        }

        if (!origIndex.contains(it.key()) && !hasFormerMatch) {
            MatchResult mr;
            mr.entityId = it.key();
            mr.matched = false;
            mr.isNew = true;
            mr.isRemoved = false;
            results.append(mr);
        }
    }

    return results;
}

// ====== 带图层映射的入口 ======
void Mission458::executeWithMapping(const QString& origDir, const QString& resultDir,
    int processMode,
    const QHash<QString, QString>& layerMapping,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks)
{
    execute(origDir, resultDir, processMode, allErrors, executedChecks, &layerMapping);
    executedChecks.append("(使用图层映射)");
}

// ====== 主入口（可选图层映射） ======
void Mission458::execute(const QString& origDir, const QString& resultDir,
    int processMode,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks,
    const QHash<QString, QString>* layerMapping)
{
    Q_UNUSED(processMode) // 458目前只有模式1，全部执行

    // 自动探测字段
    auto resolveField = [&](const QgsVectorLayer* lyr, const QStringList& candidates,
                             bool fuzzyMatch = false) -> QString {
        if (!lyr) return QString();
        // 精确匹配候选名
        for (const auto& c : candidates) {
            if (lyr->fields().indexOf(c) >= 0) return c;
        }
        // 模糊匹配（实体标识字段）
        if (fuzzyMatch) {
            const QgsFields& fields = lyr->fields();
            for (int i = 0; i < fields.count(); i++) {
                QString fn = fields.at(i).name();
                if (fn.compare("ELEMID", Qt::CaseInsensitive) == 0) return fn;
                if (fn.compare("FEATID", Qt::CaseInsensitive) == 0) return fn;
                if (fn.compare("FeatureID", Qt::CaseInsensitive) == 0) return fn;
                if (fn.contains("ELEM", Qt::CaseInsensitive) && fn.contains("ID", Qt::CaseInsensitive)) return fn;
                if (fn.contains("Entity", Qt::CaseInsensitive) && fn.contains("ID", Qt::CaseInsensitive))
                    return fn;
            }
        }
        return QString();
    };

    if (origDir.isEmpty() || resultDir.isEmpty()) {
        executedChecks.append("综合前后匹配(跳过：需要原始数据目录和成果数据目录两个数据源)");
        return;
    }

    // 扫描两个目录的SHP文件
    QDir oDir(origDir), rDir(resultDir);
    QStringList oShps = oDir.entryList({"*.shp"}, QDir::Files, QDir::Name);
    QStringList rShps = rDir.entryList({"*.shp"}, QDir::Files, QDir::Name);

    if (oShps.isEmpty() || rShps.isEmpty()) {
        executedChecks.append(QString("综合前后匹配(跳过：原始数据%1个SHP，成果数据%2个SHP)")
            .arg(oShps.size()).arg(rShps.size()));
        return;
    }

    executedChecks.append(QString("原始数据: %1个图层, 成果数据: %2个图层")
        .arg(oShps.size()).arg(rShps.size()));

    // 构建查找索引
    QMap<QString, QString> oBaseMap; // basename→full path (orig: GDB代码命名如BOUP6, BOUA6等)
    for (const auto& s : oShps) {
        QString base = s;
        base.remove(".shp");
        oBaseMap[base] = origDir + "/" + s;
    }
    QMap<QString, QString> rBaseMap; // basename→full path (result: 中文标准名命名如市级驻地、飞地_标注等)
    for (const auto& s : rShps) {
        QString base = s;
        base.remove(".shp");
        rBaseMap[base] = resultDir + "/" + s;
    }

    // 构建GDB代码→原始SHP的索引（方便快速查找）
    QHash<QString, QString> gdbToOrigPath; // "BOUP6" → origDir/BOUP6.shp
    for (auto it = oBaseMap.begin(); it != oBaseMap.end(); ++it) {
        gdbToOrigPath[it.key()] = it.value();
    }

    int totalMatched = 0, totalRemoved = 0, totalNew = 0;
    int totalAttrChanges = 0, totalGeomChanges = 0;
    int comparedLayers = 0;

    if (layerMapping && !layerMapping->isEmpty())
        executedChecks.append(QString("已加载图层映射表(%1条对应关系)").arg(layerMapping->size()));

    // 遍历成果数据图层（中文标准名命名），通过映射表找到对应的原始数据图层
    for (auto rit = rBaseMap.begin(); rit != rBaseMap.end(); ++rit) {
        const QString& rBaseName = rit.key();  // 成果图层中文名（如"市级驻地"）
        const QString& rPath = rit.value();

        // 通过图层映射表查找对应的GDB代码
        QString gdbCode;
        if (layerMapping && layerMapping->contains(rBaseName)) {
            gdbCode = (*layerMapping)[rBaseName];
        } else {
            // 无映射条目时跳过（无法确定对应关系）
            continue;
        }

        // 在原始数据中查找GDB代码对应的SHP文件
        QString oPath;
        if (gdbToOrigPath.contains(gdbCode)) {
            oPath = gdbToOrigPath[gdbCode];
        } else {
            // 尝试匹配带后缀的（如BOUP6_A, BOUP6_L, BOUP6_P）
            QStringList geomSuffixes = {"", "_A", "_L", "_P", "_R"};
            for (const auto& sfx : geomSuffixes) {
                QString tryKey = gdbCode + sfx;
                if (gdbToOrigPath.contains(tryKey)) {
                    oPath = gdbToOrigPath[tryKey];
                    break;
                }
            }
            if (oPath.isEmpty()) {
                // 尝试oBaseMap中的模糊匹配
                for (auto oit = oBaseMap.begin(); oit != oBaseMap.end(); ++oit) {
                    if (oit.key().startsWith(gdbCode)) {
                        oPath = oit.value();
                        break;
                    }
                }
            }
        }

        if (oPath.isEmpty()) {
            executedChecks.append(QString("[%1] 跳过：未找到GDB代码'%2'对应的原始SHP")
                .arg(rBaseName, gdbCode));
            continue;
        }

        comparedLayers++;

        // 打开图层
        QgsVectorLayer* oLyr = new QgsVectorLayer(oPath, gdbCode, "ogr");
        QgsVectorLayer* rLyr = new QgsVectorLayer(rPath, rBaseName, "ogr");

        if (!oLyr || !oLyr->isValid() || !rLyr || !rLyr->isValid()) {
            delete oLyr; delete rLyr;
            continue;
        }

        // 查找实体编码字段（自动探测）
        QString eidField = resolveField(oLyr,
            {"ELEMID", "ELEM_ID", "FEATID", "FeatureID", "EntityID", "ENTITYID", "entityid", "Id"}, true);
        if (eidField.isEmpty())
            eidField = resolveField(rLyr,
                {"ELEMID", "ELEM_ID", "FEATID", "FeatureID", "EntityID", "ENTITYID", "entityid", "Id"}, true);
        // FormerID：成果数据中指向原始数据EntityID的字段
        QString formerIdField = resolveField(rLyr,
            {"FormerID", "FORMERID", "formerid", "FormerId", "FORMER_ID",
             "OldID", "OLDID", "PreID", "PREID", "SourceID", "SRCID"}, false);

        if (eidField.isEmpty()) {
            executedChecks.append(QString("[%1↔%2] 跳过：未找到实体标识字段(ELEMID)")
                .arg(rBaseName, gdbCode));
            delete oLyr; delete rLyr;
            continue;
        }

        // 执行匹配
        QList<MatchResult> matchResults = matchLayers(oLyr, rLyr, eidField, formerIdField);

        int layerMatched = 0, layerRemoved = 0, layerNew = 0;

        for (const auto& mr : matchResults) {
            if (mr.isRemoved) {
                totalRemoved++; layerRemoved++;
                QgsFeature feat;
                QgsFeatureIterator fit = oLyr->getFeatures();
                int eidIdx = oLyr->fields().indexOf(eidField);
                while (fit.nextFeature(feat)) {
                    if (feat.attribute(eidIdx).toString().trimmed() == mr.entityId) break;
                }
                allErrors.append(qMakePair(feat,
                    QString("[%1↔%2] 要素消失：实体编码='%3'在原始数据中存在但在成果数据中未找到")
                        .arg(rBaseName, gdbCode, mr.entityId)));
            } else if (mr.isNew) {
                totalNew++; layerNew++;
                QgsFeature feat;
                QgsFeatureIterator fit = rLyr->getFeatures();
                int eidIdx = rLyr->fields().indexOf(eidField);
                while (fit.nextFeature(feat)) {
                    if (feat.attribute(eidIdx).toString().trimmed() == mr.entityId) break;
                }
                allErrors.append(qMakePair(feat,
                    QString("[%1↔%2] 新增要素：实体编码='%3'在成果数据中存在但在原始数据中未找到")
                        .arg(rBaseName, gdbCode, mr.entityId)));
            } else if (mr.matched) {
                totalMatched++; layerMatched++;

                QStringList warnings;
                if (std::abs(mr.areaChange) > 0.3)
                    warnings.append(QString("面积变化%1%").arg(static_cast<int>(mr.areaChange * 100)));
                if (std::abs(mr.lengthChange) > 0.3)
                    warnings.append(QString("长度变化%1%").arg(static_cast<int>(mr.lengthChange * 100)));
                if (std::abs(mr.vertexChange) > 10)
                    warnings.append(QString("顶点数变化%1").arg(mr.vertexChange));

                if (!mr.attrChanges.isEmpty()) {
                    totalAttrChanges++;
                    if (mr.attrChanges.size() <= 3)
                        warnings.append(QString("属性变化: %1").arg(mr.attrChanges.join(", ")));
                    else
                        warnings.append(QString("属性变化: %1等%2项")
                            .arg(mr.attrChanges.mid(0,3).join(", ")).arg(mr.attrChanges.size()));
                }

                if (!warnings.isEmpty()) {
                    totalGeomChanges++;
                    QgsFeature feat;
                    QgsFeatureIterator fit = rLyr->getFeatures();
                    int eidIdx = rLyr->fields().indexOf(eidField);
                    while (fit.nextFeature(feat)) {
                        if (feat.attribute(eidIdx).toString().trimmed() == mr.entityId) break;
                    }
                    allErrors.append(qMakePair(feat,
                        QString("[%1↔%2] 实体编码='%3'综合前后变化: %4")
                            .arg(rBaseName, gdbCode, mr.entityId, warnings.join("; "))));
                }
            }
        }

        executedChecks.append(QString("[%1↔%2] 匹配%3/消失%4/新增%5/属性变化%6/几何变化%7 (字段:%8%9)")
            .arg(rBaseName, gdbCode)
            .arg(layerMatched).arg(layerRemoved).arg(layerNew)
            .arg(totalAttrChanges).arg(totalGeomChanges)
            .arg(eidField)
            .arg(formerIdField.isEmpty() ? "" : QString("/%1").arg(formerIdField)));

        delete oLyr; delete rLyr;
    }

    // 汇总
    executedChecks.append(QString("综合前后匹配汇总: %1个可比图层, %2条匹配, %3条消失, %4条新增, %5条属性变化, %6条几何变化")
        .arg(comparedLayers).arg(totalMatched).arg(totalRemoved).arg(totalNew)
        .arg(totalAttrChanges).arg(totalGeomChanges));

    if (comparedLayers == 0) {
        executedChecks.append("注意：成果数据图层与原始数据图层未能通过映射表匹配。请确认mission_config.xml中<LayerMapping>配置正确。");
    }
}
