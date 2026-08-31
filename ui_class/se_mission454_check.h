#ifndef SE_MISSION454_CHECK_H
#define SE_MISSION454_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include "map_check_common.h"

class QgsVectorLayer;
class QgsFeature;

// ---- Mission 454: 点拓扑规则 ----
namespace Mission454 {
    // 模式1: 点必须重合 — Must be coincident with
    void checkCoincident(QgsVectorLayer* layer, QgsVectorLayer* refLayer,
        QList<QPair<QgsFeature, QString>>& errors, double tolerance = 0.001);
    // 模式2: 点必须分离 — Must be disjoint
    void checkDisjoint(QgsVectorLayer* layer, QgsVectorLayer* refLayer,
        QList<QPair<QgsFeature, QString>>& errors, double tolerance = 0.001);
    // 模式4: 点被线端点覆盖 — Must be covered by endpoint of
    void checkCoveredByEndpoint(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
        QList<QPair<QgsFeature, QString>>& errors, double tolerance = 0.001);
    // 模式8: 点必须被线覆盖 — Point must be covered by line
    void checkCoveredByLine(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
        QList<QPair<QgsFeature, QString>>& errors, double tolerance = 0.001);
    // 模式16: 点必须在面内部 — Must be properly inside polygons
    void checkInsidePolygon(QgsVectorLayer* pointLayer, QgsVectorLayer* polyLayer,
        QList<QPair<QgsFeature, QString>>& errors);
    // 模式32: 点必须在面边界上 — Must be covered by boundary of
    void checkOnBoundary(QgsVectorLayer* pointLayer, QgsVectorLayer* polyLayer,
        QList<QPair<QgsFeature, QString>>& errors, double tolerance = 0.001);

    void execute(QgsVectorLayer* pointLayer, QgsVectorLayer* lineLayer,
        QgsVectorLayer* polyLayer, QgsVectorLayer* refPointLayer,
        int processMode, const QHash<QString, double>& thresholds,
        QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks);
}

#endif
