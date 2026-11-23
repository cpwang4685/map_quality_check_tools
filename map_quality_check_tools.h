#ifndef MAP_QUALITY_CHECK_TOOLS_PLUGIN_H
#define MAP_QUALITY_CHECK_TOOLS_PLUGIN_H

#include "qgisplugin.h"
#include <QObject>
// 【2026-10-09】插件工具窗口管理要用到这三个：哈希表存窗口注册表，
// QPointer 做弱引用（窗口是"关窗即销毁"，必须能自动置空），QWidget 是父/宿主窗口类型。
#include <QHash>
#include <QPointer>
#include <QWidget>

#include <functional>
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
    // 【2026-09-10】参数配置（独立入口：不预置 XML，由用户在对话框里自行打开）
    void ParamConfig();

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

    // 【2026-09-10】参数配置独立菜单项。
    // 必须创建在 initGui 的最末尾：LTZK 平台用 findChildren<QAction*> 的枚举序号
    // 做 plugin_bindings.json 的 fallbackActionIndex，插在中间会让其后所有下标平移，
    // 使 JSON 里已有的绑定错位（现有 0..14 全部依赖创建顺序）。
    QAction* mActionParamConfig = nullptr;

    void updateActions();

protected:
    // 【2026-10-09】监视平台主界面的关闭事件：主界面一关，本插件开着的窗口跟着关。
    // 为什么要靠事件过滤器而不是"挂父对象"：见 hostWindow() 实现处的说明。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:

    // ===== 插件工具窗口管理【2026-10-09 新增】=====
    //
    // 要解决的两个毛病：
    //   1. 平台主界面关掉后，本插件打开的窗口还留着不关（而且因为还有可见顶层窗口，
    //      进程也不退出）；
    //   2. 同一个菜单点两次会开出两个一模一样的窗口。
    //
    // 窗口仍然保持"无父对象的独立顶层窗口"（不挂到平台上，原因见 hostWindow()），
    // 靠下面这三件事解决：注册表查重、关窗即销毁、主界面关闭时统一 close。

    // 窗口注册表：key = 每个入口一个的稳定标识，value = 弱引用。
    // 因为窗口都设了 WA_DeleteOnClose，必须用 QPointer —— 窗口被销毁后自动变空指针。
    QHash<QString, QPointer<QWidget>> m_openToolWindows;

    // 平台主界面（LTZK 下即 LTZK 主窗口本身），只用于装事件过滤器，不作为父对象。
    QPointer<QWidget> m_hostWindow;

    // 取平台主界面并在首次调用时装好关闭监视。
    QWidget* hostWindow();

    // 同一个 key 的窗口只保留一个：已经开着就拉到前台返回，否则用 factory 造一个。
    // factory 里只做"构造 + 配置"，不要 show()（本函数统一 show）。
    QWidget* openToolWindow(const QString& key, const std::function<QWidget*()>& factory);

    // 关闭本插件打开的全部工具窗口。必须"主动 close"而不是等它们被连带隐藏：
    // 有几个对话框的路径/设置写在 closeEvent 与析构里，只隐藏的话配置会丢。
    void closeAllToolWindows();

};

#endif // MAP_QUALITY_CHECK_TOOLS_PLUGIN_H
