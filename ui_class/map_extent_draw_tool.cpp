/*--------------QT---------------*/
#include <QKeyEvent>
#include <QEventLoop>
#include <QColor>
#include <QtMath>
#include <algorithm>
#include <cmath>

/*--------------QGIS---------------*/
#include <qgsmapcanvas.h>
#include <qgsrubberband.h>
#include <qgsmapmouseevent.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgswkbtypes.h>

/*--------------本模块---------------*/
#include "map_extent_draw_tool.h"

MapExtentDrawTool::MapExtentDrawTool(QgsMapCanvas* canvas)
    : QgsMapTool(canvas)
    , m_canvas(canvas)
{
}

MapExtentDrawTool::~MapExtentDrawTool()
{
    delete m_band;
    m_band = nullptr;
}

void MapExtentDrawTool::startRubber()
{
    if (!m_band)
    {
        m_band = new QgsRubberBand(m_canvas, QgsWkbTypes::LineGeometry);
        m_band->setColor(QColor(255, 60, 0, 200));
        m_band->setWidth(3);
    }
    m_band->reset(QgsWkbTypes::LineGeometry);
}

static QgsRectangle rectOfTwoPoints(const QgsPointXY& a, const QgsPointXY& b)
{
    double x1 = std::min(a.x(), b.x());
    double x2 = std::max(a.x(), b.x());
    double y1 = std::min(a.y(), b.y());
    double y2 = std::max(a.y(), b.y());
    return QgsRectangle(x1, y1, x2, y2);
}

static QgsRectangle bboxOfPoints(const QList<QgsPointXY>& pts)
{
    QgsRectangle rect;
    if (pts.isEmpty()) return rect;
    rect = QgsRectangle(pts.first().x(), pts.first().y(),
                        pts.first().x(), pts.first().y());
    for (int i = 1; i < pts.size(); ++i)
    {
        rect.setXMinimum(std::min(rect.xMinimum(), pts[i].x()));
        rect.setXMaximum(std::max(rect.xMaximum(), pts[i].x()));
        rect.setYMinimum(std::min(rect.yMinimum(), pts[i].y()));
        rect.setYMaximum(std::max(rect.yMaximum(), pts[i].y()));
    }
    return rect;
}

void MapExtentDrawTool::updateRubber(const QgsPointXY& pt)
{
    switch (m_shape)
    {
    case DrawCircle:
        updateCircleRubber(pt);
        break;
    case DrawPolygon:
        updatePolygonRubber(pt);
        break;
    case DrawRect:
    default:
        updateRectRubber(pt);
        break;
    }
}

void MapExtentDrawTool::updateRectRubber(const QgsPointXY& pt)
{
    QgsRectangle r = rectOfTwoPoints(m_startPt, pt);
    QgsPointXY p1(r.xMinimum(), r.yMinimum());
    QgsPointXY p2(r.xMaximum(), r.yMinimum());
    QgsPointXY p3(r.xMaximum(), r.yMaximum());
    QgsPointXY p4(r.xMinimum(), r.yMaximum());
    QgsGeometry line = QgsGeometry::fromPolylineXY(
        QVector<QgsPointXY>() << p1 << p2 << p3 << p4 << p1);
    if (!line.isNull())
        m_band->setToGeometry(line, nullptr);
}

void MapExtentDrawTool::updateCircleRubber(const QgsPointXY& pt)
{
    double radius = std::hypot(pt.x() - m_startPt.x(), pt.y() - m_startPt.y());
    QVector<QgsPointXY> ring;
    const int seg = 72;
    const double twoPi = 6.28318530717958647692; // 2*PI
    for (int i = 0; i <= seg; ++i)
    {
        double a = twoPi * i / seg;
        ring << QgsPointXY(m_startPt.x() + radius * std::cos(a),
                           m_startPt.y() + radius * std::sin(a));
    }
    QgsGeometry line = QgsGeometry::fromPolylineXY(ring);
    if (!line.isNull())
        m_band->setToGeometry(line, nullptr);
}

void MapExtentDrawTool::updatePolygonRubber(const QgsPointXY& mousePt)
{
    QVector<QgsPointXY> ring;
    for (const QgsPointXY& p : m_polygonPts)
        ring << p;
    ring << mousePt; // 预览当前光标到最新点之间的线段
    QgsGeometry line = QgsGeometry::fromPolylineXY(ring);
    if (!line.isNull())
        m_band->setToGeometry(line, nullptr);
}

