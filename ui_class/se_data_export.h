#ifndef SE_DATA_EXPORT_H
#define SE_DATA_EXPORT_H

#include <QDialog>
#include <QString>
#include <QButtonGroup>

#include <QSqlDatabase>

#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <qgsgeometry.h>
#include <qgsrectangle.h>
#include <qgsmaptool.h>

class QgisInterface;
class QgsMapCanvas;
class QgsMapTool;
class QgsRubberBand;
class QgsMapMouseEvent;

#include "map_tool_extent_picker.h"

// DbLayerInfo 统一在 map_check_common.h 定义（含 strSourcePath/pLoadedLayer/pLoadedRasterLayer 字段）
#include "map_check_common.h"

// 单个数据库表导出配置
struct DataExportInfo
{
	QString strSourceName;		// 源表名（含 schema）
	QString strSchema;			// Schema 名称
	QString strTableName;		// 表名（不含 schema）
	QString strDataType;		// 数据类型：矢量数据/栅格数据
	QString strCrs;				// 坐标系
	QString strHost;			// 数据库主机
	QString strPort;			// 数据库端口
	QString strDbName;			// 数据库名
	QString strUsername;		// 用户名
	QString strPassword;		// 密码

	DataExportInfo()
	{}
};

namespace Ui { class SeDataExportDialog; }

class CSE_DataExportDialog : public QDialog
{
	Q_OBJECT

public:
	CSE_DataExportDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~CSE_DataExportDialog() override;

	// 设置数据库连接
	void setDatabaseConnection(const QSqlDatabase& db);

	// 设置 QGIS 接口（用于地图框选等功能）
	void setQgisInterface(QgisInterface* iface);

	// 设置要导出的数据库图层列表（多选）
	void setAvailableLayers(const QList<DbLayerInfo>& layers);

	// 加载当前 QGIS 项目中的图层（用于 canvas 范围和裁剪 SHP）
	void loadProjectLayers();

	// 设置要导出的首个图层（兼容旧接口）
	void setExportInfo(const DataExportInfo& info);

public slots:
	// 选中项变化
	void on_treeWidget_dbLayers_itemSelectionChanged();

	// 关键字过滤
	void on_lineEdit_filter_textChanged(const QString& text);

	// 刷新图层列表
	void on_Button_RefreshLayers_clicked();

	// 全选/清空
	void on_Button_SelectAll_clicked();
	void on_Button_SelectNone_clicked();

	// 空间范围按钮
	void on_Button_GetCanvasExtent_clicked();
	void on_Button_ClearExtent_clicked();
	void on_Button_PickExtentFromMap_clicked();

	// 单选裁剪模式切换
	void on_radioButton_useExtent_toggled(bool checked);
	void on_radioButton_useShp_toggled(bool checked);

	// 接收地图框选范围并回填
	void onMapExtentSelected(const QgsRectangle& rect);
	void onMapExtentPickCancelled();

	// 裁剪 SHP 相关
	void on_Button_BrowseClipShp_clicked();
	void on_Button_LoadClipShpToMap_clicked();
	void on_lineEdit_clipShpPath_textChanged(const QString& text);
	void on_comboBox_clipAttrField_currentIndexChanged(int index);

	// 输出路径相关
	void on_Button_BrowseOutput_clicked();
	void on_Button_BrowseOutputDir_clicked();

	// 格式切换
	void on_comboBox_format_currentIndexChanged(int index);

	// 执行导出
	void on_Button_Export_clicked();

	// 取消
	void on_Button_Cancel_clicked();

private:
	Ui::SeDataExportDialog* ui;

	QSqlDatabase m_dbConnection;		// 外部传入的数据库连接引用
	QList<DbLayerInfo> m_availableLayers;	// 可导出的图层列表
	QgisInterface* m_pQgisIface = nullptr; // QGIS 接口

	QString m_currentFormat;  // "SHP" / "GDB" / "TIF" / "GPKG"

	QButtonGroup* m_pClipModeGroup = nullptr;          // 裁剪方式单选组（空间范围过滤 / SHP 裁剪）
	CMapToolExtentPicker* m_pExtentPicker = nullptr; // 地图框选工具
	QgsMapTool* m_pPreviousMapTool = nullptr;        // 框选前保留的地图工具


private:
	// 启用/禁用空间范围过滤子控件
	void setSpatialFilterEnabled(bool enabled);

	// 启用/禁用 SHP 裁剪子控件
	void setShpClipEnabled(bool enabled);

private:
	// 把可导出图层填充到 treeWidget
	void refreshLayerTree();

	// 从输入框读取空间范围（成功返回 true）
	bool getSpatialExtent(double& minX, double& minY, double& maxX, double& maxY) const;

	// 更新输出路径（根据格式自动拼接后缀）
	void updateOutputPathSuffix();

	// 当前输出文件后缀
	QString currentFormatSuffix() const;
	QString currentFormatFilter() const;
	QString currentDriverName() const;

	// 追加日志
	void appendLog(const QString& msg);

	// 从 DbLayerInfo 构造一个 PostGIS URI
	QString buildPgUri(const DbLayerInfo& layer) const;

	// 把裁剪 SHP 解析为合并几何
	bool loadClipGeometry(QgsGeometry& outGeom, QString& errMsg) const;

	// 计算并返回所有所选图层（从 treeWidget 选中项）
	QList<DbLayerInfo> getSelectedLayers() const;

	// 为指定图层执行导出（主入口）
	bool exportLayer(const DbLayerInfo& layer,
		const QString& dstPath,
		bool bUseExtent,
		double minX, double minY, double maxX, double maxY,
		const QgsGeometry& clipGeom,
		bool bIntersectOnly,
		const QString& encoding,
		int& outFeatureCount);

	// 矢量数据导出到 SHP（支持空间过滤 + 裁剪）
	bool exportVectorLayerToShp(const DbLayerInfo& layer,
		const QString& dstPath,
		bool bUseExtent,
		double minX, double minY, double maxX, double maxY,
		const QgsGeometry& clipGeom,
		bool bIntersectOnly,
		const QString& encoding,
		int& outFeatureCount);

	// 矢量数据导出到 GDB / GPKG
	bool exportVectorLayerToOgr(const DbLayerInfo& layer,
		const QString& dstPath,
		const QString& driverName,
		bool bUseExtent,
		double minX, double minY, double maxX, double maxY,
		const QgsGeometry& clipGeom,
		bool bIntersectOnly,
		const QString& encoding,
		int& outFeatureCount);

	// 栅格数据导出到 TIF
	bool exportRasterLayerToTif(const DbLayerInfo& layer,
		const QString& dstPath,
		bool bUseExtent,
		double minX, double minY, double maxX, double maxY,
		const QString& clipShpPath,
		int& outFeatureCount);

	// 添加一个输出文件到地图
	void addOutputToMap(const QString& path, const QString& formatKey, const QString& layerName);

	// 构造 postgis connstr
	QString buildPostGISConnStr() const;

	// 更新汇总标签
	void updateSummaryLabel();

	// 更新要素数显示
	void updateFeatureCountDisplay(const DbLayerInfo& layer);

	// 根据图层数据类型（矢量/栅格）动态更新目标格式选项
	void updateExportFormatOptions(const DbLayerInfo& layer);
};

#endif // SE_DATA_EXPORT_H
