#include "se_mission457_check.h"
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsfields.h"
#include "qgsgeometry.h"
#include "qgswkbtypes.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QMap>

using namespace Mission457;

// ====== 查找关键字段名 ======
// 在图层字段中智能匹配 EntityID / LocationID / ClassID 字段
QString Mission457::findEntityIdField(const QgsVectorLayer* layer)
{
    if (!layer) return QString();
    const QgsFields& fields = layer->fields();
    // 优先级：ELEMID(实体编码) > FEATID(要素编码) > EntityID > FeatureID
    QStringList candidates = {"ELEMID", "FEATID", "FeatureID", "EntityID", "ENTITYID", "entityid", "EntityId", "Id", "ENTITY_ID"};
    for (const auto& c : candidates) {
        if (fields.indexOf(c) >= 0) return c;
    }
    // 模糊匹配
    for (int i = 0; i < fields.count(); i++) {
        QString fn = fields.at(i).name();
        if (fn.compare("ELEMID", Qt::CaseInsensitive) == 0) return fn;
        if (fn.compare("FEATID", Qt::CaseInsensitive) == 0) return fn;
        if (fn.contains("ELEM", Qt::CaseInsensitive) && fn.contains("ID", Qt::CaseInsensitive)) return fn;
        if (fn.contains("Entity", Qt::CaseInsensitive) && fn.contains("ID", Qt::CaseInsensitive))
            return fn;
    }
    return QString();
}

QString Mission457::findLocationIdField(const QgsVectorLayer* layer)
{
    if (!layer) return QString();
    const QgsFields& fields = layer->fields();
    QStringList candidates = {"LocationID", "LOCATIONID", "locationid", "LOC_ID", "LocID"};
    for (const auto& c : candidates) {
        if (fields.indexOf(c) >= 0) return c;
    }
    return QString();
}

QString Mission457::findClassIdField(const QgsVectorLayer* layer)
{
    if (!layer) return QString();
    const QgsFields& fields = layer->fields();
    QStringList candidates = {"CLASID", "CLASS", "ClassID", "CLASSID", "classid", "CLAS_ID", "CLASS_ID"};
    for (const auto& c : candidates) {
        if (fields.indexOf(c) >= 0) return c;
    }
    return QString();
}

QString Mission457::findClassNameField(const QgsVectorLayer* layer)
{
    if (!layer) return QString();
    const QgsFields& fields = layer->fields();
    QStringList candidates = {"CLASS", "NAME", "NAMES", "ClassName", "CLASSNAME", "classname", "CLAS_NAME", "NAME1"};
    for (const auto& c : candidates) {
        if (fields.indexOf(c) >= 0) return c;
    }
    return QString();
}

// ====== 自动探测字段名 ======
QString Mission457::resolveField(const QgsVectorLayer* layer,
    QString (*autoDetect)(const QgsVectorLayer*))
{
    if (!layer) return QString();
    // 自动探测
    if (autoDetect)
        return autoDetect(layer);
    return QString();
}

