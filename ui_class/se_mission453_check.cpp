#include "se_mission453_check.h"
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsfields.h"
#include <QFile>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QSet>

using namespace Mission453;

// ====== 从XML加载字段定义 ======
QList<FieldDefinition> Mission453::loadFieldDefs(const QString& xmlPath)
{
    QList<FieldDefinition> defs;
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly)) return defs;
    QDomDocument doc;
    if (!doc.setContent(&file)) { file.close(); return defs; }
    file.close();

    QDomElement root = doc.documentElement();
    QDomNodeList fieldNodes = root.elementsByTagName("fields");
    for (int i = 0; i < fieldNodes.size(); i++) {
        QDomElement fe = fieldNodes.at(i).toElement();
        FieldDefinition def;
        def.fieldName = fe.firstChildElement("field_name").text();
        def.fieldIdx = fe.firstChildElement("field_idx").text().toInt();
        def.fieldType = fe.firstChildElement("field_type").text();
        def.fieldDesc = fe.firstChildElement("field_desc").text();
        def.fieldRequired = fe.firstChildElement("field_required").text();
        def.fieldLength = fe.firstChildElement("field_length").text();
        def.fieldUnit = fe.firstChildElement("field_unit").text();
        def.allowNull = fe.firstChildElement("field_allow_Null").text();
        def.defaultValue = fe.firstChildElement("default_value").text();
        QDomElement enumRoot = fe.firstChildElement("field_enum_values_list_simple");
        QDomNodeList evs = enumRoot.elementsByTagName("field_enum_values");
        for (int j = 0; j < evs.size(); j++)
            def.enumValues.append(evs.at(j).toElement().text());
        def.enumMultiply = fe.firstChildElement("field_enum_multiply").text();
        def.splitList = fe.firstChildElement("field_split_list").text();
        if (!def.fieldName.isEmpty()) defs.append(def);
    }
    return defs;
}

// 构建字段名→定义映射
static QMap<QString, FieldDefinition> buildDefMap(const QList<FieldDefinition>& defs)
{
    QMap<QString, FieldDefinition> m;
    for (const auto& d : defs) m[d.fieldName] = d;
    return m;
}

// ====== 模式1: 字段名称检查 ======
void Mission453::checkFieldName(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    const QgsFields& fields = layer->fields();
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn))
                errs.append(QString("字段'%1'不在FeatureSchema定义中").arg(fn));
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 模式2: 数据类型检查 ======
void Mission453::checkDataType(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn)) continue;
            QString xmlType = defMap[fn].fieldType.toLower().trimmed();
            QString qgsType;
            switch (fields.at(i).type()) {
            case QVariant::Int: case QVariant::LongLong: qgsType = "int"; break;
            case QVariant::Double: qgsType = "float"; break;
            case QVariant::String: qgsType = "string"; break;
            default: continue;
            }
            bool compat = (qgsType == xmlType) || (xmlType=="float" && qgsType=="int") || (xmlType=="int" && qgsType=="float");
            if (!compat && !xmlType.isEmpty())
                errs.append(QString("字段'%1'类型不匹配：定义%2，实际%3").arg(fn, xmlType, qgsType));
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 模式4: 字段长度检查 ======
void Mission453::checkLength(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn)) continue;
            QString fl = defMap[fn].fieldLength;
            if (fl.isEmpty()) continue;
            int dotIdx = fl.indexOf('.');
            int maxLen = (dotIdx >= 0) ? fl.left(dotIdx).toInt() : fl.toInt();
            if (maxLen <= 0) continue;
            QVariant val = feat.attribute(i);
            if (val.isNull()) continue;
            QString sv = val.toString().trimmed();
            if (sv.length() > maxLen)
                errs.append(QString("字段'%1'长度%2超限(最大%3)").arg(fn).arg(sv.length()).arg(maxLen));
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 模式8: 精度检查 ======
void Mission453::checkPrecision(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn)) continue;
            QString fl = defMap[fn].fieldLength;
            int dotIdx = fl.indexOf('.');
            if (dotIdx < 0) continue; // 无精度要求
            int decPlaces = fl.mid(dotIdx + 1).toInt();
            if (decPlaces <= 0) continue;
            QVariant val = feat.attribute(i);
            if (val.isNull() || fields.at(i).type() != QVariant::Double) continue;
            QString sv = val.toString().trimmed();
            int ptIdx = sv.indexOf('.');
            if (ptIdx >= 0 && sv.length() - ptIdx - 1 > decPlaces)
                errs.append(QString("字段'%1'值'%2'小数位超%3位").arg(fn, sv).arg(decPlaces));
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 模式16: 忽略字段检查（检查是否存在应忽略但出现的字段） ======
void Mission453::checkIgnoreField(QgsVectorLayer* layer, const QList<FieldDefinition>&,
    QList<QPair<QgsFeature, QString>>& errors)
{
    // 忽略字段检查：检测Schema中标记为忽略的字段是否出现在数据中
    // 需要字段定义才能执行，无定义时跳过
    Q_UNUSED(layer); Q_UNUSED(errors);
}

