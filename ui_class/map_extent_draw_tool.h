#ifndef MAP_EXTENT_DRAW_TOOL_H
#define MAP_EXTENT_DRAW_TOOL_H

#include <qgsmaptool.h>
#include <QPointer>
#include <QList>
#include <functional>
#include <qgsrectangle.h>
#include <qgspointxy.h>

class QgsMapCanvas;
class QgsRubberBand;
class QKeyEvent;

// ============================================================
// 地图画布交互式范围绘制工具（供「按范围导出」的"手动绘制"模式使用）
//
// 在调用方自己的 QGIS 画布上以橡皮筋方式交互绘制：
//   矩形   ：按住左键拖出矩形
//   圆     ：先点圆心，再拖出半径确定圆
//   多边形 ：左键逐点单击，右键（或双击）结束
//   取消   ：Esc
// 绘制完成后取图形的外包矩形作为裁剪范围。
//
// 本类不声明 Q_OBJECT（无信号/槽），由
//   MapExtentDrawTool::drawExtentOnCanvas() 静态阻塞式调用，
// 内部使用 QEventLoop 等待用户绘制完毕，避免上层对话框 exec() 冲突。
// ============================================================
class MapExtentDrawTool : public QgsMapTool
{
public:
    // 保留圆形/多边形能力供工具层复用；「按范围导出」仅使用 DrawRect
    enum DrawShape
    {
        DrawRect = 0,
        DrawCircle = 1,
        DrawPolygon = 2
    };

    explicit MapExtentDrawTool(QgsMapCanvas* canvas);
    ~MapExtentDrawTool() override;

    // 在当前画布上阻塞等待用户绘制范围。
    // 成功返回 true 并回填 outRect（地图画布坐标/地图坐标系），
    // 用户取消或绘制无效返回 false。
    static bool drawExtentOnCanvas(QgsMapCanvas* canvas, int shape,
                                   QgsRectangle& outRect);

    void setFinishedCallback(std::function<void(const QgsRectangle&, bool cancelled)> cb)
    { m_finishedCb = std::move(cb); }

    // QgsMapTool 事件
    void canvasPressEvent(QgsMapMouseEvent* e) override;
    void canvasMoveEvent(QgsMapMouseEvent* e) override;
    void canvasReleaseEvent(QgsMapMouseEvent* e) override;
    void canvasDoubleClickEvent(QgsMapMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void deactivate() override;

private:
    enum ToolState { Idle, Pressed, Drawing };

    void startRubber();
    void updateRubber(const QgsPointXY& pt);
    void updateRectRubber(const QgsPointXY& pt);
    void updateCircleRubber(const QgsPointXY& pt);
    void updatePolygonRubber(const QgsPointXY& mousePt);
    QgsRectangle currentBBox() const;
    void finish(bool cancelled, const QgsRectangle& rect = QgsRectangle());

    QgsMapCanvas* m_canvas = nullptr;
    QgsRubberBand* m_band = nullptr;
    int m_shape = DrawRect;
    ToolState m_state = Idle;
    bool m_done = false;       // 已结束，防止回调/清理重复触发
    QgsPointXY m_startPt;
    QgsPointXY m_lastPt;
    QList<QgsPointXY> m_polygonPts; // 多边形模式已固定的点
    std::function<void(const QgsRectangle&, bool)> m_finishedCb;
};

#endif // MAP_EXTENT_DRAW_TOOL_H
