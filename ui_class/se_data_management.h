#ifndef SE_DATA_MANAGEMENT_H
#define SE_DATA_MANAGEMENT_H

#include <QDialog>
#include <QString>
#include <QButtonGroup>

#include <QSqlDatabase>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <qgsgeometry.h>
#include <qgsrectangle.h>
#include <qgsmaptool.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsfeature.h>
#include <QList>

class QgisInterface;
class QgsMapCanvas;
class QgsMapTool;
class QgsRubberBand;
class QgsMapMouseEvent;
class QLineEdit;
class QComboBox;
class QCheckBox;

#include "se_database_connection.h"
#include "map_tool_extent_picker.h"

// DbLayerInfo 统一在 map_check_common.h 定义（含 pLoadedLayer/pLoadedRasterLayer 字段），
// 避免本头与 map_check_common.h 在 map_quality_check_tools.cpp 中重复定义（C2011）
#include "map_check_common.h"

namespace Ui { class SeDataManagementDialog; }

class CSE_DataManagementDialog : public QDialog
{
	Q_OBJECT

public:
	CSE_DataManagementDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~CSE_DataManagementDialog() override;

	void setQgisInterface(QgisInterface* iface);
	void addConnection(const DatabaseConnectionInfo& info);
	static void clearConnections();

	// 由外部调用：默认切换到“按主区裁切导出”模式
	void selectMainAreaMode();

public slots:
	// ==================== Tab 切换 ====================
	void on_tabWidget_source_currentChanged(int index);

	// ==================== Tab 0: Shape/文件夹导出 ====================
	void on_Button_BrowseSource_clicked();
	void on_Button_LoadToMap_Shape_clicked();
	void on_Button_UseMapLayers_clicked();

	// ==================== Tab 1: GDB 文件导出 ====================
	void on_Button_BrowseGdb_clicked();
	void on_Button_LoadToMap_Gdb_clicked();

	// ==================== Tab 2: 数据库图层导出 ====================
	void on_Button_NewConnection_clicked();
	void on_Button_DeleteConnection_clicked();
	void on_Button_Connect_clicked();
	void on_Button_Disconnect_clicked();
	void on_comboBox_connection_currentIndexChanged(int index);
	void on_lineEdit_dbFilter_textChanged(const QString& text);
	void on_Button_LoadToMap_Db_clicked();

	// ==================== Tab 3: 当前地图图层导出 ====================
	void on_Button_UseMapLayers_Tab3_clicked();

	// ==================== 图层列表 ====================
	void on_Button_SelectAll_clicked();
	void on_Button_SelectNone_clicked();
	void on_Button_RefreshLayers_clicked();
	void on_treeWidget_layers_itemChanged(QTreeWidgetItem* item, int column);

	// ==================== 裁剪面板 ====================
	void on_checkBox_enableClip_toggled(bool checked);
	void on_radioButton_useExtent_toggled(bool checked);
	void on_radioButton_useShp_toggled(bool checked);
	void on_radioButton_useMainArea_toggled(bool checked);
	void on_stackedWidget_clipParams_currentChanged(int index);
	void on_Button_GetCanvasExtent_clicked();
	void on_Button_ClearExtent_clicked();
	void on_Button_PickExtentFromMap_clicked();
	void onMapExtentSelected(const QgsRectangle& rect);
	void onMapExtentPickCancelled();
	void on_Button_BrowseClipShp_clicked();
	void on_Button_LoadClipShpToMap_clicked();
	void on_lineEdit_clipShpPath_textChanged(const QString& text);
	void on_comboBox_clipAttrField_currentIndexChanged(int index);

	// ==================== 根据主区裁切 ====================
	void on_Button_BrowseMainArea_clicked();
	void on_comboBox_mainAreaField_currentIndexChanged(int index);
	void on_listWidget_mainAreaFeatures_itemSelectionChanged();
	void on_Button_CalculateScale_clicked();
	void on_comboBox_paperSize_currentIndexChanged(int index);
	void on_comboBox_paperOrientation_currentIndexChanged(int index);
	void on_checkBox_useCustomScale_toggled(bool checked);
	void on_checkBox_enableInnerClip_toggled(bool checked);

	// ==================== 输出设置 ====================
	void on_Button_BrowseOutput_extent_clicked();
	void on_Button_BrowseOutput_shp_clicked();
	void on_Button_BrowseOutput_mainArea_clicked();

	// ==================== 导出 ====================
	void on_Button_Export_clicked();
	void on_Button_Cancel_clicked();

private:
	Ui::SeDataManagementDialog* ui;

	QgisInterface* m_pQgisIface = nullptr;
	int m_iCurrentTab = 0;							// 当前活动 Tab 索引

	// 数据库连接
	int m_iCurrentConnectionIndex = -1;
	QSqlDatabase m_dbConnection;
	bool m_bIsConnected = false;
	static QList<DatabaseConnectionInfo> s_listConnections; // 静态持久化连接列表

	// 可用图层列表
	QList<DbLayerInfo> m_listAvailableLayers;

	// 裁剪控件
	QButtonGroup* m_pClipModeGroup = nullptr;
	CMapToolExtentPicker* m_pExtentPicker = nullptr;
	QgsMapTool* m_pPreviousMapTool = nullptr;

	// 根据主区裁切数据
	QgsVectorLayer* m_pMainAreaLayer = nullptr;
	QgsFeatureId m_mainAreaSelectedFid = -1;
	QgsRectangle m_mainAreaSelectedExtent;
	QgsRectangle m_mainAreaClipExtentWgs84;
	bool m_bMainAreaClipExtentValid = false;
	// 调用 selectMainAreaMode() 后为 true：对话进入纯按主区裁切模式，其它裁切选项隐藏
	bool m_bMainAreaOnly = false;

