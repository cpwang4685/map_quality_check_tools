#ifndef SE_MISSION456_CHECK_H
#define SE_MISSION456_CHECK_H
#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include "map_check_common.h"
class QgsVectorLayer;
class QgsFeature;

namespace Mission456 {
    void checkOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors);
    void checkGaps(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol=0.001);
    void checkContainsPoint(QgsVectorLayer* polyLayer, QgsVectorLayer* pointLayer, QList<QPair<QgsFeature, QString>>& errors);
    void checkBoundaryCoveredByLine(QgsVectorLayer* polyLayer, QgsVectorLayer* lineLayer, QList<QPair<QgsFeature, QString>>& errors, double tol=0.001);
    void checkLargerThanTolerance(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double minArea=0.0);

    void execute(QgsVectorLayer* polyLayer, QgsVectorLayer* lineLayer,
        QgsVectorLayer* pointLayer, QgsVectorLayer* refPolyLayer,
        int processMode, const QHash<QString, double>& thresholds,
        QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks);
}
#endif
