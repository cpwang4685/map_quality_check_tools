#include "se_data_management.h"
#include "ui_data_management.h"
#include "se_database_connection.h"

// 【2026-08-24】与数据库配置 UI 共享连接的读取需要 QSettings
#include <QSettings>
// 【2026-08-24】恢复麒麟自适应缩放（更新版移除，本项目双平台保留）
#include "ui_fit_helper.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProgressBar>
#include <QDateTime>
#include <QApplication>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>

// QGIS includes
#include <qgisinterface.h>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include <qgsrubberband.h>
#include <qgsmapmouseevent.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsfeatureiterator.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsgeometryengine.h>
#include <qgsogrutils.h>
#include <qgsproviderregistry.h>
#include <qgsdatasourceuri.h>
#include <qgsmessagelog.h>

#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <ogr_spatialref.h>

// GDAL for raster export
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <gdalwarper.h>
#include <cpl_conv.h>
#include <cpl_string.h>

// Qt SQL
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>

// ========== 静态连接信息持久化 ==========
QList<DatabaseConnectionInfo> CSE_DataManagementDialog::s_listConnections;

// ========== UI 层坐标兜底转换（Web 墨卡托 -> WGS84 经纬度） ==========
// 在 DLL 宿主环境中，QGIS 坐标转换基础设施（QgsCoordinateTransform/QgsProject）
// 可能不可用，导致框选坐标直接呈现 Web 墨卡托米制大数值。
// 以下使用纯数学公式做兜底转换，不依赖任何 QGIS 坐标转换 API。

static const double s_dPI = 3.14159265358979323846;

/** 判断 QgsRectangle 坐标是否疑似为 Web 墨卡托（EPSG:3857）米制坐标 */
static bool rectLooksLikeWebMercator(const QgsRectangle& rect)
{
	// WGS84 经纬度范围：lon[-180,180], lat[-90,90]
	// 若任一值明显超出该范围，则认为坐标是投影坐标系（极大概率为 Web 墨卡托）
	return (rect.xMinimum() < -200.0 || rect.xMinimum() > 200.0 ||
	        rect.xMaximum() < -200.0 || rect.xMaximum() > 200.0 ||
	        rect.yMinimum() < -100.0 || rect.yMinimum() > 100.0 ||
	        rect.yMaximum() < -100.0 || rect.yMaximum() > 100.0);
}

/** 使用纯数学公式将 Web 墨卡托（EPSG:3857）米制坐标转为 WGS84（EPSG:4326）经纬度 */
static QgsRectangle webMercatorToWgs84(const QgsRectangle& mercator)
{
	// Web 墨卡托投影常数
	const double dRadius = 6378137.0;                    // 地球半径（米）
	const double dOriginShift = s_dPI * dRadius;         // = 20037508.342789244

	auto xToLon = [dOriginShift](double x) -> double {
		return x / dOriginShift * 180.0;
	};
	auto yToLat = [dRadius](double y) -> double {
		return atan(exp(y / dRadius)) * 360.0 / s_dPI - 90.0;
	};

	return QgsRectangle(xToLon(mercator.xMinimum()), yToLat(mercator.yMinimum()),
	                    xToLon(mercator.xMaximum()), yToLat(mercator.yMaximum()));
}

// ====================================================================
//  CSE_DataManagementDialog 实现
// ====================================================================

