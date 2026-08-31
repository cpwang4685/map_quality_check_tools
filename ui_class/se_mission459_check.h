#ifndef SE_MISSION459_CHECK_H
#define SE_MISSION459_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include "map_check_common.h"

class QgsVectorLayer;
class QgsFeature;
class map_check_task_manager;

// ---- Mission 459: 图形规范性检查 ----
// 10个检查项，每个对应 ProcessMode 的一个 bit

namespace Mission459 {

    // 模式1: 多部件检查
    void checkMultiPart(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double fuzzyTolerance = 0.001);

    // 模式2: 空图形检查
    void checkEmptyGeometry(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors);

    // 模式4: 尖锐角检查
    void checkAcuteAngle(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double acuteAngleThreshold = 10.0);

    // 模式8: 碎面检查（重叠+缝隙）
    void checkSliverPolygon(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double sliverArea = 0.0, double fuzzyTolerance = 0.001);

    // 模式16: 狭长碎面检查
    void checkNarrowPolygon(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double narrowWidth = 0.0);

    // 模式32: 小面积碎面检查
    void checkSmallArea(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double minArea = 0.0);

    // 模式64: 节点平均密度检查
    void checkAvgNodeDensity(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double upperBound = 0.0, double lowerBound = 0.0);

    // 模式128: 节点密度检查
    void checkNodeDensity(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double upperBound = 0.0, double lowerBound = 0.0);

    // 模式256: 线自相交检查
    void checkSelfIntersect(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors);

    // 模式512: 节点最小距离检查
    void checkMinNodeDistance(QgsVectorLayer* layer,
        QList<QPair<QgsFeature, QString>>& errors,
        double minDistance = 0.0);

    // 主入口：执行全部启用的检查项
    // processMode: 位掩码，0=全部启用
    void execute(QgsVectorLayer* layer,
        int processMode,
        const QHash<QString, double>& thresholds,
        QList<QPair<QgsFeature, QString>>& allErrors,
        QStringList& executedChecks);
}

// ---- 日志输出工具 ----
namespace QualityCheckLogger {
    // 写入JSON格式的检查日志（无字段长度限制）
    bool writeCheckLog(const QString& logFilePath,
        const QString& checkItemName,
        const QString& layerName,
        int totalFeatures,
        const QList<QPair<QgsFeature, QString>>& errors,
        const QStringList& executedChecks);

    // 生成时间戳字符串
    QString timestamp();
}

#endif // SE_MISSION459_CHECK_H
