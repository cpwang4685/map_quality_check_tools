#ifndef MAP_TOOL_EXTENT_PICKER_H
#define MAP_TOOL_EXTENT_PICKER_H

#include <qgsmaptool.h>
#include <qgsrectangle.h>
#include <qgspointxy.h>

class QgsMapCanvas;
class QgsRubberBand;
class QgsMapMouseEvent;

// 自定义地图框选工具：继承 QgsMapTool，在画布上拖拽矩形并返回范围
class CMapToolExtentPicker : public QgsMapTool
{
	Q_OBJECT
public:
	explicit CMapToolExtentPicker(QgsMapCanvas* canvas);
	~CMapToolExtentPicker() override;

	void activate() override;
	void deactivate() override;

	bool isActive() const { return m_bActive; }

signals:
	void extentSelected(const QgsRectangle& rect);
	void cancelled();

protected:
	void canvasPressEvent(QgsMapMouseEvent* e) override;
	void canvasMoveEvent(QgsMapMouseEvent* e) override;
	void canvasReleaseEvent(QgsMapMouseEvent* e) override;

private:
	QgsRubberBand* m_pRubberBand = nullptr;
	bool m_bActive = false;
	bool m_bDragging = false;
	QgsPointXY m_startMapPoint;
};

#endif // MAP_TOOL_EXTENT_PICKER_H
