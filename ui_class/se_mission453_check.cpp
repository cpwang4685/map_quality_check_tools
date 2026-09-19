#include "se_mission453_check.h"

#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsfield.h"

#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

namespace {

struct StdField {
    QString name;
    QString dataType;      // DataType_String / DataType_Int32 / DataType_Double
    int length = 0;
    bool lengthValid = false;
    int precision = 0;
    bool precisionValid = false;
    // 约束类标签（原样保存；标准XML中为占位空标签时全部为空，不触发对应检查）
    QString ignore;          // 忽略标记：标记为忽略的字段不参与结构比对
    QString primaryKey;      // 主键
    QString unique;          // 唯一约束
    QString notNull;         // 非空约束
    QString constraintType;  // 约束类型（枚举/范围）
    QString constraintSet;   // 约束集（枚举值列表，或范围的 min,max）
};

// 约束标签是否启用：非空且不为明确的"否"取值
bool tagEnabled(const QString& v)
{
    const QString t = v.trimmed().toLower();
    if (t.isEmpty()) return false;
    return t != QLatin1String("0") && t != QLatin1String("false")
        && t != QLatin1String("no") && t != QString::fromUtf8("否");
}

// 解析标准 FeatureSchema XML（<Field><Name/><DataType/><Length/><Precision/>…）
// 字段顺序即标准顺序；解析失败返回 false。
bool parseSchemaXml(const QString& xmlPath, QVector<StdField>& fields, QString* errOut)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QStringLiteral("无法打开标准XML");
        return false;
    }
    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        if (errOut) *errOut = QStringLiteral("标准XML解析失败");
        return false;
    }
    file.close();

    const QDomNodeList fieldNodes = doc.elementsByTagName(QStringLiteral("Field"));
    for (int i = 0; i < fieldNodes.count(); ++i) {
        const QDomElement fe = fieldNodes.at(i).toElement();
        StdField f;
        f.name = fe.firstChildElement(QStringLiteral("Name")).text().trimmed();
        if (f.name.isEmpty())
            continue;
        f.dataType = fe.firstChildElement(QStringLiteral("DataType")).text().trimmed();
        const QString lenText =
            fe.firstChildElement(QStringLiteral("Length")).text().trimmed();
        if (!lenText.isEmpty()) {
            f.length = lenText.toInt();
            f.lengthValid = true;
        }
        const QString precText =
            fe.firstChildElement(QStringLiteral("Precision")).text().trimmed();
        if (!precText.isEmpty()) {
            f.precision = precText.toInt();
            f.precisionValid = true;
        }
        f.ignore         = fe.firstChildElement(QStringLiteral("Ignore")).text().trimmed();
        f.primaryKey     = fe.firstChildElement(QStringLiteral("PrimaryKey")).text().trimmed();
        f.unique         = fe.firstChildElement(QStringLiteral("Unique")).text().trimmed();
        f.notNull        = fe.firstChildElement(QStringLiteral("NotNULL")).text().trimmed();
        f.constraintType = fe.firstChildElement(QStringLiteral("ConstraintType")).text().trimmed();
        f.constraintSet  = fe.firstChildElement(QStringLiteral("ConstraintSet")).text().trimmed();
        fields.append(f);
    }
    return !fields.isEmpty();
}

// QGIS 字段类型 → XML DataType 记号；对应不上返回空串（该字段跳过类型检查）
QString xmlTypeOf(const QgsField& f)
{
    switch (f.type()) {
    case QVariant::String:   return QStringLiteral("DataType_String");
    case QVariant::Int:
    case QVariant::LongLong: return QStringLiteral("DataType_Int32");
    case QVariant::Double:   return QStringLiteral("DataType_Double");
    default:                 return QString();
    }
}

} // namespace