CSE_DataManagementDialog::CSE_DataManagementDialog(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
	, ui(new Ui::SeDataManagementDialog())
{
	ui->setupUi(this);

	// 【2026-08-24】恢复麒麟自适应缩放（更新版移除，本项目双平台保留）
	DialogFitHelper::install(this);

	// 列表下方的“加载到地图”仅保留在数据库图层导出页
	ui->Button_LoadToMap_Db->setVisible(false);

	setWindowTitle(tr("数据导出"));

	// 初始化 ButtonGroup 管理裁剪方式三选一
	m_pClipModeGroup = new QButtonGroup(this);
	m_pClipModeGroup->addButton(ui->radioButton_useExtent, 0);
	m_pClipModeGroup->addButton(ui->radioButton_useShp, 1);
	m_pClipModeGroup->addButton(ui->radioButton_useMainArea, 2);

	// “空间范围裁剪”与数据列表导出的“按范围导出”重复，已按需求移除该入口（隐藏），
	// 对应参数面板与主区裁切复用的执行路径仍保留。
	ui->radioButton_useExtent->setVisible(false);

	// 初始状态：默认选中按Shape条件导出页，裁剪选项面板默认隐藏（由 UI visible 控制）
	ui->radioButton_useShp->setChecked(true);
	ui->stackedWidget_clipParams->setCurrentIndex(1);

	// 初始化根据主区裁切控件
	initMainAreaClip();

	// 加载已有数据库连接
	refreshConnectionCombo();

	// ---- 从数据库连接配置 UI（DbConfigDialog）共享的注册表配置加载连接参数 ----
	// 【2026-08-24】更新版移除了该共享逻辑，本项目按需求保留：
	// 配置 UI 连接成功后把 host/port/database/schema/user/password 写入
	// QSettings("GarMap","MapProductManager")，这里读取同一批键，实现与配置 UI
	// 共享同一个数据库（与地图数据下载 UI mapdata_download 的做法一致）。
	// 不再读取 db_config.json（新配置 UI 已不再输出该文件）。
	{
		QSettings dbSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
		QString host = dbSettings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString();
		int port = dbSettings.value(QStringLiteral("db/port"), 5432).toInt();
		QString dbname = dbSettings.value(QStringLiteral("db/database"), QStringLiteral("map_products")).toString();
		QString user = dbSettings.value(QStringLiteral("db/user"), QStringLiteral("postgres")).toString();
		// 仅当配置 UI 勾选“保存密码”时才持久化密码，未勾选则留空由用户手动输入
		QString password;
		if (dbSettings.value(QStringLiteral("db/savePassword"), false).toBool())
			password = dbSettings.value(QStringLiteral("db/password")).toString();

		DatabaseConnectionInfo autoInfo;
		autoInfo.strName = QStringLiteral("默认连接(auto)");
		autoInfo.strDbType = "PostGIS";
		autoInfo.strHost = host;
		autoInfo.strPort = QString::number(port);
		autoInfo.strDbName = dbname;
		autoInfo.strUsername = user;
		autoInfo.strPassword = password;

		if (autoInfo.isValid()) {
			// 去重：检查是否已存在相同 host+port+dbname 的连接
			bool bExists = false;
			for (int i = 0; i < s_listConnections.size(); ++i) {
				const auto& conn = s_listConnections[i];
				if (conn.strHost == autoInfo.strHost
					&& conn.strPort == autoInfo.strPort
					&& conn.strDbName == autoInfo.strDbName) {
					bExists = true;
					break;
				}
			}
			if (!bExists) {
				s_listConnections.prepend(autoInfo);
				refreshConnectionCombo();
			}

			// 在下拉框中定位并选中自动连接
			for (int i = 0; i < ui->comboBox_connection->count(); ++i) {
				QVariant varData = ui->comboBox_connection->itemData(i);
				if (varData.isValid() && varData.canConvert<DatabaseConnectionInfo>()) {
					DatabaseConnectionInfo info = varData.value<DatabaseConnectionInfo>();
					if (info.strHost == autoInfo.strHost
						&& info.strPort == autoInfo.strPort
						&& info.strDbName == autoInfo.strDbName) {
						ui->comboBox_connection->setCurrentIndex(i);
						break;
					}
				}
			}
		}

		// ---- 自动测试连接并显示状态 ----
		{
			QString connName = "export_auto_test";
			{
				QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
				db.setHostName(host);
				db.setPort(port);
				db.setDatabaseName(dbname);
				db.setUserName(user);
				db.setPassword(password);
				if (db.open())
				{
					appendLog(tr("✓ 自动连接成功 (%1:%2/%3)")
						.arg(host).arg(port).arg(dbname));
					db.close();
				}
				else
				{
					appendLog(tr("✗ 自动连接失败 (%1:%2/%3): %4")
						.arg(host).arg(port).arg(dbname)
						.arg(db.lastError().text()));
				}
			}
			QSqlDatabase::removeDatabase(connName);
		}
	}

	// GDB 导出模式输出后缀约定
	// 由 browseOutputPath 根据当前 Tab 做适配

	// 初始验证按钮状态
	validateExportReady();

	// 进度条归零
	ui->progressBar_export->setValue(0);
	ui->progressBar_export->setFormat(tr("%v / %m"));

	connect(ui->Button_Cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void CSE_DataManagementDialog::selectMainAreaMode()
{
	if (!ui) return;
	ui->checkBox_enableClip->setChecked(true);
	ui->radioButton_useMainArea->setChecked(true);

	// 进入"按主区裁切"独立模式：隐藏其他裁切方式选项与对应裁剪面板，仅保留主区裁切一项
	// 这样这个对话框只剩"选择主区->选要素->输出"三块，操作更明确
	ui->radioButton_useShp->setVisible(false);
	// useExtent 在初始化中已被永久隐藏，这里再保证一次
	ui->radioButton_useExtent->setVisible(false);
	// 切到主区裁切参数页
	ui->stackedWidget_clipParams->setCurrentIndex(2);

	m_bMainAreaOnly = true;

	// 调整窗口尺寸，因为部分控件被隐藏后，紧凑的高度更合适
	this->adjustSize();
}

CSE_DataManagementDialog::~CSE_DataManagementDialog()
{
	if (m_pExtentPicker)
	{
		m_pExtentPicker->deactivate();
		delete m_pExtentPicker;
		m_pExtentPicker = nullptr;
	}

	closeDatabaseConnection();

	if (m_pMainAreaLayer)
	{
		delete m_pMainAreaLayer;
		m_pMainAreaLayer = nullptr;
	}

	delete ui;
}

void CSE_DataManagementDialog::setQgisInterface(QgisInterface* iface)
{
	m_pQgisIface = iface;

	// 初始化框选地图工具
	if (m_pQgisIface && m_pQgisIface->mapCanvas())
	{
		m_pExtentPicker = new CMapToolExtentPicker(m_pQgisIface->mapCanvas());
		connect(m_pExtentPicker, &CMapToolExtentPicker::extentSelected,
			this, &CSE_DataManagementDialog::onMapExtentSelected);
		connect(m_pExtentPicker, &CMapToolExtentPicker::cancelled,
			this, &CSE_DataManagementDialog::onMapExtentPickCancelled);
	}
}

void CSE_DataManagementDialog::addConnection(const DatabaseConnectionInfo& info)
{
	s_listConnections.append(info);
	// 注意：此方法可能在一个还没有实例化 UI 的时机被调用，
	// 此时 comboBox 不可用，直接操作静态列表即可。
	// 下次 dialog 实例化后，refreshConnectionCombo() 会自动加载。
}

void CSE_DataManagementDialog::clearConnections()
{
	s_listConnections.clear();
}

// ====================================================================
//  Tab 切换
// ====================================================================

void CSE_DataManagementDialog::on_tabWidget_source_currentChanged(int index)
{
    m_iCurrentTab = index;

    // 切换到新 Tab 时，整套面板恢复初始默认状态，三个 Tab 配置完全隔离
    resetCurrentTabPage();

    // 切换到「当前地图图层导出」Tab 时，自动获取当前地图已加载的矢量图层
    if (index == 0)
    {
        on_Button_UseMapLayers_clicked();
    }

    // 导出格式选择器仅在「数据库导出」模式下可见
    bool bDbMode = (index == 3);
    ui->label_dbFormat_extent->setVisible(bDbMode);
    ui->comboBox_dbFormat_extent->setVisible(bDbMode);
    ui->label_dbFormat_shp->setVisible(bDbMode);
    ui->comboBox_dbFormat_shp->setVisible(bDbMode);
    ui->label_dbFormat_mainArea->setVisible(bDbMode);
    ui->comboBox_dbFormat_mainArea->setVisible(bDbMode);

    updateLayerSummary();
    validateExportReady();
    ui->progressBar_export->setValue(0);

    appendLog(tr("已切换到 %1 模式").arg(ui->tabWidget_source->tabText(index)));
}

// ====================================================================
//  Tab 0: Shape / 文件夹导出
// ====================================================================

void CSE_DataManagementDialog::on_Button_BrowseSource_clicked()
{
	// 弹出文件选择对话框，支持多选文件；若用户只选一个文件夹，则按文件夹模式处理
	QStringList selectedFiles = QFileDialog::getOpenFileNames(this,
		tr("选择矢量文件（可多选）"),
		ui->lineEdit_srcPath->text(),
		tr("Shape 文件 (*.shp);;GeoPackage (*.gpkg);;所有矢量文件 (*.shp *.gpkg)"));

	if (!selectedFiles.isEmpty())
	{
		// 多选文件模式
		QString strFirstDir = QFileInfo(selectedFiles.first()).absolutePath();
		ui->lineEdit_srcPath->setText(strFirstDir);
		scanShapeSource(selectedFiles);
		return;
	}

	// 文件多选为空时，尝试选择文件夹
	QString selectedDir = QFileDialog::getExistingDirectory(this,
		tr("选择矢量文件夹（批量）"),
		ui->lineEdit_srcPath->text());

	if (selectedDir.isEmpty()) return;

	ui->lineEdit_srcPath->setText(selectedDir);
	scanShapeSource(selectedDir);
}

void CSE_DataManagementDialog::scanShapeSource(const QString& srcPath, bool bIsFolder)
{
	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();

	QStringList shpFiles;

	if (bIsFolder)
	{
		QDir dir(srcPath);
		QStringList nameFilters;
		nameFilters << "*.shp" << "*.gpkg";

		QFileInfoList entries = dir.entryInfoList(nameFilters, QDir::Files | QDir::Readable);
		for (const QFileInfo& fi : entries)
		{
			shpFiles.append(fi.absoluteFilePath());
		}

		// 也递归一级子目录
		QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
		for (const QFileInfo& subDir : subDirs)
		{
			QDir sd(subDir.absoluteFilePath());
			QFileInfoList subFiles = sd.entryInfoList(nameFilters, QDir::Files | QDir::Readable);
			for (const QFileInfo& fi : subFiles)
			{
				shpFiles.append(fi.absoluteFilePath());
			}
		}
	}
	else
	{
		// 单文件
		QFileInfo fi(srcPath);
		if (!fi.exists())
		{
			appendLog(tr("文件不存在：%1").arg(srcPath));
			return;
		}
		shpFiles.append(fi.absoluteFilePath());
	}

	parseShapeFiles(shpFiles);
}

void CSE_DataManagementDialog::scanShapeSource(const QStringList& fileList)
{
	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();

	QStringList shpFiles;
	for (const QString& fpath : fileList)
	{
		QFileInfo fi(fpath);
		if (fi.exists() && fi.isFile())
			shpFiles.append(fi.absoluteFilePath());
	}

	parseShapeFiles(shpFiles);
}

void CSE_DataManagementDialog::parseShapeFiles(const QStringList& shpFiles)
{
	if (shpFiles.isEmpty())
	{
		appendLog(tr("未找到任何矢量文件。"));
		return;
	}

	appendLog(tr("找到 %1 个矢量文件，正在解析...").arg(shpFiles.size()));

	GDALAllRegister();

	for (const QString& fpath : shpFiles)
	{
		GDALDatasetH hDS = GDALOpenEx(fpath.toUtf8().constData(),
			GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
		if (!hDS) continue;

		int nLayers = GDALDatasetGetLayerCount(hDS);
		for (int i = 0; i < nLayers; ++i)
		{
			OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
			if (!hLayer) continue;

			DbLayerInfo info;
			info.strSourcePath = fpath;

			const char* pszName = OGR_L_GetName(hLayer);
			info.strTableName = pszName ? QString::fromUtf8(pszName) : QFileInfo(fpath).baseName();

			OGRwkbGeometryType eType = OGR_L_GetGeomType(hLayer);
			info.strGeomType = QString::fromUtf8(OGRGeometryTypeToName(eType));

			// 坐标系
			OGRSpatialReferenceH hSRS = OGR_L_GetSpatialRef(hLayer);
			if (hSRS)
			{
				const char* pszAuth = OSRGetAuthorityName(hSRS, nullptr);
				const char* pszCode = OSRGetAuthorityCode(hSRS, nullptr);
				if (pszAuth && pszCode)
					info.strCrs = QString::fromUtf8(pszAuth) + ":" + QString::fromUtf8(pszCode);
				else
					info.strCrs = QString::fromUtf8(OSRGetName(hSRS));
			}
			else
			{
				info.strCrs = tr("无坐标系");
			}

			info.iFeatureCount = (long long)OGR_L_GetFeatureCount(hLayer, 1);
			info.strSchema = tr("文件");

			m_listAvailableLayers.append(info);
		}

		GDALClose(hDS);
	}

	refreshLayerTree();
	updateLayerSummary();
	validateExportReady();

	appendLog(tr("扫描完成：共解析 %1 个图层").arg(m_listAvailableLayers.size()));
}

void CSE_DataManagementDialog::on_Button_UseMapLayers_clicked()
{
	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();

	QMap<QString, QgsMapLayer*> mapLayers = QgsProject::instance()->mapLayers();
	if (mapLayers.isEmpty())
	{
		appendLog(tr("当前地图中未加载任何图层。"));
		return;
	}

	int iLayerCount = 0;
	int iVectorCount = 0;
	int iRasterCount = 0;
	for (auto it = mapLayers.constBegin(); it != mapLayers.constEnd(); ++it)
	{
		QgsMapLayer* pLayer = it.value();
		if (!pLayer)
			continue;

		DbLayerInfo info;
		info.strTableName = pLayer->name();
		info.strSchema = tr("地图图层");

		QgsCoordinateReferenceSystem crs = pLayer->crs();
		if (crs.isValid())
		{
			QString strAuth = crs.authid();
			if (!strAuth.isEmpty())
				info.strCrs = strAuth;
			else
				info.strCrs = crs.description();
		}
		else
		{
			info.strCrs = tr("无坐标系");
		}

		if (pLayer->type() == QgsMapLayerType::VectorLayer)
		{
			QgsVectorLayer* pVecLayer = qobject_cast<QgsVectorLayer*>(pLayer);
			if (!pVecLayer)
				continue;

			info.strSourcePath = pVecLayer->source();
			info.pLoadedLayer = pVecLayer;
			info.strGeomType = QString::fromUtf8(OGRGeometryTypeToName(
				(OGRwkbGeometryType)pVecLayer->geometryType()));
			info.iFeatureCount = pVecLayer->featureCount();

			m_listAvailableLayers.append(info);
			iVectorCount++;
			iLayerCount++;
		}
		else if (pLayer->type() == QgsMapLayerType::RasterLayer)
		{
			QgsRasterLayer* pRasterLayer = qobject_cast<QgsRasterLayer*>(pLayer);
			if (!pRasterLayer)
				continue;

			info.strSourcePath = pRasterLayer->source();
			info.pLoadedRasterLayer = pRasterLayer;
			info.strGeomType = QStringLiteral("RASTER");
			info.iFeatureCount = 1;

			m_listAvailableLayers.append(info);
			iRasterCount++;
			iLayerCount++;
		}
	}

	if (iLayerCount == 0)
	{
		appendLog(tr("当前地图中未加载任何矢量或栅格图层。"));
		return;
	}

	refreshLayerTree();
	updateLayerSummary();
	validateExportReady();

	appendLog(tr("已从地图加载图层：矢量 %1 个，栅格 %2 个。").arg(iVectorCount).arg(iRasterCount));
}

// ====================================================================
//  Tab 3: 当前地图图层导出
// ====================================================================

void CSE_DataManagementDialog::on_Button_UseMapLayers_Tab3_clicked()
{
	// 复用 Tab 0 的「使用地图已加载图层」逻辑：将当前地图中的矢量图层填充到共享图层列表
	on_Button_UseMapLayers_clicked();
}

// ====================================================================
//  Tab 1: GDB 文件导出
// ====================================================================

void CSE_DataManagementDialog::on_Button_BrowseGdb_clicked()
{
	QString selected = QFileDialog::getExistingDirectory(this,
		tr("选择 GDB 目录"),
		ui->lineEdit_gdbPath->text());

	if (selected.isEmpty()) return;

	ui->lineEdit_gdbPath->setText(selected);

	// 路径赋值完成后自动解析 GDB 内部图层
	loadGdbLayers(selected);
}

void CSE_DataManagementDialog::loadGdbLayers(const QString& gdbPath)
{
	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();

	GDALAllRegister();

	GDALDatasetH hDS = GDALOpenEx(gdbPath.toUtf8().constData(),
		GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);

	if (!hDS)
	{
		appendLog(tr("无法打开 GDB：%1").arg(gdbPath));
		return;
	}

	int nLayers = GDALDatasetGetLayerCount(hDS);
	if (nLayers == 0)
	{
		appendLog(tr("GDB 中没有矢量图层。"));
		GDALClose(hDS);
		return;
	}

	appendLog(tr("GDB 中共 %1 个图层，正在解析...").arg(nLayers));

	for (int i = 0; i < nLayers; ++i)
	{
		OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
		if (!hLayer) continue;

		DbLayerInfo info;
		info.strSourcePath = gdbPath + "|layername=" + QString::fromUtf8(OGR_L_GetName(hLayer));

		const char* pszName = OGR_L_GetName(hLayer);
		info.strTableName = pszName ? QString::fromUtf8(pszName) : QString::number(i);

		OGRwkbGeometryType eType = OGR_L_GetGeomType(hLayer);
		info.strGeomType = QString::fromUtf8(OGRGeometryTypeToName(eType));

		OGRSpatialReferenceH hSRS = OGR_L_GetSpatialRef(hLayer);
		if (hSRS)
		{
			const char* pszAuth = OSRGetAuthorityName(hSRS, nullptr);
			const char* pszCode = OSRGetAuthorityCode(hSRS, nullptr);
			if (pszAuth && pszCode)
				info.strCrs = QString::fromUtf8(pszAuth) + ":" + QString::fromUtf8(pszCode);
			else
				info.strCrs = tr("未知");
		}
		else
		{
			info.strCrs = tr("无坐标系");
		}

		info.iFeatureCount = (long long)OGR_L_GetFeatureCount(hLayer, 1);
		info.strSchema = tr("GDB");

		m_listAvailableLayers.append(info);
	}

	GDALClose(hDS);

	refreshLayerTree();
	updateLayerSummary();
	validateExportReady();

	appendLog(tr("GDB 解析完成：共 %1 个图层").arg(m_listAvailableLayers.size()));
}

// ====================================================================
//  Tab 2: 数据库图层导出
// ====================================================================

void CSE_DataManagementDialog::refreshConnectionCombo()
{
	ui->comboBox_connection->clear();
	for (const auto& conn : s_listConnections)
	{
		// 绑定完整连接配置到选项用户数据，不仅展示名称
		ui->comboBox_connection->addItem(conn.strName, QVariant::fromValue(conn));
	}

	if (s_listConnections.isEmpty())
	{
		ui->comboBox_connection->addItem(tr("(无可用连接)"));
	}
}

void CSE_DataManagementDialog::on_Button_NewConnection_clicked()
{
	CSE_DatabaseConnectionDialog dlg(this);
	if (dlg.exec() == QDialog::Rejected)
	{
		dlg.close();
		return;
	}
	auto connectionInfo = dlg.getConnectionInfo();
	dlg.close();

	s_listConnections.append(connectionInfo);
	refreshConnectionCombo();
	appendLog(tr("已添加数据库连接：%1").arg(connectionInfo.strName));
}

void CSE_DataManagementDialog::on_Button_DeleteConnection_clicked()
{
	int idx = ui->comboBox_connection->currentIndex();
	if (idx < 0 || idx >= s_listConnections.size()) return;

	QString name = s_listConnections[idx].strName;
	int ret = QMessageBox::question(this,
		tr("确认删除"),
		tr("确定要删除连接「%1」吗？").arg(name));

	if (ret != QMessageBox::Yes) return;

	s_listConnections.removeAt(idx);
	refreshConnectionCombo();
	appendLog(tr("已删除连接：%1").arg(name));
}

void CSE_DataManagementDialog::on_comboBox_connection_currentIndexChanged(int index)
{
	m_iCurrentConnectionIndex = -1;

	if (index < 0 || index >= ui->comboBox_connection->count())
		return;

	// 从选项 userData 读取完整连接配置，而不只是依赖索引
	QVariant varData = ui->comboBox_connection->itemData(index);
	if (!varData.isValid() || !varData.canConvert<DatabaseConnectionInfo>())
		return;

	DatabaseConnectionInfo info = varData.value<DatabaseConnectionInfo>();
	if (!info.isValid())
		return;

	m_iCurrentConnectionIndex = index;
	ui->label_connStatus->setText(tr("状态：已选择 %1").arg(info.strName));
}

void CSE_DataManagementDialog::on_Button_Connect_clicked()
{
	int idx = ui->comboBox_connection->currentIndex();
	if (idx < 0)
	{
		QMessageBox::information(this, tr("提示"), tr("请先选择一个数据库连接。"));
		return;
	}

	// 从下拉框当前选中项的 userData 读取完整连接配置
	QVariant varData = ui->comboBox_connection->itemData(idx);
	if (!varData.isValid() || !varData.canConvert<DatabaseConnectionInfo>())
	{
		QMessageBox::information(this, tr("提示"), tr("请先选择一个数据库连接。"));
		return;
	}

	DatabaseConnectionInfo info = varData.value<DatabaseConnectionInfo>();
	if (!info.isValid())
	{
		QMessageBox::information(this, tr("提示"), tr("请先选择一个数据库连接。"));
		return;
	}

	closeDatabaseConnection();

	// PostgreSQL 连接
	if (info.strDbType.toLower() == "postgresql" || info.strDbType.toLower() == "postgis")
	{
		m_dbConnection = QSqlDatabase::addDatabase("QPSQL", "data_management_connection");
		m_dbConnection.setHostName(info.strHost);
		m_dbConnection.setPort(info.strPort.toInt());
		m_dbConnection.setDatabaseName(info.strDbName);
		m_dbConnection.setUserName(info.strUsername);
		m_dbConnection.setPassword(info.strPassword);
	}

	if (!m_dbConnection.open())
	{
		appendLog(tr("数据库连接失败：%1").arg(m_dbConnection.lastError().text()));
		m_dbConnection = QSqlDatabase();
		QSqlDatabase::removeDatabase("data_management_connection");
		return;
	}

	m_bIsConnected = true;
	updateDbConnectionUI(true);

	appendLog(tr("数据库连接成功：%1@%2:%3/%4")
		.arg(info.strUsername, info.strHost, info.strPort, info.strDbName));

	ui->label_connStatus->setText(tr("已连接") + QString("  %1://%2:%3/%4")
		.arg(info.strDbType, info.strHost, info.strPort, info.strDbName));
	ui->label_connStatus->setStyleSheet("color:#27ae60; font-size:9pt;");

	loadDatabaseLayers();
}

void CSE_DataManagementDialog::on_Button_Disconnect_clicked()
{
	closeDatabaseConnection();
}

void CSE_DataManagementDialog::closeDatabaseConnection()
{
	if (m_dbConnection.isOpen())
	{
		m_dbConnection.close();
	}

	if (m_dbConnection.isValid())
	{
		QString connName = m_dbConnection.connectionName();
		m_dbConnection = QSqlDatabase();
		QSqlDatabase::removeDatabase(connName);
	}

	m_bIsConnected = false;
	updateDbConnectionUI(false);

	ui->label_connStatus->setText(tr("状态：未连接"));
	ui->label_connStatus->setStyleSheet("color:#e67e22; font-size:9pt;");

	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();
	updateLayerSummary();
	validateExportReady();
}

void CSE_DataManagementDialog::updateDbConnectionUI(bool bConnected)
{
	ui->Button_Connect->setEnabled(!bConnected);
	ui->Button_Disconnect->setEnabled(bConnected);
	ui->Button_NewConnection->setEnabled(!bConnected);
	ui->Button_DeleteConnection->setEnabled(!bConnected);
	ui->comboBox_connection->setEnabled(!bConnected);
	ui->lineEdit_dbFilter->setEnabled(bConnected);
	ui->Button_RefreshLayers->setEnabled(bConnected);
}

void CSE_DataManagementDialog::on_lineEdit_dbFilter_textChanged(const QString& text)
{
	Q_UNUSED(text)
	if (!m_bIsConnected) return;
	refreshLayerTree();
}

void CSE_DataManagementDialog::loadDatabaseLayers()
{
	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();

	if (!m_dbConnection.isOpen())
	{
		appendLog(tr("数据库未连接，无法加载图层列表。"));
		return;
	}

	// 查询 geometry_columns 获取矢量图层
	QString sql = R"(
		SELECT
			f_table_schema AS schema_name,
			f_table_name   AS table_name,
			type           AS geom_type,
			srid,
			''             AS crs_desc
		FROM geometry_columns
		ORDER BY f_table_schema, f_table_name
	)";

	QSqlQuery query(m_dbConnection);
	if (!query.exec(sql))
	{
		// 尝试 geography_columns（PostGIS 3.x 使用 geography_columns/geometry_columns 混合）
		sql = R"(
			SELECT
				f_table_schema AS schema_name,
				f_table_name   AS table_name,
				type           AS geom_type,
				srid,
				''             AS crs_desc
			FROM geometry_columns
			UNION ALL
			SELECT
				f_table_schema AS schema_name,
				f_table_name   AS table_name,
				type           AS geom_type,
				srid,
				''             AS crs_desc
			FROM geography_columns
			ORDER BY schema_name, table_name
		)";

		if (!query.exec(sql))
		{
			appendLog(tr("查询图层列表失败：%1").arg(query.lastError().text()));
			return;
		}
	}

	while (query.next())
	{
		DbLayerInfo info;
		info.strSchema = query.value("schema_name").toString();
		info.strTableName = query.value("table_name").toString();
		info.strGeomType = query.value("geom_type").toString();

		int srid = query.value("srid").toInt();
		info.strCrs = QString("EPSG:%1").arg(srid);

		// 查询要素数
		QSqlQuery cntQ(m_dbConnection);
		QString cntSql = QString("SELECT COUNT(*) FROM \"%1\".\"%2\"")
			.arg(info.strSchema, info.strTableName);
		if (cntQ.exec(cntSql) && cntQ.next())
			info.iFeatureCount = cntQ.value(0).toLongLong();

		m_listAvailableLayers.append(info);
	}

	// 同时查询 raster_columns 获取栅格图层
	QSqlQuery rastQ(m_dbConnection);
	QString rastSql = R"(
		SELECT
			r_table_schema AS schema_name,
			r_table_name   AS table_name,
			'RASTER'       AS geom_type,
			srid,
			''             AS crs_desc
		FROM raster_columns
		ORDER BY r_table_schema, r_table_name
	)";

	if (rastQ.exec(rastSql))
	{
		while (rastQ.next())
		{
			DbLayerInfo info;
			info.strSchema = rastQ.value("schema_name").toString();
			info.strTableName = rastQ.value("table_name").toString();
			info.strGeomType = "RASTER";
			info.strCrs = QString("EPSG:%1").arg(rastQ.value("srid").toInt());
			info.iFeatureCount = 0; // 栅格不统计要素数
			m_listAvailableLayers.append(info);
		}
	}

	refreshLayerTree();
	updateLayerSummary();
	validateExportReady();

	appendLog(tr("加载完成：%1 个图层（含矢量 %2、栅格 %3）")
		.arg(m_listAvailableLayers.size())
		.arg(m_listAvailableLayers.count())
		.arg(rastQ.exec(rastSql) ? 1 : 0));
}

// ====================================================================
//  公共图层列表操作
// ====================================================================

void CSE_DataManagementDialog::refreshLayerTree()
{
	ui->treeWidget_layers->clear();

	QString filter = ui->lineEdit_dbFilter->text().trimmed().toLower();

	for (const auto& info : m_listAvailableLayers)
	{
		// 数据库模式下应用过滤
		if (m_iCurrentTab == 3 && !filter.isEmpty())
		{
			if (!info.strTableName.toLower().contains(filter) &&
				!info.strSchema.toLower().contains(filter))
				continue;
		}

		QTreeWidgetItem* item = new QTreeWidgetItem();
		item->setText(0, info.strTableName);
		item->setText(1, info.strGeomType);
		item->setText(2, info.strCrs);
		item->setData(0, Qt::UserRole, QVariant::fromValue((void*)&info));
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(0, Qt::Checked); // 默认勾选

		ui->treeWidget_layers->addTopLevelItem(item);
	}

	// 列表不为空则调整列宽
	if (ui->treeWidget_layers->topLevelItemCount() > 0)
	{
		for (int c = 0; c < 3; ++c)
			ui->treeWidget_layers->resizeColumnToContents(c);
	}
}

void CSE_DataManagementDialog::updateLayerSummary()
{
	int nChecked = 0;
	for (int i = 0; i < ui->treeWidget_layers->topLevelItemCount(); ++i)
	{
		QTreeWidgetItem* item = ui->treeWidget_layers->topLevelItem(i);
		if (item && item->checkState(0) == Qt::Checked)
			++nChecked;
	}
	ui->label_summary->setText(tr("已选择 %1 个图层").arg(nChecked));

	// === 修改点 1：根据当前 Tab 和选中状态更新【加载到地图】按钮状态 ===
	bool bHasSelection = (nChecked > 0);
	ui->Button_LoadToMap_Db->setVisible(m_iCurrentTab == 3);
	switch (m_iCurrentTab)
	{
	case 0: ui->Button_LoadToMap_Shape->setEnabled(bHasSelection); break;
	case 1: ui->Button_LoadToMap_Gdb->setEnabled(bHasSelection); break;
	case 2: ui->Button_LoadToMap_Db->setEnabled(bHasSelection && m_bIsConnected); break;
	}
}

void CSE_DataManagementDialog::on_Button_SelectAll_clicked()
{
	for (int i = 0; i < ui->treeWidget_layers->topLevelItemCount(); ++i)
	{
		QTreeWidgetItem* item = ui->treeWidget_layers->topLevelItem(i);
		if (item) item->setCheckState(0, Qt::Checked);
	}
	updateLayerSummary();
	validateExportReady();
}

void CSE_DataManagementDialog::on_Button_SelectNone_clicked()
{
	for (int i = 0; i < ui->treeWidget_layers->topLevelItemCount(); ++i)
	{
		QTreeWidgetItem* item = ui->treeWidget_layers->topLevelItem(i);
		if (item) item->setCheckState(0, Qt::Unchecked);
	}
	updateLayerSummary();
	validateExportReady();
}

void CSE_DataManagementDialog::on_Button_RefreshLayers_clicked()
{
	if (m_iCurrentTab == 3 && m_bIsConnected)
	{
		loadDatabaseLayers();
	}
	else if (m_iCurrentTab == 2)
	{
		QString gdbPath = ui->lineEdit_gdbPath->text().trimmed();
		if (!gdbPath.isEmpty())
			loadGdbLayers(gdbPath);
	}
	else if (m_iCurrentTab == 1)
	{
		QString srcPath = ui->lineEdit_srcPath->text().trimmed();
		if (!srcPath.isEmpty())
			scanShapeSource(srcPath, true);
	}
}

void CSE_DataManagementDialog::on_treeWidget_layers_itemChanged(QTreeWidgetItem* item, int column)
{
	Q_UNUSED(item)
	Q_UNUSED(column)

	// 勾选/取消勾选图层时，实时刷新【加载到地图】按钮可用状态
	updateLayerSummary();
	validateExportReady();
}

// ====================================================================
//  修改点 1：【加载到地图】按钮实现
// ====================================================================

void CSE_DataManagementDialog::loadSelectedLayersToMap()
{
	if (!m_pQgisIface)
		return;

	QList<DbLayerInfo> selected = getSelectedLayers();
	if (selected.isEmpty())
	{
		QMessageBox::information(this, tr("提示"), tr("请先勾选要加载的图层。"));
		return;
	}

	int nLoaded = 0;
	for (const auto& layer : selected)
	{
		QString layerName = layer.strTableName.isEmpty() ? layer.strTableName : layer.strTableName;

		// 数据库模式：检测栅格图层
		if (m_iCurrentTab == 3 && layer.strGeomType == "RASTER")
		{
			// 栅格图层 → 通过 GDAL PostGIS Raster 加载
			QString rasterUri = buildPgRasterConnStr(layer);
			QgsRasterLayer* pRasterLayer = new QgsRasterLayer(rasterUri, layerName);
			if (pRasterLayer && pRasterLayer->isValid())
			{
				QgsProject::instance()->addMapLayer(pRasterLayer, true);
				++nLoaded;
			}
			else
			{
				delete pRasterLayer;
				appendLog(tr("警告：栅格图层 %1 加载失败").arg(layer.strTableName));
			}
			continue;
		}

		// 当前地图图层模式：复用已加载的栅格图层
		if (m_iCurrentTab == 0 && layer.strGeomType == "RASTER")
		{
			if (layer.pLoadedRasterLayer && layer.pLoadedRasterLayer->isValid())
			{
				if (QgsProject::instance()->mapLayer(layer.pLoadedRasterLayer->id()))
				{
					appendLog(tr("栅格图层 %1 已在地图中").arg(layer.strTableName));
				}
				else
				{
					QgsProject::instance()->addMapLayer(layer.pLoadedRasterLayer, true);
					++nLoaded;
				}
			}
			else
			{
				appendLog(tr("警告：栅格图层 %1 加载失败").arg(layer.strTableName));
			}
			continue;
		}

		QgsVectorLayer* pLayer = nullptr;

		if (m_iCurrentTab == 3)
		{
			// 数据库模式：通过 PostGIS URI 加载矢量
			QString uri = buildPgUri(layer);
			layerName = layer.strTableName;
			pLayer = new QgsVectorLayer(uri, layerName, "postgres");
		}
		else if (m_iCurrentTab == 2)
		{
			// GDB 模式：strSourcePath 格式为 "gdbPath|layername=xxx"
			pLayer = new QgsVectorLayer(layer.strSourcePath, layer.strTableName, "ogr");
		}
		else
		{
			// Shape/文件夹模式：优先复用已加载图层
			if (layer.pLoadedLayer && layer.pLoadedLayer->isValid())
			{
				pLayer = layer.pLoadedLayer;
			}
			else
			{
				pLayer = new QgsVectorLayer(layer.strSourcePath, layer.strTableName, "ogr");
			}
		}

		if (pLayer && pLayer->isValid())
		{
			// 若为复用已加载图层且已在项目中，则不重复添加
			if (layer.pLoadedLayer == pLayer &&
				QgsProject::instance()->mapLayer(pLayer->id()))
			{
				appendLog(tr("图层 %1 已在地图中").arg(layer.strTableName));
				continue;
			}
			QgsProject::instance()->addMapLayer(pLayer, true);
			++nLoaded;
		}
		else if (pLayer)
		{
			if (layer.pLoadedLayer != pLayer)
				delete pLayer;
			appendLog(tr("警告：图层 %1 加载失败").arg(layer.strTableName));
		}
	}

	if (nLoaded > 0)
	{
		m_pQgisIface->mapCanvas()->refresh();
		appendLog(tr("已加载 %1 个图层到地图").arg(nLoaded));
	}
}

void CSE_DataManagementDialog::on_Button_LoadToMap_Shape_clicked()
{
	loadSelectedLayersToMap();
}

void CSE_DataManagementDialog::on_Button_LoadToMap_Gdb_clicked()
{
	loadSelectedLayersToMap();
}

void CSE_DataManagementDialog::on_Button_LoadToMap_Db_clicked()
{
	loadSelectedLayersToMap();
}

// ====================================================================
//  导出完成后自动加载结果图层到当前地图
// ====================================================================

void CSE_DataManagementDialog::loadExportedResultToMap(const DbLayerInfo& layer, const QString& dstPath)
{
	if (!m_pQgisIface)
		return;

	QFileInfo fi(dstPath);
	QString suffix = fi.suffix().toLower();
	QString layerName = layer.strTableName;

	// 栅格结果（GeoTIFF）→ 使用 QgsRasterLayer 加载
	if (suffix == "tif" || suffix == "tiff")
	{
		QgsRasterLayer* pRasterLayer = new QgsRasterLayer(dstPath, fi.completeBaseName());
		if (pRasterLayer && pRasterLayer->isValid())
		{
			QgsProject::instance()->addMapLayer(pRasterLayer, true);
			appendLog(tr("    ✓ 已加载栅格结果到地图：%1").arg(layerName));
		}
		else
		{
			delete pRasterLayer;
			appendLog(tr("    [警告] 栅格结果加载失败：%1").arg(layerName));
		}
		return;
	}

    QgsVectorLayer* pLayer = nullptr;

    if (m_iCurrentTab == 2 ||
        (m_iCurrentTab == 3 && currentFormatCombo()->currentText().contains("GDB")))
    {
        // GDB 模式 / 数据库导出为 GDB：dstPath 为 .gdb 文件夹，加载内部对应图层
		QString uri = dstPath + "|layername=" + layer.strTableName;
		pLayer = new QgsVectorLayer(uri, layerName, "ogr");
	}
	else
	{
		// Shape / 文件夹 / 数据库模式：导出结果均为文件，直接加载 dstPath
		QString displayName = fi.completeBaseName();
		pLayer = new QgsVectorLayer(dstPath, displayName, "ogr");
	}

	if (pLayer && pLayer->isValid())
	{
		QgsProject::instance()->addMapLayer(pLayer, true);
		appendLog(tr("    ✓ 已加载结果到地图：%1").arg(layerName));
	}
	else if (pLayer)
	{
		delete pLayer;
		appendLog(tr("    [警告] 导出结果加载失败：%1").arg(layerName));
	}
}

void CSE_DataManagementDialog::validateExportReady()
{
	bool bReady = false;

	switch (m_iCurrentTab)
	{
	case 1: // Shape
		bReady = (m_listAvailableLayers.size() > 0);
		break;
	case 2: // GDB
		bReady = (m_listAvailableLayers.size() > 0);
		break;
	case 3: // Database
		bReady = (m_bIsConnected && m_listAvailableLayers.size() > 0);
		break;
	case 0: // 当前地图图层导出
		bReady = (m_listAvailableLayers.size() > 0);
		break;
	}

	if (bReady)
	{
		int nChecked = 0;
		for (int i = 0; i < ui->treeWidget_layers->topLevelItemCount(); ++i)
		{
			QTreeWidgetItem* item = ui->treeWidget_layers->topLevelItem(i);
			if (item && item->checkState(0) == Qt::Checked)
				++nChecked;
		}
		if (nChecked == 0) bReady = false;
	}

	// 输出路径不能为空（Tab0、1 必填，Tab2 暂不强制在验证阶段校验）
	if (bReady && currentOutputPathEdit()->text().trimmed().isEmpty())
		bReady = false;

	ui->Button_Export->setEnabled(bReady);
}

// ====================================================================
//  裁剪面板
// ====================================================================

void CSE_DataManagementDialog::on_checkBox_enableClip_toggled(bool checked)
{
	// 勾选/取消勾选时，显示/隐藏裁剪方式选项和对应参数面板
	ui->widget_clipOptions->setVisible(checked);
	ui->stackedWidget_clipParams->setVisible(checked);

	if (checked)
	{
		// 显示时根据当前页面内容调整堆叠面板高度，避免默认统一成最大页面高度
		on_stackedWidget_clipParams_currentChanged(ui->stackedWidget_clipParams->currentIndex());
	}
}

void CSE_DataManagementDialog::on_radioButton_useExtent_toggled(bool checked)
{
	if (checked)
	{
		ui->stackedWidget_clipParams->setCurrentIndex(0);
		ui->progressBar_export->setValue(0);
		appendLog(tr("裁切模式已切换：空间范围裁剪"));
	}
}

void CSE_DataManagementDialog::on_radioButton_useShp_toggled(bool checked)
{
	if (checked)
	{
		ui->stackedWidget_clipParams->setCurrentIndex(1);
		ui->progressBar_export->setValue(0);
		appendLog(tr("导出模式已切换：按Shape条件导出"));
	}
}

void CSE_DataManagementDialog::on_radioButton_useMainArea_toggled(bool checked)
{
	if (checked)
	{
		ui->stackedWidget_clipParams->setCurrentIndex(2);
		ui->progressBar_export->setValue(0);
		appendLog(tr("导出模式已切换：按主区裁切导出"));
	}
}

void CSE_DataManagementDialog::on_stackedWidget_clipParams_currentChanged(int index)
{
	Q_UNUSED(index)

	QWidget* currentPage = ui->stackedWidget_clipParams->currentWidget();
	if (!currentPage)
		return;

	// 让裁剪参数面板高度跟随当前页面内容自适应，
	// 取消三种裁剪面板强制统一成最大高度的默认行为
	ui->stackedWidget_clipParams->setFixedHeight(currentPage->sizeHint().height());
}

void CSE_DataManagementDialog::setSpatialFilterEnabled(bool enabled)
{
	ui->lineEdit_minLon->setEnabled(enabled);
	ui->lineEdit_maxLon->setEnabled(enabled);
	ui->lineEdit_minLat->setEnabled(enabled);
	ui->lineEdit_maxLat->setEnabled(enabled);
	ui->Button_GetCanvasExtent->setEnabled(enabled);
	ui->Button_ClearExtent->setEnabled(enabled);
	ui->Button_PickExtentFromMap->setEnabled(enabled);
	ui->label_extentSummary->setEnabled(enabled);
}

void CSE_DataManagementDialog::setShpClipEnabled(bool enabled)
{
	ui->lineEdit_clipShpPath->setEnabled(enabled);
	ui->Button_BrowseClipShp->setEnabled(enabled);
	ui->Button_LoadClipShpToMap->setEnabled(enabled);
	ui->comboBox_clipAttrField->setEnabled(enabled);
	ui->lineEdit_clipAttrValue->setEnabled(enabled);
}

void CSE_DataManagementDialog::setMainAreaClipEnabled(bool enabled)
{
	ui->widget_mainAreaPanel->setEnabled(enabled);
}

QLineEdit* CSE_DataManagementDialog::currentOutputPathEdit() const
{
	if (ui->radioButton_useMainArea->isChecked())
		return ui->lineEdit_outputPath_mainArea;
	return ui->lineEdit_outputPath_shp;
}

QComboBox* CSE_DataManagementDialog::currentEncodingCombo() const
{
	if (ui->radioButton_useMainArea->isChecked())
		return ui->comboBox_encoding_mainArea;
	return ui->comboBox_encoding_shp;
}

QCheckBox* CSE_DataManagementDialog::currentOverwriteCheck() const
{
    if (ui->radioButton_useMainArea->isChecked())
        return ui->checkBox_overwrite_mainArea;
    return ui->checkBox_overwrite_shp;
}

QComboBox* CSE_DataManagementDialog::currentFormatCombo() const
{
    if (ui->radioButton_useMainArea->isChecked())
        return ui->comboBox_dbFormat_mainArea;
    return ui->comboBox_dbFormat_shp;
}

void CSE_DataManagementDialog::resetMainAreaClip()
{
	// 释放当前主区图层
	if (m_pMainAreaLayer)
	{
		delete m_pMainAreaLayer;
		m_pMainAreaLayer = nullptr;
	}

	m_mainAreaSelectedFid = -1;
	m_mainAreaSelectedExtent.setMinimal();
	m_mainAreaClipExtentWgs84.setMinimal();
	m_bMainAreaClipExtentValid = false;

	ui->lineEdit_mainAreaPath->clear();
	ui->comboBox_mainAreaField->clear();
	ui->listWidget_mainAreaFeatures->clear();
	ui->lineEdit_actualScale->clear();
	ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：未选择"));

	ui->comboBox_paperSize->setCurrentIndex(2);          // 默认 A2
	ui->comboBox_paperOrientation->setCurrentIndex(0);   // 默认自动
	ui->comboBox_standardScale->setCurrentText("10000"); // 默认标准比例尺

	// 自定义纸张尺寸
	ui->lineEdit_customPaperWidth->clear();
	ui->lineEdit_customPaperHeight->clear();
	ui->label_customPaperWidth->setVisible(false);
	ui->lineEdit_customPaperWidth->setVisible(false);
	ui->label_customPaperHeight->setVisible(false);
	ui->lineEdit_customPaperHeight->setVisible(false);

	// 自定义比例尺
	ui->checkBox_useCustomScale->setChecked(false);
	ui->lineEdit_actualScale->setEnabled(false);

	// 内图廓
	ui->checkBox_enableInnerClip->setChecked(false);
	ui->lineEdit_innerClipWidth->setText("0");
	ui->lineEdit_innerClipHeight->setText("0");
	ui->lineEdit_innerClipWidth->setEnabled(false);
	ui->lineEdit_innerClipHeight->setEnabled(false);
}

void CSE_DataManagementDialog::resetCurrentTabPage()
{
	// ---- 源输入区域 ----
	ui->lineEdit_srcPath->clear();
	ui->lineEdit_gdbPath->clear();

	// 数据库连接相关
	{
		QSignalBlocker blocker(ui->comboBox_connection);
		ui->comboBox_connection->setCurrentIndex(0);
	}
	// 同步当前连接索引：默认显示第一条历史连接时，应当认为已选中该连接
	on_comboBox_connection_currentIndexChanged(ui->comboBox_connection->currentIndex());
	ui->lineEdit_dbFilter->clear();
	if (m_bIsConnected)
	{
		closeDatabaseConnection();
	}
	else
	{
		ui->label_connStatus->setText(tr("状态：未连接"));
		ui->label_connStatus->setStyleSheet("color:#e67e22; font-size:9pt;");
		updateDbConnectionUI(false);
	}

	// ---- 图层列表 ----
	m_listAvailableLayers.clear();
	ui->treeWidget_layers->clear();

	// ---- 裁剪面板 ----
	ui->checkBox_enableClip->setChecked(false);
	ui->stackedWidget_clipParams->setCurrentIndex(1);
	ui->radioButton_useShp->setChecked(true);
	ui->lineEdit_minLon->clear();
	ui->lineEdit_maxLon->clear();
	ui->lineEdit_minLat->clear();
	ui->lineEdit_maxLat->clear();
	ui->label_extentSummary->setText(tr("已选范围：未选择"));
	ui->lineEdit_clipShpPath->clear();
	ui->comboBox_clipAttrField->clear();
	ui->lineEdit_clipAttrValue->clear();

	// ---- 主区裁切 ----
	resetMainAreaClip();

	// ---- 输出设置 ----
	ui->lineEdit_outputPath_extent->clear();
    ui->comboBox_encoding_extent->setCurrentIndex(0);   // UTF-8
    ui->checkBox_overwrite_extent->setChecked(true);
    ui->comboBox_dbFormat_extent->setCurrentIndex(0);
    ui->lineEdit_outputPath_shp->clear();
    ui->comboBox_encoding_shp->setCurrentIndex(0);       // UTF-8
    ui->checkBox_overwrite_shp->setChecked(true);
    ui->comboBox_dbFormat_shp->setCurrentIndex(0);
    ui->lineEdit_outputPath_mainArea->clear();
    ui->comboBox_encoding_mainArea->setCurrentIndex(0);  // UTF-8
    ui->checkBox_overwrite_mainArea->setChecked(true);
    ui->comboBox_dbFormat_mainArea->setCurrentIndex(0);
}

void CSE_DataManagementDialog::initMainAreaClip()
{
	ui->comboBox_paperSize->setCurrentIndex(2); // 默认 A2
	ui->comboBox_paperOrientation->setCurrentIndex(0); // 默认自动

	// 自定义纸张尺寸控件初始隐藏
	ui->label_customPaperWidth->setVisible(false);
	ui->lineEdit_customPaperWidth->setVisible(false);
	ui->label_customPaperHeight->setVisible(false);
	ui->lineEdit_customPaperHeight->setVisible(false);

	// 在纸张尺寸下拉中追加 "自定义" 选项，点击该选项时弹出长宽输入框
	int n = ui->comboBox_paperSize->count();
	if (n > 0 && ui->comboBox_paperSize->itemText(n - 1) != tr("自定义"))
		ui->comboBox_paperSize->addItem(tr("自定义"));

	// 自定义比例尺初始未启用
	ui->checkBox_useCustomScale->setChecked(false);
	ui->lineEdit_actualScale->setEnabled(false);

	// 内图廓初始未启用
	ui->checkBox_enableInnerClip->setChecked(false);
	ui->lineEdit_innerClipWidth->setEnabled(false);
	ui->lineEdit_innerClipHeight->setEnabled(false);

	QStringList listScale;
	listScale << "100" << "200" << "250" << "500" << "1000" << "2000" << "2500"
			  << "5000" << "10000" << "20000" << "25000" << "30000" << "40000"
			  << "50000" << "100000" << "200000" << "250000" << "500000"
			  << "1000000" << "2000000" << "5000000" << "10000000";
	ui->comboBox_standardScale->clear();
	ui->comboBox_standardScale->addItems(listScale);
	ui->comboBox_standardScale->setCurrentText("10000");
}

void CSE_DataManagementDialog::on_Button_BrowseMainArea_clicked()
{
	QString strFilter = tr("矢量数据 (*.shp);;所有文件 (*.*)");
	QString strPath = QFileDialog::getOpenFileName(this, tr("选择主区矢量数据"),
		ui->lineEdit_mainAreaPath->text(), strFilter);
	if (strPath.isEmpty())
		return;

	ui->lineEdit_mainAreaPath->setText(strPath);
	loadMainAreaLayer(strPath);
}

void CSE_DataManagementDialog::loadMainAreaLayer(const QString& path)
{
	// 释放旧图层
	if (m_pMainAreaLayer)
	{
		delete m_pMainAreaLayer;
		m_pMainAreaLayer = nullptr;
	}

	m_mainAreaSelectedFid = -1;
	m_mainAreaSelectedExtent.setMinimal();
	m_mainAreaClipExtentWgs84.setMinimal();
	m_bMainAreaClipExtentValid = false;
	ui->comboBox_mainAreaField->clear();
	ui->listWidget_mainAreaFeatures->clear();
	ui->lineEdit_actualScale->clear();
	ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：未选择"));

	QFileInfo fileInfo(path);
	if (!fileInfo.exists())
	{
		appendLog(tr("[错误] 主区数据文件不存在：%1").arg(path));
		return;
	}

	QString strUri = path;
	m_pMainAreaLayer = new QgsVectorLayer(strUri, fileInfo.baseName(), "ogr");
	if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
	{
		appendLog(tr("[错误] 无法加载主区矢量数据：%1").arg(path));
		if (m_pMainAreaLayer)
		{
			delete m_pMainAreaLayer;
			m_pMainAreaLayer = nullptr;
		}
		return;
	}

	// 填充标识字段下拉框（优先字符串、整型字段）
	const QgsFields fields = m_pMainAreaLayer->fields();
	int iDefaultIndex = -1;
	for (int i = 0; i < fields.count(); ++i)
	{
		QVariant::Type eType = fields.at(i).type();
		if (eType == QVariant::String || eType == QVariant::Int || eType == QVariant::LongLong)
		{
			ui->comboBox_mainAreaField->addItem(fields.at(i).name(), i);
			if (iDefaultIndex < 0 && eType == QVariant::String)
				iDefaultIndex = ui->comboBox_mainAreaField->count() - 1;
		}
	}

	if (ui->comboBox_mainAreaField->count() == 0)
	{
		for (int i = 0; i < fields.count(); ++i)
			ui->comboBox_mainAreaField->addItem(fields.at(i).name(), i);
	}

	if (iDefaultIndex >= 0)
		ui->comboBox_mainAreaField->setCurrentIndex(iDefaultIndex);
	else if (ui->comboBox_mainAreaField->count() > 0)
		ui->comboBox_mainAreaField->setCurrentIndex(0);

	refreshMainAreaFeatureList();

	appendLog(tr("[信息] 已加载主区数据：%1，共 %2 个要素")
			  .arg(fileInfo.baseName()).arg(m_pMainAreaLayer->featureCount()));
}

void CSE_DataManagementDialog::on_comboBox_mainAreaField_currentIndexChanged(int index)
{
	Q_UNUSED(index)
	refreshMainAreaFeatureList();
}

void CSE_DataManagementDialog::refreshMainAreaFeatureList()
{
	ui->listWidget_mainAreaFeatures->clear();
	m_mainAreaSelectedFid = -1;
	m_mainAreaSelectedExtent.setMinimal();
	m_mainAreaClipExtentWgs84.setMinimal();
	m_bMainAreaClipExtentValid = false;

	if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
		return;

	int iFieldIndex = ui->comboBox_mainAreaField->currentData().toInt();
	if (iFieldIndex < 0 || iFieldIndex >= m_pMainAreaLayer->fields().count())
		return;

	QgsFeatureIterator it = m_pMainAreaLayer->getFeatures(
		QgsFeatureRequest()
			.setFlags(QgsFeatureRequest::NoGeometry)
			.setSubsetOfAttributes(QgsAttributeList() << iFieldIndex));
	QgsFeature feature;
	while (it.nextFeature(feature))
	{
		QVariant varValue = feature.attribute(iFieldIndex);
		QString strText = varValue.isNull() ? tr("<空>") : varValue.toString();
		QListWidgetItem* pItem = new QListWidgetItem(strText, ui->listWidget_mainAreaFeatures);
		pItem->setData(Qt::UserRole, QVariant(static_cast<qlonglong>(feature.id())));
		ui->listWidget_mainAreaFeatures->addItem(pItem);
	}

	if (ui->listWidget_mainAreaFeatures->count() > 0)
		ui->listWidget_mainAreaFeatures->setCurrentRow(0);
}

void CSE_DataManagementDialog::on_listWidget_mainAreaFeatures_itemSelectionChanged()
{
	updateMainAreaFeatureExtent();
	calculateMainAreaScaleAndExtent();
}

void CSE_DataManagementDialog::updateMainAreaFeatureExtent()
{
	m_mainAreaSelectedFid = -1;
	m_mainAreaSelectedExtent.setMinimal();

	if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
		return;

	QListWidgetItem* pItem = ui->listWidget_mainAreaFeatures->currentItem();
	if (!pItem)
		return;

	QgsFeatureId fid = static_cast<QgsFeatureId>(pItem->data(Qt::UserRole).toLongLong());
	QgsFeatureRequest request;
	request.setFilterFid(fid);
	request.setNoAttributes();

	QgsFeature feature;
	if (!m_pMainAreaLayer->getFeatures(request).nextFeature(feature))
		return;

	if (!feature.hasGeometry())
		return;

	m_mainAreaSelectedFid = fid;
	m_mainAreaSelectedExtent = feature.geometry().boundingBox();
}

void CSE_DataManagementDialog::on_comboBox_paperSize_currentIndexChanged(int index)
{
	Q_UNUSED(index)
	bool bIsCustom = (ui->comboBox_paperSize->currentText() == tr("自定义"));
	ui->label_customPaperWidth->setVisible(bIsCustom);
	ui->lineEdit_customPaperWidth->setVisible(bIsCustom);
	ui->label_customPaperHeight->setVisible(bIsCustom);
	ui->lineEdit_customPaperHeight->setVisible(bIsCustom);
	calculateMainAreaScaleAndExtent();
}

void CSE_DataManagementDialog::on_comboBox_paperOrientation_currentIndexChanged(int index)
{
	Q_UNUSED(index)
	calculateMainAreaScaleAndExtent();
}

void CSE_DataManagementDialog::on_Button_CalculateScale_clicked()
{
	calculateMainAreaScaleAndExtent();
}

void CSE_DataManagementDialog::on_checkBox_useCustomScale_toggled(bool checked)
{
	ui->lineEdit_actualScale->setEnabled(checked);
	if (!checked)
	{
		// 取消勾选时重新自动计算
		calculateMainAreaScaleAndExtent();
	}
}

void CSE_DataManagementDialog::on_checkBox_enableInnerClip_toggled(bool checked)
{
	ui->lineEdit_innerClipWidth->setEnabled(checked);
	ui->lineEdit_innerClipHeight->setEnabled(checked);
	calculateMainAreaScaleAndExtent();
}

void CSE_DataManagementDialog::calculateMainAreaScaleAndExtent()
{
	m_mainAreaClipExtentWgs84.setMinimal();
	m_bMainAreaClipExtentValid = false;

	if (!ui->checkBox_useCustomScale->isChecked())
		ui->lineEdit_actualScale->clear();

	if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid() || m_mainAreaSelectedFid < 0 ||
		!m_mainAreaSelectedExtent.isFinite())
	{
		ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：未选择"));
		return;
	}

	// 纸张与图廓尺寸（单位：毫米）
	struct PaperInfo
	{
		double dFrameW;
		double dFrameH;
	};
	static const QMap<QString, PaperInfo> mapPaper = {
		{"A0", {893.0, 654.0}},
		{"A1", {677.0, 496.0}},
		{"A2", {544.0, 350.0}},
		{"A3", {374.0, 251.0}}
	};

	QString strPaper = ui->comboBox_paperSize->currentText();
	double dFrameWidthMM = 0.0;
	double dFrameHeightMM = 0.0;
	bool bLandscape = true;

	if (strPaper == tr("自定义"))
	{
		dFrameWidthMM = ui->lineEdit_customPaperWidth->text().toDouble();
		dFrameHeightMM = ui->lineEdit_customPaperHeight->text().toDouble();
		if (dFrameWidthMM <= 0.0 || dFrameHeightMM <= 0.0)
		{
			ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：自定义纸张尺寸无效"));
			return;
		}

		QString strOrientation = ui->comboBox_paperOrientation->currentText();
		if (strOrientation == tr("横向"))
			bLandscape = true;
		else if (strOrientation == tr("纵向"))
			bLandscape = false;
		else
		{
			double dBboxWidth = m_mainAreaSelectedExtent.width();
			double dBboxHeight = m_mainAreaSelectedExtent.height();
			bLandscape = dBboxWidth >= dBboxHeight;
		}

		if (!bLandscape)
			qSwap(dFrameWidthMM, dFrameHeightMM);
	}
	else
	{
		if (!mapPaper.contains(strPaper))
		{
			ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：纸张类型无效"));
			return;
		}

		const PaperInfo& info = mapPaper.value(strPaper);
		QString strOrientation = ui->comboBox_paperOrientation->currentText();
		if (strOrientation == tr("横向"))
			bLandscape = true;
		else if (strOrientation == tr("纵向"))
			bLandscape = false;
		else
		{
			double dBboxWidth = m_mainAreaSelectedExtent.width();
			double dBboxHeight = m_mainAreaSelectedExtent.height();
			bLandscape = dBboxWidth >= dBboxHeight;
		}

		dFrameWidthMM = bLandscape ? info.dFrameW : info.dFrameH;
		dFrameHeightMM = bLandscape ? info.dFrameH : info.dFrameW;
	}

	// 比例尺（分母）
	double dActualScale = 0.0;
	if (ui->checkBox_useCustomScale->isChecked())
	{
		dActualScale = ui->lineEdit_actualScale->text().toDouble();
		if (dActualScale <= 0.0)
		{
			ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：自定义比例尺无效"));
			return;
		}
	}
	else
	{
		double dScaleW = m_mainAreaSelectedExtent.width() / dFrameWidthMM;
		double dScaleH = m_mainAreaSelectedExtent.height() / dFrameHeightMM;
		dActualScale = qMax(dScaleW, dScaleH);
		if (dActualScale <= 0.0)
			return;

		ui->lineEdit_actualScale->setText(QString::number(dActualScale, 'f', 4));
	}

	int iStandardScale = roundUpToStandardScale(dActualScale);
	if (iStandardScale <= 0)
		return;

	ui->comboBox_standardScale->setCurrentText(QString::number(iStandardScale));

	// 内图廓大小
	double dInnerFrameW = dFrameWidthMM;
	double dInnerFrameH = dFrameHeightMM;
	if (ui->checkBox_enableInnerClip->isChecked())
	{
		dInnerFrameW = ui->lineEdit_innerClipWidth->text().toDouble();
		dInnerFrameH = ui->lineEdit_innerClipHeight->text().toDouble();
	}

	if (dInnerFrameW > dFrameWidthMM || dInnerFrameH > dFrameHeightMM)
	{
		ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：内图廓尺寸超过纸张大小"));
		return;
	}
	if (dInnerFrameW <= 0.0 || dInnerFrameH <= 0.0)
	{
		ui->label_mainAreaExtentSummary->setText(tr("已选主区范围：内图廓尺寸无效"));
		return;
	}

	double dActualFrameW = dInnerFrameW;
	double dActualFrameH = dInnerFrameH;

	// 计算以主区外接矩形中心为图廓中心的裁切范围（图层坐标系）
	double dCenterX = m_mainAreaSelectedExtent.center().x();
	double dCenterY = m_mainAreaSelectedExtent.center().y();
	double dHalfW = (dActualFrameW * iStandardScale) / 2.0;
	double dHalfH = (dActualFrameH * iStandardScale) / 2.0;

	QgsRectangle rectClip;
	rectClip.setXMinimum(dCenterX - dHalfW);
	rectClip.setXMaximum(dCenterX + dHalfW);
	rectClip.setYMinimum(dCenterY - dHalfH);
	rectClip.setYMaximum(dCenterY + dHalfH);

	// 转换为 WGS84，供现有导出流程使用
	QgsCoordinateReferenceSystem crsSrc = m_pMainAreaLayer->crs();
	QgsCoordinateReferenceSystem crsDst("EPSG:4326");
	if (crsSrc.isValid() && crsSrc != crsDst)
	{
		try
		{
			QgsCoordinateTransform transform(crsSrc, crsDst, QgsProject::instance());
			m_mainAreaClipExtentWgs84 = transform.transformBoundingBox(rectClip);
		}
		catch (...)
		{
			appendLog(tr("[错误] 主区裁切范围转换到 WGS84 失败。"));
			return;
		}
	}
	else
	{
		m_mainAreaClipExtentWgs84 = rectClip;
	}

	// 兜底：若 QGIS 坐标转换在 DLL 宿主环境中未生效，
	// 且结果仍为 Web 墨卡托米制大数值，则手动转为 WGS84 经纬度
	if (rectLooksLikeWebMercator(m_mainAreaClipExtentWgs84))
	{
		m_mainAreaClipExtentWgs84 = webMercatorToWgs84(m_mainAreaClipExtentWgs84);
	}

	m_bMainAreaClipExtentValid = m_mainAreaClipExtentWgs84.isFinite();

	if (m_bMainAreaClipExtentValid)
	{
		QString strDesc = strPaper;
		if (strPaper == tr("自定义"))
			strDesc = tr("自定义(%1x%2mm)").arg(dFrameWidthMM).arg(dFrameHeightMM);

		QString strInnerClip;
		if (ui->checkBox_enableInnerClip->isChecked())
			strInnerClip = tr("，内图廓(%1x%2mm)").arg(dInnerFrameW).arg(dInnerFrameH);

		ui->label_mainAreaExtentSummary->setText(
			tr("已选主区范围：%1 %2，比例尺 1:%3%4，WGS84裁切框：[%5,%6,%7,%8]")
				.arg(strDesc)
				.arg(bLandscape ? tr("横向") : tr("纵向"))
				.arg(iStandardScale)
				.arg(strInnerClip)
				.arg(m_mainAreaClipExtentWgs84.xMinimum(), 0, 'f', 6)
				.arg(m_mainAreaClipExtentWgs84.yMinimum(), 0, 'f', 6)
				.arg(m_mainAreaClipExtentWgs84.xMaximum(), 0, 'f', 6)
				.arg(m_mainAreaClipExtentWgs84.yMaximum(), 0, 'f', 6));
	}
}

