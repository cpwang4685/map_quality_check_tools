#pragma once
#include "ui_metadata_manager_dialog.h"
#include "database/product_metadata.h"
#include <QDialog>
#include <QTreeWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QTextEdit>
#include <QLineEdit>
#include <QFormLayout>
#include <QSet>

class ProductDAO;
class QgisInterface;

class MetadataManagerDialog : public QDialog
{
	Q_OBJECT

public:
	explicit MetadataManagerDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~MetadataManagerDialog();

	void setQgisInterface(QgisInterface* iface) { mQGisIface = iface; }

protected:
	// 每次显示时刷新树与产品列表，确保最新入库数据立即可见
	void showEvent(QShowEvent* event) override;

private slots:
	void onTypeTreeItemClicked(QTreeWidgetItem* item, int column);
	void onResultDirTreeItemClicked(QTreeWidgetItem* item, int column);
	void onResultDirTreeContextMenu(const QPoint& pos);      // 按地图成果类型分类树右键菜单
	void onAddResultDirSelectionToMap();                     // 将选中节点及其子孙产品添加到地图
	void onTagTreeItemClicked(QTreeWidgetItem* item, int column);
	void onProductSelected();
	void onSaveMetadata();
	void onResetForm();
	void onAddToMap();
	void onAddTag();
	void onRemoveTag();
	void onTagDoubleClicked(QListWidgetItem* item);
	void onNewTag();
	void onDeleteTag();

private:
	void setupConnections();
	void setupTableColumns();
	void loadProductTypeTree();
	void filterProductsByType(const QString& typeName);
	void loadResultDirTree();                                 // 加载"按地图成果类型分类"固定目录树
	void populateResultDirChildren(QTreeWidgetItem* parent, int parentDirId);
	void loadProductListByDir(int dirId);                     // 按目录过滤产品列表
	void fillMetadataForm(const ProductMetadata& meta);
	void collectMetadataForm(ProductMetadata& meta);

	// 动态构建编制信息 TabWidget 中的表单控件
	void buildCompileForm(const ProductMetadata& meta);
	void collectCompileFields(ProductMetadata& meta);

	// 从专有元数据 Tab 控件中收集值
	void collectSpeFromForm(ProductVectorMeta& vm);
	void collectSpeFromForm(ProductRasterMeta& rm);
	void collectSpeFromForm(ProductDiagramMeta& dm);
	void collectSpeFromForm(ProductDocumentMeta& dm);

	void loadAllProducts();
	void loadProductsToTable(const QList<ProductMetadata>& products);
	void ensureDefaultTags();
	void loadTags();
	void loadTagTree();

	Ui::MetadataManagerDialog ui;

	int mCurrentProductId = -1;
	QTreeWidgetItem* mCurrentDirItem = nullptr;
	int mCurrentDirId = 0;         // 0=全部, other=目录ID（保留兼容）
	QString mCurrentTypeFilter;    // 当前选中的产品类型过滤
	QList<ProductMetadata> mAllProducts;
	ProductDAO* mDAO = nullptr;

	// QGIS 接口（用于添加到地图）
	QgisInterface* mQGisIface = nullptr;

	// 专有元数据 Tab 内的 ScrollArea 及表单（不同产品类型重建）
	QScrollArea* mSpeScroll = nullptr;
	QWidget*     mSpePage = nullptr;
	QVBoxLayout* mSpeLayout = nullptr;

	// ProductStorageDialog 一致的类型树 item 指针，用于高亮
	QTreeWidgetItem* mCurrentTypeTreeItem = nullptr;

	// "按地图成果类型分类"树中固定目录节点的 ID 集合（制图成果/制图要素/制图资料及其二级节点）
	QSet<int> mFixedDirIds;

	// 需要在节点下展示产品数据子节点的固定叶子节点 ID 集合
	// （制图成果:AI/PDF/其它；制图要素:影像/晕渲；制图资料:文档资料/表格资料/其它）
	QSet<int> mProductLeafDirIds;
};
