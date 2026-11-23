#ifndef DATA_CATALOG_BROWSER_H
#define DATA_CATALOG_BROWSER_H

#include <QIcon>
#include <QList>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <qgsmimedatautils.h>

#include "DataCatalogConnectionDialog.h"

class QTreeWidget;
class QTreeWidgetItem;
class QgsBrowserGuiModel;

// ============================================================
// 数据目录树（数据库数据 / 本地数据 / 地图图层）
//
// 【2026-10-25 数据裁剪改造】列表改成和「从数据库获取数据源」对话框
// （ProductSourceDialog）的数据列表同一套设计：
//   - 一棵 QTreeWidget，数据库数据 / 本地数据 / 地图图层 是三个固定顶级节点；
//   - 可裁剪的数据节点带复选框，勾父带子、子全勾父自动勾（含半选态）；
//   - 图标按 点/线/面/栅格 区分；
//   - 目录 / GDB 展开时才枚举子节点（懒加载），避免一打开对话框就扫全盘。
// 原来那套"平台左侧数据列表"的三段式分隔布局与 QgsBrowserTreeView 不再使用。
//
// 本地数据的"已连接文件夹"仍然存在 QGIS 收藏夹里（与 LTZK 平台左侧数据列表、
// QGIS 自己的浏览器共用同一份清单），这里只把收藏夹当清单用，子节点自己用 QDir 枚举。
//
// 本类只负责"树 + 连接/刷新/勾选"，数据的导出与加载由宿主对话框处理：
// 右键时发出 contextMenuRequested()，双击时发出 itemActivated()，
// 勾选变化时发出 checkedCountChanged()。
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
        // 【2026-10-25】是否为"已连接的本地数据文件夹"根节点（右键时可断开）
        bool isLocalFolderRoot = false;
    };

    explicit CSE_DataCatalogBrowser(QWidget* parent = nullptr);
    ~CSE_DataCatalogBrowser() override;

    // ---- 顶部按钮 / 右键菜单绑定的动作 ----
    // "选择数据库"：连接参数对话框 → 打开连接 → 刷新数据库段
    void connectDatabase();
    // 只刷新数据库段
    void refreshDatabaseData();
    // "选择文件夹"：挑一个目录加入已连接文件夹（QGIS 收藏夹）→ 刷新本地段
    void connectLocalFolder();
    // 断开当前选中的那个已连接文件夹（只断根节点）
    void disconnectSelectedLocalFolder();
    // 只刷新本地段
    void refreshLocalFolders();
    // 只刷新地图图层段
    void refreshMapLayers();
    // 三段一起刷新（顶部"刷新"按钮）
    void refreshAll();

    // 关键字过滤：空格分隔多个关键字，全部命中才显示（不区分大小写）
    void setFilterKeyword(const QString& keyword);

    // 当前勾选（含半选）的、能产出裁剪数据的节点个数，用于按钮置灰
    int checkedCount() const;
    // 勾选展开成裁剪数据：每个具体数据节点一条，已按数据源去重
    QList<CatalogItem> checkedItems() const;

signals:
    // 列表上右键：globalPos 为屏幕坐标，item 为命中的节点
    void contextMenuRequested(const QPoint& globalPos,
        const CSE_DataCatalogBrowser::CatalogItem& item);
    // 列表上双击具体数据节点：请求把数据加到地图
    void itemActivated(const CSE_DataCatalogBrowser::CatalogItem& item);
    // 提示信息（isError 为 true 时为错误）
    void message(const QString& text, bool isError);
    // 勾选变化（参数为 checkedCount()）
    void checkedCountChanged(int count);
    // 顶部状态行文字（连接的是哪个库、几个文件夹、几个图层）
    void statusTextChanged(const QString& text);

private slots:
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onTreeItemExpanded(QTreeWidgetItem* item);
    void onTreeItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onTreeContextMenu(const QPoint& pos);

