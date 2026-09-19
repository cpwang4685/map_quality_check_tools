#ifndef DATA_CATALOG_BROWSER_H
#define DATA_CATALOG_BROWSER_H

#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <QIcon>
#include <qgsmimedatautils.h>

#include "DataCatalogConnectionDialog.h"

class QLabel;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class QStandardItem;
class QStandardItemModel;
class QModelIndex;
class QWidget;
class QgsBrowserGuiModel;
class QgsBrowserTreeView;
class QgsDataItem;

// ============================================================
// 数据目录浏览器（数据库数据 / 本地数据 / 地图图层 三段）
//
// 由 LTZK 平台左侧数据列表 DataCatalogWidget（D:\thelast\01_Source）移植：
//   - 数据库段：QStandardItemModel + QTreeView，连接走 DataCatalogConnectionDialog
//     （平台同名对话框），树内容为 PostGIS 表/空间图层枚举（平台
//     refreshDatabaseData() 的"普通表浏览"分支，去掉成果目录那一段）；
//   - 本地段：QgsBrowserGuiModel + QgsBrowserTreeView（setBrowserModel 直接绑，
//     不带平台那个 CatalogBrowserProxyModel 过滤层——本次不做勾选/批量导出），
//     与平台一致的"收藏夹即本地数据文件夹"实现；
//   - 地图图层段：插件自有（平台没有），列出当前 QGIS 工程中已加载的图层；
//   - 各段 objectName / 属性全部照平台拼写，以复用平台注入的 QSS。
//
// 本类只负责"列表 + 连接/刷新"，数据的导出与加载由宿主对话框处理：
// 右键时发出 contextMenuRequested()，双击时发出 itemActivated()。
// ============================================================
class CSE_DataCatalogBrowser : public QWidget
{
    Q_OBJECT

public:
    // 右键/双击命中的节点信息
    struct CatalogItem
    {
        enum SourceKind
        {
            Database = 0,
            Local = 1,
            MapLayer = 2
        };

        SourceKind kind = Database;
        bool valid = false;         // 是否真的命中了节点
        bool isLayer = false;       // 是否是可裁剪的具体数据（数据库表 / 本地数据文件）
        bool isRaster = false;      // 数据是否为栅格（单项时有效）
        QString displayName;        // 节点显示名
        QString detail;             // 数据库的 "schema.table" / 本地路径，用于日志与输出命名
        // 可加载到地图的数据源（数据库表 / 本地文件；目录节点为其下所有数据）
        QList<QgsMimeDataUtils::Uri> uris;
        // 栅格裁剪用的 GDAL 连接串候选（数据库栅格有多种写法，逐个探测）
        QStringList rasterUriCandidates;
        QString mapLayerId;         // kind == MapLayer 时的地图图层 id
    };

    explicit CSE_DataCatalogBrowser(QWidget* parent = nullptr);
    ~CSE_DataCatalogBrowser() override;

    // ---- 顶部按钮绑定的动作：与平台 DataCatalogWidget 的同名函数一一对应 ----
    // "选择数据库" → 平台 connectDatabase()（登录检查 → 连接参数对话框 → 打开连接 → 刷新）
    void connectDatabase();
    // "刷新"（数据库组）→ 平台 refreshDatabaseData()
    void refreshDatabaseData();
    // "选择文件夹" → 平台 connectLocalFolder()
    void connectLocalFolder();
    // "断开连接" → 平台 disconnectSelectedLocalFolder()（只断开当前选中的文件夹根节点）
    void disconnectSelectedLocalFolder();
    // "刷新"（本地组）→ 平台 refreshLocalFolders()
    void refreshLocalFolders();
    // "刷新"（地图图层组）→ 插件自有
    void refreshMapLayers();

signals:
    // 列表上右键：globalPos 为屏幕坐标，item 为命中的节点
    void contextMenuRequested(const QPoint& globalPos,
        const CSE_DataCatalogBrowser::CatalogItem& item);
    // 列表上双击：请求把数据加到地图
    void itemActivated(const CSE_DataCatalogBrowser::CatalogItem& item);
    // 提示信息（isError 为 true 时为错误）
    void message(const QString& text, bool isError);

private slots:
    void onDatabaseDoubleClicked(const QModelIndex& index);
    void onLocalDoubleClicked(const QModelIndex& index);
    void onMapLayerDoubleClicked(QTreeWidgetItem* item, int column);
    void onDatabaseContextMenu(const QPoint& pos);
    void onLocalContextMenu(const QPoint& pos);
    void onMapLayerContextMenu(const QPoint& pos);
    void showFavoritesInTree();

private:
    void buildUi();
    void initializeLocalBrowser();
    // 展开本地目录树到指定路径。麒麟 SDK 缺 QgsBrowserTreeView::expandPath，故自行实现
    void expandLocalTreeToPath(const QString& path);
    QModelIndex favoritesRootIndex();

    // ---- 数据库段（平台实现） ----
    bool openDatabaseConnection(const DataCatalogConnectionParameters& parameters,
        QString* errorMessage);
    void closeDatabaseConnection();
    void showDatabaseMessage(const QString& message, bool isError);
    QStandardItem* schemaItem(const QString& schemaName, QStandardItem* databaseItem);
    QgsMimeDataUtils::Uri databaseUri(const QStandardItem* item) const;
    // 数据库栅格的 GDAL 连接串候选（多种写法，供裁剪引擎逐个探测）
    QStringList databaseRasterUriCandidates(const QString& schema,
        const QString& table, const QString& geometryColumn) const;
    QIcon databaseItemIcon(const QString& layerType) const;

    // ---- 节点 → 数据源 ----
    void collectLocalUris(QgsDataItem* item,
        QList<QgsMimeDataUtils::Uri>* uris, int* unloadedCount) const;
    CatalogItem databaseItemAt(const QPoint& pos) const;
    CatalogItem localItemAt(const QPoint& pos) const;
    CatalogItem mapLayerItemAt(const QPoint& pos) const;

    // ---- 成员（命名与平台一致） ----
    QStandardItemModel* m_databaseModel = nullptr;
    QTreeView* m_databaseTree = nullptr;
    QgsBrowserGuiModel* m_browserModel = nullptr;
    QgsBrowserTreeView* m_localTree = nullptr;
    QLabel* m_localStatusLabel = nullptr;
    QString m_databaseConnectionName;
    int m_databaseConnectionSerial = 0;

    // 地图图层段（插件自有）
    QTreeWidget* m_mapLayerTree = nullptr;
};

#endif // DATA_CATALOG_BROWSER_H
