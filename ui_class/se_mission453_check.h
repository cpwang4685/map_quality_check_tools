#ifndef SE_MISSION453_CHECK_H
#define SE_MISSION453_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>

class QgsVectorLayer;
class QgsFeature;

// ---- Mission 453: 属性表检查 ----
// 纯 QGIS 本地实现（引擎 Nmo DataAttributeCheck 在成果数据上失败后改用本地比对）。
// 标准字段定义来自随包内置的 FeatureSchemaXML（config/FeatureSchema/<图层名>.xml，
// 由标准数据的DBF字段结构生成，格式为 MapGeneBatchProcessing 任务XML，含 FieldName/Field 字段块）。
// 模式位掩码：1字段名称 2数据类型 4字段长度 8精度 16忽略字段 32主键
// 128唯一约束 256非空约束 512约束类型 1024约束集；
// 值级检查（数值字段值解析、标识字段重复/空值）不依赖标准XML，所有图层均执行。
namespace Mission453 {
    void execute(QgsVectorLayer* layer, const QString& featureSchemaXmlPath,
        int processMode, QList<QPair<QgsFeature, QString>>& allErrors,
        QStringList& executedChecks);
}
#endif
