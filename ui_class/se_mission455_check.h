#ifndef SE_MISSION455_CHECK_H
#define SE_MISSION455_CHECK_H
#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include "map_check_common.h"
class QgsVectorLayer;
class QgsFeature;

namespace Mission455 {
    void checkDangles(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol=0.001);
    void checkPseudos(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors, double tol=0.001);
    void checkSelfOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors);
    void checkSelfIntersect(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors);
    void checkSinglePart(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors);
    void checkOverlap(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors);
    void checkIntersect(QgsVectorLayer* layer, QList<QPair<QgsFeature, QString>>& errors);
    void checkInsidePolygon(QgsVectorLayer* lineLayer, QgsVectorLayer* polyLayer, QList<QPair<QgsFeature, QString>>& errors);

    void execute(QgsVectorLayer* lineLayer, QgsVectorLayer* pointLayer,
        QgsVectorLayer* polyLayer, QgsVectorLayer* refLineLayer,
        int processMode, const QHash<QString, double>& thresholds,
        QList<QPair<QgsFeature, QString>>& allErrors, QStringList& executedChecks);
}
#endif
