#include "map_quality_check_tools.h"

/*------------QGIS include-----------------*/
#include "qgisinterface.h"
#include "qgsguiutils.h"
#include "qgsproject.h"
#include "qgsmessagebar.h"
#include "qgsmapcanvas.h"
#include "qgsapplication.h"

/*-------------Qt--------------------*/
#include <QAction>
#include <QMessageBox>
#include <QtCore/QProcess>
#include <QMenu>
#include <QSettings>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>

/*-------------SE---------------------*/

#include "ui_class/se_vector_format_conversion.h"
#include "ui_class/db_config_dialog.h"
#include "ui_class/generalization_config_dialog.h"
#include "ui_class/se_data_import.h"
#include "ui_class/se_auto_quality_check.h"

// 数据导出 & 备份恢复
#include "ui_class/se_data_restore.h"
#include "ui_class/se_database_connection.h"
#include "ui_class/se_data_management.h"
#include "ui_class/se_data_list_export.h"   // 【2026-08-24】数据导出主入口改为数据列表式导出
#include "ui_class/clip_dialog.h"
#include "ui_class/merge_dialog.h"
#include "ui_class/format_conversion_dialog.h"
#include "ui_class/auto_edge_match_dialog.h"
#include "map_check_backup_manager.h"

// 地图成果管理
#include "ui_class/login_dialog.h"
#include "ui_class/product_storage_dialog.h"
#include "ui_class/metadata_manager_dialog.h"
#include "ui_class/access_control_dialog.h"
#include "ui_class/data_import_wizard.h"
#include "ui_class/mapdata_download.h"
#include "database/postgis_connector.h"
#include "database/schema_manager.h"
#include "database/product_metadata.h"
#include "database/product_dao.h"

//#include "ui_class/se_feature_extent_check.h"
//#include "ui_class/DBManagerDlg.h"
//#include "ui_class/se_theme_mapping.h"
/*------------------------------------*/



// 消息日志
#include "qgsmessagelog.h"
#include "qgsmessagebar.h"

static const QString sName = QObject::tr("地图成果质检工具箱");
static const QString sDescription = QObject::tr("提供地图成果质检功能。");
static const QString sCategory = QObject::tr("Tile");
static const QString sPluginVersion = QObject::tr("Version 1.0");
static const QgisPlugin::PluginType sPluginType = QgisPlugin::UI;

// 插件图标
static const QString sPluginIcon = QStringLiteral(":/ptp/images/ptp.png");


// ============================================================================
// 平台登录态静默同步（方案 A）
//
// 新平台（LTZK）启动登录成功后，会把用户名/密码明文写入
//   <平台安装目录>/config/user_login.ini（键 user/user_name、user/password），
// 退出登录时截断清空。插件据此读用户名 → 按用户名从 user_permissions 表取角色
// （ProductDAO::getUserPermission，无需密码）→ 填入插件自己的 gCurrentUserSession。
// 平台登录后，插件的成果管理功能无需再登录一次。
// ============================================================================

// 定位平台写出的 user_login.ini（与平台 LTZK::userLogPath() 逻辑一致：
// user_login.ini 与 config/database.ini 同目录 —— 先按 database.ini 探测，再取同目录 user_login.ini）
static QString platformUserLoginIniPath()
{
	const QString appDir = QCoreApplication::applicationDirPath();
	const QStringList candidates = {
		appDir + QStringLiteral("/config/database.ini"),
		QDir(appDir).absoluteFilePath(QStringLiteral("../config/database.ini")),
		QDir(appDir).absoluteFilePath(QStringLiteral("../../config/database.ini")),
		QDir::currentPath() + QStringLiteral("/config/database.ini")
	};
	for (const QString& candidate : candidates)
	{
		if (QFileInfo::exists(candidate))
		{
			return QFileInfo(candidate).absoluteDir().absoluteFilePath(QStringLiteral("user_login.ini"));
		}
	}
	return QDir(appDir).absoluteFilePath(QStringLiteral("config/user_login.ini"));
}

// 从 user_login.ini 同步平台登录态到插件会话。返回 true 表示已生效（isLoggedIn=true）。
static bool syncSessionFromPlatform()
{
	gCurrentUserSession.clear();

	QSettings ini(platformUserLoginIniPath(), QSettings::IniFormat);
	const QString userName = ini.value(QStringLiteral("user/user_name")).toString().trimmed();
	if (userName.isEmpty())
		return false;

	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return false;

	ProductDAO dao;
	const UserPermission perm = dao.getUserPermission(userName);
	if (perm.id < 0)
		return false;   // 平台登录用户不在 user_permissions 表中

	gCurrentUserSession.userName = perm.userName;
	gCurrentUserSession.role = perm.role;
	gCurrentUserSession.isLoggedIn = true;
	return true;
}