int CSE_DataManagementDialog::roundUpToStandardScale(double scale) const
{
	static const QList<int> listStandard = {
		100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000, 20000,
		25000, 30000, 40000, 50000, 100000, 200000, 250000, 500000,
		1000000, 2000000, 5000000, 10000000
	};

	for (int value : listStandard)
	{
		if (value >= scale)
			return value;
	}
	return listStandard.last();
}

bool CSE_DataManagementDialog::getMainAreaClipExtent(double& minLon, double& minLat,
												 double& maxLon, double& maxLat) const
{
	if (!m_bMainAreaClipExtentValid || !m_mainAreaClipExtentWgs84.isFinite())
		return false;

	minLon = m_mainAreaClipExtentWgs84.xMinimum();
	minLat = m_mainAreaClipExtentWgs84.yMinimum();
	maxLon = m_mainAreaClipExtentWgs84.xMaximum();
	maxLat = m_mainAreaClipExtentWgs84.yMaximum();
	return true;
}

void CSE_DataManagementDialog::on_Button_GetCanvasExtent_clicked()
{
	if (!m_pQgisIface || !m_pQgisIface->mapCanvas())
	{
		QMessageBox::information(this, tr("提示"), tr("无法获取地图画布。"));
		return;
	}

	// 获取所有图层联合范围
	QgsRectangle fullExtent;
	bool bFirst = true;

	QList<QgsMapLayer*> layers = m_pQgisIface->mapCanvas()->layers();
	for (QgsMapLayer* layer : layers)
	{
		QgsRectangle r = layer->extent();
		if (bFirst)
		{
			fullExtent = r;
			bFirst = false;
		}
		else
		{
			fullExtent.combineExtentWith(r);
		}
	}

	if (bFirst)
	{
		QMessageBox::information(this, tr("提示"), tr("地图中没有图层，无法获取范围。"));
		return;
	}

	// 转换为 WGS84 经纬度
	QgsCoordinateReferenceSystem srcCrs = m_pQgisIface->mapCanvas()->mapSettings().destinationCrs();
	QgsCoordinateReferenceSystem wgs84("EPSG:4326");

	if (srcCrs != wgs84 && srcCrs.isValid() && wgs84.isValid())
	{
		QgsCoordinateTransform xform(srcCrs, wgs84, QgsProject::instance());
		try
		{
			fullExtent = xform.transformBoundingBox(fullExtent);
		}
		catch (...)
		{
			appendLog(tr("坐标转换失败，将使用原始坐标"));
		}
	}

	// 兜底：若 QGIS 坐标转换在 DLL 宿主环境中未生效，
	// 且坐标仍为 Web 墨卡托米制大数值，则手动转为 WGS84 经纬度展示
	if (rectLooksLikeWebMercator(fullExtent))
	{
		fullExtent = webMercatorToWgs84(fullExtent);
	}

	ui->lineEdit_minLon->setText(QString::number(fullExtent.xMinimum(), 'f', 6));
	ui->lineEdit_maxLon->setText(QString::number(fullExtent.xMaximum(), 'f', 6));
	ui->lineEdit_minLat->setText(QString::number(fullExtent.yMinimum(), 'f', 6));
	ui->lineEdit_maxLat->setText(QString::number(fullExtent.yMaximum(), 'f', 6));

	ui->label_extentSummary->setText(
		tr("已选范围：X[%1, %2] Y[%3, %4]")
		.arg(fullExtent.xMinimum(), 0, 'f', 4)
		.arg(fullExtent.xMaximum(), 0, 'f', 4)
		.arg(fullExtent.yMinimum(), 0, 'f', 4)
		.arg(fullExtent.yMaximum(), 0, 'f', 4));

	appendLog(tr("已获取当前地图范围（WGS84）：lon(%1, %2) lat(%3, %4)")
		.arg(fullExtent.xMinimum(), 0, 'f', 4)
		.arg(fullExtent.xMaximum(), 0, 'f', 4)
		.arg(fullExtent.yMinimum(), 0, 'f', 4)
		.arg(fullExtent.yMaximum(), 0, 'f', 4));
}

