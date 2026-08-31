#ifndef SE_MISSION458_CHECK_H
#define SE_MISSION458_CHECK_H
#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>

class QgsVectorLayer;
class QgsFeature;

namespace Mission458 {

// 综合前后匹配结果
struct MatchResult {
    QString entityId;
    bool matched;         // 是否匹配成功
    bool isNew;           // 是否为新增要素（仅在成果数据中存在）
    bool isRemoved;       // 是否为消失要素（仅在原始数据中存在）
    double areaChange;    // 面积变化率（面要素）
    double lengthChange;  // 长度变化率（线要素）
    int vertexChange;     // 顶点数变化
    QStringList attrChanges; // 属性变化描述
};

// ====== 执行综合前后匹配 ======
// origDir: 原始数据目录（综合前）
// resultDir: 成果数据目录（综合后）
// layerMapping: 标准图层名→GDB图层代码映射（nullptr=仅按文件名匹配）
void execute(const QString& origDir, const QString& resultDir,
    int processMode,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks,
    const QHash<QString, QString>* layerMapping = nullptr);

// 带图层映射的入口（用户配置了图层映射表时使用）
void executeWithMapping(const QString& origDir, const QString& resultDir,
    int processMode,
    const QHash<QString, QString>& layerMapping,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks);

// ====== 辅助函数 ======

// 匹配两个图层中的要素（通过EntityID/FormerID）
QList<MatchResult> matchLayers(QgsVectorLayer* origLayer, QgsVectorLayer* resultLayer,
    const QString& eidField, const QString& formerIdField);

// 比较两个要素的几何变化
void compareGeometry(const QgsFeature& origFeat, const QgsFeature& resultFeat,
    double& areaChange, double& lengthChange, int& vertexChange);

// 比较两个要素的属性变化
QStringList compareAttributes(const QgsFeature& origFeat, const QgsFeature& resultFeat,
    const QStringList& ignoreFields);

} // namespace Mission458

#endif // SE_MISSION458_CHECK_H