CGarMap_MapQualityCheckToolsPlugin::CGarMap_MapQualityCheckToolsPlugin(QgisInterface* qgisInterface)
	: QgisPlugin(sName, sDescription, sCategory, sPluginVersion, sPluginType)
	, mQGisIface(qgisInterface)
{

}

CGarMap_MapQualityCheckToolsPlugin::~CGarMap_MapQualityCheckToolsPlugin()
{
}


void CGarMap_MapQualityCheckToolsPlugin::initGui()
{
    // 统一 QSettings 存储格式：Windows 默认注册表，Linux 默认 ~/.config/*.conf。
    // 跨平台统一用 IniFormat（~/.config/GarMap/MapProductManager.conf），键名不变。
    QSettings::setDefaultFormat(QSettings::IniFormat);

    // mActionTest 已隐藏，保留源码
    // delete mActionTest;


    // 程序运行目录
    QString curExePath = QCoreApplication::applicationDirPath();

    /*【专题图制作】— 保留源码，暂不显示菜单 */
    // QIcon iconThemeMapping;
    // QString strThemeMapping = curExePath + "/resource/toolbox/theme_data_mapping.png";
    // iconThemeMapping.addFile(strThemeMapping);
    // mActionTest = new QAction(iconThemeMapping, tr("测试矢量数据格式转换"), this);
    // mActionTest->setObjectName(QStringLiteral("mActionThemeMapping"));
    // mActionTest->setWhatsThis(tr("测试矢量数据格式转换"));
    // connect(mActionTest, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::Test);
    // mQGisIface->addToolBarIcon(mActionTest);
    // mQGisIface->addPluginToVectorMenu(tr("&测试矢量数据格式转换"), mActionTest);
    // mActionTest->setEnabled(true);

	// ====== 表单化 UI 入口 ======

	/*【数据库连接配置】*/
	QIcon iconDbConfig;
	QString strDbConfig = curExePath + "/resource/toolbox/theme_data_mapping.png";
	iconDbConfig.addFile(strDbConfig);
	mActionDbConfig = new QAction(iconDbConfig, tr("数据库连接配置..."), this);
	mActionDbConfig->setObjectName(QStringLiteral("mActionDbConfig"));
	mActionDbConfig->setWhatsThis(tr("配置 PostGIS 数据库连接参数"));
	connect(mActionDbConfig, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::openDbConfig);
	mQGisIface->addToolBarIcon(mActionDbConfig);
	mQGisIface->addPluginToVectorMenu(tr("&数据库连接配置"), mActionDbConfig);

	/*【地图综合】*/
	QIcon iconGeneralization;
	QString strGeneralization = curExePath + "/resource/toolbox/theme_data_mapping.png";
	iconGeneralization.addFile(strGeneralization);
	mActionGeneralizationConfig = new QAction(iconGeneralization, tr("地图综合..."), this);
	mActionGeneralizationConfig->setObjectName(QStringLiteral("mActionGeneralizationConfig"));
	mActionGeneralizationConfig->setWhatsThis(tr("综合缩编管线: 预处理→图斑合并→协调化简→分要素缩编→冲突处理"));
	connect(mActionGeneralizationConfig, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::openGeneralizationConfig);
	mQGisIface->addToolBarIcon(mActionGeneralizationConfig);
	mQGisIface->addPluginToVectorMenu(tr("&地图综合"), mActionGeneralizationConfig);
#if defined(SE_NMO_NO_SDK)
	mActionGeneralizationConfig->setEnabled(false);
	mActionGeneralizationConfig->setToolTip(QStringLiteral("NMO SDK 未就绪（地图综合不可用）"));
#endif

	/*【导入数据到PostGIS】*/
	QIcon iconDataImport;
	QString strDataImport = curExePath + "/resource/toolbox/data_import_postgis.png";
	iconDataImport.addFile(strDataImport);
	mActionDataImportToPostGIS = new QAction(iconDataImport, tr("导入数据到PostGIS"), this);
	mActionDataImportToPostGIS->setObjectName(QStringLiteral("mActionDataImportToPostGIS"));
	mActionDataImportToPostGIS->setWhatsThis(tr("将 SHP/TIF/GDB 等数据导入到 PostGIS 数据库"));
	connect(mActionDataImportToPostGIS, &QAction::triggered,
	        this, &CGarMap_MapQualityCheckToolsPlugin::DataImportToPostGIS);
	mQGisIface->addToolBarIcon(mActionDataImportToPostGIS);
	mQGisIface->addPluginToVectorMenu(tr("&导入数据到PostGIS"), mActionDataImportToPostGIS);

	/*【综合成果自动化质检】*/
	QIcon iconAutoQualityCheck;
	QString strAutoQualityCheck = curExePath + "/resource/toolbox/feature_geometry_check.png";
	iconAutoQualityCheck.addFile(strAutoQualityCheck);
	mActionAutoQualityCheck = new QAction(iconAutoQualityCheck, tr("综合成果自动化质检..."), this);
	mActionAutoQualityCheck->setObjectName(QStringLiteral("mActionAutoQualityCheck"));
	mActionAutoQualityCheck->setWhatsThis(tr("综合成果自动化质检: 7组Mission 45项检查"));
	connect(mActionAutoQualityCheck, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::AutoQualityCheck);
	mQGisIface->addToolBarIcon(mActionAutoQualityCheck);
	mQGisIface->addPluginToVectorMenu(tr("&综合成果自动化质检"), mActionAutoQualityCheck);

/*【数据自动备份恢复】*/
	QIcon iconDataRestore;
	QString strDataRestore = curExePath + "/resource/toolbox/theme_data_mapping.png";
	iconDataRestore.addFile(strDataRestore);
	mActionDataRestore = new QAction(iconDataRestore, tr("数据自动备份恢复"), this);
	mActionDataRestore->setObjectName(QStringLiteral("mActionDataRestore"));
	mActionDataRestore->setWhatsThis(tr("数据自动备份与恢复管理"));
	connect(mActionDataRestore, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::DataRestore);
	mQGisIface->addToolBarIcon(mActionDataRestore);
	mQGisIface->addPluginToVectorMenu(tr("&数据自动备份恢复"), mActionDataRestore);

	/*【数据库连接】— 保留源码，暂不显示菜单（功能已由"数据库连接配置"覆盖）*/
	// QIcon iconDatabaseConnection;
	// QString strDatabaseConnection = curExePath + "/resource/toolbox/theme_data_mapping.png";
	// iconDatabaseConnection.addFile(strDatabaseConnection);
	// mActionDatabaseConnection = new QAction(iconDatabaseConnection, tr("数据库连接"), this);
	// mActionDatabaseConnection->setObjectName(QStringLiteral("mActionDatabaseConnection"));
	// mActionDatabaseConnection->setWhatsThis(tr("新建数据库连接"));
	// connect(mActionDatabaseConnection, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::DatabaseConnection);
	// mQGisIface->addToolBarIcon(mActionDatabaseConnection);
	// mQGisIface->addPluginToVectorMenu(tr("&数据库连接"), mActionDatabaseConnection);

	/*【数据导出】*/
	QIcon iconDataManagement;
	QString strDataManagement = curExePath + "/resource/toolbox/theme_data_mapping.png";
	iconDataManagement.addFile(strDataManagement);
	mActionDataManagement = new QAction(iconDataManagement, tr("数据导出"), this);
	mActionDataManagement->setObjectName(QStringLiteral("mActionDataManagement"));
	// 【2026-08-24】数据导出改为数据列表式导出
	mActionDataManagement->setWhatsThis(tr("以数据列表方式批量导出、按条件导出、按范围导出数据"));
	connect(mActionDataManagement, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::DataManagement);
	mQGisIface->addToolBarIcon(mActionDataManagement);
	mQGisIface->addPluginToVectorMenu(tr("&数据导出"), mActionDataManagement);

	/*【裁剪】— 已隐藏 */
	// QIcon iconClip;
	// QString strClip = curExePath + "/resource/toolbox/feature_extent_check.png";
	// iconClip.addFile(strClip);
	// mActionClip = new QAction(iconClip, tr("裁剪..."), this);
	// mActionClip->setObjectName(QStringLiteral("mActionClip"));
	// mActionClip->setWhatsThis(tr("使用裁剪要素或坐标范围对矢量数据进行裁剪"));
	// connect(mActionClip, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::Clip);
	// mQGisIface->addToolBarIcon(mActionClip);
	// mQGisIface->addPluginToVectorMenu(tr("&裁剪"), mActionClip);

	/*【合并】*/
	QIcon iconMerge;
	QString strMerge = curExePath + "/resource/toolbox/feature_extent_check.png";
	iconMerge.addFile(strMerge);
	mActionMerge = new QAction(iconMerge, tr("合并..."), this);
	mActionMerge->setObjectName(QStringLiteral("mActionMerge"));
	mActionMerge->setWhatsThis(tr("将多个矢量图层合并为一个数据集"));
	connect(mActionMerge, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::Merge);
	mQGisIface->addToolBarIcon(mActionMerge);
	mQGisIface->addPluginToVectorMenu(tr("&合并"), mActionMerge);
#if defined(SE_NMO_NO_SDK)
	mActionMerge->setEnabled(false);
	mActionMerge->setToolTip(QStringLiteral("NMO SDK 未就绪（合并不可用）"));
#endif

	/*【格式转换】*/
	QIcon iconFormatConversion;
	QString strFormatConversion = curExePath + "/resource/toolbox/theme_data_mapping.png";
	iconFormatConversion.addFile(strFormatConversion);
	mActionFormatConversion = new QAction(iconFormatConversion, tr("格式转换..."), this);
	mActionFormatConversion->setObjectName(QStringLiteral("mActionFormatConversion"));
	mActionFormatConversion->setWhatsThis(tr("矢量数据格式转换，支持GeoJSON、SHP、GPKG、GDB互转"));
	connect(mActionFormatConversion, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::FormatConversion);
	mQGisIface->addToolBarIcon(mActionFormatConversion);
	mQGisIface->addPluginToVectorMenu(tr("&格式转换"), mActionFormatConversion);

	/*【接边】*/
	QIcon iconAutoEdgeMatch;
	QString strAutoEdgeMatch = curExePath + "/resource/toolbox/feature_attribute_check.png";
	iconAutoEdgeMatch.addFile(strAutoEdgeMatch);
	mActionAutoEdgeMatch = new QAction(iconAutoEdgeMatch, tr("接边..."), this);
	mActionAutoEdgeMatch->setObjectName(QStringLiteral("mActionAutoEdgeMatch"));
	mActionAutoEdgeMatch->setWhatsThis(tr("对相邻图幅的矢量要素进行自动接边处理"));
	connect(mActionAutoEdgeMatch, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::AutoEdgeMatch);
	mQGisIface->addToolBarIcon(mActionAutoEdgeMatch);
	mQGisIface->addPluginToVectorMenu(tr("&接边"), mActionAutoEdgeMatch);
#if defined(SE_NMO_NO_SDK)
	mActionAutoEdgeMatch->setEnabled(false);
	mActionAutoEdgeMatch->setToolTip(QStringLiteral("NMO SDK 未就绪（接边不可用）"));
#endif

		// ====== 地图成果管理功能 ======

		/*【用户登录】*/
		QIcon iconLogin(QStringLiteral(":/theme_data_process_tools/images/icon_login.svg"));
		mActionProductLogin = new QAction(iconLogin, tr("用户登录"), this);
		mActionProductLogin->setObjectName(QStringLiteral("mActionProductLogin"));
		mActionProductLogin->setWhatsThis(tr("登录成果库管理系统"));
		connect(mActionProductLogin, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::ProductLogin);
		mQGisIface->addToolBarIcon(mActionProductLogin);
		mQGisIface->addPluginToVectorMenu(tr("&用户登录"), mActionProductLogin);

		/*【成果入库】*/
		QIcon iconStorage(QStringLiteral(":/theme_data_process_tools/images/icon_product_storage.svg"));
		mActionProductStorage = new QAction(iconStorage, tr("成果入库"), this);
		mActionProductStorage->setObjectName(QStringLiteral("mActionProductStorage"));
		mActionProductStorage->setWhatsThis(tr("上传与管理矢量、栅格、AI/CDR/PDF等多类型制图成果文件，支持版本管控"));
		connect(mActionProductStorage, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::ProductStorage);
		mQGisIface->addToolBarIcon(mActionProductStorage);
		mQGisIface->addPluginToVectorMenu(tr("&成果入库"), mActionProductStorage);

		/*【元数据管理】*/
		QIcon iconMetadata(QStringLiteral(":/theme_data_process_tools/images/icon_metadata.svg"));
		mActionMetadataManager = new QAction(iconMetadata, tr("元数据管理"), this);
		mActionMetadataManager->setObjectName(QStringLiteral("mActionMetadataManager"));
		mActionMetadataManager->setWhatsThis(tr("自动提取成果元数据，支持人工补录比例尺、编制信息、审图号、密级等信息"));
		connect(mActionMetadataManager, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::MetadataManager);
		mQGisIface->addToolBarIcon(mActionMetadataManager);
		mQGisIface->addPluginToVectorMenu(tr("&元数据管理"), mActionMetadataManager);

		/*【权限管理】*/
		QIcon iconAccess(QStringLiteral(":/theme_data_process_tools/images/icon_access_control.svg"));
		mActionAccessControl = new QAction(iconAccess, tr("权限管理"), this);
		mActionAccessControl->setObjectName(QStringLiteral("mActionAccessControl"));
		mActionAccessControl->setWhatsThis(tr("管理数据入库员和数据审核员账户，增删人员、设置初始密码"));
		connect(mActionAccessControl, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::AccessControl);
		mQGisIface->addToolBarIcon(mActionAccessControl);
		mQGisIface->addPluginToVectorMenu(tr("&权限管理"), mActionAccessControl);

	/*【数据导入】*/
	QIcon iconDataImportWizard(QStringLiteral(":/theme_data_process_tools/images/icon_product_storage.svg"));
	mActionDataImport = new QAction(iconDataImportWizard, tr("数据导入"), this);
	mActionDataImport->setObjectName(QStringLiteral("mActionDataImport"));
	mActionDataImport->setWhatsThis(tr("向导式数据导入，支持矢量/栅格/制图文件批量入库"));
	connect(mActionDataImport, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::DataImport);
	mQGisIface->addToolBarIcon(mActionDataImport);
	mQGisIface->addPluginToVectorMenu(tr("&数据导入"), mActionDataImport);

	/*【地图数据下载】*/
	QIcon iconMapDataDownload(QStringLiteral(":/theme_data_process_tools/images/icon_product_storage.svg"));
	mActionMapDataDownload = new QAction(iconMapDataDownload, tr("地图数据下载"), this);
	mActionMapDataDownload->setObjectName(QStringLiteral("mActionMapDataDownload"));
	mActionMapDataDownload->setWhatsThis(tr("从成果库检索并下载矢量、栅格、制图等成果数据"));
	connect(mActionMapDataDownload, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::MapDataDownload);
	mQGisIface->addToolBarIcon(mActionMapDataDownload);
	mQGisIface->addPluginToVectorMenu(tr("&地图数据下载"), mActionMapDataDownload);

	// 启动定时备份后台管理器（仅在启用时启动）
	CMapCheckBackupManager::instance()->loadSettings();
	if (CMapCheckBackupManager::instance()->isEnabled())
	{
		CMapCheckBackupManager::instance()->start();
	}

	///*【要素范围检查】*/
	//QIcon iconFeatureExtentCheck;
	//QString strFeatureExtentCheck = curExePath + "/resource/toolbox/feature_extent_check.png";
	//iconFeatureExtentCheck.addFile(strFeatureExtentCheck);
	//mActionFeatureExtentCheck = new QAction(iconFeatureExtentCheck, tr("检查要素范围..."), this);
	//mActionFeatureExtentCheck->setObjectName(QStringLiteral("mActionFeatureExtentCheck"));
	//mActionFeatureExtentCheck->setWhatsThis(tr("实现专题地理要素范围检查功能"));
	//connect(mActionFeatureExtentCheck, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::FeatureExtentCheck);
	//mQGisIface->addToolBarIcon(mActionFeatureExtentCheck);
	//mQGisIface->addPluginToVectorMenu(tr("&专题地理要素数据质量评估"), mActionFeatureExtentCheck);
	//mActionFeatureExtentCheck->setEnabled(true);

	///*【要素属性检查】*/
	//QIcon iconFeatureAttributeCheck;
	//QString strFeatureAttributeCheck = curExePath + "/resource/toolbox/feature_attribute_check.png";
	//iconFeatureAttributeCheck.addFile(strFeatureAttributeCheck);
	//mActionFeatureAttributeCheck = new QAction(iconFeatureAttributeCheck, tr("检查要素属性..."), this);
	//mActionFeatureAttributeCheck->setObjectName(QStringLiteral("mActionFeatureAttributeCheck"));
	//mActionFeatureAttributeCheck->setWhatsThis(tr("实现专题地理要素属性检查功能"));
	//connect(mActionFeatureAttributeCheck, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::FeatureAttributeCheck);
	//mQGisIface->addToolBarIcon(mActionFeatureAttributeCheck);
	//mQGisIface->addPluginToVectorMenu(tr("&专题地理要素数据质量评估"), mActionFeatureAttributeCheck);
	//mActionFeatureAttributeCheck->setEnabled(true);

	///*【数据存储与管理】*/
	//QIcon iconDataStorageManagement;
	//QString strDataStorageManagement = curExePath + "/resource/toolbox/feature_extent_check.png";
	//iconDataStorageManagement.addFile(strDataStorageManagement);
	//mActionDataStorageManagement = new QAction(iconDataStorageManagement, tr("数据存储与管理..."), this);
	//mActionDataStorageManagement->setObjectName(QStringLiteral("mActionDataStorageManagement"));
	//mActionDataStorageManagement->setWhatsThis(tr("实现专题地理要素数据存储与管理功能"));
	//connect(mActionDataStorageManagement, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::DataStorageManagement);
	//mQGisIface->addToolBarIcon(mActionDataStorageManagement);
	//mQGisIface->addPluginToVectorMenu(tr("&专题地理要素数据存储与管理"), mActionDataStorageManagement);
	//mActionDataStorageManagement->setEnabled(true);


}


