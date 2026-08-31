#ifndef MAP_QUALITY_CHECK_TOOLS_PLUGIN_H
#define MAP_QUALITY_CHECK_TOOLS_PLUGIN_H

#include "qgisplugin.h"
#include <QObject>

#include <string>
#include <vector>
using namespace std;

class QAction;
class QgisInterface;

// 前向声明表单化对话框
class DbConfigDialog;
class GeneralizationConfigDialog;
class CSE_DataImportDialog;
class CSE_AutoQualityCheckDialog;
class CSE_DataRestoreDialog;
class CSE_DatabaseConnectionDialog;
class CSE_DataListExportDialog;
class ClipDialog;
class MergeDialog;
class FormatConversionDialog;
class AutoEdgeMatchDialog;

// 地图成果管理模块
class LoginDialog;
class ProductStorageDialog;
class MetadataManagerDialog;
class AccessControlDialog;
class DataImportWizard;
class mapdata_download;

class CGarMap_MapQualityCheckToolsPlugin : public QObject, public QgisPlugin
{
    Q_OBJECT

public:
    explicit CGarMap_MapQualityCheckToolsPlugin(QgisInterface* qgisInterface);
    ~CGarMap_MapQualityCheckToolsPlugin() override;

public slots:

    //! 初始化Gui
    void initGui() override;
    //! actions

    // 测试矢量数据格式转换
    void Test();

    // 表单化 UI 对话框入口
    void openDbConfig();
    void openGeneralizationConfig();
    void DataImportToPostGIS();

    // 综合成果自动化质检
    void AutoQualityCheck();

    // 数据自动备份恢复
    void DataRestore();

    // 数据库连接
    void DatabaseConnection();

    // 数据导出（数据列表式导出：批量/按条件/按范围/按主区裁切，se_data_list_export）
    void DataManagement();

    // 裁剪
    void Clip();
    // 合并
    void Merge();
    // 格式转换
    void FormatConversion();
    // 要素自动接边
    void AutoEdgeMatch();

    // === 地图成果管理 ===
    void ProductLogin();
    void ProductStorage();
    void MetadataManager();
    void AccessControl();
    void DataImport();
    void MapDataDownload();

    //! 卸载插件
    void unload() override;

private:

    //! Pointer to the QGIS interface object
    QgisInterface* mQGisIface = nullptr;

    //! pointer to the qaction for this plugin

    // "矢量数据格式转换"菜单
    QAction* mActionTest = nullptr;

    // 表单化UI菜单
    QAction* mActionDbConfig = nullptr;
    QAction* mActionGeneralizationConfig = nullptr;
    QAction* mActionDataImportToPostGIS = nullptr;
    QAction* mActionAutoQualityCheck = nullptr;

    // 数据导出 & 备份恢复
    QAction* mActionDataRestore = nullptr;
    QAction* mActionDatabaseConnection = nullptr;
    QAction* mActionDataManagement = nullptr;

    // 裁剪/合并/格式转换/要素自动接边
    QAction* mActionClip = nullptr;
    QAction* mActionMerge = nullptr;
    QAction* mActionFormatConversion = nullptr;
    QAction* mActionAutoEdgeMatch = nullptr;

    // 地图成果管理
    QAction* mActionProductLogin = nullptr;
    QAction* mActionProductStorage = nullptr;
    QAction* mActionMetadataManager = nullptr;
    QAction* mActionAccessControl = nullptr;
    QAction* mActionDataImport = nullptr;
    QAction* mActionMapDataDownload = nullptr;

    void updateActions();

private:



};

#endif // MAP_QUALITY_CHECK_TOOLS_PLUGIN_H