namespace Mission453 {

// ====== 主入口：纯 QGIS 本地实现 ======
// 2026-08-23 原 Nmo DataAttributeCheck 进程内路径在成果数据上全部失败
// （先"无法打开图层"，导出中转修复后卡"无法创建结果存储"，CreateLayer 65 图层
// 全失败），且崩溃风险高。改成本地解析标准 FeatureSchema XML 逐字段比对。
// 模式口径（processMode 位掩码）：
//   1 字段名称（缺标准字段/多非标准字段）、2 数据类型、4 字段长度超限、
//   8 数值精度不符、16 忽略字段（信息性）、32 主键、128 唯一约束、
//   256 非空约束、512 约束类型（约束定义合法性）、1024 约束集（值级符合性）。
// 结果仅写日志（表结构级问题，非空间错误）。
//
// 本次移植 7-27 集成版的值级检查（字段定义来源改为标准 FeatureSchema XML，
// 不再依赖已注释掉的 attribute_check_config.xml）：
//   - 值类型检查（不依赖标准XML，所有图层都跑）：数值字段的值能否解析为声明类型；
//   - 标识字段检查（不依赖标准XML）：ELEMID/EntityID/FormerID 重复与空值统计；
//   - 值级长度/精度（需标准XML，模式4/8）：记录值长度、小数位是否超标准。
// 值级问题按"字段+条数+示例"汇总为一条日志，避免逐要素刷屏。
void execute(QgsVectorLayer* layer, const QString& featureSchemaXmlPath,
             int processMode, QList<QPair<QgsFeature, QString>>& allErrors,
             QStringList& executedChecks)
{
    if (!layer) {
        executedChecks.append(QStringLiteral("属性检查：无图层数据，本次不涉及"));
        return;
    }

    const QString base = QFileInfo(layer->source()).completeBaseName();
    const QgsFields& layerFields = layer->fields();
    QStringList problems;

    // ---- 值级检查：数值字段的值与声明类型是否对得上（不依赖标准XML）----
    for (int i = 0; i < layerFields.count(); i++) {
        const QVariant::Type t = layerFields.at(i).type();
        if (t != QVariant::Int && t != QVariant::LongLong && t != QVariant::Double)
            continue;
        int bad = 0;
        QStringList examples;
        QgsFeatureIterator it = layer->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QVariant v = f.attribute(i);
            if (v.isNull() || v.toString().trimmed().isEmpty())
                continue;
            if (v.type() != QVariant::String)
                continue; // 数值类型存的就是数值，正常
            // 数值字段里存了字符串：验证能否解析回声明类型
            bool ok = false;
            if (t == QVariant::Double)
                v.toString().toDouble(&ok);
            else
                v.toString().toLongLong(&ok);
            if (!ok) {
                bad++;
                if (examples.size() < 3)
                    examples.append(v.toString().trimmed());
            }
        }
        if (bad > 0)
            problems.append(QStringLiteral("字段%1共%2条记录的值与类型不符(示例:%3)")
                .arg(layerFields.at(i).name()).arg(bad)
                .arg(examples.join(QStringLiteral(","))));
    }

