#ifndef LAYER_ICON_HELPER_H
#define LAYER_ICON_HELPER_H

#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QApplication>
#include <qgsapplication.h>
#include "database/product_metadata.h"

/**
 * @brief 点/线/面/栅格 图层图标公共绘制
 *
 * 供"成果存储与管理 / 元数据管理 / 地图服务单"等对话框的目录树复用，
 * 保证各处图标一致（16x16 逻辑尺寸，与文件夹图标同尺寸观感）。
 */
namespace LayerIconHelper {

// 用 QPainter 绘制 ArcGIS 风格的点/线/面/栅格小图标
inline QIcon drawLayerIcon(int kind)
{
	const qreal dpr = qApp->devicePixelRatio();
	QPixmap pm(qRound(16 * dpr), qRound(16 * dpr));
	pm.fill(Qt::transparent);
	pm.setDevicePixelRatio(dpr);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);

	// kind: 0=点 1=线 2=面 3=栅格（图形铺满画布，与 SP_DirIcon 视觉尺寸一致）
	if (kind == 0) // 点图层：实心圆点
	{
		p.setPen(QPen(QColor(0, 90, 180), 1.2));
		p.setBrush(QColor(255, 210, 0));
		p.drawEllipse(QRectF(1.5, 1.5, 13, 13));
	}
	else if (kind == 1) // 线图层：折线
	{
		QPen pen(QColor(0, 90, 180), 2);
		p.setPen(pen);
		p.drawPolyline(QPolygonF() << QPointF(1.5, 13.5) << QPointF(5, 8) << QPointF(8, 11) << QPointF(14.5, 3));
	}
	else if (kind == 2) // 面图层：填充多边形
	{
		p.setPen(QPen(QColor(0, 90, 180), 1.2));
		p.setBrush(QColor(140, 200, 255));
		p.drawPolygon(QPolygonF() << QPointF(2, 13.5) << QPointF(2, 5) << QPointF(8, 1.5) << QPointF(14, 6) << QPointF(10, 13.5));
	}
	else // 栅格图层：网格状
	{
		p.setPen(QPen(QColor(120, 120, 120), 1.2));
		p.setBrush(QColor(230, 230, 230));
		p.drawRect(QRectF(1.5, 1.5, 13, 13));
		p.setPen(QPen(QColor(160, 160, 160), 1));
		p.drawLine(1.5, 8, 14.5, 8);
		p.drawLine(8, 1.5, 8, 14.5);
	}
	return QIcon(pm);
}

// 根据产品类型返回图层节点图标：0=点 1=线 2=面 3=栅格；非矢量/栅格返回 -1
inline int layerIconKindForProduct(const ProductMetadata& meta)
{
	if (meta.productType == ProductType::Raster)
		return 3; // 栅格
	if (meta.productType == ProductType::Vector)
	{
		QString geom = meta.geometryType.toLower();
		if (geom.contains("point"))
			return 0;
		if (geom.contains("line") || geom.contains("curve"))
			return 1;
		if (geom.contains("polygon") || geom.contains("surface"))
			return 2;
	}
	return -1; // 其它
}

// GDB 文件夹图标：直接用 QGIS 主题自带的 mIconDbSchema.svg，
// 与 LTZK 平台左侧「本地数据」里 .gdb 数据项的图标完全一致。
inline QIcon gdbFolderIcon()
{
	static const QIcon icon = []() {
		QIcon i = QgsApplication::getThemeIcon(QStringLiteral("mIconDbSchema.svg"));
		if (i.isNull())
			i = QIcon(QgsApplication::defaultThemePath() + QStringLiteral("/mIconDbSchema.svg"));
		return i;
	}();
	return icon;
}

// 是否为 GDB 容器目录：新版入库会打 node_type=3；旧数据只能靠目录名 / 直属产品反推
inline bool isGdbDirectory(int nodeType, const QString& dirName,
	const QList<ProductMetadata>& directProducts)
{
	if (nodeType == 3)
		return true;
	if (dirName.endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
		return true;
	for (const auto& p : directProducts)
	{
		if (p.fileFormat.compare(QLatin1String("gdb"), Qt::CaseInsensitive) == 0
			&& p.productName.startsWith(dirName + QLatin1Char('_')))
			return true;
	}
	return false;
}

} // namespace LayerIconHelper

#endif // LAYER_ICON_HELPER_H
