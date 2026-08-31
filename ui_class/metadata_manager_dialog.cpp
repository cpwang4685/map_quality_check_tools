#include "metadata_manager_dialog.h"
#include "database/product_dao.h"
#include "database/postgis_connector.h"
#include "core/calendar_helper.h"
#include "core/directory_helper.h"
#include <QSettings>
#include <qgsapplication.h>
#include <qgsmessagelog.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QApplication>
#include <QTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QPushButton>
#include <QStyle>
#include <QListWidget>
#include <QMenu>
#include <QAction>
#include <QShowEvent>
#include "qgsgui.h"
#include "core/add_to_map_helper.h"

MetadataManagerDialog::MetadataManagerDialog(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
{
	QgsMessageLog::logMessage("MetadataManagerDialog constructor begin", "MapProduct", Qgis::Info);
	mDAO = new ProductDAO(this);
	QgsMessageLog::logMessage("  1. DAO created", "MapProduct", Qgis::Info);

	ui.setupUi(this);
	QgsMessageLog::logMessage("  2. setupUi done", "MapProduct", Qgis::Info);

	ui.editScroll->setFrameShape(QFrame::NoFrame);

	QgsGui::enableAutoGeometryRestore(this);
	QgsMessageLog::logMessage("  3. enableAutoGeometryRestore done", "MapProduct", Qgis::Info);
	setupTableColumns();
	QgsMessageLog::logMessage("  4. setupTableColumns done", "MapProduct", Qgis::Info);
	setupConnections();
	QgsMessageLog::logMessage("  5. setupConnections done", "MapProduct", Qgis::Info);
	loadProductTypeTree();
	QgsMessageLog::logMessage("  6. loadProductTypeTree done", "MapProduct", Qgis::Info);
	loadAllProducts();
	QgsMessageLog::logMessage("  7. loadAllProducts done", "MapProduct", Qgis::Info);
	loadResultDirTree();
	QgsMessageLog::logMessage("  7.1 loadResultDirTree done", "MapProduct", Qgis::Info);
	ensureDefaultTags();  // 确保默认标签存在后再加载
	QgsMessageLog::logMessage("  8. ensureDefaultTags done", "MapProduct", Qgis::Info);
	loadTags();
	QgsMessageLog::logMessage("  9. loadTags done", "MapProduct", Qgis::Info);
	loadTagTree();
	QgsMessageLog::logMessage("  10. loadTagTree done", "MapProduct", Qgis::Info);

	QgsMessageLog::logMessage("  11. 对话框构造完成", "MapProduct", Qgis::Info);

	setWindowTitle("元数据管理");
	QgsMessageLog::logMessage("MetadataManagerDialog constructor end", "MapProduct", Qgis::Info);
	resize(1100, 750);
}

MetadataManagerDialog::~MetadataManagerDialog()
{
}

void MetadataManagerDialog::setupTableColumns()
{
	ui.mProductTable->setColumnCount(6);
	ui.mProductTable->setHorizontalHeaderLabels({"ID", "产品名称", "类型", "密级", "版本", "更新时间"});
	ui.mProductTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	ui.mProductTable->horizontalHeader()->setStretchLastSection(true);
}

void MetadataManagerDialog::setupConnections()
{
	// 类型树点击 → 过滤产品列表
	connect(ui.mDirectoryTree, &QTreeWidget::itemClicked, this, &MetadataManagerDialog::onTypeTreeItemClicked);
	// 地图成果类型分类树点击 → 按目录过滤产品列表
	connect(ui.mResultDirTree, &QTreeWidget::itemClicked, this, &MetadataManagerDialog::onResultDirTreeItemClicked);
	// 地图成果类型分类树右键菜单（添加到地图）
	ui.mResultDirTree->setContextMenuPolicy(Qt::CustomContextMenu);
	ui.mResultDirTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
	connect(ui.mResultDirTree, &QWidget::customContextMenuRequested, this, &MetadataManagerDialog::onResultDirTreeContextMenu);
	connect(ui.mProductTable, &QTableWidget::itemClicked, this, &MetadataManagerDialog::onProductSelected);
	connect(ui.mSaveBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onSaveMetadata);
	connect(ui.mResetBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onResetForm);
	connect(ui.mAddToMapBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onAddToMap);
	connect(ui.mAddTagBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onAddTag);
	connect(ui.mRemoveTagBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onRemoveTag);
	connect(ui.mNewTagBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onNewTag);
	connect(ui.mDeleteTagBtn, &QPushButton::clicked, this, &MetadataManagerDialog::onDeleteTag);
	connect(ui.mTagList, &QListWidget::itemDoubleClicked, this, &MetadataManagerDialog::onTagDoubleClicked);
	connect(ui.mTagTreeWidget, &QTreeWidget::itemClicked, this, &MetadataManagerDialog::onTagTreeItemClicked);

	// 数据库 schema 切换后自动刷新目录和产品列表
	connect(PostgisConnector::instance(), &PostgisConnector::schemaChanged, this, [this](const QString&) {
		loadProductTypeTree();
		loadAllProducts();
		loadResultDirTree();
		loadTags();
		loadTagTree();
	});
}

//────────────────── 类型树 ──────────────────
// 多类型筛选标记（供 category 父节点使用，格式: "CAD;AI;CDR;PDF"）
static const QString kCategorySep = QStringLiteral(";");

void MetadataManagerDialog::loadProductTypeTree()
{
	ui.mDirectoryTree->clear();

	// 始终从数据库重新加载，确保 schema 切换后数据正确
	mAllProducts = mDAO->getAllProducts();

	QMap<QString, int> typeCounts;
	int totalCount = 0;
	for (const auto& meta : mAllProducts)
	{
		QString typeStr = productTypeToString(meta.productType);
		typeCounts[typeStr]++;
		totalCount++;
	}

	// 根节点: 全部成果 (总数)
	auto* rootItem = new QTreeWidgetItem(ui.mDirectoryTree);
	rootItem->setText(0, QString::fromUtf8("全部成果 (%1)").arg(totalCount));
	rootItem->setData(0, Qt::UserRole, "");             // 空字符串 = 全部
	rootItem->setExpanded(true);

	// ── 矢量数据 ──
	{
		int cnt = typeCounts.value(QString::fromUtf8("矢量数据"), 0);
		auto* item = new QTreeWidgetItem(rootItem);
		item->setText(0, QString::fromUtf8("矢量数据 (%1)").arg(cnt));
		item->setData(0, Qt::UserRole, QString::fromUtf8("矢量数据"));
	}

	// ── 栅格数据 ──
	{
		int cnt = typeCounts.value(QString::fromUtf8("栅格数据"), 0);
		auto* item = new QTreeWidgetItem(rootItem);
		item->setText(0, QString::fromUtf8("栅格数据 (%1)").arg(cnt));
		item->setData(0, Qt::UserRole, QString::fromUtf8("栅格数据"));
	}

	// ── 制图文件（含 CAD / AI / CDR / PDF） ──
	{
		QStringList drawingTypes = {
			QStringLiteral("CAD"),
			QStringLiteral("AI"),
			QStringLiteral("CDR"),
			QStringLiteral("PDF")
		};
		int drawingTotal = 0;
		for (const auto& t : drawingTypes)
			drawingTotal += typeCounts.value(t, 0);

		auto* drawingItem = new QTreeWidgetItem(rootItem);
		drawingItem->setText(0, QString::fromUtf8("制图文件 (%1)").arg(drawingTotal));
		drawingItem->setData(0, Qt::UserRole, drawingTypes.join(kCategorySep));  // "CAD;AI;CDR;PDF"
	}

	// ── 其它类型文件（文档+压缩包+其他统一归并）──
	{
		int cnt = typeCounts.value(QStringLiteral("文档文件"), 0)
		        + typeCounts.value(QStringLiteral("压缩包"), 0)
		        + typeCounts.value(QStringLiteral("Other"), 0);
		auto* item = new QTreeWidgetItem(rootItem);
		item->setText(0, QString::fromUtf8("其它类型文件 (%1)").arg(cnt));
		item->setData(0, Qt::UserRole, QStringLiteral("文档文件;压缩包;Other"));
	}
}

void MetadataManagerDialog::filterProductsByType(const QString& typeName)
{
	// typeName 为空 → 全部；含 ";" → 多类型（如制图文件父节点）
	QSet<QString> allowedTypes;
	if (!typeName.isEmpty())
	{
		if (typeName.contains(kCategorySep))
		{
			const QStringList parts = typeName.split(kCategorySep, QString::SkipEmptyParts);
			for (const auto& p : parts)
				allowedTypes.insert(p);
		}
		else
		{
			allowedTypes.insert(typeName);
		}
	}

	QList<ProductMetadata> filtered;
	for (const auto& meta : mAllProducts)
	{
		if (allowedTypes.isEmpty() || allowedTypes.contains(productTypeToString(meta.productType)))
			filtered.append(meta);
	}

	ui.mProductTable->setRowCount(0);
	for (int i = 0; i < filtered.size(); ++i)
	{
		const auto& meta = filtered[i];
		ui.mProductTable->insertRow(i);
		ui.mProductTable->setItem(i, 0, new QTableWidgetItem(QString::number(meta.id)));
		ui.mProductTable->setItem(i, 1, new QTableWidgetItem(meta.productName));
		ui.mProductTable->setItem(i, 2, new QTableWidgetItem(productTypeToDisplayName(meta.productType)));
		ui.mProductTable->setItem(i, 3, new QTableWidgetItem(securityLevelToString(meta.securityLevel)));
		ui.mProductTable->setItem(i, 4, new QTableWidgetItem(QString::number(meta.currentVersion)));
		ui.mProductTable->setItem(i, 5, new QTableWidgetItem(meta.updatedAt.toString("yyyy-MM-dd hh:mm")));
	}
}

void MetadataManagerDialog::onTypeTreeItemClicked(QTreeWidgetItem* item, int /*column*/)
{
	if (!item) return;

	QString typeName = item->data(0, Qt::UserRole).toString();
	if (typeName.isEmpty())
	{
		// 根节点 "全部成果"：显示全部
		mCurrentDirId = 0;
		mCurrentProductId = -1;
		mCurrentTypeFilter.clear();
		loadAllProducts();
	}
	else
	{
		// 类型节点：按类型筛选
		mCurrentTypeFilter = typeName;
		mCurrentProductId = -1;
		filterProductsByType(typeName);
	}
}

//────────────────── 地图成果类型分类树（固定目录树） ──────────────────

void MetadataManagerDialog::populateResultDirChildren(QTreeWidgetItem* parentItem, int parentDirId)
{
	QStyle* style = QApplication::style();
	const auto children = mDAO->getChildDirectories(parentDirId);
	for (const auto& child : children)
	{
		auto* ci = new QTreeWidgetItem(parentItem);
		ci->setText(0, child.name); // 纯结构展示，不显示统计数目
		ci->setData(0, Qt::UserRole, child.id);
		ci->setData(0, Qt::UserRole + 1, 0); // 0=目录节点
		// 固定分类节点（制图成果/制图要素/制图资料及其二级节点）用文件夹图标
		if (mFixedDirIds.contains(child.id))
		{
			ci->setIcon(0, style->standardIcon(QStyle::SP_DirIcon));
		}
		// 其余子节点（图层/普通目录）统一用文件图标
		else
		{
			ci->setIcon(0, style->standardIcon(QStyle::SP_FileIcon));
		}

		// 固定叶子节点（AI/PDF/其它、影像/晕渲、文档资料/表格资料/其它）下展示产品数据子节点
		if (mProductLeafDirIds.contains(child.id))
		{
			const auto products = mDAO->getProductsByDirectory(child.id);
			for (const auto& p : products)
			{
				auto* pi = new QTreeWidgetItem(ci);
				pi->setText(0, p.productName);
				pi->setData(0, Qt::UserRole, p.id);
				pi->setData(0, Qt::UserRole + 1, 1); // 1=产品数据节点
				pi->setIcon(0, style->standardIcon(QStyle::SP_FileIcon));
				pi->setToolTip(0, QStringLiteral("数据名称: %1\n密级: %2").arg(p.productName, securityLevelToString(p.securityLevel)));
			}
		}

		populateResultDirChildren(ci, child.id);
	}
}

void MetadataManagerDialog::loadResultDirTree()
{
	ui.mResultDirTree->clear();

	// 根节点 = 数据库名称（dirId=0 表示全部）
	QSettings settings;
	QString dbName = settings.value(QStringLiteral("db/database"), QStringLiteral("数据库")).toString();
	if (dbName.isEmpty())
		dbName = QStringLiteral("数据库");

	auto* root = new QTreeWidgetItem(ui.mResultDirTree);
	root->setText(0, dbName);
	root->setData(0, Qt::UserRole, 0);
	root->setIcon(0, QApplication::style()->standardIcon(QStyle::SP_DirHomeIcon));
	root->setExpanded(true);

	// 幂等创建固定目录节点（制图成果/制图要素/制图资料及其子节点）
	auto fixedIds = DirectoryHelper::ensureFixedDirectories(*mDAO);
	mFixedDirIds.clear();
	mFixedDirIds << fixedIds.drawingResults << fixedIds.resultAI << fixedIds.resultPDF << fixedIds.resultOther
	             << fixedIds.drawingElements << fixedIds.elementImage << fixedIds.elementShading << fixedIds.elementFeature
	             << fixedIds.drawingMaterials << fixedIds.materialDoc << fixedIds.materialTable << fixedIds.materialOther;

	// 需要在节点下展示产品数据子节点的固定叶子节点（AI/PDF/其它、影像/晕渲、文档资料/表格资料/其它）
	mProductLeafDirIds.clear();
	mProductLeafDirIds << fixedIds.resultAI << fixedIds.resultPDF << fixedIds.resultOther
	                   << fixedIds.elementImage << fixedIds.elementShading
	                   << fixedIds.materialDoc << fixedIds.materialTable << fixedIds.materialOther;

	// 制图成果 / 制图要素 / 制图资料（从数据库实际目录加载）
	populateResultDirChildren(root, 0);

	ui.mResultDirTree->expandAll();
}

void MetadataManagerDialog::showEvent(QShowEvent* event)
{
	QDialog::showEvent(event);
	// 每次显示时重新加载，确保"按地图成果类型分类"树和产品列表反映最新入库数据
	loadAllProducts();
	loadResultDirTree();
}

void MetadataManagerDialog::onResultDirTreeItemClicked(QTreeWidgetItem* item, int column)
{
	Q_UNUSED(column);
	if (!item) return;

	// 产品数据节点：定位到对应产品并填充详情
	if (item->data(0, Qt::UserRole + 1).toInt() == 1)
	{
		int productId = item->data(0, Qt::UserRole).toInt();
		auto meta = mDAO->getProduct(productId);
		if (meta.id <= 0) return;

		mCurrentTypeFilter.clear();
		mCurrentDirId = meta.parentDirId;
		mCurrentProductId = -1;
		loadProductListByDir(mCurrentDirId);

		// 在表格中定位并选中该产品，填充详情
		for (int r = 0; r < ui.mProductTable->rowCount(); ++r)
		{
			if (ui.mProductTable->item(r, 0)->text().toInt() == productId)
			{
				ui.mProductTable->selectRow(r);
				break;
			}
		}
		fillMetadataForm(meta);
		return;
	}

	mCurrentTypeFilter.clear();
	mCurrentDirId = item->data(0, Qt::UserRole).toInt();
	mCurrentProductId = -1;
	loadProductListByDir(mCurrentDirId);
}

void MetadataManagerDialog::onResultDirTreeContextMenu(const QPoint& pos)
{
	auto* item = ui.mResultDirTree->itemAt(pos);
	if (!item)
		return;

	QMenu menu(this);
	QAction* actAddToMap = menu.addAction(QStringLiteral("添加到地图"));
	QAction* chosen = menu.exec(ui.mResultDirTree->viewport()->mapToGlobal(pos));
	if (chosen == actAddToMap)
		onAddResultDirSelectionToMap();
}

void MetadataManagerDialog::onAddResultDirSelectionToMap()
{
	if (!mQGisIface)
	{
		QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("未获取到 QGIS 接口，无法添加到地图"));
		return;
	}

	QList<QTreeWidgetItem*> items = ui.mResultDirTree->selectedItems();
	if (items.isEmpty())
	{
		QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选中需要添加到地图的节点"));
		return;
	}

	// 汇总所有需要添加到地图的产品（去重）
	QSet<int> productIds;
	QList<ProductMetadata> metas;
	for (auto* item : items)
	{
		if (!item) continue;

		// 产品数据节点：直接添加该产品
		if (item->data(0, Qt::UserRole + 1).toInt() == 1)
		{
			int pid = item->data(0, Qt::UserRole).toInt();
			if (productIds.contains(pid)) continue;
			auto meta = mDAO->getProduct(pid);
			if (meta.id <= 0) continue;
			productIds.insert(pid);
			metas.append(meta);
			continue;
		}

		// 目录节点：递归收集该目录下所有子孙产品
		int dirId = item->data(0, Qt::UserRole).toInt();
		if (dirId <= 0) continue; // 根节点（全部）不处理
		QSet<int> dirIds;
		DirectoryHelper::collectDirectoryIds(*mDAO, dirId, dirIds);
		for (const auto& meta : mAllProducts)
		{
			if (dirIds.contains(meta.parentDirId) && !productIds.contains(meta.id))
			{
				productIds.insert(meta.id);
				metas.append(meta);
			}
		}
	}

	if (metas.isEmpty())
	{
		QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("所选节点下没有可添加到地图的数据"));
		return;
	}

	int okCount = 0;
	for (const auto& meta : metas)
	{
		if (AddToMapHelper::addToMapWithFeedback(this, mQGisIface, meta))
			++okCount;
	}

	if (okCount < metas.size())
	{
		QMessageBox::information(this, QStringLiteral("提示"),
			QStringLiteral("已将 %1/%2 个数据添加到地图，其余数据不支持添加到地图或加载失败。")
				.arg(okCount).arg(metas.size()));
	}
}