QgsRectangle MapExtentDrawTool::currentBBox() const
{
    if (m_shape == DrawRect)
        return rectOfTwoPoints(m_startPt, m_lastPt);
    return bboxOfPoints(m_polygonPts);
}

void MapExtentDrawTool::finish(bool cancelled, const QgsRectangle& rect)
{
    if (m_done) return;
    m_done = true;
    if (m_band)
    {
        delete m_band;
        m_band = nullptr;
    }
    if (m_finishedCb)
        m_finishedCb(rect, cancelled);
}

void MapExtentDrawTool::canvasPressEvent(QgsMapMouseEvent* e)
{
    if (m_done) return;
    const QgsPointXY pt = toMapCoordinates(e->pos());

    if (m_shape == DrawPolygon)
    {
        if (e->button() == Qt::RightButton)
        {
            // 右键结束多边形
            QgsRectangle b = bboxOfPoints(m_polygonPts);
            if (m_polygonPts.size() >= 3 && !b.isNull() && !b.isEmpty())
                finish(false, b);
            else
                finish(true);
            return;
        }
        if (e->button() == Qt::LeftButton)
        {
            startRubber();
            m_polygonPts << pt;
            m_state = Drawing;
            updatePolygonRubber(pt);
        }
        return;
    }

    // 矩形 / 圆：按下开始拖拽
    if (e->button() == Qt::LeftButton)
    {
        m_startPt = pt;
        m_lastPt = pt;
        m_state = Pressed;
        startRubber();
    }
}

void MapExtentDrawTool::canvasMoveEvent(QgsMapMouseEvent* e)
{
    if (m_done || m_state == Idle) return;
    const QgsPointXY pt = toMapCoordinates(e->pos());
    if (m_shape == DrawPolygon)
    {
        updatePolygonRubber(pt);
        return;
    }
    m_lastPt = pt;
    if (m_shape == DrawCircle)
        updateCircleRubber(pt);
    else
        updateRectRubber(pt);
}

void MapExtentDrawTool::canvasReleaseEvent(QgsMapMouseEvent* e)
{
    if (m_done || e->button() != Qt::LeftButton) return;

    if (m_shape == DrawPolygon)
    {
        // 多边形点由 press 添加，释放不需要额外处理
        return;
    }

    const QgsPointXY pt = toMapCoordinates(e->pos());
    m_lastPt = pt;
    m_state = Idle;

    QgsRectangle b = currentBBox();
    if (b.isNull() || b.isEmpty())
    {
        finish(true); // 几乎没有拖拽，视为取消
        return;
    }
    finish(false, b);
}

void MapExtentDrawTool::canvasDoubleClickEvent(QgsMapMouseEvent* e)
{
    // 多边形也可通过双击结束：双击的最后一击作为多边形末点
    if (m_done || m_shape != DrawPolygon || e->button() != Qt::LeftButton)
        return;
    const QgsPointXY pt = toMapCoordinates(e->pos());
    m_polygonPts << pt;
    startRubber();
    updatePolygonRubber(pt);
    QgsRectangle b = bboxOfPoints(m_polygonPts);
    if (b.isNull() || b.isEmpty())
        finish(true);
    else
        finish(false, b);
}

void MapExtentDrawTool::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape)
        finish(true); // Esc 取消
    else
        QgsMapTool::keyPressEvent(e);
}

void MapExtentDrawTool::deactivate()
{
    QgsMapTool::deactivate();
    if (!m_done)
        finish(true); // 工具被外部切换/注销时按取消处理
}

bool MapExtentDrawTool::drawExtentOnCanvas(QgsMapCanvas* canvas, int shape,
                                           QgsRectangle& outRect)
{
    if (!canvas) return false;

    MapExtentDrawTool* tool = new MapExtentDrawTool(canvas);
    if (shape >= DrawRect && shape <= DrawPolygon)
        tool->m_shape = shape;

    QEventLoop loop;
    bool ok = false;
    QgsRectangle rect;
    tool->setFinishedCallback([&](const QgsRectangle& r, bool cancelled)
    {
        rect = r;
        ok = !cancelled && !r.isNull() && !r.isEmpty();
        loop.quit();
    });

    // 之前没有设置过当前工具时 QGIS 会使用默认导航工具
    canvas->setMapTool(tool);
    loop.exec();

    canvas->unsetMapTool(tool); // 恢复原工具
    tool->deleteLater();

    outRect = rect;
    return ok;
}