// ====== 自动发现关联图层组 ======
QList<AssociationGroup> Mission457::discoverAssociationGroups(const QString& dataDir)
{
    QList<AssociationGroup> groups;
    QDir dir(dataDir);
    QStringList shpFiles = dir.entryList({"*.shp"}, QDir::Files, QDir::Name);
    if (shpFiles.isEmpty()) return groups;

    // 收集所有图层的基础名（去掉 _名称、_点、_方向点、_跳绘、_线 等后缀和变体）
    struct LayerInfo {
        QString fileName;    // 完整文件名（不含路径）
        QString baseName;    // 分组基础名
        bool isBase;         // 是否为基础图层
    };
    QList<LayerInfo> infos;

    for (const auto& shp : shpFiles) {
        LayerInfo info;
        info.fileName = shp;
        QString stem = shp;
        stem.remove(".shp");
        info.baseName = stem;
        info.isBase = true;

        // 检测常见衍生后缀
        QStringList suffixes = {"_名称", "_点", "_方向点", "_跳绘", "_线", "_注记",
                                "_Pt", "_PT", "_pt", "_Anno", "_ANNO", "_anno",
                                "_Label", "_LABEL", "_label"};
        for (const auto& sfx : suffixes) {
            if (stem.endsWith(sfx)) {
                info.baseName = stem.left(stem.length() - sfx.length());
                info.isBase = false;
                break;
            }
        }

        infos.append(info);
    }

    // 按 baseName 分组（只有>=2个图层的组才需要关联检查）
    QMap<QString, QList<LayerInfo>> grouped;
    for (const auto& info : infos) {
        grouped[info.baseName].append(info);
    }

    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (it.value().size() < 2) continue; // 单个图层无需关联检查

        AssociationGroup ag;
        ag.groupName = it.key();

        // 打开图层，基础图层优先
        for (const auto& info : it.value()) {
            QString path = dataDir + "/" + info.fileName;
            QgsVectorLayer* lyr = new QgsVectorLayer(path, info.fileName, "ogr");
            if (!lyr || !lyr->isValid()) {
                delete lyr;
                continue;
            }
            ag.layers.append(lyr);
            ag.layerNames.append(info.fileName);

            // 基础图层：不含衍生后缀的第一个图层
            if (info.isBase && !ag.baseLayer)
                ag.baseLayer = lyr;
        }

        // 如果没有明确的基础图层，选面图层或第一个
        if (!ag.baseLayer && !ag.layers.isEmpty()) {
            for (auto* lyr : ag.layers) {
                if (lyr->geometryType() == QgsWkbTypes::PolygonGeometry) {
                    ag.baseLayer = lyr;
                    break;
                }
            }
            if (!ag.baseLayer)
                ag.baseLayer = ag.layers.first();
        }

        groups.append(ag);
    }

    return groups;
}

// ====== 辅助：构建 EntityID → Features 索引 ======
static QMap<QString, QList<QgsFeature>> buildEntityIndex(QgsVectorLayer* layer, const QString& eidField)
{
    QMap<QString, QList<QgsFeature>> idx;
    if (!layer || eidField.isEmpty()) return idx;

    int eidIdx = layer->fields().indexOf(eidField);
    if (eidIdx < 0) return idx;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QString eid = feat.attribute(eidIdx).toString().trimmed();
        if (!eid.isEmpty())
            idx[eid].append(feat);
    }
    return idx;
}

// ====== 模式1: 外键引用检查 ======
void Mission457::checkForeignKeyReference(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed)
{
    if (group.layers.size() < 2) {
        executed.append("外键引用检查(跳过：关联图层不足2个)");
        return;
    }

    QString eidField = resolveField(group.baseLayer, findEntityIdField);
    QString locField = resolveField(group.baseLayer, findLocationIdField);
    if (eidField.isEmpty()) eidField = locField;
    if (eidField.isEmpty()) {
        executed.append("外键引用检查(跳过：未找到实体标识字段(如ELEMID))");
        return;
    }

    // 构建基础图层的 EntityID 集合
    QSet<QString> baseIds;
    {
        int eidIdx = group.baseLayer->fields().indexOf(eidField);
        QgsFeatureIterator it = group.baseLayer->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            QString eid = feat.attribute(eidIdx).toString().trimmed();
            if (!eid.isEmpty()) baseIds.insert(eid);
        }
    }

    int totalErrs = 0;
    // 对每个非基础图层检查其EntityID是否在基础图层中存在
    for (int i = 0; i < group.layers.size(); i++) {
        QgsVectorLayer* lyr = group.layers[i];
        if (lyr == group.baseLayer) continue;

        QString lyrEidField = resolveField(lyr, findEntityIdField);
        if (lyrEidField.isEmpty()) lyrEidField = resolveField(lyr, findLocationIdField);
        if (lyrEidField.isEmpty()) continue;

        int eidIdx = lyr->fields().indexOf(lyrEidField);
        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            QString eid = feat.attribute(eidIdx).toString().trimmed();
            if (eid.isEmpty()) continue;
            if (!baseIds.contains(eid)) {
                errors.append(qMakePair(feat,
                    QString("[%1] 外键'%2'='%3'在基础图层'%4'中不存在")
                        .arg(group.layerNames[i], lyrEidField, eid,
                             group.layerNames[group.layers.indexOf(group.baseLayer)])));
                totalErrs++;
            }
        }
    }

    executed.append(QString("外键引用检查(%1个错误)").arg(totalErrs));
}