void MetadataManagerDialog::loadProductListByDir(int dirId)
{
	// dirId <= 0 表示全部；否则递归包含所有子目录的产品
	QSet<int> dirIds;
	if (dirId > 0)
		DirectoryHelper::collectDirectoryIds(*mDAO, dirId, dirIds);

	QList<ProductMetadata> filtered;
	for (const auto& meta : mAllProducts)
	{
		if (dirId <= 0 || dirIds.contains(meta.parentDirId))
			filtered.append(meta);
	}
	loadProductsToTable(filtered);
}

void MetadataManagerDialog::onProductSelected()
{
	int row = ui.mProductTable->currentRow();
	if (row < 0) return;

	int productId = ui.mProductTable->item(row, 0)->text().toInt();

	auto meta = mDAO->getProduct(productId);
	if (meta.id <= 0) return;

	// fillMetadataForm 内部设置 mCurrentProductId
	fillMetadataForm(meta);
}

void MetadataManagerDialog::onSaveMetadata()
{
	if (mCurrentProductId < 0)
	{
		QMessageBox::warning(this, "提示", "请先选择一个产品");
		return;
	}

	auto meta = mDAO->getProduct(mCurrentProductId);
	collectMetadataForm(meta);
	meta.updatedBy = "current_user";

	if (mDAO->updateProduct(meta))
	{
		// 同时保存专有元数据
		switch (meta.productType)
		{
		case ProductType::Vector: {
			ProductVectorMeta vMeta = mDAO->getVectorMeta(mCurrentProductId);
			collectSpeFromForm(vMeta);
			mDAO->saveVectorMeta(vMeta);
			break;
		}
		case ProductType::Raster: {
			ProductRasterMeta rMeta = mDAO->getRasterMeta(mCurrentProductId);
			collectSpeFromForm(rMeta);
			mDAO->saveRasterMeta(rMeta);
			break;
		}
		case ProductType::CAD:
		case ProductType::AI:
		case ProductType::CDR:
		case ProductType::PDF: {
			ProductDiagramMeta dMeta = mDAO->getDiagramMeta(mCurrentProductId);
			collectSpeFromForm(dMeta);
			mDAO->saveDiagramMeta(dMeta);
			break;
		}
		default: {
			ProductDocumentMeta docMeta = mDAO->getDocumentMeta(mCurrentProductId);
			if (docMeta.productId > 0)
			{
				collectSpeFromForm(docMeta);
				mDAO->saveDocumentMeta(docMeta);
			}
			break;
		}
		}

		QMessageBox::information(this, "成功", "元数据保存成功");

		// 刷新表格和类型树
		mAllProducts = mDAO->getAllProducts();
		loadProductTypeTree();
		filterProductsByType(mCurrentTypeFilter);
		loadTagTree();
	}
	else
	{
		QMessageBox::critical(this, "错误", "元数据保存失败");
	}
}