private:
    void buildUi();

    // ---- 三个顶级节点 ----
    void buildRoots();
    void clearChildren(QTreeWidgetItem* item);
    // 造一个子节点（kind/文字/图标/是否可勾选），返回它
    QTreeWidgetItem* addNode(QTreeWidgetItem* parent, int kind,
        const QString& text, bool checkable);
    // 灰字提示节点（未连接 / 空 / 警告），不可勾选
    QTreeWidgetItem* addMessageNode(QTreeWidgetItem* parent, const QString& text,
        bool isError);
    // 给容器挂一个"展开时再读"的占位子节点
    void markLazy(QTreeWidgetItem* item);

    // ---- 数据库段 ----
    bool openDatabaseConnection(const DataCatalogConnectionParameters& parameters,
        QString* errorMessage);
    void closeDatabaseConnection();
    QIcon databaseItemIcon(const QString& layerType, const QString& geometryType) const;
    // 数据库表 → QGIS 数据源
    QgsMimeDataUtils::Uri databaseUriFor(const QString& schema,
        const QString& table, const QString& geometryColumn,
        const QString& layerType) const;
    // 数据库栅格的 GDAL 连接串候选（多种写法，供裁剪引擎逐个探测）
    QStringList databaseRasterUriCandidates(const QString& schema,
        const QString& table, const QString& geometryColumn) const;

    // ---- 本地段 ----
    // QGIS 收藏夹只当"已连接文件夹清单"用，不挂任何视图
    void initializeFavoritesModel();
    // 已连接文件夹（路径, 显示名）
    QList<QPair<QString, QString> > favoriteFolders() const;
    // 展开时才读子节点（目录枚举 / GDB、GPKG 图层枚举）
    void populateLocalChildren(QTreeWidgetItem* item);
    QTreeWidgetItem* appendLocalFileItem(QTreeWidgetItem* parent,
        const QString& filePath);

    // ---- 地图图层段 ----
    void rebuildMapLayerSection();

    // ---- 勾选 ----
    void setSubtreeCheck(QTreeWidgetItem* item, Qt::CheckState state);
    void refreshParentCheckStates(QTreeWidgetItem* item);
    // 重建整棵树后自下而上重算父节点状态（懒加载未展开的节点跳过）
    Qt::CheckState recomputeCheckState(QTreeWidgetItem* item);
    // 重建前记下勾选，重建后恢复（避免刷新一次勾选就丢）
    QSet<QString> checkedKeys() const;
    void restoreChecked(const QSet<QString>& keys);
    static QString nodeKey(QTreeWidgetItem* item);
    static bool isCheckable(QTreeWidgetItem* item);

    // ---- 过滤 ----
    void applyFilter();
    void applyFilterTo(QTreeWidgetItem* item, bool* anyVisible);
    bool textMatchesFilter(const QString& text) const;

    // ---- 节点 → 裁剪数据 ----
    CatalogItem catalogItemFor(QTreeWidgetItem* item) const;
    // 单个具体数据节点 → CatalogItem（isLayer==true）
    CatalogItem leafCatalogItem(QTreeWidgetItem* item) const;
    // 一个节点的整棵子树 → CatalogItem 列表（右键"导出这个节点"用）
    void collectLeafItems(QTreeWidgetItem* item, QList<CatalogItem>* out,
        QSet<QString>* seen) const;
    // 勾选树 → CatalogItem 列表（三态语义：勾选=整棵子树，半选=逐个子节点）
    void collectCheckedItems(QTreeWidgetItem* item, QList<CatalogItem>* out,
        QSet<QString>* seen) const;
    // 本地路径 → 其下所有数据源（递归）
    void collectLocalSources(const QString& path,
        QList<QgsMimeDataUtils::Uri>* uris) const;

    void updateStatusText();
    // 地图图层节点是否可以勾选（不能：地图里的图层没有可喂给裁剪引擎的 uri）
    static bool kindIsCheckable(int kind);

    // ---- 成员 ----
    QTreeWidget* m_tree = nullptr;
    QTreeWidgetItem* m_rootDatabase = nullptr;
    QTreeWidgetItem* m_rootLocal = nullptr;
    QTreeWidgetItem* m_rootMapLayer = nullptr;

    // 只用于读写 QGIS 收藏夹（已连接文件夹），不挂到任何视图
    QgsBrowserGuiModel* m_favoritesModel = nullptr;
    int m_favoritesRetry = 0;

    QString m_databaseConnectionName;
    int m_databaseConnectionSerial = 0;

    QStringList m_filterKeys;
    bool m_syncing = false;   // 复选状态批量传播时的重入保护
    int m_filterDepth = 0;    // 过滤进行中（>0 时不再重入，避免展开→过滤→再展开的递归）
};

#endif // DATA_CATALOG_BROWSER_H