// ====== 模式2: 关联记录存在性检查 ======
void Mission457::checkRecordExistence(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed)
{
    if (group.layers.size() < 2) {
        executed.append("关联记录存在性检查(跳过：关联图层不足2个)");
        return;
    }

    QString eidField = resolveField(group.baseLayer, findEntityIdField);
    QString locField = resolveField(group.baseLayer, findLocationIdField);
    if (eidField.isEmpty()) eidField = locField;
    if (eidField.isEmpty()) {
        executed.append("关联记录存在性检查(跳过：未找到实体标识字段)");
        return;
    }

    int eidIdx = group.baseLayer->fields().indexOf(eidField);

    // 对非基础图层构建 EntityID 集合
    QSet<QString> derivedIds;
    for (auto* lyr : group.layers) {
        if (lyr == group.baseLayer) continue;
        QString df = resolveField(lyr, findEntityIdField);
        if (df.isEmpty()) df = resolveField(lyr, findLocationIdField);
        if (df.isEmpty()) continue;
        int didx = lyr->fields().indexOf(df);
        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            QString eid = feat.attribute(didx).toString().trimmed();
            if (!eid.isEmpty()) derivedIds.insert(eid);
        }
    }

    int totalErrs = 0;
    QgsFeatureIterator it = group.baseLayer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QString eid = feat.attribute(eidIdx).toString().trimmed();
        if (eid.isEmpty()) continue;
        if (!derivedIds.contains(eid)) {
            errors.append(qMakePair(feat,
                QString("[%1] 基础记录实体编码='%2'在关联图层中缺少对应记录")
                    .arg(group.layerNames[group.layers.indexOf(group.baseLayer)], eid)));
            totalErrs++;
        }
    }

    executed.append(QString("关联记录存在性检查(%1个错误)").arg(totalErrs));
}

// ====== 模式4: 关联字段一致性检查 ======
void Mission457::checkFieldConsistency(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed)
{
    if (group.layers.size() < 2) {
        executed.append("关联字段一致性检查(跳过：关联图层不足2个)");
        return;
    }

    QString eidField = resolveField(group.baseLayer, findEntityIdField);
    QString locField = resolveField(group.baseLayer, findLocationIdField);
    if (eidField.isEmpty()) eidField = locField;
    if (eidField.isEmpty()) {
        executed.append("关联字段一致性检查(跳过：未找到实体标识字段)");
        return;
    }

    // 收集所有图层中同时存在的字段（排除几何字段和ID字段）
    QStringList commonFields;
    QStringList excludeFields = {"ELEMID", "FEATID", "EntityID", "ENTITYID", "LocationID", "LOCATIONID",
                                  "EntityName", "ENTITYNAME", "BSM", "FormerID",
                                  "Shape_Area", "Shape_Leng", "Shape_Area_1", "Shape_Leng_1"};
    QString cidField = resolveField(group.baseLayer, findClassIdField);
    QString cnameField = resolveField(group.baseLayer, findClassNameField);

    QStringList checkFields;
    if (!cidField.isEmpty()) checkFields.append(cidField);
    if (!cnameField.isEmpty()) checkFields.append(cnameField);
    if (checkFields.isEmpty()) {
        executed.append("关联字段一致性检查(跳过：未找到ClassID/ClassName等可比较字段)");
        return;
    }

    int totalErrs = 0;
    // 为每个检查字段建立 图层→(EntityID→值) 映射
    for (const auto& cf : checkFields) {
        QMap<QString, QMap<QString, QString>> layerValues; // layerName -> {eid -> value}

        for (int i = 0; i < group.layers.size(); i++) {
            QgsVectorLayer* lyr = group.layers[i];
            QString leid = resolveField(lyr, findEntityIdField);
            if (leid.isEmpty()) leid = resolveField(lyr, findLocationIdField);
            if (leid.isEmpty()) continue;

            int eidIdx = lyr->fields().indexOf(leid);
            int cfIdx = lyr->fields().indexOf(cf);
            if (cfIdx < 0) continue;

            QMap<QString, QString>& vals = layerValues[group.layerNames[i]];
            QgsFeatureIterator it = lyr->getFeatures();
            QgsFeature feat;
            while (it.nextFeature(feat)) {
                QString eid = feat.attribute(eidIdx).toString().trimmed();
                QString val = feat.attribute(cfIdx).toString().trimmed();
                if (!eid.isEmpty())
                    vals[eid] = val;
            }
        }

        if (layerValues.size() < 2) continue;

        // 选取第一个图层作为参考
        QStringList keys = layerValues.keys();
        const QMap<QString, QString>& refVals = layerValues[keys[0]];

        for (int i = 1; i < keys.size(); i++) {
            const QMap<QString, QString>& cmpVals = layerValues[keys[i]];
            for (auto it = cmpVals.begin(); it != cmpVals.end(); ++it) {
                const QString& eid = it.key();
                const QString& cmpVal = it.value();
                if (refVals.contains(eid) && refVals[eid] != cmpVal) {
                    // 找一个示例feature报错
                    QgsFeature feat;
                    int eidIdx2 = group.layers[i]->fields().indexOf(
                        resolveField(group.layers[i], findEntityIdField));
                    QgsFeatureIterator fit = group.layers[i]->getFeatures();
                    while (fit.nextFeature(feat)) {
                        if (feat.attribute(eidIdx2).toString().trimmed() == eid) break;
                    }
                    errors.append(qMakePair(feat,
                        QString("[%1] 实体编码='%2'字段'%3'不一致：'%4'(参考=%5) vs '%6'(当前=%7)")
                            .arg(group.groupName, eid, cf, refVals[eid],
                                 keys[0], cmpVal, keys[i])));
                    totalErrs++;
                }
            }
        }
    }

    executed.append(QString("关联字段一致性检查(%1个错误)").arg(totalErrs));
}