void MetadataManagerDialog::onResetForm()
{
	if (mCurrentProductId < 0) return;
	auto meta = mDAO->getProduct(mCurrentProductId);
	fillMetadataForm(meta);
}

void MetadataManagerDialog::onAddToMap()
{
	if (!mQGisIface)
	{
		QMessageBox::warning(this, "提示", "未获取到 QGIS 接口，无法添加到地图");
		return;
	}
	if (mCurrentProductId < 0)
	{
		QMessageBox::warning(this, "提示", "请先在成果列表中选中一个产品");
		return;
	}

	auto meta = mDAO->getProduct(mCurrentProductId);
	if (meta.id <= 0)
	{
		QMessageBox::warning(this, "提示", "未找到对应的成果信息");
		return;
	}

	AddToMapHelper::addToMapWithFeedback(this, mQGisIface, meta);
}

//────────────────── 标签相关 (不变) ──────────────────

void MetadataManagerDialog::onAddTag()
{
	auto* item = ui.mTagList->currentItem();
	if (!item) return;

	QString tagName = item->text();
	for (int i = 0; i < ui.mProductTagList->count(); ++i)
	{
		if (ui.mProductTagList->item(i)->text() == tagName)
			return;
	}
	ui.mProductTagList->addItem(tagName);
}

void MetadataManagerDialog::onRemoveTag()
{
	auto* item = ui.mProductTagList->currentItem();
	if (!item) return;
	delete ui.mProductTagList->takeItem(ui.mProductTagList->row(item));
}

void MetadataManagerDialog::onTagDoubleClicked(QListWidgetItem* /*item*/)
{
	onAddTag();
}

void MetadataManagerDialog::onNewTag()
{
	QInputDialog dlg(this);
	dlg.setWindowTitle("新建标签");
	dlg.setLabelText("标签名称:");
	dlg.setInputMode(QInputDialog::TextInput);
	dlg.setOkButtonText("确定");
	dlg.setCancelButtonText("取消");
	if (dlg.exec() != QDialog::Accepted) return;

	QString name = dlg.textValue();
	if (name.trimmed().isEmpty()) return;

	QString trimmed = name.trimmed();

	for (int i = 0; i < ui.mTagList->count(); ++i)
	{
		if (ui.mTagList->item(i)->text() == trimmed)
		{
			QMessageBox::warning(this, "提示", "标签已存在");
			return;
		}
	}

	if (mDAO->addTag(trimmed))
	{
		loadTags();
		loadTagTree();
	}
	else
	{
		QMessageBox::critical(this, "错误", "标签创建失败");
	}
}

void MetadataManagerDialog::onDeleteTag()
{
	auto* item = ui.mTagList->currentItem();
	if (!item)
	{
		QMessageBox::warning(this, "提示", "请先选择一个标签");
		return;
	}

	int tagId = item->data(Qt::UserRole).toInt();
	if (tagId <= 0) return;

	auto reply = QMessageBox::question(this, "确认删除",
		QString("确定要删除标签 \"%1\" 吗？\n注意：所有产品上的此标签也将被移除。").arg(item->text()),
		QMessageBox::Yes | QMessageBox::No);

	if (reply == QMessageBox::Yes)
	{
		if (mDAO->removeTag(tagId))
		{
			loadTags();
			loadTagTree();
			if (mCurrentProductId > 0)
			{
				auto meta = mDAO->getProduct(mCurrentProductId);
				fillMetadataForm(meta);
			}
		}
		else
		{
			QMessageBox::critical(this, "错误", "标签删除失败");
		}
	}
}

//────────────────── 产品表和标签树 (不变) ──────────────────

void MetadataManagerDialog::loadAllProducts()
{
	mAllProducts = mDAO->getAllProducts();
	loadProductsToTable(mAllProducts);
}

void MetadataManagerDialog::loadProductsToTable(const QList<ProductMetadata>& products)
{
	ui.mProductTable->setRowCount(0);
	for (int i = 0; i < products.size(); ++i)
	{
		const auto& meta = products[i];
		ui.mProductTable->insertRow(i);
		ui.mProductTable->setItem(i, 0, new QTableWidgetItem(QString::number(meta.id)));
		ui.mProductTable->setItem(i, 1, new QTableWidgetItem(meta.productName));
		ui.mProductTable->setItem(i, 2, new QTableWidgetItem(productTypeToDisplayName(meta.productType)));
		ui.mProductTable->setItem(i, 3, new QTableWidgetItem(securityLevelToString(meta.securityLevel)));
		ui.mProductTable->setItem(i, 4, new QTableWidgetItem(QString::number(meta.currentVersion)));
		ui.mProductTable->setItem(i, 5, new QTableWidgetItem(meta.updatedAt.toString("yyyy-MM-dd hh:mm")));
	}
}