void CGarMap_MapQualityCheckToolsPlugin::Test()
{
	CSE_VectorFormatConversionDialog* pDlg = new CSE_VectorFormatConversionDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}


void CGarMap_MapQualityCheckToolsPlugin::openDbConfig()
{
	DbConfigDialog dlg(mQGisIface->mainWindow());
	dlg.exec();
}


void CGarMap_MapQualityCheckToolsPlugin::openGeneralizationConfig()
{
	GeneralizationConfigDialog* pDlg = new GeneralizationConfigDialog(nullptr);
	pDlg->setAttribute(Qt::WA_DeleteOnClose);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::DataImportToPostGIS()
{
	CSE_DataImportDialog* pDlg = new CSE_DataImportDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::AutoQualityCheck()
{
	CSE_AutoQualityCheckDialog* pDlg = new CSE_AutoQualityCheckDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::DataRestore()
{
	CSE_DataRestoreDialog* pDlg = new CSE_DataRestoreDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::DatabaseConnection()
{
	CSE_DatabaseConnectionDialog* pDlg = new CSE_DatabaseConnectionDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::DataManagement()
{
	// 【2026-08-24】数据导出主入口：se_data_list_export（数据列表式导出，与样例包行为一致：批量/按条件/按范围/按主区裁切，
	// 功能入口在数据树节点右键菜单，见 CSE_DataListExportDialog）
	CSE_DataListExportDialog* pDlg = new CSE_DataListExportDialog(nullptr);
	pDlg->setWindowFlags(pDlg->windowFlags() | Qt::WindowCloseButtonHint);
	pDlg->setQgisInterface(mQGisIface);
	pDlg->setModal(false);
	pDlg->show();
}


void CGarMap_MapQualityCheckToolsPlugin::Clip()
{
	ClipDialog* pDlg = new ClipDialog(mQGisIface, nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::Merge()
{
	MergeDialog* pDlg = new MergeDialog(nullptr, Qt::WindowCloseButtonHint);
	// 对话框只发 addLayerToMap 信号（与平台无关）。
	// 集成到非 QGIS 平台时，替换下面这段 connect 为对方系统的图层加载接口即可。
	connect(pDlg, &MergeDialog::addLayerToMap, this, [this](const QString& path) {
		mQGisIface->addVectorLayer(path, QFileInfo(path).completeBaseName(), QStringLiteral("ogr"));
	});
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::FormatConversion()
{
	FormatConversionDialog* pDlg = new FormatConversionDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::AutoEdgeMatch()
{
	AutoEdgeMatchDialog* pDlg = new AutoEdgeMatchDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

// ====== 地图成果管理槽函数 ======

void CGarMap_MapQualityCheckToolsPlugin::ProductLogin()
{
	if (!PostgisConnector::instance()->isConnected())
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("登录失败"),
				tr("数据库未连接，请先配置数据库连接。"));
		}
		return;
	}

	// 静默同步平台登录态：平台已登录则直接复用其用户名与角色，不再弹登录框
	if (syncSessionFromPlatform())
	{
		if (mQGisIface)
		{
			QMessageBox::information(mQGisIface->mainWindow(),
				tr("用户登录"),
				tr("已同步平台登录用户：%1（%2）")
					.arg(gCurrentUserSession.userName)
					.arg(accessRoleToString(gCurrentUserSession.role)));
		}
		return;
	}

	if (mQGisIface)
	{
		QMessageBox::warning(mQGisIface->mainWindow(),
			tr("登录提示"),
			tr("请先在平台主界面完成用户登录，再使用本插件的成果管理功能。"));
	}
}

void CGarMap_MapQualityCheckToolsPlugin::ProductStorage()
{
	// 静默同步平台登录态（平台已登录则直接生效，无需再登录）
	syncSessionFromPlatform();

	if (!gCurrentUserSession.isLoggedIn || !gCurrentUserSession.canUploadProduct())
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("权限不足"),
				tr("您没有权限使用「成果入库」功能，请先登录或联系管理员升级权限。"));
		}
		return;
	}

	ProductStorageDialog* pDlg = new ProductStorageDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setQgisInterface(mQGisIface);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::MetadataManager()
{
	// 静默同步平台登录态（平台已登录则直接生效，无需再登录）
	syncSessionFromPlatform();

	if (!gCurrentUserSession.isLoggedIn || !gCurrentUserSession.canEditMetadata())
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("权限不足"),
				tr("您没有权限使用「元数据管理」功能，请先连接数据库并登录或联系管理员升级权限。"));
		}
		return;
	}

	MetadataManagerDialog* pDlg = new MetadataManagerDialog(nullptr, Qt::WindowCloseButtonHint);
	// 传入 QGIS 接口，启用"添加到地图"功能（新增的按目录树/按钮入口需要）
	pDlg->setQgisInterface(mQGisIface);
	// 继承全局深色 QSS（与上游一致）：追加透明规则，让 tab 页与栈容器透出 pane 深色背景
	QString dlgStyle = qApp->styleSheet();
	dlgStyle += QStringLiteral(
		"\nQTabWidget#mCompileTabWidget > QStackedWidget,"
		"\nQTabWidget#mCompileTabWidget > QStackedWidget > QWidget { background: transparent; }");
	pDlg->setStyleSheet(dlgStyle);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::AccessControl()
{
	// 静默同步平台登录态（平台已登录则直接生效，无需再登录）
	syncSessionFromPlatform();

	if (!gCurrentUserSession.isLoggedIn || !gCurrentUserSession.canManageUsers())
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("权限不足"),
				tr("您没有权限使用「权限管理」功能，仅数据库管理员可访问。"));
		}
		return;
	}

	ProductDAO dao;
	AccessControlDialog* pDlg = new AccessControlDialog(&dao, nullptr, Qt::WindowCloseButtonHint);
	pDlg->setCurrentOperator(gCurrentUserSession.userName);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::DataImport()
{
	// 静默同步平台登录态（平台已登录则直接生效，无需再登录）
	syncSessionFromPlatform();

	if (!gCurrentUserSession.isLoggedIn)
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("登录提示"),
				tr("请先在平台主界面完成用户登录并连接数据库。"));
		}
		return;
	}
	if (!gCurrentUserSession.canUploadProduct())
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("权限不足"),
				tr("您没有权限使用「数据导入」功能，请先登录或联系管理员升级权限。"));
		}
		return;
	}
	if (!PostgisConnector::instance()->isConnected())
	{
		if (mQGisIface)
		{
			QMessageBox::warning(mQGisIface->mainWindow(),
				tr("导入失败"),
				tr("数据库未连接，请先配置数据库连接。"));
		}
		return;
	}

	DataImportWizard wizard(mQGisIface->mainWindow());
	// 继承全局深色 QSS（与上游一致）：追加透明规则，让两个页面透出 QDialog 深色背景
	QString dlgStyle = qApp->styleSheet();
	dlgStyle += QStringLiteral(
		"\nQStackedWidget#mStackedWidget { background: transparent; }"
		"\nQStackedWidget#mStackedWidget > QWidget { background: transparent; }");
	wizard.setStyleSheet(dlgStyle);
	wizard.exec();
}