    // ---- 值级检查：标识字段重复/空值（不依赖标准XML）----
    QString idField;
    for (int i = 0; i < layerFields.count(); i++) {
        const QString fn = layerFields.at(i).name();
        if (fn.compare(QStringLiteral("ELEMID"), Qt::CaseInsensitive) == 0
            || fn.compare(QStringLiteral("EntityID"), Qt::CaseInsensitive) == 0
            || fn.compare(QStringLiteral("FormerID"), Qt::CaseInsensitive) == 0) {
            idField = fn;
            break;
        }
    }
    if (!idField.isEmpty()) {
        const int idx = layerFields.indexOf(idField);
        QSet<QString> seen;
        int dup = 0, empty = 0;
        QStringList dupExamples;
        QgsFeatureIterator it = layer->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QString v = f.attribute(idx).toString().trimmed();
            if (v.isEmpty()) {
                empty++;
                continue;
            }
            if (seen.contains(v)) {
                dup++;
                if (dupExamples.size() < 3)
                    dupExamples.append(v);
            } else {
                seen.insert(v);
            }
        }
        if (dup > 0)
            problems.append(QStringLiteral("标识字段%1重复%2条(示例:%3)")
                .arg(idField).arg(dup).arg(dupExamples.join(QStringLiteral(","))));
        if (empty > 0)
            problems.append(QStringLiteral("标识字段%1有%2条记录为空").arg(idField).arg(empty));
    }

    // ---- 结构级检查 + 值级长度/精度（需要标准 FeatureSchema XML）----
    QVector<StdField> stdFields;
    bool haveSchema = !featureSchemaXmlPath.isEmpty()
                      && QFileInfo::exists(featureSchemaXmlPath);
    QString parseErr;
    if (haveSchema) {
        haveSchema = parseSchemaXml(featureSchemaXmlPath, stdFields, &parseErr);
        if (!haveSchema)
            problems.append(QStringLiteral("标准XML无效：%1").arg(parseErr));
    }

    if (haveSchema) {
        QHash<QString, StdField> stdByName;
        QSet<QString> stdNames;
        for (const StdField& f : stdFields) {
            stdByName.insert(f.name, f);
            stdNames.insert(f.name);
        }
        QSet<QString> layerNames;
        for (const QgsField& f : layerFields)
            layerNames.insert(f.name());

        // 模式16：忽略字段检查（信息性：列出标准中标记忽略、不参与比对的字段）
        if (processMode & 16) {
            for (const StdField& f : stdFields)
                if (tagEnabled(f.ignore))
                    problems.append(QStringLiteral("忽略字段%1（标准标记为忽略，不参与结构比对）")
                        .arg(f.name));
        }

        // 模式1：字段名称（缺标准字段 / 多非标准字段；标记忽略的标准字段不判缺失）
        if (processMode & 1) {
            for (const StdField& f : stdFields)
                if (!tagEnabled(f.ignore) && !layerNames.contains(f.name))
                    problems.append(QStringLiteral("缺少标准字段%1").arg(f.name));
            for (const QgsField& f : layerFields)
                if (!stdNames.contains(f.name()))
                    problems.append(QStringLiteral("多出非标准字段%1").arg(f.name()));
        }

        // 模式2/4/8：按共同字段逐字段比对（表结构级 + 值级；忽略字段跳过）
        for (const QgsField& lf : layerFields) {
            if (!stdByName.contains(lf.name()))
                continue;
            const StdField& sf = stdByName.value(lf.name());
            if (tagEnabled(sf.ignore))
                continue;
            if ((processMode & 2) && !sf.dataType.isEmpty()) {
                const QString actual = xmlTypeOf(lf);
                if (!actual.isEmpty() && actual != sf.dataType)
                    problems.append(QStringLiteral("字段%1类型%2与标准%3不符")
                        .arg(lf.name(), actual, sf.dataType));
            }
            if ((processMode & 4) && sf.lengthValid && lf.length() > sf.length)
                problems.append(QStringLiteral("字段%1长度%2超过标准%3")
                    .arg(lf.name()).arg(lf.length()).arg(sf.length));
            if ((processMode & 8) && sf.precisionValid
                && lf.type() == QVariant::Double && lf.precision() != sf.precision)
                problems.append(QStringLiteral("字段%1精度%2与标准%3不符")
                    .arg(lf.name()).arg(lf.precision()).arg(sf.precision));

            // 值级长度/精度：记录值是否超标准（按字段汇总条数）
            if (!((processMode & 4) && sf.lengthValid)
                && !((processMode & 8) && sf.precisionValid && lf.type() == QVariant::Double))
                continue;
            int badLen = 0, badPrec = 0;
            QString exLen, exPrec;
            QgsFeatureIterator it = layer->getFeatures();
            QgsFeature f;
            while (it.nextFeature(f)) {
                const QVariant v = f.attribute(layerFields.indexOf(lf.name()));
                if (v.isNull() || v.toString().trimmed().isEmpty())
                    continue;
                const QString sv = v.toString().trimmed();
                if ((processMode & 4) && sf.lengthValid && sv.length() > sf.length) {
                    badLen++;
                    if (exLen.isEmpty())
                        exLen = sv.left(20);
                }
                if ((processMode & 8) && sf.precisionValid && lf.type() == QVariant::Double
                    && !sv.contains(QLatin1Char('e')) && !sv.contains(QLatin1Char('E'))) {
                    const int pt = sv.indexOf(QLatin1Char('.'));
                    if (pt >= 0 && sv.length() - pt - 1 > sf.precision) {
                        badPrec++;
                        if (exPrec.isEmpty())
                            exPrec = sv.left(20);
                    }
                }
            }
            if (badLen > 0)
                problems.append(QStringLiteral("字段%1共%2条记录值长度超标准%3(示例:%4)")
                    .arg(lf.name()).arg(badLen).arg(sf.length)
                    .arg(exLen.isEmpty() ? QStringLiteral("-") : exLen));
            if (badPrec > 0)
                problems.append(QStringLiteral("字段%1共%2条记录小数位超标准%3位(示例:%4)")
                    .arg(lf.name()).arg(badPrec).arg(sf.precision)
                    .arg(exPrec.isEmpty() ? QStringLiteral("-") : exPrec));
        }

        // ---- 约束类检查（模式32/128/256/512/1024；标准XML对应标签为空时不触发）----

        // 模式32：主键检查（主键字段必须存在，值必须非空且唯一）
        if (processMode & 32) {
            for (const StdField& f : stdFields) {
                if (!tagEnabled(f.primaryKey)) continue;
                const int idx = layerFields.indexOf(f.name);
                if (idx < 0) {
                    problems.append(QStringLiteral("主键字段%1在数据中不存在").arg(f.name));
                    continue;
                }
                QSet<QString> seen;
                int empty = 0, dup = 0;
                QStringList dupEx;
                QgsFeatureIterator it = layer->getFeatures();
                QgsFeature feat;
                while (it.nextFeature(feat)) {
                    const QString v = feat.attribute(idx).toString().trimmed();
                    if (v.isEmpty()) { empty++; continue; }
                    if (seen.contains(v)) {
                        dup++;
                        if (dupEx.size() < 3) dupEx.append(v);
                    } else {
                        seen.insert(v);
                    }
                }
                if (empty > 0)
                    problems.append(QStringLiteral("主键字段%1有%2条记录为空").arg(f.name).arg(empty));
                if (dup > 0)
                    problems.append(QStringLiteral("主键字段%1重复%2条(示例:%3)")
                        .arg(f.name).arg(dup).arg(dupEx.join(QStringLiteral(","))));
            }
        }

        // 模式128：唯一约束检查（非空值必须唯一）
        if (processMode & 128) {
            for (const StdField& f : stdFields) {
                if (!tagEnabled(f.unique)) continue;
                const int idx = layerFields.indexOf(f.name);
                if (idx < 0) {
                    problems.append(QStringLiteral("唯一约束字段%1在数据中不存在").arg(f.name));
                    continue;
                }
                QSet<QString> seen;
                int dup = 0;
                QStringList dupEx;
                QgsFeatureIterator it = layer->getFeatures();
                QgsFeature feat;
                while (it.nextFeature(feat)) {
                    const QString v = feat.attribute(idx).toString().trimmed();
                    if (v.isEmpty()) continue;
                    if (seen.contains(v)) {
                        dup++;
                        if (dupEx.size() < 3) dupEx.append(v);
                    } else {
                        seen.insert(v);
                    }
                }
                if (dup > 0)
                    problems.append(QStringLiteral("唯一约束字段%1重复%2条(示例:%3)")
                        .arg(f.name).arg(dup).arg(dupEx.join(QStringLiteral(","))));
            }
        }

        // 模式256：非空约束检查（非空字段不得有空值）
        if (processMode & 256) {
            for (const StdField& f : stdFields) {
                if (!tagEnabled(f.notNull)) continue;
                const int idx = layerFields.indexOf(f.name);
                if (idx < 0) {
                    problems.append(QStringLiteral("非空约束字段%1在数据中不存在").arg(f.name));
                    continue;
                }
                int empty = 0;
                QgsFeatureIterator it = layer->getFeatures();
                QgsFeature feat;
                while (it.nextFeature(feat)) {
                    const QVariant v = feat.attribute(idx);
                    if (v.isNull() || v.toString().trimmed().isEmpty())
                        empty++;
                }
                if (empty > 0)
                    problems.append(QStringLiteral("非空约束字段%1有%2条记录为空").arg(f.name).arg(empty));
            }
        }

        // 模式512：约束类型检查（约束定义合法性：类型取值合法、约束集可解析）
        if (processMode & 512) {
            const QRegularExpression rangeSep(QStringLiteral("[~,，;；]"));
            for (const StdField& f : stdFields) {
                if (f.constraintType.trimmed().isEmpty()) continue;
                const QString ct = f.constraintType.trimmed().toLower();
                const QString cs = f.constraintSet.trimmed();
                if (cs.isEmpty()) {
                    problems.append(QStringLiteral("字段%1约束类型%2未提供约束集")
                        .arg(f.name, f.constraintType));
                    continue;
                }
                if (ct == QString::fromUtf8("枚举") || ct == QLatin1String("enum")
                    || ct == QString::fromUtf8("列表") || ct == QLatin1String("list")) {
                    // 枚举：约束集为值列表，非空即合法
                } else if (ct == QString::fromUtf8("范围") || ct == QLatin1String("range")) {
                    // 【2026-09-15】刻意用 QString::SkipEmptyParts 而不是 Qt::SkipEmptyParts：
                    // 后者是 Qt 5.14 才加进 Qt 命名空间的，麒麟（Qt 5.12.12）编不过；前者
                    // 5.12/5.15 都可用，也是本工程既有写法（product_dao.cpp 等处共 11 处）。
                    // 本文件下面另两处同理。
                    const QStringList parts = cs.split(rangeSep, QString::SkipEmptyParts);
                    bool ok1 = false, ok2 = false;
                    if (parts.size() == 2) {
                        parts[0].trimmed().toDouble(&ok1);
                        parts[1].trimmed().toDouble(&ok2);
                    }
                    if (!ok1 || !ok2)
                        problems.append(QStringLiteral("字段%1约束类型%2的约束集%3无法解析为范围")
                            .arg(f.name, f.constraintType, cs));
                } else {
                    problems.append(QStringLiteral("字段%1约束类型%2未知(支持枚举/范围)")
                        .arg(f.name, f.constraintType));
                }
            }
        }

        // 模式1024：约束集检查（值级符合性：枚举值落在集合内/范围值落在[min,max]内）
        if (processMode & 1024) {
            const QRegularExpression enumSep(QStringLiteral("[|,，;；]"));
            const QRegularExpression rangeSep2(QStringLiteral("[~,，;；]"));
            for (const StdField& f : stdFields) {
                const QString ct = f.constraintType.trimmed().toLower();
                const QString cs = f.constraintSet.trimmed();
                if (ct.isEmpty() || cs.isEmpty()) continue;
                const int idx = layerFields.indexOf(f.name);
                if (idx < 0) continue;
                if (ct == QString::fromUtf8("枚举") || ct == QLatin1String("enum")
                    || ct == QString::fromUtf8("列表") || ct == QLatin1String("list")) {
                    QSet<QString> allowed;
                    const QStringList vals = cs.split(enumSep, QString::SkipEmptyParts);
                    for (const QString& v : vals) allowed.insert(v.trimmed());
                    int bad = 0;
                    QStringList ex;
                    QgsFeatureIterator it = layer->getFeatures();
                    QgsFeature feat;
                    while (it.nextFeature(feat)) {
                        const QVariant v = feat.attribute(idx);
                        if (v.isNull() || v.toString().trimmed().isEmpty()) continue;
                        if (!allowed.contains(v.toString().trimmed())) {
                            bad++;
                            if (ex.size() < 3) ex.append(v.toString().trimmed());
                        }
                    }
                    if (bad > 0)
                        problems.append(QStringLiteral("字段%1共%2条记录的值不在约束集内(示例:%3)")
                            .arg(f.name).arg(bad).arg(ex.join(QStringLiteral(","))));
                } else if (ct == QString::fromUtf8("范围") || ct == QLatin1String("range")) {
                    const QStringList parts = cs.split(rangeSep2, QString::SkipEmptyParts);
                    bool ok1 = false, ok2 = false;
                    double lo = 0.0, hi = 0.0;
                    if (parts.size() == 2) {
                        lo = parts[0].trimmed().toDouble(&ok1);
                        hi = parts[1].trimmed().toDouble(&ok2);
                    }
                    if (!ok1 || !ok2) continue;
                    int bad = 0;
                    QStringList ex;
                    QgsFeatureIterator it = layer->getFeatures();
                    QgsFeature feat;
                    while (it.nextFeature(feat)) {
                        const QVariant v = feat.attribute(idx);
                        if (v.isNull() || v.toString().trimmed().isEmpty()) continue;
                        bool ok = false;
                        const double dv = v.toString().trimmed().toDouble(&ok);
                        if (ok && (dv < lo || dv > hi)) {
                            bad++;
                            if (ex.size() < 3) ex.append(v.toString().trimmed());
                        }
                    }
                    if (bad > 0)
                        problems.append(QStringLiteral("字段%1共%2条记录的值超出范围[%3,%4](示例:%5)")
                            .arg(f.name).arg(bad)
                            .arg(lo, 0, 'f', 3).arg(hi, 0, 'f', 3)
                            .arg(ex.join(QStringLiteral(","))));
                }
            }
        }
    }

    if (!problems.isEmpty()) {
        // 表结构级问题挂在图层第一个要素上（日志按错误信息分组展示，与要素无关）
        QgsFeature carrier;
        QgsFeatureIterator it = layer->getFeatures();
        it.nextFeature(carrier);
        if (!carrier.isValid())
            carrier = QgsFeature(0); // 空图层：挂在 FID 0，仅用于日志记录
        for (const QString& p : problems)
            allErrors.append(qMakePair(carrier, QStringLiteral("属性检查(%1)：%2").arg(base, p)));
    }
    executedChecks.append(QStringLiteral("属性检查(%1)%2：%3")
        .arg(base)
        .arg(haveSchema ? QString() : QStringLiteral("(缺少标准XML，仅值级检查)"))
        .arg(problems.isEmpty() ? QStringLiteral("未检出异常")
                                : QStringLiteral("检出异常%1处").arg(problems.size())));
}

} // namespace Mission453
