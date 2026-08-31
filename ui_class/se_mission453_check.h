#ifndef SE_MISSION453_CHECK_H
#define SE_MISSION453_CHECK_H
#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>
#include "map_check_common.h"

class QgsVectorLayer;
class QgsFeature;

// 字段定义结构体（复用 se_auto_quality_check.h 格式）
struct FieldDefinition {
    QString fieldName;
    int fieldIdx = -1;
    QString fieldType;
    QString fieldDesc;
    QString fieldRequired;
    QString fieldLength;
    QString fieldUnit;
    QString allowNull;
    QString defaultValue;
    QStringList enumValues;
    QString enumMultiply;
    QString splitList;
};

namespace Mission453 {
    // 从XML加载FeatureSchema
    QList<FieldDefinition> loadFieldDefs(const QString& xmlPath);

    // 各模式检查
    void checkFieldName(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkDataType(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkLength(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkPrecision(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkIgnoreField(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkPrimaryKey(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkUnique(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkNotNull(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkConstraintType(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);
    void checkConstraintSet(QgsVectorLayer* layer, const QList<FieldDefinition>& defs,
        QList<QPair<QgsFeature, QString>>& errors);

    void execute(QgsVectorLayer* layer, const QList<FieldDefinition>& fieldDefs,
        int processMode, QList<QPair<QgsFeature, QString>>& allErrors,
        QStringList& executedChecks);
}
#endif