void MetadataManagerDialog::ensureDefaultTags()
{
	struct { const char* name; const char* color; const char* desc; } defaults[] = {
		{"基础地理", "#4CAF50", "基础地理信息数据"},
		{"专题图",   "#2196F3", "专题地图成果"},
		{"影像图",   "#FF9800", "影像地图成果"},
		{"地形图",   "#9C27B0", "地形图成果"},
		{"电子地图", "#00BCD4", "电子地图瓦片数据"},
		{"行政区划", "#E91E63", "行政区划数据"},
		{"交通路网", "#FF5722", "交通道路网络数据"},
		{"水系",     "#03A9F4", "水系/水文数据"},
		{"POI",      "#795548", "兴趣点数据"},
		{"建筑",     "#607D8B", "建筑物数据"},
		{"DEM",      "#8BC34A", "数字高程模型"},
		{"遥感影像", "#CDDC39", "遥感影像数据"},
		{"三维模型", "#009688", "三维模型数据"},
		{"矢量瓦片", "#673AB7", "矢量瓦片数据"},
		{"审核通过", "#4CAF50", "已通过审核"},
		{"待审核",   "#FFC107", "待审核"},
		{"退回修改", "#F44336", "需修改后重新提交"},
		{"归档",     "#607D8B", "已归档"},
		{"历史版本", "#9E9E9E", "历史版本数据"},
	};

	for (const auto& t : defaults)
	{
		mDAO->addTag(QString::fromUtf8(t.name),
					 QString::fromUtf8(t.color),
					 QString::fromUtf8(t.desc));
	}
}

void MetadataManagerDialog::loadTagTree()
{
	ui.mTagTreeWidget->clear();

	auto allProducts = mDAO->getAllProducts();

	QMap<QString, QList<ProductMetadata>> tagProductMap;
	QSet<int> taggedProductIds;

	for (const auto& p : allProducts)
	{
		auto tags = mDAO->getProductTags(p.id);
		if (tags.isEmpty()) continue;

		for (const auto& tag : tags)
		{
			tagProductMap[tag].append(p);
			taggedProductIds.insert(p.id);
		}
	}

	QStringList sortedTags = tagProductMap.keys();
	sortedTags.sort(Qt::CaseInsensitive);

	auto* rootItem = new QTreeWidgetItem(ui.mTagTreeWidget);
	rootItem->setText(0, "标签分类");
	rootItem->setData(0, Qt::UserRole, 0);
	rootItem->setData(0, Qt::UserRole + 1, 0);
	rootItem->setExpanded(true);

	for (const auto& tagName : sortedTags)
	{
		const auto& products = tagProductMap[tagName];
		auto* tagItem = new QTreeWidgetItem(rootItem);
		tagItem->setText(0, QString("%1 (%2)").arg(tagName).arg(products.size()));
		tagItem->setData(0, Qt::UserRole, 0);
		tagItem->setData(0, Qt::UserRole + 1, 0);
		tagItem->setData(0, Qt::UserRole + 2, tagName);
		tagItem->setExpanded(true);
		tagItem->setIcon(0, QApplication::style()->standardIcon(QStyle::SP_DirIcon));

		for (const auto& p : products)
		{
			auto* prodItem = new QTreeWidgetItem(tagItem);
			prodItem->setText(0, p.productName);
			prodItem->setData(0, Qt::UserRole, p.id);
			prodItem->setData(0, Qt::UserRole + 1, 1);
			prodItem->setToolTip(0, QString("类型: %1 | 密级: %2")
				.arg(productTypeToString(p.productType))
				.arg(securityLevelToString(p.securityLevel)));
		}
	}

	QList<ProductMetadata> untagged;
	for (const auto& p : allProducts)
	{
		if (!taggedProductIds.contains(p.id))
			untagged.append(p);
	}

	if (!untagged.isEmpty())
	{
		auto* untaggedItem = new QTreeWidgetItem(rootItem);
		untaggedItem->setText(0, QString("未打标签 (%1)").arg(untagged.size()));
		untaggedItem->setData(0, Qt::UserRole, 0);
		untaggedItem->setData(0, Qt::UserRole + 1, 2);
		untaggedItem->setExpanded(true);
		untaggedItem->setIcon(0, QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));

		for (const auto& p : untagged)
		{
			auto* prodItem = new QTreeWidgetItem(untaggedItem);
			prodItem->setText(0, p.productName);
			prodItem->setData(0, Qt::UserRole, p.id);
			prodItem->setData(0, Qt::UserRole + 1, 1);
			prodItem->setToolTip(0, QString("类型: %1 | 密级: %2")
				.arg(productTypeToString(p.productType))
				.arg(securityLevelToString(p.securityLevel)));
		}
	}
	else
	{
		auto* untaggedItem = new QTreeWidgetItem(rootItem);
		untaggedItem->setText(0, "未打标签 (0)");
		untaggedItem->setData(0, Qt::UserRole, 0);
		untaggedItem->setData(0, Qt::UserRole + 1, 2);
		untaggedItem->setIcon(0, QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
	}
}

void MetadataManagerDialog::onTagTreeItemClicked(QTreeWidgetItem* item, int /*column*/)
{
	if (!item) return;

	int nodeType = item->data(0, Qt::UserRole + 1).toInt();

	if (nodeType == 1)
	{
		int productId = item->data(0, Qt::UserRole).toInt();
		if (productId <= 0) return;

		ui.mProductTable->setRowCount(0);
		auto meta = mDAO->getProduct(productId);
		if (meta.id > 0)
		{
			ui.mProductTable->insertRow(0);
			ui.mProductTable->setItem(0, 0, new QTableWidgetItem(QString::number(meta.id)));
			ui.mProductTable->setItem(0, 1, new QTableWidgetItem(meta.productName));
			ui.mProductTable->setItem(0, 2, new QTableWidgetItem(productTypeToDisplayName(meta.productType)));
			ui.mProductTable->setItem(0, 3, new QTableWidgetItem(securityLevelToString(meta.securityLevel)));
			ui.mProductTable->setItem(0, 4, new QTableWidgetItem(QString::number(meta.currentVersion)));
			ui.mProductTable->setItem(0, 5, new QTableWidgetItem(meta.updatedAt.toString("yyyy-MM-dd hh:mm")));
			ui.mProductTable->selectRow(0);
		}

		fillMetadataForm(meta);
	}
	else if (nodeType == 2)
	{
		auto allProducts = mDAO->getAllProducts();
		QSet<int> taggedIds;
		for (const auto& p : allProducts)
		{
			auto tags = mDAO->getProductTags(p.id);
			if (!tags.isEmpty())
				taggedIds.insert(p.id);
		}

		QList<ProductMetadata> untagged;
		for (const auto& p : allProducts)
		{
			if (!taggedIds.contains(p.id))
				untagged.append(p);
		}

		loadProductsToTable(untagged);
	}
	else
	{
		QString tagName = item->data(0, Qt::UserRole + 2).toString();

		if (tagName.isEmpty())
		{
			loadAllProducts();
		}
		else
		{
			ProductDAO::SearchCondition cond;
			cond.productType = ProductType::Other;
			cond.tags = QStringList() << tagName;
			cond.limit = 500;
			auto products = mDAO->searchProducts(cond);
			loadProductsToTable(products);
		}
	}
}

void MetadataManagerDialog::loadTags()
{
	ui.mTagList->clear();
	auto tags = mDAO->getAllTags();
	for (const auto& tag : tags)
	{
		auto map = tag.toMap();
		auto* item = new QListWidgetItem(map.value("name").toString());
		item->setData(Qt::UserRole, map.value("id").toInt());
		item->setToolTip(map.value("description").toString());
		ui.mTagList->addItem(item);
	}
}

//────────────────── 元数据表单 (fill + collect + build) ──────────────────

void MetadataManagerDialog::fillMetadataForm(const ProductMetadata& meta)
{
	mCurrentProductId = meta.id;

	// --- 编制信息 TabWidget (动态构建) ---
	buildCompileForm(meta);

	// --- 空间信息（只读） ---
	ui.mSpatialInfoLabel->setText(QString("CRS: %1\n范围: [%2, %3] - [%4, %5]\n几何类型: %6")
		.arg(meta.crs)
		.arg(meta.minX, 0, 'f', 4).arg(meta.minY, 0, 'f', 4)
		.arg(meta.maxX, 0, 'f', 4).arg(meta.maxY, 0, 'f', 4)
		.arg(meta.geometryType));

	ui.mFileInfoLabel->setText(QString("格式: %1\n大小: %2 KB\n波段: %3\n分辨率: %4")
		.arg(meta.fileFormat)
		.arg(meta.fileSize / 1024)
		.arg(meta.bandCount)
		.arg(meta.pixelResolution, 0, 'f', 6));

	// --- 产品标签 ---
	ui.mProductTagList->clear();
	ui.mProductTagList->addItems(meta.tags.split(QLatin1Char(';'), QString::SkipEmptyParts));
}

void MetadataManagerDialog::collectMetadataForm(ProductMetadata& meta)
{
	// 收集 27 项基本元数据（从 Tab 控件中读取）
	collectCompileFields(meta);

	// 产品标签
	QStringList tagNames;
	for (int i = 0; i < ui.mProductTagList->count(); ++i)
		tagNames << ui.mProductTagList->item(i)->text();
	meta.tags = tagNames.join(";");
}