	// ==================== 辅助方法 ====================

	// 刷新连接下拉框
	void refreshConnectionCombo();

	// 关闭数据库连接
	void closeDatabaseConnection();

	// 更新数据库连接相关按钮状态
	void updateDbConnectionUI(bool bConnected);

	// 填充图层列表到 treeWidget
	void refreshLayerTree();

	// 更新已选图层数标签
	void updateLayerSummary();

	// 追加日志
	void appendLog(const QString& msg);

	// 校验导出按钮状态
	void validateExportReady();

	// ==================== 裁剪相关方法 ====================

	// 启用/禁用空间范围裁剪子控件
	void setSpatialFilterEnabled(bool enabled);

	// 启用/禁用 SHP 文件裁剪子控件
	void setShpClipEnabled(bool enabled);

	// ==================== 输出设置辅助方法 ====================
	// 根据当前选中的裁剪方式返回对应的输出控件（三种裁剪输出互相独立）
	QLineEdit* currentOutputPathEdit() const;
    QComboBox* currentEncodingCombo() const;
    QComboBox* currentFormatCombo() const;
    QCheckBox* currentOverwriteCheck() const;

	// 打开浏览对话框并写入当前裁剪页对应的输出路径
	void browseOutputPath();

	// 删除已有 Shapefile 及其所有附属文件，并在必要时移除地图中占用该路径的图层以释放文件锁
	bool deleteExistingShapefile(const QString& shpPath, QString& errMsg);

	// ==================== 根据主区裁切相关方法 ====================
	void initMainAreaClip();
	void setMainAreaClipEnabled(bool enabled);
	void resetMainAreaClip();          // 切换 Tab 时清空当前页主区裁切状态
	void resetCurrentTabPage();        // 切换 Tab 时整套面板恢复初始默认状态
	void loadMainAreaLayer(const QString& path);
	void refreshMainAreaFeatureList();
	void updateMainAreaFeatureExtent();
	void calculateMainAreaScaleAndExtent();
	bool getMainAreaClipExtent(double& minLon, double& minLat, double& maxLon, double& maxLat) const;
	int roundUpToStandardScale(double scale) const;

	// 从输入框获取空间范围（成功返回 true）
	bool getSpatialExtent(double& minLon, double& minLat, double& maxLon, double& maxLat) const;

	// 从裁剪 SHP 加载合并几何
	bool loadClipGeometry(QgsGeometry& outGeom, QString& errMsg) const;

	// 获取当前选中的图层列表（含排序去重）
	QList<DbLayerInfo> getSelectedLayers() const;

	// 将当前勾选的图层加载到主地图窗口预览
	void loadSelectedLayersToMap();

	// 导出成功后，自动将单个导出结果加载到当前地图
	void loadExportedResultToMap(const DbLayerInfo& layer, const QString& dstPath);
	// ==================== 导出核心方法 ====================

	// 扫描目录/文件中的矢量图层，填充到可用图层列表
	void scanShapeSource(const QString& srcPath, bool bIsFolder = true);
	void scanShapeSource(const QStringList& fileList);
	void parseShapeFiles(const QStringList& shpFiles);

	// 从 GDB 加载图层列表
	void loadGdbLayers(const QString& gdbPath);

	// 从数据库加载矢量+栅格图层列表
	void loadDatabaseLayers();

	// 通过 OGR 读取源图层并应用过滤/裁剪，写入目标文件
	bool exportVectorLayerFromFile(const DbLayerInfo& layer,
		const QString& dstPath, const QString& driverName,
		bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
		const QgsGeometry& clipGeom, bool bIntersectOnly, bool bClipGeometry,
		const QString& encoding, int& outFeatureCount);

	// 通过 PostGIS 读取源图层并应用过滤/裁剪，写入目标文件
	bool exportVectorLayerFromDB(const DbLayerInfo& layer,
		const QString& dstPath, const QString& driverName,
		bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
		const QgsGeometry& clipGeom, bool bIntersectOnly, bool bClipGeometry,
		const QString& encoding, int& outFeatureCount);

	// 通过 PostGIS 读取栅格图层，使用 GDAL 导出为 GeoTIFF
	bool exportRasterLayerFromDB(const DbLayerInfo& layer,
		const QString& dstPath,
		bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
		bool bUseShpClip, const QgsGeometry& clipGeom,
		int& outFeatureCount);

	// 从地图已加载的栅格图层导出为 GeoTIFF（Tab 3），支持空间范围与 Shape 裁剪
	bool exportRasterLayerFromMapRaster(const DbLayerInfo& layer,
		const QString& dstPath,
		bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
		bool bUseShpClip, const QgsGeometry& clipGeom,
		int& outFeatureCount);

	// 通过 GDAL 将栅格数据源导出为 GeoTIFF（支持空间范围裁剪与 Shape 裁剪）
	bool doExportRaster(
		const QString& rasterUri,
		const QString& dstPath,
		bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
		bool bUseShpClip, const QgsGeometry& clipGeom,
		int& outFeatureCount);

	// 将裁剪几何写入临时 GeoJSON 文件，返回文件路径（供 GDAL -cutline 使用）；失败返回空串
	QString writeClipGeometryToTempFile(const QgsGeometry& clipGeom) const;

	// 构建 PostGIS URL（矢量）
	QString buildPgUri(const DbLayerInfo& layer) const;

	// 构建 PostGIS 栅格连接字符串（GDAL raster 格式）
	QString buildPgRasterConnStr(const DbLayerInfo& layer) const;
};

#endif // SE_DATA_MANAGEMENT_H
