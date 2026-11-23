#ifndef PRODUCT_SOURCE_DIALOG_H
#define PRODUCT_SOURCE_DIALOG_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>

#include "database/product_metadata.h"
#include "core/clip_export_engine.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QgisInterface;
class ProductDAO;

// ============================================================
// 「从数据库获取数据源」对话框
//
// 入口：地图综合配置对话框 → 数据源 → 矢量数据目录下方的
//       「从数据库获取数据」按钮。
//
// 主体是地图成果库的固定目录树（制图成果 / 制图要素 / 制图资料），
// 目录与数据节点前带复选框，勾选可批量导出；右键目录或数据节点
// 可直接「按范围导出并设置为数据源」「按制图导出并设置为数据源」
// 「将该数据设置为数据源」或「添加到地图」。
//
// 两个裁剪导出复用成果模块同一套界面与引擎：
//   - 按范围导出：CSE_RangeExportDialogUi + ClipExportEngine
//   - 按制图导出：CSE_MainAreaClipDialogUi + ClipExportEngine
// 产品记录先转换成 ClipExportEngine::Source：
//   - 矢量：layer_table_name 对应的 PostGIS 图层表（schema 取 search_path）
//   - 栅格：PG 大对象 file_oid 落盘为临时文件后再交给 GDAL
// 其余类型（AI/PDF/CAD/文档/压缩包等）不是空间数据，不显示复选框、
// 右键导出置灰。
// ============================================================
class ProductSourceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProductSourceDialog(QWidget* parent = nullptr);
    ~ProductSourceDialog() override;

    // 地图接口（画布用于手动绘制范围；主窗口由 QGIS 内部使用）
    void setQgisInterface(QgisInterface* iface);
    // 导出完成后是否自动加载到地图（默认 true）
    void setAutoLoadAfterExport(bool on) { m_autoLoad = on; }

signals:
    // 【2026-10-18】右键「…并设置为数据源」/「将该数据设置为数据源」导出成功后发出。
    // 参数可直接填进地图综合对话框的「矢量数据目录」：
    //   GDB → .gdb 目录路径；其余（shp / gpkg / 栅格）→ 所在文件夹路径。
    void dataSourceReady(const QString& path);

    // 【2026-11-07】「按范围导出」里点「开始绘制」后，本对话框会先隐藏、等用户在
    // 地图上框选完再弹出。父对话框（地图综合对话框）若也要跟着一起藏起来，
    // 连这两个信号做 hide()/show() 即可。
    void drawingStarted();
    void drawingFinished();

private slots:
    void refreshTree();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onContextMenuRequested(const QPoint& pos);
    void onExportCheckedByRange();
    void onExportCheckedByMainArea();

private:
    void buildUi();
    void populateChildren(QTreeWidgetItem* parentItem, int parentDirId);
    static void setItemCheckable(QTreeWidgetItem* item, bool on);
    static void setSubtreeCheck(QTreeWidgetItem* item, Qt::CheckState state);
    static void refreshParentCheckStates(QTreeWidgetItem* item);
    void collectProductsFromItem(QTreeWidgetItem* item,
        QList<ProductMetadata>& out, QSet<int>& seen) const;
    QList<ProductMetadata> productsOfItem(QTreeWidgetItem* item) const;
    QList<ProductMetadata> checkedProducts() const;
    static QList<ProductMetadata> clippableOnly(const QList<ProductMetadata>& in);

    // 产品记录 → 裁剪引擎数据源。失败时 err 返回原因。
    // 栅格大对象落盘的临时文件记录在 m_tempFiles，导出结束后按需清理。
    bool productToSource(const ProductMetadata& meta,
        ClipExportEngine::Source& src, QString& err);

    // 【2026-10-18】导出文件的基础名：数据挂在图层节点（node_type=1）下时用节点名，
    // 也就是 GDB 里的原始图层名（如 A_LvYouJingQu）；其余情况仍用产品名。
    // 产品名带 "县名_v1_" 前缀，直接拿来导出会得到 永登县_v1_a_lvyoujingqu.shp，
    // 不是用户认得的原始名称。
    QString exportBaseName(const ProductMetadata& meta);

    void exportProducts(const QList<ProductMetadata>& products, bool byRange,
        bool setAsSource = false);
    // 【2026-10-18】不裁剪，直接把所选数据整份导出到输出文件夹，并设为数据源。
    // 输出格式/目录复用「按范围导出」对话框（范围可选模式）。
    void exportDirectToSource(const QList<ProductMetadata>& products);

    // 产品记录 → 引擎数据源（两个导出入口共用）。无可用数据时返回 false。
    bool prepareSources(const QList<ProductMetadata>& products,
        const QString& title, QList<ClipExportEngine::Source>& sources);

    QStringList runClipExport(const QList<ClipExportEngine::Source>& sources,
        const ClipExportEngine::Options& opt, const QString& title, bool autoLoad);
    void autoLoadOutputToMap(const QString& path);
    // 实际输出文件 → 可填进「矢量数据目录」的路径（.gdb 用自身，其余用所在目录）
    static QString dataSourcePathFor(const QString& writtenFile);
    void updateButtonStates();

    static QString stripDataSuffix(const QString& name);
    static bool driverForFormat(const QString& format, QString& driver, bool& merge);

    QTreeWidget* m_tree = nullptr;
    QLabel* m_lblStatus = nullptr;
    QPushButton* m_btnRangeChecked = nullptr;
    QPushButton* m_btnMainAreaChecked = nullptr;
    QPushButton* m_btnRefresh = nullptr;
    QPushButton* m_btnClose = nullptr;

    QgisInterface* m_iface = nullptr;
    ProductDAO* m_dao = nullptr;

    bool m_autoLoad = true;
    bool m_syncing = false;          // 复选状态批量传播时的重入保护
    QStringList m_tempFiles;         // 本次导出的栅格 LO 临时文件
    QHash<int, DirectoryNode> m_dirCache; // 【2026-10-18】目录节点缓存（取图层名用）
};

#endif // PRODUCT_SOURCE_DIALOG_H
