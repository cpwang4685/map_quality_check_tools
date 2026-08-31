#include "map_tool_extent_picker.h"

#include <qgsmapcanvas.h>
#include <qgsrubberband.h>
#include <qgsmapmouseevent.h>
#include <qgswkbtypes.h>
#include <QColor>
#include <algorithm>

CMapToolExtentPicker::CMapToolExtentPicker(QgsMapCanvas* canvas)
	: QgsMapTool(canvas)
{
}

CMapToolExtentPicker::~CMapToolExtentPicker()
{
	if (m_pRubberBand)
	{
		delete m_pRubberBand;
		m_pRubberBand = nullptr;
	}
}

void CMapToolExtentPicker::activate()
{
	QgsMapTool::activate();

	if (m_pRubberBand)
	{
		delete m_pRubberBand;
		m_pRubberBand = nullptr;
	}

	m_pRubberBand = new QgsRubberBand(canvas(), QgsWkbTypes::PolygonGeometry);
	m_pRubberBand->setColor(QColor(0, 120, 255, 180));
	m_pRubberBand->setFillColor(QColor(0, 120, 255, 60));
	m_pRubberBand->setWidth(2);
	m_pRubberBand->setLineStyle(Qt::DashLine);

	m_bActive = true;
	m_bDragging = false;
}

void CMapToolExtentPicker::deactivate()
{
	if (m_pRubberBand)
	{
		delete m_pRubberBand;
		m_pRubberBand = nullptr;
	}
	m_bActive = false;
	m_bDragging = false;
	QgsMapTool::deactivate();
}

void CMapToolExtentPicker::canvasPressEvent(QgsMapMouseEvent* e)
{
	if (!m_bActive)
		return;

	if (e->button() == Qt::LeftButton)
	{
		m_startMapPoint = toMapCoordinates(e->pos());
		m_bDragging = true;

		if (m_pRubberBand)
		{
			m_pRubberBand->reset(QgsWkbTypes::PolygonGeometry);
			m_pRubberBand->addPoint(m_startMapPoint, false);
			m_pRubberBand->addPoint(m_startMapPoint, false);
			m_pRubberBand->addPoint(m_startMapPoint, false);
			m_pRubberBand->addPoint(m_startMapPoint, true);
		}
	}
}

void CMapToolExtentPicker::canvasMoveEvent(QgsMapMouseEvent* e)
{
	if (!m_bActive || !m_bDragging || !m_pRubberBand)
		return;

	QgsPointXY currentMapPoint = toMapCoordinates(e->pos());

	QgsPointXY bottomLeft(std::min(m_startMapPoint.x(), currentMapPoint.x()),
		std::min(m_startMapPoint.y(), currentMapPoint.y()));
	QgsPointXY topRight(std::max(m_startMapPoint.x(), currentMapPoint.x()),
		std::max(m_startMapPoint.y(), currentMapPoint.y()));

	m_pRubberBand->reset(QgsWkbTypes::PolygonGeometry);
	m_pRubberBand->addPoint(bottomLeft, false);
	m_pRubberBand->addPoint(QgsPointXY(topRight.x(), bottomLeft.y()), false);
	m_pRubberBand->addPoint(topRight, false);
	m_pRubberBand->addPoint(QgsPointXY(bottomLeft.x(), topRight.y()), true);
}

void CMapToolExtentPicker::canvasReleaseEvent(QgsMapMouseEvent* e)
{
	if (!m_bActive)
		return;

	if (e->button() == Qt::LeftButton && m_bDragging)
	{
		m_bDragging = false;
		QgsPointXY endMapPoint = toMapCoordinates(e->pos());

		double minX = std::min(m_startMapPoint.x(), endMapPoint.x());
		double minY = std::min(m_startMapPoint.y(), endMapPoint.y());
		double maxX = std::max(m_startMapPoint.x(), endMapPoint.x());
		double maxY = std::max(m_startMapPoint.y(), endMapPoint.y());

		if (m_pRubberBand)
		{
			m_pRubberBand->reset(QgsWkbTypes::PolygonGeometry);
		}

		if (maxX - minX > 1e-12 && maxY - minY > 1e-12)
		{
			emit extentSelected(QgsRectangle(minX, minY, maxX, maxY));
		}
		else
		{
			emit cancelled();
		}
	}
	else if (e->button() == Qt::RightButton)
	{
		m_bDragging = false;
		if (m_pRubberBand)
		{
			m_pRubberBand->reset(QgsWkbTypes::PolygonGeometry);
		}
		emit cancelled();
	}
}
