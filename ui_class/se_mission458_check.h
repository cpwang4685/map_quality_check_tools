#ifndef SE_MISSION458_CHECK_H
#define SE_MISSION458_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>

class QgsVectorLayer;
class QgsFeature;

// ---- Mission 458: 综合前后缓冲匹配检查 ----
// QGIS 本地实现：按图层映射/同名配对原始与成果图层，成果要素按缓冲距离
// 与原始要素求交（重叠面积/长度比），无原始来源或显著变形的记为异常。
// 需要 原始数据(综合前) 与 成果数据(综合后) 两个数据目录。
namespace Mission458 {

// origDir: 原始数据目录（综合前）
// resultDir: 成果数据目录（综合后）
// layerMapping: 成果图层名→源图层代码，用于原始数据图层配对（无映射时按同名规则回退）
// matchParamPath: matchParameter.xml 参数文件路径，读取失败时使用内置默认参数并记录提示
void execute(const QString& origDir, const QString& resultDir,
    int processMode,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks,
    const QHash<QString, QString>* layerMapping = nullptr,
    const QString& matchParamPath = QString());

void executeWithMapping(const QString& origDir, const QString& resultDir,
    int processMode,
    const QHash<QString, QString>& layerMapping,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks,
    const QString& matchParamPath = QString());

} // namespace Mission458

#endif // SE_MISSION458_CHECK_H