void CSE_DataManagementDialog::on_Button_ClearExtent_clicked()
{
	ui->lineEdit_minLon->clear();
	ui->lineEdit_maxLon->clear();
	ui->lineEdit_minLat->clear();
	ui->lineEdit_maxLat->clear();
	ui->label_extentSummary->setText(tr("已选范围：未选择"));
}

void CSE_DataManagementDialog::on_Button_PickExtentFromMap_clicked()
{
	if (!m_pExtentPicker)
	{
		QMessageBox::information(this, tr("提示"), tr("地图工具未初始化。"));
		return;
	}

	if (!m_pQgisIface || !m_pQgisIface->mapCanvas())
		return;

	// 保存当前地图工具
	m_pPreviousMapTool = m_pQgisIface->mapCanvas()->mapTool();

	// === 修改点 3：隐藏数据导出弹窗，让用户在地图上框选 ===
	this->hide();

	// 激活框选工具
	m_pQgisIface->mapCanvas()->setMapTool(m_pExtentPicker);
	appendLog(tr("请在地图画布上拖拽框选矩形范围……"));
}

void CSE_DataManagementDialog::onMapExtentSelected(const QgsRectangle& rect)
{
	// === 修改点 3：重新显示数据导出弹窗 ===
	this->show();
	this->raise();
	this->activateWindow();

	// 恢复之前的地图工具
	if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
	{
		m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
	}

	// 转换为 WGS84 经纬度
	QgsRectangle wgs84Rect = rect;
	if (m_pQgisIface && m_pQgisIface->mapCanvas())
	{
		QgsCoordinateReferenceSystem srcCrs = m_pQgisIface->mapCanvas()->mapSettings().destinationCrs();
		QgsCoordinateReferenceSystem wgs84("EPSG:4326");
		if (srcCrs != wgs84 && srcCrs.isValid() && wgs84.isValid())
		{
			QgsCoordinateTransform xform(srcCrs, wgs84, QgsProject::instance());
			try
			{
				wgs84Rect = xform.transformBoundingBox(rect);
			}
			catch (...)
			{
				// 转换失败，使用原始坐标
			}
		}
	}

	// 兜底：若 QGIS 坐标转换在 DLL 宿主环境中未生效，
	// 且坐标仍为 Web 墨卡托米制大数值，则手动转为 WGS84 经纬度展示
	if (rectLooksLikeWebMercator(wgs84Rect))
	{
		wgs84Rect = webMercatorToWgs84(wgs84Rect);
	}

	ui->lineEdit_minLon->setText(QString::number(wgs84Rect.xMinimum(), 'f', 6));
	ui->lineEdit_maxLon->setText(QString::number(wgs84Rect.xMaximum(), 'f', 6));
	ui->lineEdit_minLat->setText(QString::number(wgs84Rect.yMinimum(), 'f', 6));
	ui->lineEdit_maxLat->setText(QString::number(wgs84Rect.yMaximum(), 'f', 6));

	ui->label_extentSummary->setText(
		tr("已选范围：X[%1, %2] Y[%3, %4]")
		.arg(wgs84Rect.xMinimum(), 0, 'f', 4)
		.arg(wgs84Rect.xMaximum(), 0, 'f', 4)
		.arg(wgs84Rect.yMinimum(), 0, 'f', 4)
		.arg(wgs84Rect.yMaximum(), 0, 'f', 4));

	appendLog(tr("地图框选完成：lon(%1,%2), lat(%3,%4)")
		.arg(wgs84Rect.xMinimum(), 0, 'f', 4)
		.arg(wgs84Rect.xMaximum(), 0, 'f', 4)
		.arg(wgs84Rect.yMinimum(), 0, 'f', 4)
		.arg(wgs84Rect.yMaximum(), 0, 'f', 4));

	m_pPreviousMapTool = nullptr;
}

