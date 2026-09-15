#ifndef SE_MISSION456_CHECK_H
#define SE_MISSION456_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>

class QgsVectorLayer;
class QgsFeature;

// ---- Mission 456: 面拓扑规则 ----
// 检查逻辑由引擎子进程完成，本模块负责数据存储准备、参数设置与结果收集。
// 目标面图层每次一个（全部图层逐层检查）；参考图层为已合并好的
// 线/点参考 SHP 路径（为空则不参与），进入任务目录时复制为相对路径。
namespace Mission456 {
    void execute(QgsVectorLayer* polyLayer, const QString& refLineShp,
        const QString& refPointShp,
        int processMode, const QHash<QString, double>& thresholds,
        QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks);
}

#endif