//────────────────── 动态构建编制信息 TabWidget ──────────────────
//
// 结构：compileGroup
//        └── QVBoxLayout
//             └── mCompileTabWidget (在 .ui 中已定义)
//                  ├── mBasicMetaTab "基本元数据" — 动态 QFormLayout
//                  └── mSpecializedMetaTab "专有元数据" — 动态 QScrollArea + QFormLayout

// 辅助：创建一个带 label 的行
// 不设置局部样式，控件默认继承 qApp->styleSheet() 的统一风格
static QWidget* makeFieldRow(const QString& labelText, QWidget* editor)
{
	auto* row = new QHBoxLayout();
	row->setContentsMargins(16, 2, 12, 2);

	auto* lbl = new QLabel(labelText);
	lbl->setMinimumWidth(110);
	lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	row->addWidget(lbl);

	editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	row->addWidget(editor, 1);

	auto* wrapper = new QWidget();
	wrapper->setLayout(row);
	return wrapper;
}

static QLineEdit* makeLineEdit(const QString& objName, const QString& placeholder = "")
{
	auto* e = new QLineEdit();
	e->setObjectName(objName);
	if (!placeholder.isEmpty())
		e->setPlaceholderText(placeholder);
	return e;
}

static QTextEdit* makeTextEdit(const QString& objName, const QString& placeholder = "", int maxH = 60)
{
	auto* e = new QTextEdit();
	e->setObjectName(objName);
	e->setMaximumHeight(maxH);
	if (!placeholder.isEmpty())
		e->setPlaceholderText(placeholder);
	return e;
}

static QDateEdit* makeDateEdit(const QString& objName)
{
	auto* e = new QDateEdit();
	e->setObjectName(objName);
	e->setCalendarPopup(true);
	e->setDisplayFormat("yyyy-MM-dd");
	return e;
}

static QComboBox* makeCombo(const QString& objName, const QStringList& items, const QString& current = "")
{
	auto* e = new QComboBox();
	e->setObjectName(objName);
	e->addItems(items);
	if (!current.isEmpty())
		e->setCurrentText(current);
	return e;
}

//────────── 27 项基本元数据（与 ProductStorageDialog 保持一致）──────────