void CSE_DataManagementDialog::onMapExtentPickCancelled()
{
	// === 修改点 3：取消框选后重新显示数据导出弹窗 ===
	this->show();
	this->raise();
	this->activateWindow();

	if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
	{
		m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
	}
	m_pPreviousMapTool = nullptr;
	appendLog(tr("已取消地图框选。"));
}

void CSE_DataManagementDialog::on_Button_BrowseClipShp_clicked()
{
	QString selected = QFileDialog::getOpenFileName(this,
		tr("选择裁剪边界 Shape 文件"),
		ui->lineEdit_clipShpPath->text(),
		tr("Shape 文件 (*.shp)"));

	if (selected.isEmpty()) return;
	ui->lineEdit_clipShpPath->setText(selected);
}

void CSE_DataManagementDialog::on_Button_LoadClipShpToMap_clicked()
{
	QString clipPath = ui->lineEdit_clipShpPath->text().trimmed();
	if (clipPath.isEmpty()) return;

	if (!m_pQgisIface) return;

	QFileInfo fi(clipPath);
	QgsVectorLayer* pLayer = new QgsVectorLayer(clipPath, fi.baseName(), "ogr");
	if (!pLayer->isValid())
	{
		appendLog(tr("无法加载裁剪边界 SHP：%1").arg(clipPath));
		delete pLayer;
		return;
	}

	QgsProject::instance()->addMapLayer(pLayer, true);
	m_pQgisIface->mapCanvas()->refresh();

	appendLog(tr("已加载裁剪边界到地图：%1").arg(fi.baseName()));
}