void CGarMap_MapQualityCheckToolsPlugin::MapDataDownload()
{
	mapdata_download* pDlg = new mapdata_download(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::unload()
{
	//// 去掉ui界面
	// mActionTest 已隐藏
	// mQGisIface->removePluginVectorMenu(tr("&测试矢量数据格式转换"), mActionTest);
	// mQGisIface->removeToolBarIcon(mActionTest);

	// 移除表单化UI菜单
	if (mActionDbConfig) {
		mQGisIface->removePluginVectorMenu(tr("&数据库连接配置"), mActionDbConfig);
		mQGisIface->removeToolBarIcon(mActionDbConfig);
		delete mActionDbConfig;
		mActionDbConfig = nullptr;
	}
	if (mActionGeneralizationConfig) {
		mQGisIface->removePluginVectorMenu(tr("&地图综合"), mActionGeneralizationConfig);
		mQGisIface->removeToolBarIcon(mActionGeneralizationConfig);
		delete mActionGeneralizationConfig;
		mActionGeneralizationConfig = nullptr;
	}
	if (mActionDataImportToPostGIS) {
		mQGisIface->removePluginVectorMenu(tr("&导入数据到PostGIS"), mActionDataImportToPostGIS);
		mQGisIface->removeToolBarIcon(mActionDataImportToPostGIS);
		delete mActionDataImportToPostGIS;
		mActionDataImportToPostGIS = nullptr;
	}
	if (mActionAutoQualityCheck) {
		mQGisIface->removePluginVectorMenu(tr("&综合成果自动化质检"), mActionAutoQualityCheck);
		mQGisIface->removeToolBarIcon(mActionAutoQualityCheck);
		delete mActionAutoQualityCheck;
		mActionAutoQualityCheck = nullptr;
	}

	// 移除数据导出 & 备份恢复菜单
	if (mActionDataRestore) {
		mQGisIface->removePluginVectorMenu(tr("&数据自动备份恢复"), mActionDataRestore);
		mQGisIface->removeToolBarIcon(mActionDataRestore);
		delete mActionDataRestore;
		mActionDataRestore = nullptr;
	}
	/*【数据库连接】— 已隐藏，暂不清理 */
	// if (mActionDatabaseConnection) {
	// 	mQGisIface->removePluginVectorMenu(tr("&数据库连接"), mActionDatabaseConnection);
	// 	mQGisIface->removeToolBarIcon(mActionDatabaseConnection);
	// 	delete mActionDatabaseConnection;
	// 	mActionDatabaseConnection = nullptr;
	// }
	if (mActionDataManagement) {
		mQGisIface->removePluginVectorMenu(tr("&数据导出"), mActionDataManagement);
		mQGisIface->removeToolBarIcon(mActionDataManagement);
		delete mActionDataManagement;
		mActionDataManagement = nullptr;
	}

	// 移除裁剪/合并/格式转换/接边菜单
	/*【裁剪】— 已隐藏 */
	// if (mActionClip) {
	// 	mQGisIface->removePluginVectorMenu(tr("&裁剪"), mActionClip);
	// 	mQGisIface->removeToolBarIcon(mActionClip);
	// 	delete mActionClip;
	// 	mActionClip = nullptr;
	// }
	if (mActionMerge) {
		mQGisIface->removePluginVectorMenu(tr("&合并"), mActionMerge);
		mQGisIface->removeToolBarIcon(mActionMerge);
		delete mActionMerge;
		mActionMerge = nullptr;
	}
	if (mActionFormatConversion) {
		mQGisIface->removePluginVectorMenu(tr("&格式转换"), mActionFormatConversion);
		mQGisIface->removeToolBarIcon(mActionFormatConversion);
		delete mActionFormatConversion;
		mActionFormatConversion = nullptr;
	}
	if (mActionAutoEdgeMatch) {
		mQGisIface->removePluginVectorMenu(tr("&接边"), mActionAutoEdgeMatch);
		mQGisIface->removeToolBarIcon(mActionAutoEdgeMatch);
		delete mActionAutoEdgeMatch;
		mActionAutoEdgeMatch = nullptr;
	}

	// 清理地图成果管理功能菜单
	gCurrentUserSession.clear();

	if (mActionProductLogin) {
			mQGisIface->removePluginVectorMenu(tr("&用户登录"), mActionProductLogin);
			mQGisIface->removeToolBarIcon(mActionProductLogin);
		delete mActionProductLogin;
		mActionProductLogin = nullptr;
	}
	if (mActionProductStorage) {
			mQGisIface->removePluginVectorMenu(tr("&成果入库"), mActionProductStorage);
			mQGisIface->removeToolBarIcon(mActionProductStorage);
		delete mActionProductStorage;
		mActionProductStorage = nullptr;
	}
	if (mActionMetadataManager) {
			mQGisIface->removePluginVectorMenu(tr("&元数据管理"), mActionMetadataManager);
			mQGisIface->removeToolBarIcon(mActionMetadataManager);
		delete mActionMetadataManager;
		mActionMetadataManager = nullptr;
	}
	if (mActionAccessControl) {
			mQGisIface->removePluginVectorMenu(tr("&权限管理"), mActionAccessControl);
			mQGisIface->removeToolBarIcon(mActionAccessControl);
		delete mActionAccessControl;
		mActionAccessControl = nullptr;
	}
	if (mActionDataImport) {
		mQGisIface->removePluginVectorMenu(tr("&数据导入"), mActionDataImport);
		mQGisIface->removeToolBarIcon(mActionDataImport);
		delete mActionDataImport;
		mActionDataImport = nullptr;
	}
	if (mActionMapDataDownload) {
		mQGisIface->removePluginVectorMenu(tr("&地图数据下载"), mActionMapDataDownload);
		mQGisIface->removeToolBarIcon(mActionMapDataDownload);
		delete mActionMapDataDownload;
		mActionMapDataDownload = nullptr;
	}

	// 停止定时备份管理器
	CMapCheckBackupManager::instance()->stop();

	// 清理数据管理模块中保留的数据库连接记录
	CSE_DataManagementDialog::clearConnections();

	// mActionTest 已隐藏
	// delete mActionTest;
}
void CGarMap_MapQualityCheckToolsPlugin::updateActions()
{
	bool dbConnected = PostgisConnector::instance()->isConnected();
	if (mActionProductLogin) mActionProductLogin->setEnabled(dbConnected);
	if (mActionProductStorage) mActionProductStorage->setEnabled(dbConnected);
	if (mActionMetadataManager) mActionMetadataManager->setEnabled(dbConnected);
	if (mActionAccessControl) mActionAccessControl->setEnabled(dbConnected);
	if (mActionDataImport) mActionDataImport->setEnabled(dbConnected);
}

/**
 * Required extern functions needed  for every plugin
 * These functions can be called prior to creating an instance
 * of the plugin class
 */
 // Class factory to return a new instance of the plugin class
QGISEXTERN QgisPlugin* classFactory(QgisInterface* qgisInterfacePointer)
{
	return new CGarMap_MapQualityCheckToolsPlugin(qgisInterfacePointer);
}

// Return the name of the plugin - note that we do not user class members as
// the class may not yet be insantiated when this method is called.
QGISEXTERN const QString* name()
{
	return &sName;
}

// Return the description
QGISEXTERN const QString* description()
{
	return &sDescription;
}

// Return the category
QGISEXTERN const QString* category()
{
	return &sCategory;
}

// Return the type (either UI or MapLayer plugin)
QGISEXTERN int type()
{
	return sPluginType;
}

// Return the version number for the plugin
QGISEXTERN const QString* version()
{
	return &sPluginVersion;
}

QGISEXTERN const QString* icon()
{
	return &sPluginIcon;
}

// Delete ourself
QGISEXTERN void unload(QgisPlugin* pluginPointer)
{
	delete pluginPointer;
}