void MetadataManagerDialog::buildCompileForm(const ProductMetadata& meta)
{
	QTabWidget* tw = ui.mCompileTabWidget;
	tw->setCurrentIndex(0);

	// ─── Tab 0: 基本元数据（27 项） ───
	QWidget* basicTab = tw->widget(0);   // mBasicMetaTab
	// 删除旧 layout 内容
	if (basicTab->layout())
	{
		QLayout* old = basicTab->layout();
		QLayoutItem* child;
		while ((child = old->takeAt(0)) != nullptr)
		{
			if (child->widget()) child->widget()->deleteLater();
			delete child;
		}
		delete old;
	}

	// 基本元数据 Tab 使用 QScrollArea -->
	QScrollArea* scroll = new QScrollArea(basicTab);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setObjectName("meta27ScrollArea");
	// 全局 QSS 的 `QWidget { color: ... }` 规则会让普通 QWidget 容器被填充为
	// 不透明背景（系统 palette 白色），盖住 QTabWidget pane 深色背景，
	// 导致字段标签白底浅字。显式让 scroll/viewport/page 透明，透出 pane 背景。

	QWidget* page = new QWidget();
	scroll->setWidget(page);

	QVBoxLayout* formLayout = new QVBoxLayout(page);
	formLayout->setContentsMargins(12, 8, 12, 8);
	formLayout->setSpacing(2);

	// ── 辅助函数 ──
	auto addSectionTitle = [&](const QString& title) {
		auto* lbl = new QLabel(title);
		formLayout->addWidget(lbl);
	};

	auto addROField = [&](const QString& label, const QString& objName, const QString& val) {
		auto* row = new QHBoxLayout();
		auto* lbl = new QLabel(label);
		lbl->setMinimumWidth(110);
		lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		auto* ed = new QLineEdit(val);
		ed->setObjectName(objName);
		ed->setReadOnly(true);
		row->addWidget(lbl);
		row->addWidget(ed, 1);
		formLayout->addLayout(row);
	};

	auto addEditRow = [&](const QString& label, QWidget* editor) {
		auto* row = new QHBoxLayout();
		auto* lbl = new QLabel(label);
		lbl->setMinimumWidth(110);
		lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		row->addWidget(lbl);
		row->addWidget(editor, 1);
		formLayout->addLayout(row);
	};

	// ── 创建控件并填充值 ──
	// == 基本信息（6 项）==
	addSectionTitle(QString::fromUtf8("【基本信息】"));

	QLineEdit* edDataId = new QLineEdit(meta.dataId);
	edDataId->setObjectName("meta_dataId");
	edDataId->setReadOnly(true);
	edDataId->setToolTip(QString::fromUtf8("系统生成，不可修改"));
	addEditRow(QString::fromUtf8("数据唯一标识:"), edDataId);

	QLineEdit* edProductName = new QLineEdit(meta.productName);
	edProductName->setObjectName("meta_productName");
	edProductName->setPlaceholderText(QString::fromUtf8("请输入数据名称"));
	addEditRow(QString::fromUtf8("数据名称:"), edProductName);

	QTextEdit* edDescription = new QTextEdit(meta.description);
	edDescription->setObjectName("meta_description");
	edDescription->setMaximumHeight(60);
	edDescription->setPlaceholderText(QString::fromUtf8("请输入数据描述"));
	addEditRow(QString::fromUtf8("数据描述:"), edDescription);

	QLineEdit* edSource = new QLineEdit(meta.source);
	edSource->setObjectName("meta_source");
	edSource->setPlaceholderText(QString::fromUtf8("数据来源"));
	addEditRow(QString::fromUtf8("数据来源:"), edSource);

	QLineEdit* edVersionNote = new QLineEdit(meta.versionNote);
	edVersionNote->setObjectName("meta_versionNote");
	edVersionNote->setPlaceholderText(QString::fromUtf8("版本说明"));
	addEditRow(QString::fromUtf8("版本说明:"), edVersionNote);

	QLineEdit* edTags = new QLineEdit(meta.tags);
	edTags->setObjectName("meta_tags");
	edTags->setPlaceholderText(QString::fromUtf8("标签，多个用分号分隔"));
	addEditRow(QString::fromUtf8("标签:"), edTags);

	// == 格式与存储（3 项）==
	addSectionTitle(QString::fromUtf8("【格式与存储】"));
	addROField(QString::fromUtf8("数据格式:"), "meta_fileFormat", meta.fileFormat);
	addROField(QString::fromUtf8("数据量(Bytes):"), "meta_fileSize",
		QString::number(meta.fileSize));

	QComboBox* cmbCompressed = new QComboBox();
	cmbCompressed->setObjectName("meta_isCompressed");
	cmbCompressed->addItems({ QString::fromUtf8("否"), QString::fromUtf8("是") });
	cmbCompressed->setCurrentIndex(meta.isCompressed == QString::fromUtf8("是") ? 1 : 0);
	addEditRow(QString::fromUtf8("是否压缩:"), cmbCompressed);

	// == 空间信息（5 项）==
	addSectionTitle(QString::fromUtf8("【空间信息】"));
	addROField(QString::fromUtf8("空间范围:"), "meta_bounds", meta.bounds);
	addROField(QString::fromUtf8("中心点经度:"), "meta_centerLon",
		QString::number(meta.centerLon.toDouble(), 'f', 6));
	addROField(QString::fromUtf8("中心点纬度:"), "meta_centerLat",
		QString::number(meta.centerLat.toDouble(), 'f', 6));
	addROField(QString::fromUtf8("坐标参考系:"), "meta_crs", meta.crs);

	QLineEdit* edScale = new QLineEdit(meta.scale);
	edScale->setObjectName("meta_scale");
	edScale->setPlaceholderText(QString::fromUtf8("如 1:10000"));
	addEditRow(QString::fromUtf8("比例尺分母:"), edScale);

	// == 时间与周期（4 项）==
	addSectionTitle(QString::fromUtf8("【时间与周期】"));

	QDateEdit* deStart = new QDateEdit();
	deStart->setObjectName("meta_startDatetime");
	deStart->setCalendarPopup(true);
	deStart->setDisplayFormat("yyyy-MM-dd");
	deStart->setDate(meta.startDatetime.isValid() ? meta.startDatetime.date() : QDate::currentDate());
	addEditRow(QString::fromUtf8("数据现势性起始:"), deStart);

	QDateEdit* deEnd = new QDateEdit();
	deEnd->setObjectName("meta_endDatetime");
	deEnd->setCalendarPopup(true);
	deEnd->setDisplayFormat("yyyy-MM-dd");
	deEnd->setDate(meta.endDatetime.isValid() ? meta.endDatetime.date() : QDate::currentDate());
	addEditRow(QString::fromUtf8("数据现势性截止:"), deEnd);

	addROField(QString::fromUtf8("创建时间:"), "meta_createdAt",
		meta.createdAt.isValid() ? meta.createdAt.toString("yyyy-MM-dd hh:mm:ss") : "");
	addROField(QString::fromUtf8("更新时间:"), "meta_updatedAt",
		meta.updatedAt.isValid() ? meta.updatedAt.toString("yyyy-MM-dd hh:mm:ss") : "");

	// == 管理与来源（9 项）==
	addSectionTitle(QString::fromUtf8("【管理与来源】"));

	QComboBox* cmbSec = new QComboBox();
	cmbSec->setObjectName("meta_securityLevel");
	cmbSec->addItems({
		QString::fromUtf8("非密"),
		QString::fromUtf8("内部"),
		QString::fromUtf8("机密"),
		QString::fromUtf8("秘密"),
		QString::fromUtf8("绝密")
	});
	QString secStr = securityLevelToString(meta.securityLevel);
	int secIdx = cmbSec->findText(secStr);
	cmbSec->setCurrentIndex(secIdx >= 0 ? secIdx : 0);
	addEditRow(QString::fromUtf8("密级:"), cmbSec);

	addROField(QString::fromUtf8("创建人员:"), "meta_createdBy", meta.createdBy);

	QLineEdit* edCity = new QLineEdit(meta.city);
	edCity->setObjectName("meta_city");
	edCity->setPlaceholderText(QString::fromUtf8("所在市州"));
	addEditRow(QString::fromUtf8("所在市州:"), edCity);

	QLineEdit* edProjectName = new QLineEdit(meta.projectName);
	edProjectName->setObjectName("meta_projectName");
	edProjectName->setPlaceholderText(QString::fromUtf8("项目名称"));
	addEditRow(QString::fromUtf8("项目名称:"), edProjectName);

	QLineEdit* edProducer = new QLineEdit(meta.producer);
	edProducer->setObjectName("meta_producer");
	edProducer->setPlaceholderText(QString::fromUtf8("生产单位"));
	addEditRow(QString::fromUtf8("生产单位:"), edProducer);

	QDateEdit* deProdDate = new QDateEdit();
	deProdDate->setObjectName("meta_productionDate");
	deProdDate->setCalendarPopup(true);
	deProdDate->setDisplayFormat("yyyy-MM-dd");
	deProdDate->setDate(meta.productionDate.isValid() ? meta.productionDate : QDate::currentDate());
	addEditRow(QString::fromUtf8("生产日期:"), deProdDate);

	QTextEdit* edCompilation = new QTextEdit(meta.compilationInfo);
	edCompilation->setObjectName("meta_compilationInfo");
	edCompilation->setMaximumHeight(60);
	edCompilation->setPlaceholderText(QString::fromUtf8("编制单位、编制人员、编制依据等信息..."));
	addEditRow(QString::fromUtf8("编制信息:"), edCompilation);

	QLineEdit* edDeliveryStatus = new QLineEdit(meta.deliveryStatus);
	edDeliveryStatus->setObjectName("meta_deliveryStatus");
	edDeliveryStatus->setPlaceholderText(QString::fromUtf8("汇交情况"));
	addEditRow(QString::fromUtf8("汇交情况:"), edDeliveryStatus);

	QDateEdit* deDeliveryTime = new QDateEdit();
	deDeliveryTime->setObjectName("meta_deliveryTime");
	deDeliveryTime->setCalendarPopup(true);
	deDeliveryTime->setDisplayFormat("yyyy-MM-dd");
	deDeliveryTime->setDate(meta.deliveryTime.isValid() ? meta.deliveryTime.date() : QDate::currentDate());
	addEditRow(QString::fromUtf8("汇交时间:"), deDeliveryTime);

	formLayout->addStretch();

	// 将 ScrollArea 放入 basicTab
	QVBoxLayout* tabLayout = new QVBoxLayout(basicTab);
	tabLayout->setContentsMargins(0, 0, 0, 0);
	tabLayout->addWidget(scroll);

	// ─── Tab 1: 专有元数据 ───
	QWidget* speTab = tw->widget(1);     // mSpecializedMetaTab
	// 删除旧内容
	if (speTab->layout())
	{
		QLayout* old = speTab->layout();
		QLayoutItem* child;
		while ((child = old->takeAt(0)) != nullptr)
		{
			if (child->widget()) child->widget()->deleteLater();
			delete child;
		}
		delete old;
	}

	// 专有元数据 Tab：ScrollArea
	mSpeScroll = new QScrollArea(speTab);
	mSpeScroll->setWidgetResizable(true);
	mSpeScroll->setFrameShape(QFrame::NoFrame);
	// 同上：让滚动区/内容页透明，透出 pane 深色背景

	mSpePage = new QWidget();
	mSpeLayout = new QVBoxLayout(mSpePage);
	mSpeLayout->setContentsMargins(0, 0, 0, 0);
	mSpeLayout->setSpacing(0);

	// 根据产品类型生成不同的字段
	auto addSpeField = [&](const QString& label, QWidget* editor) {
		mSpeLayout->addWidget(makeFieldRow(label, editor));
	};

	// 每个字段 setObjectName("spe_xxx")，方便 collectSpeFromForm 通过 findChild 读取
	int productId = meta.id;

	switch (meta.productType)
	{
	case ProductType::Vector: {
		ProductVectorMeta vMeta = mDAO->getVectorMeta(productId);
		auto* e1 = makeLineEdit("spe_geomType");       e1->setText(vMeta.geomType);       addSpeField(QString::fromUtf8("几何类型:"), e1);
		auto* e2 = makeLineEdit("spe_invScale");        e2->setText(vMeta.invScale > 0 ? QString::number(vMeta.invScale) : ""); addSpeField(QString::fromUtf8("比例尺分母:"), e2);
		auto* e3 = makeLineEdit("spe_csType");          e3->setText(vMeta.csType);         addSpeField(QString::fromUtf8("坐标系类型:"), e3);
		auto* e4 = makeLineEdit("spe_geodeticDatum");   e4->setText(vMeta.geodeticDatum);  addSpeField(QString::fromUtf8("大地基准:"), e4);
		auto* e5 = makeLineEdit("spe_epsgCode");        e5->setText(vMeta.epsgCode);       addSpeField(QString::fromUtf8("地图投影代码:"), e5);
		auto* e6 = makeLineEdit("spe_projDesc");        e6->setText(vMeta.projDesc);       addSpeField(QString::fromUtf8("地图投影描述:"), e6);
		auto* e7 = makeTextEdit("spe_fieldDesc");       e7->setText(vMeta.fieldDesc);      addSpeField(QString::fromUtf8("属性字段说明:"), e7);
		break;
	}
	case ProductType::Raster: {
		ProductRasterMeta rMeta = mDAO->getRasterMeta(productId);
		auto* e1 = makeLineEdit("spe_satelliteName");   e1->setText(rMeta.satelliteName);  addSpeField(QString::fromUtf8("卫星名称:"), e1);
		auto* e2 = makeLineEdit("spe_sensorType");      e2->setText(rMeta.sensorType);     addSpeField(QString::fromUtf8("传感器类型:"), e2);
		auto* e3 = makeLineEdit("spe_acquireTime");     e3->setText(rMeta.acquireTime.isValid() ? rMeta.acquireTime.toString("yyyy-MM-dd hh:mm") : ""); addSpeField(QString::fromUtf8("成像时间:"), e3);
		auto* e4 = makeLineEdit("spe_gsd");             e4->setText(rMeta.gsd > 0 ? QString::number(rMeta.gsd, 'f', 6) : ""); addSpeField(QString::fromUtf8("地面分辨率:"), e4);
		auto* e5 = makeLineEdit("spe_resolutionUnit");  e5->setText(rMeta.resolutionUnit); addSpeField(QString::fromUtf8("分辨率单位:"), e5);
		auto* e6 = makeLineEdit("spe_colorType");       e6->setText(rMeta.colorType);      addSpeField(QString::fromUtf8("色彩类型:"), e6);
		auto* e7 = makeLineEdit("spe_bitDepth");        e7->setText(rMeta.bitDepth > 0 ? QString::number(rMeta.bitDepth) : ""); addSpeField(QString::fromUtf8("颜色级数:"), e7);
		auto* e8 = makeLineEdit("spe_bandCount");       e8->setText(QString::number(rMeta.bandCount)); addSpeField(QString::fromUtf8("波段数:"), e8);
		auto* e9 = makeLineEdit("spe_nodataValue");     e9->setText(QString::number(rMeta.nodataValue)); addSpeField(QString::fromUtf8("无数据值:"), e9);
		auto* ea = makeLineEdit("spe_rcsType");         ea->setText(rMeta.csType);         addSpeField(QString::fromUtf8("坐标系类型:"), ea);
		auto* eb = makeLineEdit("spe_rgeodeticDatum");  eb->setText(rMeta.geodeticDatum);  addSpeField(QString::fromUtf8("大地基准:"), eb);
		auto* ec = makeLineEdit("spe_repsgCode");       ec->setText(rMeta.epsgCode);       addSpeField(QString::fromUtf8("地图投影代码:"), ec);
		auto* ed = makeLineEdit("spe_rprojDesc");       ed->setText(rMeta.projDesc);       addSpeField(QString::fromUtf8("地图投影描述:"), ed);
		auto* ee = makeLineEdit("spe_planarUnit");      ee->setText(rMeta.planarUnit);     addSpeField(QString::fromUtf8("平面单位:"), ee);
		break;
	}
	case ProductType::CAD:
	case ProductType::AI:
	case ProductType::CDR:
	case ProductType::PDF: {
		ProductDiagramMeta dMeta = mDAO->getDiagramMeta(productId);
		auto* e1  = makeLineEdit("spe_dataId");          e1->setText(dMeta.dataId);          addSpeField(QString::fromUtf8("图名:"), e1);
		auto* e2  = makeLineEdit("spe_mapSeries");       e2->setText(dMeta.mapSeries);       addSpeField(QString::fromUtf8("图集系列名:"), e2);
		auto* e3  = makeLineEdit("spe_cityPrefecture");  e3->setText(dMeta.cityPrefecture);  addSpeField(QString::fromUtf8("制图区域:"), e3);
		auto* e4  = makeLineEdit("spe_mapScale");        e4->setText(dMeta.mapScale > 0 ? QString::number(dMeta.mapScale) : ""); addSpeField(QString::fromUtf8("比例尺分母:"), e4);
		auto* e5  = makeLineEdit("spe_dprojDesc");       e5->setText(dMeta.projDesc);        addSpeField(QString::fromUtf8("地图投影:"), e5);
		auto* e6  = makeDateEdit("spe_productionDate");  e6->setDate(dMeta.productionDate.isValid() ? dMeta.productionDate : QDate::currentDate()); addSpeField(QString::fromUtf8("制图完成日期:"), e6);
		auto* e7  = makeLineEdit("spe_approvalNo");      e7->setText(dMeta.approvalNo);      addSpeField(QString::fromUtf8("审图号:"), e7);
		auto* e8  = makeLineEdit("spe_cartoSoftware");   e8->setText(dMeta.cartoSoftware);   addSpeField(QString::fromUtf8("制图软件:"), e8);
		auto* e9  = makeLineEdit("spe_format");          e9->setText(dMeta.format);          addSpeField(QString::fromUtf8("数据格式:"), e9);
		auto* e10 = makeLineEdit("spe_paperSize");       e10->setText(dMeta.paperSize);      addSpeField(QString::fromUtf8("图件尺寸:"), e10);
		auto* e11 = makeCombo("spe_legendIncluded",      {"否","是"}, dMeta.legendIncluded ? "是" : "否"); addSpeField(QString::fromUtf8("是否含图例:"), e11);
		auto* e12 = makeLineEdit("spe_modifier");        e12->setText(dMeta.modifier);       addSpeField(QString::fromUtf8("修改人:"), e12);
		auto* e13 = makeDateEdit("spe_lastModified");    e13->setDate(dMeta.lastModified.isValid() ? dMeta.lastModified : QDate::currentDate()); addSpeField(QString::fromUtf8("最后修改时间:"), e13);
		auto* e14 = makeCombo("spe_printReady",          {"否","是"}, dMeta.printReady ? "是" : "否"); addSpeField(QString::fromUtf8("是否印刷级:"), e14);
		auto* e15 = makeLineEdit("spe_colorMode");       e15->setText(dMeta.colorMode);      addSpeField(QString::fromUtf8("色彩模式:"), e15);
		auto* e16 = makeLineEdit("spe_dpi");             e16->setText(dMeta.dpi > 0 ? QString::number(dMeta.dpi) : ""); addSpeField(QString::fromUtf8("输出分辨率:"), e16);
		auto* e17 = makeLineEdit("spe_projName");        e17->setText(dMeta.projName);       addSpeField(QString::fromUtf8("项目名称:"), e17);
		auto* e18 = makeLineEdit("spe_rasterIds");       e18->setText(dMeta.rasterIds);      addSpeField(QString::fromUtf8("关联栅格资产ID:"), e18);
		auto* e19 = makeLineEdit("spe_vectorIds");       e19->setText(dMeta.vectorIds);      addSpeField(QString::fromUtf8("关联矢量资产ID:"), e19);
		auto* e20 = makeLineEdit("spe_endDatetime");     e20->setText(dMeta.endDatetime.isValid() ? dMeta.endDatetime.toString("yyyy-MM-dd hh:mm") : ""); addSpeField(QString::fromUtf8("数据现势性截止:"), e20);
		auto* e21 = makeLineEdit("spe_mapProductor");    e21->setText(dMeta.mapProductor);   addSpeField(QString::fromUtf8("制图人:"), e21);
		auto* e22 = makeCombo("spe_hasMathBase",         {"否","是"}, dMeta.hasMathBase ? "是" : "否"); addSpeField(QString::fromUtf8("是否有数学基础:"), e22);
		break;
	}
	default: {
		ProductDocumentMeta docMeta = mDAO->getDocumentMeta(productId);
		if (docMeta.productId > 0)
		{
			auto* e1  = makeLineEdit("spe_publisher");      e1->setText(docMeta.publisher);      addSpeField(QString::fromUtf8("来源:"), e1);
			auto* e2  = makeLineEdit("spe_fileType");       e2->setText(docMeta.fileType);       addSpeField(QString::fromUtf8("文件类型:"), e2);
			auto* e3  = makeLineEdit("spe_docFormat");      e3->setText(docMeta.format);         addSpeField(QString::fromUtf8("文件格式:"), e3);
			auto* e4  = makeLineEdit("spe_fileSize");       e4->setText(docMeta.fileSize);       addSpeField(QString::fromUtf8("文件大小:"), e4);
			auto* e5  = makeLineEdit("spe_languageType");   e5->setText(docMeta.languageType);   addSpeField(QString::fromUtf8("语言类型:"), e5);
			auto* e6  = makeLineEdit("spe_keyWords");       e6->setText(docMeta.keyWords);       addSpeField(QString::fromUtf8("关键字:"), e6);
			auto* e7  = makeTextEdit("spe_summary");        e7->setText(docMeta.summary);        addSpeField(QString::fromUtf8("内容摘要:"), e7);
			auto* e8  = makeTextEdit("spe_qualityIssues");  e8->setText(docMeta.qualityIssues);  addSpeField(QString::fromUtf8("质量问题记录:"), e8);
			auto* e9  = makeLineEdit("spe_collectTime");    e9->setText(docMeta.collectTime.isValid() ? docMeta.collectTime.toString("yyyy-MM-dd hh:mm") : ""); addSpeField(QString::fromUtf8("收集时间:"), e9);
			auto* e10 = makeLineEdit("spe_docEndDatetime"); e10->setText(docMeta.endDatetime.isValid() ? docMeta.endDatetime.toString("yyyy-MM-dd hh:mm") : ""); addSpeField(QString::fromUtf8("数据现势性截止:"), e10);
			auto* e11 = makeLineEdit("spe_collector");      e11->setText(docMeta.collector);     addSpeField(QString::fromUtf8("收集人:"), e11);
			auto* e12 = makeLineEdit("spe_collectPurpose"); e12->setText(docMeta.collectPurpose); addSpeField(QString::fromUtf8("收集目的:"), e12);
			auto* e13 = makeLineEdit("spe_docProjectName"); e13->setText(docMeta.projectName);   addSpeField(QString::fromUtf8("项目名称:"), e13);
			auto* e14 = makeCombo("spe_isCompressed",       {"否","是"}, docMeta.isCompressed ? "是" : "否"); addSpeField(QString::fromUtf8("是否压缩:"), e14);
		}
		else
		{
			auto* lbl = new QLabel(QString::fromUtf8("该类型暂无专有元数据"));
			lbl->setAlignment(Qt::AlignCenter);
			mSpeLayout->addWidget(lbl);
		}
		break;
	}
	}

	mSpeLayout->addStretch();
	mSpeScroll->setWidget(mSpePage);

	QVBoxLayout* speTabLayout = new QVBoxLayout(speTab);
	speTabLayout->setContentsMargins(0, 0, 0, 0);
	speTabLayout->addWidget(mSpeScroll);
}