void CSE_DataManagementDialog::on_lineEdit_clipShpPath_textChanged(const QString& text)
{
	Q_UNUSED(text)
	// 加载字段列表（如果有变化）
	ui->comboBox_clipAttrField->clear();

	QString shpPath = ui->lineEdit_clipShpPath->text().trimmed();
	if (shpPath.isEmpty()) return;

	GDALAllRegister();
	GDALDatasetH hDS = GDALOpenEx(shpPath.toUtf8().constData(),
		GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
	if (!hDS) return;

	OGRLayerH hLayer = GDALDatasetGetLayer(hDS, 0);
	if (!hLayer)
	{
		GDALClose(hDS);
		return;
	}

	OGRFeatureDefnH hDefn = OGR_L_GetLayerDefn(hLayer);
	int nFields = OGR_FD_GetFieldCount(hDefn);

	ui->comboBox_clipAttrField->addItem(tr("(不过滤)"), QString());

	for (int i = 0; i < nFields; ++i)
	{
		OGRFieldDefnH hFDefn = OGR_FD_GetFieldDefn(hDefn, i);
		QString fieldName = QString::fromUtf8(OGR_Fld_GetNameRef(hFDefn));
		ui->comboBox_clipAttrField->addItem(fieldName, fieldName);
	}

	GDALClose(hDS);
}

void CSE_DataManagementDialog::on_comboBox_clipAttrField_currentIndexChanged(int index)
{
	Q_UNUSED(index)
}

bool CSE_DataManagementDialog::getSpatialExtent(double& minLon, double& minLat,
	double& maxLon, double& maxLat) const
{
	bool ok;
	minLon = ui->lineEdit_minLon->text().toDouble(&ok);
	if (!ok) return false;
	maxLon = ui->lineEdit_maxLon->text().toDouble(&ok);
	if (!ok) return false;
	minLat = ui->lineEdit_minLat->text().toDouble(&ok);
	if (!ok) return false;
	maxLat = ui->lineEdit_maxLat->text().toDouble(&ok);
	if (!ok) return false;

	if (minLon < -180.0 || minLon > 180.0) return false;
	if (maxLon < -180.0 || maxLon > 180.0) return false;
	if (minLat < -90.0 || minLat > 90.0) return false;
	if (maxLat < -90.0 || maxLat > 90.0) return false;
	if (minLon >= maxLon) return false;
	if (minLat >= maxLat) return false;

	return true;
}

bool CSE_DataManagementDialog::loadClipGeometry(QgsGeometry& outGeom, QString& errMsg) const
{
	QString shpPath = ui->lineEdit_clipShpPath->text().trimmed();
	if (shpPath.isEmpty())
	{
		errMsg = tr("裁剪 SHP 路径为空");
		return false;
	}

	QgsVectorLayer clipLayer(shpPath, "clip_tmp", "ogr");
	if (!clipLayer.isValid())
	{
		errMsg = tr("无法加载裁剪 SHP：%1").arg(shpPath);
		return false;
	}

	QgsGeometry merged;

	QString filterField = ui->comboBox_clipAttrField->currentData().toString();
	QString filterValue = ui->lineEdit_clipAttrValue->text().trimmed();

	QgsFeatureIterator it;

	if (!filterField.isEmpty() && !filterValue.isEmpty())
	{
		QgsFeatureRequest request;
		request.setFilterExpression(QString("\"%1\" = '%2'").arg(filterField, filterValue));
		it = clipLayer.getFeatures(request);
	}
	else
	{
		it = clipLayer.getFeatures();
	}

	QgsFeature feat;
	while (it.nextFeature(feat))
	{
		if (!feat.hasGeometry()) continue;
		QgsGeometry g = feat.geometry();
		if (merged.isNull())
			merged = g;
		else
			merged = merged.combine(g);
	}

	if (merged.isNull())
	{
		errMsg = tr("裁剪 SHP 中没有有效的几何数据");
		return false;
	}

	outGeom = merged;
	return true;
}

QList<DbLayerInfo> CSE_DataManagementDialog::getSelectedLayers() const
{
	QList<DbLayerInfo> result;
	for (int i = 0; i < ui->treeWidget_layers->topLevelItemCount(); ++i)
	{
		QTreeWidgetItem* item = ui->treeWidget_layers->topLevelItem(i);
		if (item && item->checkState(0) == Qt::Checked)
		{
			void* ptr = item->data(0, Qt::UserRole).value<void*>();
			if (ptr)
				result.append(*reinterpret_cast<const DbLayerInfo*>(ptr));
		}
	}
	return result;
}

// ====================================================================
//  输出设置
// ====================================================================

void CSE_DataManagementDialog::browseOutputPath()
{
	QString selected;

	switch (m_iCurrentTab)
	{
	case 1: // Shape/Folder → 选择保存文件
		selected = QFileDialog::getSaveFileName(this,
			tr("选择保存位置"),
			currentOutputPathEdit()->text(),
			tr("Shapefile (*.shp);;GeoPackage (*.gpkg);;所有文件 (*)"));
		break;

	case 2: // GDB → 选择输出文件夹（内部自动创建 .gdb）
		selected = QFileDialog::getExistingDirectory(this,
			tr("选择输出文件夹（将在此创建 .gdb）"),
			currentOutputPathEdit()->text());
		break;

    case 3: // 数据库 → 根据导出格式选择路径
        {
            QString fmt = currentFormatCombo()->currentText();
            if (fmt.contains("GDB") || fmt.contains("gdb"))
            {
                selected = QFileDialog::getExistingDirectory(this,
                    tr("选择输出文件夹（将在此创建 .gdb）"),
                    currentOutputPathEdit()->text());
            }
            else if (fmt.contains("GeoPackage") || fmt.contains("gpkg"))
            {
                selected = QFileDialog::getSaveFileName(this,
                    tr("选择保存位置"),
                    currentOutputPathEdit()->text(),
                    tr("GeoPackage (*.gpkg);;所有文件 (*)"));
            }
            else
            {
                selected = QFileDialog::getSaveFileName(this,
                    tr("选择保存位置"),
                    currentOutputPathEdit()->text(),
                    tr("Shapefile (*.shp);;所有文件 (*)"));
            }
        }
        break;
	}

	if (selected.isEmpty()) return;
	currentOutputPathEdit()->setText(selected);

	validateExportReady();
}

void CSE_DataManagementDialog::on_Button_BrowseOutput_extent_clicked()
{
	browseOutputPath();
}

void CSE_DataManagementDialog::on_Button_BrowseOutput_shp_clicked()
{
	browseOutputPath();
}

void CSE_DataManagementDialog::on_Button_BrowseOutput_mainArea_clicked()
{
	browseOutputPath();
}

bool CSE_DataManagementDialog::deleteExistingShapefile(const QString& shpPath, QString& errMsg)
{
	errMsg.clear();

	QFileInfo fi(shpPath);
	if (!fi.exists() && !fi.dir().exists())
		return true; // 文件不存在且目录也不存在，无需处理

	QString baseName = fi.completeBaseName();
	QDir dir = fi.absoluteDir();

	// 先移除地图中可能占用该 Shapefile 的图层，释放文件锁
	QList<QgsMapLayer*> layersToRemove;
	QMap<QString, QgsMapLayer*> mapLayers = QgsProject::instance()->mapLayers();
	for (auto it = mapLayers.constBegin(); it != mapLayers.constEnd(); ++it)
	{
		QgsMapLayer* pLayer = it.value();
		if (!pLayer)
			continue;
		QString src = pLayer->source();
		if (src == shpPath || src.startsWith(shpPath + "|"))
			layersToRemove.append(pLayer);
	}
	for (QgsMapLayer* pLayer : layersToRemove)
	{
		QgsProject::instance()->removeMapLayer(pLayer->id());
	}

	// Shapefile 常见附属扩展名
	QStringList extensions = {
		QStringLiteral("shp"), QStringLiteral("shx"), QStringLiteral("dbf"),
		QStringLiteral("prj"), QStringLiteral("cpg"), QStringLiteral("sbn"),
		QStringLiteral("sbx"), QStringLiteral("idx"), QStringLiteral("qix"),
		QStringLiteral("aih"), QStringLiteral("ain"), QStringLiteral("atx"),
		QStringLiteral("xml"), QStringLiteral("fbn"), QStringLiteral("fbx"),
		QStringLiteral("rrr")
	};

	bool bAllOk = true;
	QStringList failedFiles;
	for (const QString& ext : extensions)
	{
		QString filePath = dir.absoluteFilePath(baseName + "." + ext);
		QFile file(filePath);
		if (file.exists())
		{
			if (!file.remove())
			{
				bAllOk = false;
				failedFiles.append(filePath);
			}
		}
	}

	if (!bAllOk)
	{
		errMsg = tr("无法删除以下旧文件：%1").arg(failedFiles.join(", "));
		return false;
	}
	return true;
}

// ====================================================================
//  日志与辅助
// ====================================================================

void CSE_DataManagementDialog::appendLog(const QString& msg)
{
	QString ts = QDateTime::currentDateTime().toString("hh:mm:ss");
	ui->textEdit_log->append(QString("[%1] %2").arg(ts, msg));

	// 自动滚动到底部
	QTextCursor cursor = ui->textEdit_log->textCursor();
	cursor.movePosition(QTextCursor::End);
	ui->textEdit_log->setTextCursor(cursor);
}

// ====================================================================
//  导出执行
// ====================================================================

void CSE_DataManagementDialog::on_Button_Export_clicked()
{
	// ========== 参数校验 ==========
	QList<DbLayerInfo> selectedLayers = getSelectedLayers();
	if (selectedLayers.isEmpty())
	{
		QMessageBox::warning(this, tr("提示"), tr("请至少勾选一个图层。"));
		return;
	}

	QString outputPath = currentOutputPathEdit()->text().trimmed();
	if (outputPath.isEmpty())
	{
		QMessageBox::warning(this, tr("提示"), tr("请设置输出文件。"));
		return;
	}

	bool bUseClip = ui->checkBox_enableClip->isChecked();
	bool bUseExtent = bUseClip && ui->radioButton_useExtent->isChecked();
	bool bUseShpClip = bUseClip && ui->radioButton_useShp->isChecked();
	bool bUseMainArea = bUseClip && ui->radioButton_useMainArea->isChecked();
	bool bClipGeometry = bUseExtent;  // 仅真正的空间范围裁剪才做几何切割

	double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
	QgsGeometry clipGeom;

	if (bUseClip)
	{
		if (bUseMainArea)
		{
			// 根据主区裁切：使用计算出的 WGS84 范围，复用现有空间范围导出路径
			if (!getMainAreaClipExtent(minLon, minLat, maxLon, maxLat))
			{
				QMessageBox::warning(this, tr("提示"), tr("根据主区裁切未计算出有效范围，请先选择主区要素并计算比例尺。"));
				return;
			}
			bUseExtent = true;
		}
		else if (bUseExtent)
		{
			if (!getSpatialExtent(minLon, minLat, maxLon, maxLat))
			{
				QMessageBox::warning(this, tr("提示"), tr("经纬度数值不合法"));
				return;
			}
		}
		else if (bUseShpClip) // Shape 裁剪
		{
			QString errMsg;
			if (!loadClipGeometry(clipGeom, errMsg))
			{
				QMessageBox::warning(this, tr("提示"), errMsg);
				return;
			}
		}

	}

	QString encoding = currentEncodingCombo()->currentText();
	bool bOverwrite = currentOverwriteCheck()->isChecked();
	bool bIntersectOnly = true; // 默认只输出相交部分

	// 禁用导出按钮
	ui->Button_Export->setEnabled(false);
	ui->Button_Cancel->setEnabled(false);
	ui->progressBar_export->setValue(0);
	ui->progressBar_export->setMaximum(selectedLayers.size());

	appendLog(tr("==================== 开始导出 ===================="));
	appendLog(tr("模式：%1,  图层数：%2,  裁剪：%3")
		.arg(ui->tabWidget_source->tabText(m_iCurrentTab))
		.arg(selectedLayers.size())
		.arg(bUseClip ? tr("开启") : tr("关闭")));

	int nSuccess = 0;
	int nFail = 0;

	for (int idx = 0; idx < selectedLayers.size(); ++idx)
	{
		const DbLayerInfo& layer = selectedLayers[idx];

		ui->progressBar_export->setValue(idx);
		QApplication::processEvents();

        // 判断是否为 GDB 输出模式（Tab1 GDB源 或 Tab2 数据库导出选GDB格式）
        bool bIsGdbOutput = (m_iCurrentTab == 2) ||
            (m_iCurrentTab == 3 && currentFormatCombo()->currentText().contains("GDB"));

        // 构建输出路径
        QString dstPath;

        if (bIsGdbOutput)
        {
            // GDB 模式：输出到单个 GDB 文件
            dstPath = outputPath + "/output.gdb";
        }
        else
        {
            // Shape / 数据库模式：outputPath 为完整文件路径（目录 + 文件名 + 扩展名）
            QFileInfo pathFi(outputPath);
            QString outputDir = pathFi.absolutePath();
            QString outputBase = pathFi.completeBaseName();
            QString outputExt = pathFi.suffix().toLower();

            if (selectedLayers.size() == 1)
            {
                // 单图层：直接使用用户指定的完整路径
                dstPath = outputPath;
            }
            else
            {
                // 多图层：在基础名后追加图层名
                QString layerSuffix = layer.strTableName;
                layerSuffix.replace(QRegExp("[\\\\/:*?\"<>|]"), "_");
                dstPath = outputDir + "/" + outputBase + "_" + layerSuffix;
                if (!outputExt.isEmpty())
                    dstPath += "." + outputExt;
            }
        }

        // 检查是否覆盖
        if (!bOverwrite)
        {
            QFileInfo dfi(dstPath);
            if (dfi.exists() && !bIsGdbOutput) // GDB 模式单独处理
            {
                appendLog(tr("跳过（文件已存在）：%1").arg(dstPath));
                nFail++;
                continue;
            }
        }

		appendLog(tr("正在导出 [%1/%2]：%3")
			.arg(idx + 1).arg(selectedLayers.size()).arg(layer.strTableName));

		int featureCount = 0;
		bool bOk = false;

        if (m_iCurrentTab == 3)
        {
            // 数据库模式：区分栅格/矢量导出
            if (layer.strGeomType == "RASTER")
            {
                // 栅格数据 → 始终导出为 GeoTIFF，不受格式选择器影响
                if (!bIsGdbOutput)
                {
                    QFileInfo fi(dstPath);
                    QString baseNoExt = fi.absolutePath() + "/" + fi.completeBaseName();
                    dstPath = baseNoExt + ".tif";
                }
                else
                {
                    // GDB 输出模式下，栅格单独存为 .tif 文件，不放入 GDB 目录
                    dstPath = outputPath + "/" + layer.strTableName + ".tif";
                }
                if (bOverwrite)
                {
                    QFile f(dstPath);
                    if (f.exists())
                        f.remove();
                }
                bOk = exportRasterLayerFromDB(layer, dstPath,
                    bUseExtent, minLon, minLat, maxLon, maxLat,
                    bUseShpClip, clipGeom, featureCount);
            }
            else
            {
                // 矢量数据 → 根据选择的导出格式使用对应驱动
                QString driver;
                {
                    QString fmt = currentFormatCombo()->currentText();
                    if (fmt.contains("GDB") || fmt.contains("gdb"))
                        driver = "FileGDB";
                    else if (fmt.contains("GeoPackage") || fmt.contains("gpkg"))
                        driver = "GPKG";
                    else
                        driver = "ESRI Shapefile";
                }

                if (bOverwrite && driver == "ESRI Shapefile")
                {
                    QString delErr;
                    if (!deleteExistingShapefile(dstPath, delErr))
                        appendLog(tr("    [警告] 无法清理旧文件：%1").arg(delErr));
                }
                bOk = exportVectorLayerFromDB(layer, dstPath, driver,
                    bUseExtent, minLon, minLat, maxLon, maxLat,
                    clipGeom, bIntersectOnly, bClipGeometry, encoding, featureCount);
            }
        }
		else
		{
			// Shape / GDB / 当前地图图层模式
			if (layer.strGeomType == "RASTER")
			{
				// 栅格数据 → 导出为 GeoTIFF
				if (bIsGdbOutput || m_iCurrentTab == 2)
				{
					// GDB 输出模式下，栅格单独存为 .tif 文件，不放入 GDB 目录
					dstPath = outputPath + "/" + layer.strTableName + ".tif";
				}
				else
				{
					QFileInfo fi(dstPath);
					QString baseNoExt = fi.absolutePath() + "/" + fi.completeBaseName();
					dstPath = baseNoExt + ".tif";
				}

				if (bOverwrite)
				{
					QFile f(dstPath);
					if (f.exists())
						f.remove();
				}

				bOk = exportRasterLayerFromMapRaster(layer, dstPath,
					bUseExtent, minLon, minLat, maxLon, maxLat,
					bUseShpClip, clipGeom, featureCount);
			}
			else
			{
				// 矢量数据：通过 OGR 文件读取
				QString driver;

				if (m_iCurrentTab == 2)
				{
					driver = "FileGDB";
					dstPath = outputPath + "/output.gdb";
				}
				else
				{
					// 根据输出路径扩展名决定驱动，dstPath 已在上方统一构建
					QString outExt = QFileInfo(outputPath).suffix().toLower();
					if (outExt == "gpkg")
						driver = "GPKG";
					else
						driver = "ESRI Shapefile";
				}

				if (bOverwrite && driver == "ESRI Shapefile")
				{
					QString delErr;
					if (!deleteExistingShapefile(dstPath, delErr))
						appendLog(tr("    [警告] 无法清理旧文件：%1").arg(delErr));
				}

				bOk = exportVectorLayerFromFile(layer, dstPath, driver,
					bUseExtent, minLon, minLat, maxLon, maxLat,
					clipGeom, bIntersectOnly, bClipGeometry, encoding, featureCount);
			}
		}

		if (bOk)
		{
			nSuccess++;
			if (layer.strGeomType == "RASTER")
				appendLog(tr("    ✓ 成功（栅格数据）→ %1").arg(dstPath));
			else
				appendLog(tr("    ✓ 成功（%1 条要素）→ %2").arg(featureCount).arg(dstPath));

			// 导出成功后，若勾选则自动加载结果到地图
			if (ui->checkBox_loadResultToMap->isChecked())
				loadExportedResultToMap(layer, dstPath);
		}
		else
		{
			nFail++;
			appendLog(tr("    ✗ 失败 → %1").arg(dstPath));
		}
	}

	// 对于 GDB 模式，导出完成后追加 .shp 后缀避免混乱
	// （实际 GDB 创建由 driver 支持）

	ui->progressBar_export->setValue(selectedLayers.size());

	if (nFail > 0)
		appendLog(tr("==================== 导出失败 ===================="));
	else
		appendLog(tr("==================== 导出完成 ===================="));
	appendLog(tr("成功：%1,  失败：%2,  总计：%3").arg(nSuccess).arg(nFail).arg(selectedLayers.size()));

	if (nFail > 0)
		QMessageBox::warning(this, tr("导出结果"), tr("导出失败"));
	else
		QMessageBox::information(this, tr("导出结果"), tr("导出完成"));

	// 恢复按钮
	ui->Button_Export->setEnabled(true);
	ui->Button_Cancel->setEnabled(true);
	validateExportReady();
}

void CSE_DataManagementDialog::on_Button_Cancel_clicked()
{
	close();
}

// ====================================================================
//  导出辅助方法
// ====================================================================

bool CSE_DataManagementDialog::exportVectorLayerFromFile(
	const DbLayerInfo& layer,
	const QString& dstPath, const QString& driverName,
	bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
	const QgsGeometry& clipGeom, bool bIntersectOnly, bool bClipGeometry,
	const QString& encoding, int& outFeatureCount)
{
	outFeatureCount = 0;

	// 解析源路径（GDB 源路径格式为：path|layername=xxx）
	QString srcPath = layer.strSourcePath;
	QString layerName = layer.strTableName;

	// 从地图直接获取的已加载图层：复用现有指针，无需重新加载
	if (layer.pLoadedLayer)
	{
		QgsVectorLayer* pSrcLayer = layer.pLoadedLayer;
		if (!pSrcLayer->isValid())
		{
			appendLog(tr("    ✗ 地图图层无效：%1").arg(layerName));
			return false;
		}

		QgsFields fields = pSrcLayer->fields();
		QgsCoordinateReferenceSystem crs = pSrcLayer->crs();

		QgsVectorFileWriter::SaveVectorOptions options;
		options.driverName = driverName;
		options.fileEncoding = encoding;
		options.layerName = layerName;

		if (driverName == "FileGDB")
		{
			options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteLayer;
		}

		QgsVectorFileWriter* writer = QgsVectorFileWriter::create(
			dstPath, fields, pSrcLayer->wkbType(), crs,
			QgsProject::instance()->transformContext(), options);

		if (writer->hasError() != QgsVectorFileWriter::NoError)
		{
			appendLog(tr("    ✗ 无法创建输出文件：%1").arg(writer->errorMessage()));
			delete writer;
			return false;
		}

		QgsFeature feat;
		QgsFeatureIterator it = pSrcLayer->getFeatures();

		while (it.nextFeature(feat))
		{
			if (!feat.hasGeometry()) continue;

			bool bKeep = true;

			if (bUseExtent)
			{
				QgsCoordinateReferenceSystem wgs84("EPSG:4326");
				QgsCoordinateTransform xform(crs, wgs84, QgsProject::instance());
				QgsRectangle layerExtent = feat.geometry().boundingBox();

				try
				{
					layerExtent = xform.transformBoundingBox(layerExtent);
				}
				catch (...) {}

				QgsRectangle clipRect(minLon, minLat, maxLon, maxLat);
				if (!clipRect.intersects(layerExtent))
				{
					bKeep = false;
				}
				else if (bClipGeometry)
				{
					QgsGeometry clipRectGeom = QgsGeometry::fromRect(clipRect);
					QgsCoordinateTransform xformInv(wgs84, crs, QgsProject::instance());
					QgsGeometry clipRectInSrcCrs;
					try
					{
						clipRectInSrcCrs = clipRectGeom;
						clipRectInSrcCrs.transform(xformInv);
					}
					catch (...)
					{
						clipRectInSrcCrs = clipRectGeom;
					}

					QgsGeometry clippedGeom = feat.geometry().intersection(clipRectInSrcCrs);
					if (clippedGeom.isNull() || clippedGeom.isEmpty())
					{
						bKeep = false;
					}
					else
					{
						feat.setGeometry(clippedGeom);
						bKeep = true;
					}
				}
			}

			if (bKeep && !clipGeom.isNull())
			{
				QgsGeometry featGeom = feat.geometry();
				if (bIntersectOnly)
					bKeep = featGeom.intersects(clipGeom);
				else
					bKeep = clipGeom.contains(featGeom);
			}

			if (bKeep)
			{
				writer->addFeature(feat, QgsFeatureSink::FastInsert);
				outFeatureCount++;
			}
		}

		delete writer;
		return true;
	}

	// 如果是 GDB 路径
	if (srcPath.contains("|layername="))
	{
		int pipePos = srcPath.indexOf("|layername=");
		QString basePath = srcPath.left(pipePos);
		layerName = srcPath.mid(pipePos + 11); // skip "|layername="

		QgsVectorLayer* pSrcLayer = new QgsVectorLayer(basePath + "|layername=" + layerName,
			"src_temp_" + layerName, "ogr");
		if (!pSrcLayer->isValid())
		{
			appendLog(tr("    ✗ 无法加载源 GDB 图层：%1").arg(layerName));
			delete pSrcLayer;
			return false;
		}

		// 准备字段定义
		QgsFields fields = pSrcLayer->fields();

		// 准备坐标参考系
		QgsCoordinateReferenceSystem crs = pSrcLayer->crs();

		// 准备写入参数
		QgsVectorFileWriter::SaveVectorOptions options;
		options.driverName = driverName;
		options.fileEncoding = encoding;
		options.layerName = layerName;

		// 对于 GDB，使用 update 模式追加图层
		if (driverName == "FileGDB")
		{
			options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteLayer;
		}

		QgsVectorFileWriter* writer = QgsVectorFileWriter::create(
			dstPath, fields, pSrcLayer->wkbType(), crs,
			QgsProject::instance()->transformContext(), options);

		if (writer->hasError() != QgsVectorFileWriter::NoError)
		{
			appendLog(tr("    ✗ 无法创建输出文件：%1").arg(writer->errorMessage()));
			delete writer;
			delete pSrcLayer;
			return false;
		}

		// 迭代并写入要素
		QgsFeature feat;
		QgsFeatureIterator it = pSrcLayer->getFeatures();

		while (it.nextFeature(feat))
		{
			if (!feat.hasGeometry()) continue;

			bool bKeep = true;

			if (bUseExtent)
			{
				// 使用 WGS84 构建范围矩形
				QgsCoordinateReferenceSystem wgs84("EPSG:4326");
				QgsCoordinateTransform xform(crs, wgs84, QgsProject::instance());
				QgsRectangle layerExtent = feat.geometry().boundingBox();

				try
				{
					layerExtent = xform.transformBoundingBox(layerExtent);
				}
				catch (...) {}

				QgsRectangle clipRect(minLon, minLat, maxLon, maxLat);
				if (!clipRect.intersects(layerExtent))
				{
					bKeep = false;
				}
				else if (bClipGeometry)
				{
					// 空间裁剪：对要素几何进行实际切割，仅保留框选范围内的部分
					QgsGeometry clipRectGeom = QgsGeometry::fromRect(clipRect);
					QgsCoordinateTransform xformInv(wgs84, crs, QgsProject::instance());
					QgsGeometry clipRectInSrcCrs;
					try
					{
						clipRectInSrcCrs = clipRectGeom;
						clipRectInSrcCrs.transform(xformInv);
					}
					catch (...)
					{
						clipRectInSrcCrs = clipRectGeom;
					}

					QgsGeometry clippedGeom = feat.geometry().intersection(clipRectInSrcCrs);
					if (clippedGeom.isNull() || clippedGeom.isEmpty())
					{
						bKeep = false;
					}
					else
					{
						feat.setGeometry(clippedGeom);
						bKeep = true;
					}
				}
				// else: bClipGeometry=false → 仅按 bbox 相交筛选，保留完整要素（主区裁切复用此路径）
			}

			if (bKeep && !clipGeom.isNull())
			{
				QgsGeometry featGeom = feat.geometry();
				if (bIntersectOnly)
					bKeep = featGeom.intersects(clipGeom);
				else
					bKeep = clipGeom.contains(featGeom);
			}

			if (bKeep)
			{
				writer->addFeature(feat, QgsFeatureSink::FastInsert);
				outFeatureCount++;
			}
		}

		delete writer;
		delete pSrcLayer;
		return true;
	}
	else
	{
		// 普通 Shape / 文件
		QFileInfo fi(srcPath);
		QgsVectorLayer* pSrcLayer = new QgsVectorLayer(srcPath, fi.baseName(), "ogr");
		if (!pSrcLayer->isValid())
		{
			appendLog(tr("    ✗ 无法加载源文件：%1").arg(srcPath));
			delete pSrcLayer;
			return false;
		}

		QgsFields fields = pSrcLayer->fields();
		QgsCoordinateReferenceSystem crs = pSrcLayer->crs();

		QgsVectorFileWriter::SaveVectorOptions options;
		options.driverName = driverName;
		options.fileEncoding = encoding;

		QgsVectorFileWriter* writer = QgsVectorFileWriter::create(
			dstPath, fields, pSrcLayer->wkbType(), crs,
			QgsProject::instance()->transformContext(), options);

		if (writer->hasError() != QgsVectorFileWriter::NoError)
		{
			appendLog(tr("    ✗ 无法创建输出文件：%1").arg(writer->errorMessage()));
			delete writer;
			delete pSrcLayer;
			return false;
		}

		QgsFeature feat;
		QgsFeatureIterator it = pSrcLayer->getFeatures();

		while (it.nextFeature(feat))
		{
			if (!feat.hasGeometry()) continue;

			bool bKeep = true;

			if (bUseExtent)
			{
				QgsCoordinateReferenceSystem wgs84("EPSG:4326");
				QgsCoordinateTransform xform(crs, wgs84, QgsProject::instance());
				QgsRectangle layerExtent = feat.geometry().boundingBox();

				try
				{
					layerExtent = xform.transformBoundingBox(layerExtent);
				}
				catch (...) {}

				QgsRectangle clipRect(minLon, minLat, maxLon, maxLat);
				if (!clipRect.intersects(layerExtent))
				{
					bKeep = false;
				}
				else if (bClipGeometry)
				{
					// 空间裁剪：对要素几何进行实际切割，仅保留框选范围内的部分
					QgsGeometry clipRectGeom = QgsGeometry::fromRect(clipRect);
					QgsCoordinateTransform xformInv(wgs84, crs, QgsProject::instance());
					QgsGeometry clipRectInSrcCrs;
					try
					{
						clipRectInSrcCrs = clipRectGeom;
						clipRectInSrcCrs.transform(xformInv);
					}
					catch (...)
					{
						clipRectInSrcCrs = clipRectGeom;
					}

					QgsGeometry clippedGeom = feat.geometry().intersection(clipRectInSrcCrs);
					if (clippedGeom.isNull() || clippedGeom.isEmpty())
					{
						bKeep = false;
					}
					else
					{
						feat.setGeometry(clippedGeom);
						bKeep = true;
					}
				}
				// else: bClipGeometry=false → 仅按 bbox 相交筛选，保留完整要素（主区裁切复用此路径）
			}

			if (bKeep && !clipGeom.isNull())
			{
				QgsGeometry featGeom = feat.geometry();
				if (bIntersectOnly)
					bKeep = featGeom.intersects(clipGeom);
				else
					bKeep = clipGeom.contains(featGeom);
			}

			if (bKeep)
			{
				writer->addFeature(feat, QgsFeatureSink::FastInsert);
				outFeatureCount++;
			}
		}

		delete writer;
		delete pSrcLayer;
		return true;
	}
}

bool CSE_DataManagementDialog::exportVectorLayerFromDB(
	const DbLayerInfo& layer,
	const QString& dstPath, const QString& driverName,
	bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
	const QgsGeometry& clipGeom, bool bIntersectOnly, bool bClipGeometry,
	const QString& encoding, int& outFeatureCount)
{
	outFeatureCount = 0;

	QString uri = buildPgUri(layer);
	QgsVectorLayer* pSrcLayer = new QgsVectorLayer(uri, layer.strTableName, "postgres");
	if (!pSrcLayer->isValid())
	{
		appendLog(tr("    ✗ 无法加载源数据库图层：%1").arg(layer.strTableName));
		delete pSrcLayer;
		return false;
	}

	QgsFields fields = pSrcLayer->fields();
	QgsCoordinateReferenceSystem crs = pSrcLayer->crs();

	QgsVectorFileWriter::SaveVectorOptions options;
	options.driverName = driverName;
	options.fileEncoding = encoding;

	QgsVectorFileWriter* writer = QgsVectorFileWriter::create(
		dstPath, fields, pSrcLayer->wkbType(), crs,
		QgsProject::instance()->transformContext(), options);

	if (writer->hasError() != QgsVectorFileWriter::NoError)
	{
		appendLog(tr("    ✗ 无法创建输出文件：%1").arg(writer->errorMessage()));
		delete writer;
		delete pSrcLayer;
		return false;
	}

	QgsFeature feat;
	QgsFeatureIterator it = pSrcLayer->getFeatures();

	while (it.nextFeature(feat))
	{
		if (!feat.hasGeometry()) continue;

		bool bKeep = true;

		if (bUseExtent)
		{
			QgsCoordinateReferenceSystem wgs84("EPSG:4326");
			QgsCoordinateTransformContext ctx;
			QgsCoordinateTransform xform(crs, wgs84, ctx);
			QgsRectangle layerExtent = feat.geometry().boundingBox();

			try
			{
				layerExtent = xform.transformBoundingBox(layerExtent);
			}
			catch (...) {}

			QgsRectangle clipRect(minLon, minLat, maxLon, maxLat);
			if (!clipRect.intersects(layerExtent))
			{
				bKeep = false;
			}
			else if (bClipGeometry)
			{
				// 空间裁剪：对要素几何进行实际切割，仅保留框选范围内的部分
				QgsGeometry clipRectGeom = QgsGeometry::fromRect(clipRect);
				QgsCoordinateTransform xformInv(wgs84, crs, ctx);
				QgsGeometry clipRectInSrcCrs;
				try
				{
					clipRectInSrcCrs = clipRectGeom;
					clipRectInSrcCrs.transform(xformInv);
				}
				catch (...)
				{
					clipRectInSrcCrs = clipRectGeom;
				}

				QgsGeometry clippedGeom = feat.geometry().intersection(clipRectInSrcCrs);
				if (clippedGeom.isNull() || clippedGeom.isEmpty())
				{
					bKeep = false;
				}
				else
				{
					feat.setGeometry(clippedGeom);
					bKeep = true;
				}
			}
			// else: bClipGeometry=false → 仅按 bbox 相交筛选，保留完整要素（主区裁切复用此路径）
		}

		if (bKeep && !clipGeom.isNull())
		{
			QgsGeometry featGeom = feat.geometry();
			if (bIntersectOnly)
				bKeep = featGeom.intersects(clipGeom);
			else
				bKeep = clipGeom.contains(featGeom);
		}

		if (bKeep)
		{
			writer->addFeature(feat, QgsFeatureSink::FastInsert);
			outFeatureCount++;
		}
	}

	delete writer;
	delete pSrcLayer;
	return true;
}

QString CSE_DataManagementDialog::buildPgUri(const DbLayerInfo& layer) const
{
	if (m_iCurrentConnectionIndex < 0 || m_iCurrentConnectionIndex >= s_listConnections.size())
		return QString();

	const auto& conn = s_listConnections[m_iCurrentConnectionIndex];

	QString uri = QString("dbname='%1' host=%2 port=%3 user='%4' password='%5' key='%6' srid=%7")
		.arg(conn.strDbName,
			conn.strHost,
			conn.strPort,
			conn.strUsername,
			conn.strPassword,
			"gid",
			layer.strCrs.split(":").last());

	// 部分 PostGIS 版本需要明确的 table/schema
	uri += QString(" table=\"%1\".\"%2\" (geom)")
		.arg(layer.strSchema, layer.strTableName);

	uri += " type=POINT|LINESTRING|POLYGON|MULTIPOINT|MULTILINESTRING|MULTIPOLYGON";

	return uri;
}

// 构建 PostGIS 栅格连接字符串（GDAL raster 格式：
// PG:dbname='db' host='host' port='port' user='user' password='pass' schema='schema' table='table' mode=2）
QString CSE_DataManagementDialog::buildPgRasterConnStr(const DbLayerInfo& layer) const
{
	if (m_iCurrentConnectionIndex < 0 || m_iCurrentConnectionIndex >= s_listConnections.size())
		return QString();

	const auto& conn = s_listConnections[m_iCurrentConnectionIndex];

	QString connStr = QString("PG:dbname='%1' host='%2' port='%3' user='%4' password='%5' schema='%6' table='%7' mode=2")
		.arg(conn.strDbName,
			conn.strHost,
			conn.strPort,
			conn.strUsername,
			conn.strPassword,
			layer.strSchema,
			layer.strTableName);

	return connStr;
}

// 将裁剪几何写入临时 GeoJSON 文件（供 GDAL -cutline 使用）
QString CSE_DataManagementDialog::writeClipGeometryToTempFile(const QgsGeometry& clipGeom) const
{
	if (clipGeom.isNull() || clipGeom.isEmpty())
		return QString();

	// 转换为 WKT，作为 GeoJSON 的坐标来源
	QString wkt = clipGeom.asWkt();
	if (wkt.isEmpty())
		return QString();

	QString tmpPath = QDir::tempPath() + QStringLiteral("/qgis_clip_%1.json")
		.arg(QDateTime::currentMSecsSinceEpoch());

	// 使用 GDAL OGR 驱动写入 GeoJSON
	GDALAllRegister();

	GDALDriverH hDrv = GDALGetDriverByName("GeoJSON");
	if (!hDrv)
		return QString();

	char** papszOpts = nullptr;
	GDALDatasetH hDS = GDALCreate(hDrv, tmpPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, papszOpts);
	CSLDestroy(papszOpts);
	if (!hDS)
		return QString();

	OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
	OSRImportFromEPSG(hSRS, 4326);

	OGRLayerH hLayer = GDALDatasetCreateLayer(hDS, "clip", hSRS, wkbPolygon, nullptr);
	if (hLayer)
	{
		OGRFeatureH hFeat = OGR_F_Create(OGR_L_GetLayerDefn(hLayer));
		OGRGeometryH hGeom = nullptr;
		QByteArray wktBuf = wkt.toUtf8();
		char* pszWkt = wktBuf.data();
		OGR_G_CreateFromWkt(&pszWkt, hSRS, &hGeom);
		if (hGeom)
		{
			OGR_F_SetGeometry(hFeat, hGeom);
			OGR_L_CreateFeature(hLayer, hFeat);
			OGR_G_DestroyGeometry(hGeom);
		}
		OGR_F_Destroy(hFeat);
	}

	OSRDestroySpatialReference(hSRS);
	GDALClose(hDS);

	return tmpPath;
}

// 通过 GDAL 将栅格数据源导出为 GeoTIFF（支持空间范围裁剪与 Shape 裁剪）
bool CSE_DataManagementDialog::doExportRaster(
	const QString& rasterUri,
	const QString& dstPath,
	bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
	bool bUseShpClip, const QgsGeometry& clipGeom,
	int& outFeatureCount)
{
	outFeatureCount = 0;

	// 注册 GDAL 驱动
	GDALAllRegister();

	// 打开栅格数据源
	GDALDataset* poSrcDS = static_cast<GDALDataset*>(GDALOpen(rasterUri.toUtf8().constData(), GA_ReadOnly));
	if (!poSrcDS)
	{
		appendLog(tr("    [错误] GDAL 无法打开栅格数据源：%1").arg(rasterUri));
		appendLog(tr("    GDAL 错误：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg())));
		return false;
	}

	bool bOk = false;
	QString tmpCutlinePath;
	char** papszOptions = nullptr;

	// 输出格式：GeoTIFF
	papszOptions = CSLSetNameValue(papszOptions, "-of", "GTiff");

	// TIF 压缩选项
	papszOptions = CSLSetNameValue(papszOptions, "-co", "COMPRESS=LZW");
	papszOptions = CSLSetNameValue(papszOptions, "-co", "BIGTIFF=IF_SAFER");

	// Shape 裁剪：使用 -cutline 对栅格做矢量边界裁剪
	if (bUseShpClip && !clipGeom.isNull() && !clipGeom.isEmpty())
	{
		tmpCutlinePath = writeClipGeometryToTempFile(clipGeom);
		if (tmpCutlinePath.isEmpty())
		{
			appendLog(tr("    [错误] 无法创建裁剪边界临时文件"));
			GDALClose(poSrcDS);
			CSLDestroy(papszOptions);
			return false;
		}
		papszOptions = CSLSetNameValue(papszOptions, "-cutline", tmpCutlinePath.toUtf8().constData());
		papszOptions = CSLSetNameValue(papszOptions, "-crop_to_cutline", "TRUE");

		// 裁剪边界与栅格数据源假定在同一坐标系，用栅格源 CRS 声明 cutline 坐标系
		OGRSpatialReferenceH hSrcSRS = const_cast<OGRSpatialReference*>(poSrcDS->GetSpatialRef());
		if (hSrcSRS)
		{
			const char* pszAuth = OSRGetAuthorityName(hSrcSRS, nullptr);
			const char* pszCode = OSRGetAuthorityCode(hSrcSRS, nullptr);
			if (pszAuth && pszCode)
				papszOptions = CSLSetNameValue(papszOptions, "-cutline_srs",
					(QString("%1:%2").arg(pszAuth, pszCode)).toUtf8().constData());
			else
				papszOptions = CSLSetNameValue(papszOptions, "-cutline_srs", "EPSG:4326");
		}
		else
		{
			papszOptions = CSLSetNameValue(papszOptions, "-cutline_srs", "EPSG:4326");
		}
	}
	// 空间范围裁剪（含主区裁切，主区裁切已将范围写入 minLon/maxLat/maxLon/minLat）
	else if (bUseExtent)
	{
		QString projWin = QString("%1 %2 %3 %4")
			.arg(minLon, 0, 'f', 10)
			.arg(maxLat, 0, 'f', 10)
			.arg(maxLon, 0, 'f', 10)
			.arg(minLat, 0, 'f', 10);
		papszOptions = CSLSetNameValue(papszOptions, "-projwin", projWin.toUtf8().constData());
		papszOptions = CSLSetNameValue(papszOptions, "-projwin_srs", "EPSG:4326");
	}

	// 执行 gdal_translate
	GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(papszOptions, nullptr);
	if (!psOptions)
	{
		appendLog(tr("    [错误] 无法创建 GDALTranslate 选项"));
		GDALClose(poSrcDS);
		CSLDestroy(papszOptions);
		if (!tmpCutlinePath.isEmpty())
			QFile::remove(tmpCutlinePath);
		return false;
	}

	int bUsageError = FALSE;
	GDALDatasetH hOutDS = GDALTranslate(dstPath.toUtf8().constData(),
		GDALDataset::ToHandle(poSrcDS),
		psOptions, &bUsageError);

	if (hOutDS)
	{
		outFeatureCount = 1; // 栅格导出完成标记
		GDALClose(hOutDS);
		bOk = true;
	}
	else
	{
		if (bUsageError)
			appendLog(tr("    [错误] gdal_translate 参数错误"));
		else
			appendLog(tr("    [错误] gdal_translate 执行失败：%1")
				.arg(QString::fromUtf8(CPLGetLastErrorMsg())));
	}

	GDALTranslateOptionsFree(psOptions);
	CSLDestroy(papszOptions);
	GDALClose(poSrcDS);
	if (!tmpCutlinePath.isEmpty())
		QFile::remove(tmpCutlinePath);

	return bOk;
}

// 通过 PostGIS GDAL 读取栅格图层，导出为 GeoTIFF
bool CSE_DataManagementDialog::exportRasterLayerFromDB(
	const DbLayerInfo& layer,
	const QString& dstPath,
	bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
	bool bUseShpClip, const QgsGeometry& clipGeom,
	int& outFeatureCount)
{
	QString rasterUri = buildPgRasterConnStr(layer);
	if (rasterUri.isEmpty())
	{
		appendLog(tr("    [错误] 无法构建栅格连接字符串"));
		return false;
	}

	return doExportRaster(rasterUri, dstPath,
		bUseExtent, minLon, minLat, maxLon, maxLat,
		bUseShpClip, clipGeom, outFeatureCount);
}

// 从地图已加载的栅格图层导出为 GeoTIFF（Tab 3）
bool CSE_DataManagementDialog::exportRasterLayerFromMapRaster(
	const DbLayerInfo& layer,
	const QString& dstPath,
	bool bUseExtent, double minLon, double minLat, double maxLon, double maxLat,
	bool bUseShpClip, const QgsGeometry& clipGeom,
	int& outFeatureCount)
{
	if (!layer.pLoadedRasterLayer)
	{
		appendLog(tr("    [错误] 未获取到已加载的栅格图层"));
		return false;
	}

	// 获取栅格底层数据源 URI（文件路径或 PG: 连接串）
	QString rasterUri = layer.strSourcePath;
	if (rasterUri.isEmpty() && layer.pLoadedRasterLayer->dataProvider())
		rasterUri = layer.pLoadedRasterLayer->dataProvider()->dataSourceUri();
	if (rasterUri.isEmpty())
	{
		appendLog(tr("    [错误] 无法获取栅格图层的数据源路径"));
		return false;
	}

	return doExportRaster(rasterUri, dstPath,
		bUseExtent, minLon, minLat, maxLon, maxLat,
		bUseShpClip, clipGeom, outFeatureCount);
}