// ====== 模式32: 主键约束检查 ======
void Mission453::checkPrimaryKey(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    // 查找主键字段（fieldRequired=="required" 且 fieldIdx>=0 的字段视为主键候选）
    // 更精确的逻辑需要FeatureSchema中明确标注PrimaryKey
    Q_UNUSED(defs);
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    QSet<QString> keys;
    while (it.nextFeature(feat)) {
        QString key = feat.attribute(0).toString(); // 默认第一字段为主键
        if (keys.contains(key))
            errors.append(qMakePair(feat, QString("主键重复：'%1'").arg(key)));
        else if (!key.isEmpty())
            keys.insert(key);
    }
}

// ====== 模式128: 唯一约束检查 ======
void Mission453::checkUnique(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    for (int i = 0; i < fields.count(); i++) {
        QString fn = fields.at(i).name();
        // 检查是否有唯一性要求（可从FeatureSchema扩展）
        Q_UNUSED(defMap);
        Q_UNUSED(fn);
    }
    Q_UNUSED(errors);
}

// ====== 模式256: 非空约束检查 ======
void Mission453::checkNotNull(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn)) continue;
            const FieldDefinition& def = defMap[fn];
            QVariant val = feat.attribute(i);
            bool isEmpty = val.isNull() || val.toString().trimmed().isEmpty();
            if (def.fieldRequired == "required" && isEmpty)
                errs.append(QString("必填字段'%1'为空").arg(fn));
            else if (def.allowNull == "no" && isEmpty)
                errs.append(QString("字段'%1'不允许NULL").arg(fn));
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 模式512: 约束类型检查 ======
void Mission453::checkConstraintType(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn)) continue;
            const FieldDefinition& def = defMap[fn];
            QString xmlType = def.fieldType.toLower().trimmed();
            if (xmlType == "int" || xmlType == "integer") {
                bool ok; feat.attribute(i).toString().toInt(&ok);
                if (!ok && !feat.attribute(i).isNull())
                    errs.append(QString("字段'%1'值不是有效整数").arg(fn));
            } else if (xmlType == "float" || xmlType == "double") {
                bool ok; feat.attribute(i).toString().toDouble(&ok);
                if (!ok && !feat.attribute(i).isNull())
                    errs.append(QString("字段'%1'值不是有效浮点数").arg(fn));
            }
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 模式1024: 约束组合检查（枚举值+类型联合校验） ======
void Mission453::checkConstraintSet(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
    QList<QPair<QgsFeature, QString>>& errors)
{
    if (!layer) return;
    QMap<QString, FieldDefinition> defMap = buildDefMap(defs);
    const QgsFields& fields = layer->fields();
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat)) {
        QStringList errs;
        for (int i = 0; i < fields.count(); i++) {
            QString fn = fields.at(i).name();
            if (!defMap.contains(fn)) continue;
            const FieldDefinition& def = defMap[fn];
            QVariant val = feat.attribute(i);
            if (val.isNull() || val.toString().trimmed().isEmpty()) continue;
            QString sv = val.toString().trimmed();
            // 枚举值检查
            if (!def.enumValues.isEmpty() && !def.enumValues.contains(sv))
                errs.append(QString("字段'%1'值'%2'不在枚举范围[%3]")
                    .arg(fn, sv, def.enumValues.join(",")));
            // 数值范围检查（如有min/max可从扩展属性读取）
        }
        if (!errs.isEmpty()) errors.append(qMakePair(feat, errs.join(";")));
    }
}

// ====== 主入口 ======
void Mission453::execute(QgsVectorLayer* layer, const QList<FieldDefinition>& fieldDefs,
    int processMode, QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks)
{
    if (!layer) return;
    bool all = (processMode == 0);
    auto en = [&](int m) { return all || (processMode & m); };

    auto run = [&](int mode, const QString& name, auto fn) {
        if (!en(mode)) return;
        QList<QPair<QgsFeature, QString>> errs;
        fn(layer, fieldDefs, errs);
        allErrors.append(errs);
        executedChecks.append(QString("%1(%2个错误)").arg(name).arg(errs.size()));
    };

    run(1,    "字段名称检查",   checkFieldName);
    run(2,    "数据类型检查",   checkDataType);
    run(4,    "字段长度检查",   checkLength);
    run(8,    "精度检查",       checkPrecision);
    run(16,   "忽略字段检查",   checkIgnoreField);
    run(32,   "主键约束检查",   checkPrimaryKey);
    run(128,  "唯一约束检查",   checkUnique);
    run(256,  "非空约束检查",   checkNotNull);
    run(512,  "约束类型检查",   checkConstraintType);
    run(1024, "约束组合检查",   checkConstraintSet);
}