void MetadataManagerDialog::collectCompileFields(ProductMetadata& meta)
{
	// 使用 findChild 从 27 项基本元数据 Tab 的控件中读取值
	QWidget* basicTab = ui.mCompileTabWidget->widget(0);
	if (!basicTab) return;

	auto leVal = [&](const QString& name) -> QString {
		auto* w = basicTab->findChild<QLineEdit*>(name);
		return w ? w->text() : QString();
	};
	auto teVal = [&](const QString& name) -> QString {
		auto* w = basicTab->findChild<QTextEdit*>(name);
		return w ? w->toPlainText() : QString();
	};
	auto cbVal = [&](const QString& name) -> int {
		auto* w = basicTab->findChild<QComboBox*>(name);
		return w ? w->currentIndex() : 0;
	};
	auto deVal = [&](const QString& name) -> QDate {
		auto* w = basicTab->findChild<QDateEdit*>(name);
		return (w && w->date().isValid()) ? w->date() : QDate();
	};

	// → ProductMetadata 字段映射（共 27 项）
	meta.dataId          = leVal("meta_dataId");
	meta.productName     = leVal("meta_productName");
	meta.description     = teVal("meta_description");
	meta.source          = leVal("meta_source");
	meta.versionNote     = leVal("meta_versionNote");
	meta.tags            = leVal("meta_tags");

	meta.fileFormat      = leVal("meta_fileFormat");
	meta.fileSize        = leVal("meta_fileSize").toLongLong();
	meta.isCompressed    = (cbVal("meta_isCompressed") == 1) ? QString::fromUtf8("是") : QString::fromUtf8("否");

	meta.bounds          = leVal("meta_bounds");
	meta.centerLon       = leVal("meta_centerLon");
	meta.centerLat       = leVal("meta_centerLat");
	meta.crs             = leVal("meta_crs");
	meta.scale           = leVal("meta_scale");

	meta.startDatetime   = QDateTime(deVal("meta_startDatetime"), QTime(0, 0));
	meta.endDatetime     = QDateTime(deVal("meta_endDatetime"), QTime(23, 59, 59));
	// createdAt / updatedAt 是只读的，不从表单改回数据库

	meta.securityLevel   = stringToSecurityLevel(
		basicTab->findChild<QComboBox*>("meta_securityLevel") ?
		basicTab->findChild<QComboBox*>("meta_securityLevel")->currentText() : "");
	meta.createdBy       = leVal("meta_createdBy");
	meta.city            = leVal("meta_city");
	meta.projectName     = leVal("meta_projectName");
	meta.producer        = leVal("meta_producer");
	meta.productionDate  = deVal("meta_productionDate");
	meta.compilationInfo = teVal("meta_compilationInfo");
	meta.deliveryStatus  = leVal("meta_deliveryStatus");
	meta.deliveryTime    = QDateTime(deVal("meta_deliveryTime"), QTime(0, 0));
}