// ====== 模式8: 关联表结构检查 ======
void Mission457::checkTableStructure(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed)
{
    // 必需字段：关联图层必须具备实体标识字段
    QStringList requiredFields = {"ELEMID", "FEATID", "FeatureID", "EntityID", "LocationID"};
    QStringList recommendedFields = {"CLASID", "CLASS", "NAME", "ClassID", "ClassName", "EntityName"};

    int totalErrs = 0;
    for (int i = 0; i < group.layers.size(); i++) {
        QgsVectorLayer* lyr = group.layers[i];
        const QgsFields& fields = lyr->fields();
        QStringList fieldNames;
        for (int f = 0; f < fields.count(); f++)
            fieldNames.append(fields.at(f).name());

        // 检查必需字段
        bool hasEid = false;
        for (const auto& rf : requiredFields) {
            if (fieldNames.contains(rf, Qt::CaseInsensitive)) {
                hasEid = true;
                break;
            }
        }

        if (!hasEid) {
            // 用图层第一个feature报结构错误
            QgsFeature feat;
            QgsFeatureIterator it = lyr->getFeatures();
            if (it.nextFeature(feat)) {
                errors.append(qMakePair(feat,
                    QString("[%1] 缺少关联关键字段：需包含实体标识字段如ELEMID/FEATID(现有字段：%2)")
                        .arg(group.layerNames[i], fieldNames.join(","))));
                totalErrs++;
            }
        }

        // 检查推荐字段
        QStringList missingRec;
        for (const auto& rf : recommendedFields) {
            if (!fieldNames.contains(rf, Qt::CaseInsensitive))
                missingRec.append(rf);
        }
        if (!missingRec.isEmpty() && hasEid) {
            QgsFeature feat;
            QgsFeatureIterator it = lyr->getFeatures();
            if (it.nextFeature(feat)) {
                errors.append(qMakePair(feat,
                    QString("[%1] 建议添加关联字段：%2(便于关联完整性校验)")
                        .arg(group.layerNames[i], missingRec.join(","))));
                // 这是建议，不计入错误数
            }
        }
    }

    executed.append(QString("关联表结构检查(%1个错误)").arg(totalErrs));
}

