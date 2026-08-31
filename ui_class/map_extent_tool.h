#ifndef MAP_EXTENT_TOOL_H
#define MAP_EXTENT_TOOL_H

#include <qgsmaptool.h>
#include <qgsmapmouseevent.h>
#include <qgsrubberband.h>
#include <qgsmapcanvas.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <QMouseEvent>
#include <functional>

class MapExtentTool : public QgsMapTool
{
public:
    using Callback = std::function<void(const QgsRectangle&)>;

    explicit MapExtentTool(QgsMapCanvas* canvas, Callback cb)
        : QgsMapTool(canvas)
        , mCallback(std::move(cb))
        , mRubberBand(nullptr)
    {
    }

    ~MapExtentTool() override
    {
        delete mRubberBand;
    }

    void canvasPressEvent(QgsMapMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;

        mStartPoint = toMapCoordinates(e->pos());
        delete mRubberBand;
        mRubberBand = new QgsRubberBand(canvas(), QgsWkbTypes::PolygonGeometry);
        mRubberBand->setFillColor(QColor(30, 136, 229, 60));
        mRubberBand->setStrokeColor(QColor(30, 136, 229));
        mRubberBand->setWidth(2);
        mRubberBand->show();
    }

    void canvasMoveEvent(QgsMapMouseEvent* e) override
    {
        if (!mRubberBand) return;

        QgsPointXY endPoint = toMapCoordinates(e->pos());
        QgsRectangle rect(mStartPoint, endPoint);
        mRubberBand->reset(QgsWkbTypes::PolygonGeometry);
        mRubberBand->addPoint(QgsPointXY(rect.xMinimum(), rect.yMinimum()), false);
        mRubberBand->addPoint(QgsPointXY(rect.xMaximum(), rect.yMinimum()), false);
        mRubberBand->addPoint(QgsPointXY(rect.xMaximum(), rect.yMaximum()), false);
        mRubberBand->addPoint(QgsPointXY(rect.xMinimum(), rect.yMaximum()), true);
        mRubberBand->show();
    }

    void canvasReleaseEvent(QgsMapMouseEvent* e) override
    {
        if (!mRubberBand || e->button() != Qt::LeftButton) return;

        QgsPointXY endPoint = toMapCoordinates(e->pos());
        QgsRectangle rect(mStartPoint, endPoint);
        rect.normalize();

        delete mRubberBand;
        mRubberBand = nullptr;

        if (mCallback)
            mCallback(rect);
    }

    void deactivate() override
    {
        delete mRubberBand;
        mRubberBand = nullptr;
        QgsMapTool::deactivate();
    }

private:
    QgsPointXY mStartPoint;
    QgsRubberBand* mRubberBand;
    Callback mCallback;
};

#endif // MAP_EXTENT_TOOL_H
