#ifndef SE_MISSION459_CHECK_H
#define SE_MISSION459_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>

class QgsVectorLayer;
class QgsFeature;

// ---- Mission 459: 图形规范性检查 ----
// 全部 10 个模式均为 QGIS 本地实现（引擎对多个模式实测返回"任务执行失败"，
// 且对线图层喂面检查会段错误/挂死）：
// 1多部件 2空图形 4尖锐角 8碎面 16狭长面 32小面积 64节点平均密度
// 128节点密度 256自相交 512节点最小距离；阈值经 thresholds 哈希传入，
// 密度类阈值 0 表示该侧不启用。
namespace Mission459 {
    void execute(QgsVectorLayer* layer,
        int processMode,
        const QHash<QString, double>& thresholds,
        QList<QPair<QgsFeature, QString>>& allErrors,
        QStringList& executedChecks);
}

#endif // SE_MISSION459_CHECK_H