// ====== 模式16: 关联数据完整性检查 ======
void Mission457::checkDataCompleteness(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed)
{
    if (group.layers.size() < 2) {
        executed.append("关联数据完整性检查(跳过：关联图层不足2个)");
        return;
    }

    QString eidField = resolveField(group.baseLayer, findEntityIdField);
    QString locField = resolveField(group.baseLayer, findLocationIdField);
    if (eidField.isEmpty()) eidField = locField;
    if (eidField.isEmpty()) {
        executed.append("关联数据完整性检查(跳过：未找到实体标识字段)");
        return;
    }

    // 统计各图层的记录数和唯一EntityID数
    struct Stats { int totalRecs; int uniqueEids; int emptyEids; };
    QMap<QString, Stats> layerStats;
    QSet<QString> allEids; // 所有图层EntityID的并集

    for (int i = 0; i < group.layers.size(); i++) {
        QgsVectorLayer* lyr = group.layers[i];
        QString leid = resolveField(lyr, findEntityIdField);
        if (leid.isEmpty()) leid = resolveField(lyr, findLocationIdField);

        Stats s = {0, 0, 0};
        QSet<QString> uniqueSet;
        int eidIdx = leid.isEmpty() ? -1 : lyr->fields().indexOf(leid);

        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            s.totalRecs++;
            if (eidIdx >= 0) {
                QString eid = feat.attribute(eidIdx).toString().trimmed();
                if (eid.isEmpty())
                    s.emptyEids++;
                else {
                    uniqueSet.insert(eid);
                    allEids.insert(eid);
                }
            }
        }
        s.uniqueEids = uniqueSet.size();
        layerStats[group.layerNames[i]] = s;
    }

    // 报告统计
    QStringList report;
    for (auto it = layerStats.begin(); it != layerStats.end(); ++it) {
        report.append(QString("%1: %2条/ %3个唯一ID/ %4个空ID")
            .arg(it.key()).arg(it.value().totalRecs)
            .arg(it.value().uniqueEids).arg(it.value().emptyEids));
    }
    executed.append(QString("关联数据完整性检查(统计：%1)").arg(report.join("; ")));

    // 记录空EntityID为Warning（不计入error，仅统计）
    int emptyWarn = 0;
    for (int i = 0; i < group.layers.size(); i++) {
        QgsVectorLayer* lyr = group.layers[i];
        QString leid = resolveField(lyr, findEntityIdField);
        if (leid.isEmpty()) leid = resolveField(lyr, findLocationIdField);
        if (leid.isEmpty()) continue;
        int eidIdx = lyr->fields().indexOf(leid);

        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            QString eid = feat.attribute(eidIdx).toString().trimmed();
            if (eid.isEmpty()) {
                errors.append(qMakePair(feat,
                    QString("[%1] 实体编码为空，无法进行关联校验")
                        .arg(group.layerNames[i])));
                emptyWarn++;
            }
        }
    }
    if (emptyWarn > 0)
        executed.last().append(QString(" +%1条空EntityID记录").arg(emptyWarn));
}


// ====== 主入口（无映射表） ======
void Mission457::execute(const QString& dataDir,
    int processMode,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks)
{
    // 发现关联图层组
    QList<AssociationGroup> groups = discoverAssociationGroups(dataDir);

    if (groups.isEmpty()) {
        executedChecks.append("关联表检查(跳过：数据目录中未发现关联图层组。需要至少2个共享EntityID前缀的图层)");
        return;
    }

    // 列出发现的关联组
    QStringList groupNames;
    for (const auto& g : groups)
        groupNames.append(QString("%1(%2个图层)").arg(g.groupName).arg(g.layers.size()));
    executedChecks.append(QString("发现 %1 个关联图层组：%2").arg(groups.size()).arg(groupNames.join(", ")));

    bool all = (processMode == 0);
    auto en = [&](int m) { return all || (processMode & m); };

    // 对每个关联组执行检查（无映射表，传nullptr）
    for (const auto& group : groups) {
        auto run = [&](int mode, const QString& name, auto fn) {
            if (!en(mode)) return;
            QList<QPair<QgsFeature, QString>> errs;
            QStringList exec;
            fn(group, errs, exec);
            allErrors.append(errs);
            executedChecks.append(QString("[%1] %2").arg(group.groupName, exec.join("; ")));
        };

        run(1,  "外键引用检查",       checkForeignKeyReference);
        run(2,  "关联记录存在性检查",  checkRecordExistence);
        run(4,  "关联字段一致性检查",  checkFieldConsistency);
        run(8,  "关联表结构检查",      checkTableStructure);
        run(16, "关联数据完整性检查",  checkDataCompleteness);
    }

    // 清理图层（AssociationGroup 持有图层所有权，需在返回前释放）
    for (auto& group : groups) {
        for (auto* lyr : group.layers)
            delete lyr;
        group.layers.clear();
    }
}
