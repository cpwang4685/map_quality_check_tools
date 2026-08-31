#ifndef SE_MISSION457_CHECK_H
#define SE_MISSION457_CHECK_H
#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>
#include <QSet>

class QgsVectorLayer;
class QgsFeature;

namespace Mission457 {

// 关联图层组：共享同一 EntityID 空间的多个图层
struct AssociationGroup {
    QString groupName;           // 组名（如 "HYDA_HPSK"）
    QList<QgsVectorLayer*> layers;  // 该组内的所有图层
    QList<QString> layerNames;      // 图层文件名（用于报告）
    QgsVectorLayer* baseLayer = nullptr; // 基础图层（面积/线图层，不含_名称_点后缀）
};

// 从数据目录自动发现关联图层组
// 规则：同名前缀的 SHP（如 HYDA_HPSK, HYDA_HPSK_名称, HYDA_HPSK_点）归为一组
QList<AssociationGroup> discoverAssociationGroups(const QString& dataDir);

// 在图层列表中查找 EntityID / LocationID / ClassID 字段（自动探测）
QString findEntityIdField(const QgsVectorLayer* layer);
QString findLocationIdField(const QgsVectorLayer* layer);
QString findClassIdField(const QgsVectorLayer* layer);
QString findClassNameField(const QgsVectorLayer* layer);

// ====== 自动探测字段名 ======
QString resolveField(const QgsVectorLayer* layer,
    QString (*autoDetect)(const QgsVectorLayer*));

// ====== 各模式检查 ======

// 模式1: 外键引用检查 — 关联图层的EntityID必须在基础图层中存在
void checkForeignKeyReference(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed);

// 模式2: 关联记录存在性检查 — 基础图层每条记录在关联图层中应有对应
void checkRecordExistence(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed);

// 模式4: 关联字段一致性检查 — 同一EntityID的ClassID/ClassName必须一致
void checkFieldConsistency(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed);

// 模式8: 关联表结构检查 — 关联图层必须包含关键字段
void checkTableStructure(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed);

// 模式16: 关联数据完整性检查 — 统计记录数、孤立记录、缺失依赖
void checkDataCompleteness(const AssociationGroup& group,
    QList<QPair<QgsFeature, QString>>& errors, QStringList& executed);

// ====== 主入口 ======
void execute(const QString& dataDir,
    int processMode,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks);

} // namespace Mission457

#endif // SE_MISSION457_CHECK_H