//── 辅助：从专有元数据 tab 的控件读取值 ──

static QString readText(QWidget* parent, const QString& objName)
{
	auto* w = parent->findChild<QWidget*>(objName);
	if (auto* le = qobject_cast<QLineEdit*>(w)) return le->text();
	if (auto* te = qobject_cast<QTextEdit*>(w)) return te->toPlainText();
	if (auto* cb = qobject_cast<QComboBox*>(w)) return cb->currentText();
	if (auto* de = qobject_cast<QDateEdit*>(w)) return de->date().toString("yyyy-MM-dd");
	return QString();
}

static int readInt(QWidget* parent, const QString& objName)
{
	return readText(parent, objName).toInt();
}

static double readDouble(QWidget* parent, const QString& objName)
{
	return readText(parent, objName).toDouble();
}

static QDate readDate(QWidget* parent, const QString& objName)
{
	QString s = readText(parent, objName);
	return QDate::fromString(s, "yyyy-MM-dd");
}

static QDateTime readDateTime(QWidget* parent, const QString& objName)
{
	QString s = readText(parent, objName);
	return QDateTime::fromString(s, "yyyy-MM-dd hh:mm");
}

void MetadataManagerDialog::collectSpeFromForm(ProductVectorMeta& vm)
{
	vm.geomType       = readText(mSpePage, "spe_geomType");
	vm.invScale       = readInt(mSpePage, "spe_invScale");
	vm.csType         = readText(mSpePage, "spe_csType");
	vm.geodeticDatum  = readText(mSpePage, "spe_geodeticDatum");
	vm.epsgCode       = readText(mSpePage, "spe_epsgCode");
	vm.projDesc       = readText(mSpePage, "spe_projDesc");
	vm.fieldDesc      = readText(mSpePage, "spe_fieldDesc");
}

void MetadataManagerDialog::collectSpeFromForm(ProductRasterMeta& rm)
{
	rm.satelliteName  = readText(mSpePage, "spe_satelliteName");
	rm.sensorType     = readText(mSpePage, "spe_sensorType");
	rm.acquireTime    = readDateTime(mSpePage, "spe_acquireTime");
	rm.gsd            = readDouble(mSpePage, "spe_gsd");
	rm.resolutionUnit = readText(mSpePage, "spe_resolutionUnit");
	rm.colorType      = readText(mSpePage, "spe_colorType");
	rm.bitDepth       = readInt(mSpePage, "spe_bitDepth");
	rm.bandCount      = readInt(mSpePage, "spe_bandCount");
	rm.nodataValue    = readInt(mSpePage, "spe_nodataValue");
	rm.csType         = readText(mSpePage, "spe_rcsType");
	rm.geodeticDatum  = readText(mSpePage, "spe_rgeodeticDatum");
	rm.epsgCode       = readText(mSpePage, "spe_repsgCode");
	rm.projDesc       = readText(mSpePage, "spe_rprojDesc");
	rm.planarUnit     = readText(mSpePage, "spe_planarUnit");
}

void MetadataManagerDialog::collectSpeFromForm(ProductDiagramMeta& dm)
{
	dm.dataId         = readText(mSpePage, "spe_dataId");
	dm.mapSeries      = readText(mSpePage, "spe_mapSeries");
	dm.cityPrefecture = readText(mSpePage, "spe_cityPrefecture");
	dm.mapScale       = readInt(mSpePage, "spe_mapScale");
	dm.projDesc       = readText(mSpePage, "spe_dprojDesc");
	dm.productionDate = readDate(mSpePage, "spe_productionDate");
	dm.approvalNo     = readText(mSpePage, "spe_approvalNo");
	dm.cartoSoftware  = readText(mSpePage, "spe_cartoSoftware");
	dm.format         = readText(mSpePage, "spe_format");
	dm.paperSize      = readText(mSpePage, "spe_paperSize");
	dm.legendIncluded = readText(mSpePage, "spe_legendIncluded") == "是";
	dm.modifier       = readText(mSpePage, "spe_modifier");
	dm.lastModified   = readDate(mSpePage, "spe_lastModified");
	dm.printReady     = readText(mSpePage, "spe_printReady") == "是";
	dm.colorMode      = readText(mSpePage, "spe_colorMode");
	dm.dpi            = readInt(mSpePage, "spe_dpi");
	dm.projName       = readText(mSpePage, "spe_projName");
	dm.rasterIds      = readText(mSpePage, "spe_rasterIds");
	dm.vectorIds      = readText(mSpePage, "spe_vectorIds");
	dm.endDatetime    = readDateTime(mSpePage, "spe_endDatetime");
	dm.mapProductor   = readText(mSpePage, "spe_mapProductor");
	dm.hasMathBase    = readText(mSpePage, "spe_hasMathBase") == "是";
}

void MetadataManagerDialog::collectSpeFromForm(ProductDocumentMeta& dm)
{
	dm.publisher      = readText(mSpePage, "spe_publisher");
	dm.fileType       = readText(mSpePage, "spe_fileType");
	dm.format         = readText(mSpePage, "spe_docFormat");
	dm.fileSize       = readText(mSpePage, "spe_fileSize");
	dm.languageType   = readText(mSpePage, "spe_languageType");
	dm.keyWords       = readText(mSpePage, "spe_keyWords");
	dm.summary        = readText(mSpePage, "spe_summary");
	dm.qualityIssues  = readText(mSpePage, "spe_qualityIssues");
	dm.collectTime    = readDateTime(mSpePage, "spe_collectTime");
	dm.endDatetime    = readDateTime(mSpePage, "spe_docEndDatetime");
	dm.collector      = readText(mSpePage, "spe_collector");
	dm.collectPurpose = readText(mSpePage, "spe_collectPurpose");
	dm.projectName    = readText(mSpePage, "spe_docProjectName");
	dm.isCompressed   = readText(mSpePage, "spe_isCompressed") == "是";
}
