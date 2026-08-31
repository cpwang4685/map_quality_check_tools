#include "product_storage_dialog.h"
#include "gdb_layer_selector_dialog.h"
#include "core/metadata_extractor.h"
#include "core/file_storage_manager.h"
#include "core/data_importer.h"
#include "core/user_info_bar.h"
#include "database/product_dao.h"
#include "database/postgis_connector.h"
#include "database/schema_manager.h"
#include "qgsmessagelog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QUuid>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QApplication>
#include "qgsgui.h"
#include "qgsvectorlayer.h"
#include "qgsrasterlayer.h"
#include "qgsproject.h"
#include <libpq-fe.h>

#include "ui_fit_helper.h"

ProductStorageDialog::ProductStorageDialog(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
{
	mExtractor = new MetadataExtractor(this);
	mStorageManager = new FileStorageManager(this);
	mDAO = new ProductDAO(this);
	mImporter = new DataImporter(this);

	ui.setupUi(this);
	QgsGui::enableAutoGeometryRestore(this);
	DialogFitHelper::install(this);
	setupTableColumns();
	setupConnections();
	loadProductList();

	// 右上角显示当前登录用户信息
	addUserInfoBar(this, layout());

	setWindowTitle("成果存储与管理");
	resize(1000, 700);
}

ProductStorageDialog::~ProductStorageDialog()
{
}

void ProductStorageDialog::setupTableColumns()
{
	ui.mProductTable->setColumnCount(8);
	ui.mProductTable->setHorizontalHeaderLabels({
		"ID", "产品名称", "类型", "格式", "大小", "版本", "密级", "更新时间"
	});
	ui.mProductTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	ui.mProductTable->horizontalHeader()->setStretchLastSection(true);
}

void ProductStorageDialog::setupConnections()
{
	connect(ui.mBrowseBtn, &QPushButton::clicked, this, &ProductStorageDialog::onSelectFile);
	connect(ui.mBrowseFolderBtn, &QPushButton::clicked, this, &ProductStorageDialog::onSelectFolder);
	connect(ui.mUploadBtn, &QPushButton::clicked, this, &ProductStorageDialog::onUploadFile);
	connect(ui.mRefreshBtn, &QPushButton::clicked, this, &ProductStorageDialog::onRefreshList);
	connect(ui.mDeleteBtn, &QPushButton::clicked, this, &ProductStorageDialog::onDeleteProduct);
	connect(ui.mAddToMapBtn, &QPushButton::clicked, this, &ProductStorageDialog::onAddToMap);
	connect(ui.mProductTable, &QTableWidget::cellClicked, this, &ProductStorageDialog::onProductSelected);
	connect(ui.mProductTypeCombo, &QComboBox::currentTextChanged, this, &ProductStorageDialog::updateRasterFieldsVisibility);
	connect(mExtractor, &MetadataExtractor::extractionProgress, this, &ProductStorageDialog::onExtractionProgress);
	connect(mImporter, &DataImporter::importProgress, this, [this](int percent, const QString& msg) {
		ui.mProgressBar->setValue(percent);
		ui.mProgressBar->setFormat(msg + " (%p%)");
	});
}

void ProductStorageDialog::onSelectFile()
{
	QString filePath = QFileDialog::getOpenFileName(this,
		"选择制图成果文件",
		QString(),
		"所有支持格式 (*.shp *.geojson *.kml *.gdb *.mdb *.tif *.tiff *.img *.jpg *.jp2 *.png *.pdf *.ai *.cdr *.dwg *.dxf);;"
		"矢量文件 (*.shp *.geojson *.kml *.gdb *.mdb);;"
		"栅格文件 (*.tif *.tiff *.img *.jpg *.jp2);;"
		"制图源文件 (*.pdf *.ai *.cdr *.dwg *.dxf);;"
		"所有文件 (*.*)");

	if (filePath.isEmpty())
		return;

	ui.mFilePathEdit->setText(filePath);
	ui.mUploadBtn->setEnabled(true);

	// 预填产品名称
	QFileInfo fi(filePath);
	ui.mProductNameEdit->setText(fi.baseName());
	ui.mFileFormatEdit->setText(fi.suffix().toLower());
	ui.mFileSizeLabel->setText(formatFileSize(fi.size()));

	// 自动检测产品类型
	ProductType detected = MetadataExtractor::detectProductType(filePath);
	ui.mProductTypeCombo->setCurrentText(productTypeToString(detected));

	// GDB 格式：弹出图层选择对话框
	mIsGdbMultiLayer = false;
	mSelectedGdbLayers.clear();
	mSelectedGdbPath.clear();

	if (fi.suffix().compare("gdb", Qt::CaseInsensitive) == 0)
	{
		QgsMessageLog::logMessage(
			QString("[GDB导入] onSelectFile: 检测到 GDB 文件: %1").arg(filePath),
			"GDB导入", Qgis::Info);

		GDBLayerSelectorDialog dlg(filePath, this);
		if (dlg.exec() != QDialog::Accepted)
		{
			QgsMessageLog::logMessage(
				QString("[GDB导入] 用户取消了图层选择对话框"),
				"GDB导入", Qgis::Info);
			// 用户取消，清空选择
			ui.mFilePathEdit->clear();
			ui.mUploadBtn->setEnabled(false);
			ui.mProductNameEdit->clear();
			return;
		}

		mSelectedGdbPath = filePath;
		mSelectedGdbLayers = dlg.selectedLayers();
		mIsGdbMultiLayer = true;

		QgsMessageLog::logMessage(
			QString("[GDB导入] 用户选择了 %1 个图层: %2")
				.arg(mSelectedGdbLayers.size())
				.arg(mSelectedGdbLayers.join(", ")),
			"GDB导入", Qgis::Info);

		// 更新界面显示：展示 GDB 品名 + 图层数
		ui.mProductNameEdit->setText(fi.baseName());
		ui.mFileSizeLabel->setText(
			QString("GDB (%1 个图层)").arg(mSelectedGdbLayers.size()));

		updateProgressText(QString("已选择 %1 个图层，点击「上传」开始入库").arg(mSelectedGdbLayers.size()), 0);
	}
}

void ProductStorageDialog::onUploadFile()
{
	QString filePath = ui.mFilePathEdit->text();
	if (filePath.isEmpty())
	{
		QMessageBox::warning(this, "提示", "请先选择文件");
		return;
	}

	// ============= GDB 多图层导入分支 =============
	if (mIsGdbMultiLayer && !mSelectedGdbLayers.isEmpty())
	{
		QString productType = ui.mProductTypeCombo->currentText().trimmed();
		QString versionNote = ui.mVersionEdit->text().trimmed();

		QgsMessageLog::logMessage(
			QString("[GDB导入] onUploadFile: 触发 GDB 多图层导入"),
			"GDB导入", Qgis::Info);
		QgsMessageLog::logMessage(
			QString("[GDB导入] gdbPath=%1, productType=%2, versionNote=\"%3\"")
				.arg(mSelectedGdbPath, productType, versionNote),
			"GDB导入", Qgis::Info);

		processGdbLayers(mSelectedGdbPath, mSelectedGdbLayers, productType, versionNote);

		ui.mProgressBar->setVisible(false);
		ui.mProgressLabel->setVisible(false);
		ui.mUploadBtn->setEnabled(true);
		clearForm();
		loadProductList();
		return;
	}

	ui.mProgressBar->setVisible(true);
	ui.mProgressBar->setValue(0);
	ui.mUploadBtn->setEnabled(false);

	// 自动提取元数据
	QApplication::processEvents();
	ProductMetadata meta = mExtractor->extractMetadata(filePath);

	// 补充基本信息
	meta.productName = ui.mProductNameEdit->text();
	meta.productType = stringToProductType(ui.mProductTypeCombo->currentText());
	meta.versionNote = ui.mVersionNoteEdit->toPlainText();
	meta.createdBy = "postgres";
	meta.updatedBy = "postgres";

	// 检查是否已存在同名产品（精确匹配，使用已加载的产品列表）
	ProductMetadata existingMeta;
	bool isUpdate = false;  // 是否为更新已有产品
	for (const auto& p : mProducts)
	{
		if (p.productName == meta.productName)
		{
			existingMeta = p;
			isUpdate = true;
			break;
		}
	}

	// 如果 mProducts 中没找到，再用数据库精确查询一次
	if (!isUpdate)
	{
		ProductDAO::SearchCondition nameCond;
		nameCond.keyword = meta.productName;
		nameCond.limit = 100;
		auto existingProducts = mDAO->searchProducts(nameCond);
		for (const auto& p : existingProducts)
		{
			if (p.productName == meta.productName)
			{
				existingMeta = p;
				isUpdate = true;
				break;
			}
		}
	}

	if (isUpdate)
	{
		// 同名产品已存在，检查哈希是否相同
		if (existingMeta.fileHash == meta.fileHash)
		{
			// 哈希相同，文件内容未变化，拒绝上传
			ui.mProgressBar->setVisible(false);
			ui.mUploadBtn->setEnabled(true);
			QMessageBox::information(this, "提示",
				QString("同名产品「%1」已存在，且文件哈希（SHA256）与已有数据完全相同，不建议进行数据入库。\n"
						"当前版本: %2")
					.arg(meta.productName)
					.arg(existingMeta.currentVersion));
			return;
		}

		// 哈希不同，版本号自动 +1
		int newVersion = existingMeta.currentVersion + 1;
		meta.currentVersion = newVersion;
		meta.id = existingMeta.id;
		meta.dataId = existingMeta.dataId;  // 保持同一 UUID

		// 更新 UI 上的版本号
		ui.mVersionEdit->setText(QString::number(newVersion));

	}
	else
	{
		// 全新产品
		meta.currentVersion = 1;
		meta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		ui.mVersionEdit->setText("1");
	}

	// 自动按产品类型分配到对应目录
	{
		QString typeDirName = productTypeToString(meta.productType);
		int dirId = mDAO->findOrCreateDirectory(typeDirName, 0);  // 根级目录
		if (dirId > 0)
		{
			meta.parentDirId = dirId;
			meta.directoryPath = typeDirName;
		}
	}

	// 导入文件到存储（使用当前版本号，确保每个版本的本地文件独立存储）
	QString storagePath = mStorageManager->importFile(filePath, meta.dataId, meta.currentVersion);
	if (storagePath.isEmpty())
	{
		QMessageBox::critical(this, "错误", "文件导入存储失败");
		ui.mProgressBar->setVisible(false);
		ui.mUploadBtn->setEnabled(true);
		return;
	}
	meta.filePath = storagePath;

	// 生成缩略图
	QString thumbnailPath = mStorageManager->getThumbnailPath(meta.dataId);
	if (MetadataExtractor::generateThumbnail(filePath, thumbnailPath))
	{
		meta.thumbnailPath = thumbnailPath;
	}

	// 生成图层表名
	QString layerTableName;
	if (meta.productType == ProductType::Vector || meta.productType == ProductType::Raster)
	{
		QFileInfo fi(filePath);
		QString safeName = fi.completeBaseName();
		safeName.replace(QRegularExpression("[^a-zA-Z0-9_\\u4e00-\\u9fff]"), "_");
		if (!safeName.isEmpty() && !safeName.at(0).isLetter())
			safeName = "l_" + safeName;
		if (safeName.isEmpty())
			safeName = "layer_data";
		layerTableName = safeName;
		meta.layerTableName = layerTableName;
	}

	// ===== 第一步：完整数据入库（矢量/栅格）——先入库，再写元数据 =====
	bool dataImported = true;  // 非矢量/栅格类型视为不需要入库，直接成功
	int featureCount = -1;
	QString importError;

	if (meta.productType == ProductType::Vector)
	{
		ui.mProgressBar->setValue(10);
		ui.mProgressBar->setFormat("正在将矢量数据导入 PostGIS...");
		QApplication::processEvents();

		int srcSrid = 0;
		if (meta.crs.startsWith("EPSG:"))
			srcSrid = meta.crs.mid(5).toInt();

		featureCount = mImporter->importVectorToPostGIS(
			filePath, layerTableName, srcSrid, 4490);

		if (featureCount < 0)
		{
			dataImported = false;
			importError = mImporter->lastError();
		}
		else if (featureCount == 0)
		{
			dataImported = false;
			const VectorImportStats& stats = mImporter->lastVectorStats();

			// 根据统计信息生成具体的错误原因
			if (stats.allSkipped())
			{
				importError = QString(
					"文件包含 %1 个要素，但全部没有空间几何数据（纯属性表）。\n"
					"该文件可能是不含几何字段的 CSV/DBF 等格式，\n"
					"不支持作为空间数据入库。")
					.arg(stats.totalRead);
			}
			else if (stats.allInsertFailed())
			{
				importError = QString(
					"文件包含 %1 个要素，但全部 INSERT 失败。\n"
					"可能原因：字段类型不兼容、几何数据损坏、或编码问题。\n"
					"请查看 QGIS 日志面板「数据入库」分类了解详情。")
					.arg(stats.totalRead);
			}
			else if (stats.noFeatures())
			{
				importError = QString(
					"GDAL 打开了文件，但未读取到任何要素。\n"
					"源文件可能确实为空，或文件格式无法被 GDAL 正确解析。\n"
					"文件路径: %1").arg(filePath);
			}
			else
			{
				importError = QString(
					"未成功导入任何要素。\n"
					"统计: 总读取 %1 个, 成功 %2, 无几何跳过 %3, INSERT失败 %4。\n"
					"请查看 QGIS 日志面板「数据入库」分类了解详情。")
					.arg(stats.totalRead).arg(stats.featureCount)
					.arg(stats.skipNoGeom).arg(stats.skipInsertFail);
			}

			// 清理空表
			SchemaManager schemaMgr;
			schemaMgr.dropDataLayerTable(layerTableName);
		}
		else
		{
		}
	}
	else if (meta.productType == ProductType::Raster)
	{
		ui.mProgressBar->setValue(10);
		ui.mProgressBar->setFormat("正在将栅格数据导入 PostGIS...");
		QApplication::processEvents();

		if (mImporter->importRasterToPostGIS(filePath, layerTableName, 4490))
		{
			featureCount = 1;
		}
		else
		{
			dataImported = false;
			importError = mImporter->lastError();
		}
	}

	// ===== 入库失败：清理并报错，不写元数据 =====
	if (!dataImported)
	{
		ui.mProgressBar->setVisible(false);
		ui.mUploadBtn->setEnabled(true);

		QString detailMsg = importError.isEmpty()
			? "未知错误，请检查 QGIS 日志面板（「数据入库」和「成果存储」分类）获取详细信息"
			: importError;

		// 提取第一行作为简短原因
		QString shortReason = detailMsg.section('\n', 0, 0);

		QMessageBox::critical(this, "入库失败",
			QString("数据入库失败，产品未保存！\n\n"
					"原因：%1\n\n"
					"排查建议：\n"
					"  1. 在 QGIS 菜单「视图 → 面板 → 日志消息」中查看详细日志\n"
					"  2. 确认源文件是有效的空间数据格式（如 GeoJSON、Shapefile、GPKG 等）\n"
					"  3. 检查数据库连接是否正常\n"
					"  4. 如果文件是纯属性表（CSV/DBF 不含几何），不支持空间入库").arg(shortReason));
		return;
	}

	ui.mProgressBar->setValue(70);
	ui.mProgressBar->setFormat("正在写入元数据...");
	QApplication::processEvents();

	// ===== 第二步：写入/更新元数据到数据库 =====
	int productId = -1;

	if (isUpdate)
	{
		// 更新已有产品（updateProduct 内部会根据哈希自动创建版本）
		if (!mDAO->updateProduct(meta))
		{
			QString errMsg = PostgisConnector::instance()->lastError();
			ui.mProgressBar->setVisible(false);
			ui.mUploadBtn->setEnabled(true);
			QMessageBox::critical(this, "错误", "产品更新失败\n" + errMsg);
			return;
		}
		productId = meta.id;
	}
	else
	{
		// 新建产品
		productId = mDAO->insertProduct(meta);
		if (productId <= 0)
		{
			// 元数据写入失败，回滚：删除已入库的图层表
			if (meta.productType == ProductType::Vector || meta.productType == ProductType::Raster)
			{
				SchemaManager schemaMgr;
				schemaMgr.dropDataLayerTable(layerTableName);
			}

			QString errMsg = PostgisConnector::instance()->lastError();

			ui.mProgressBar->setVisible(false);
			ui.mUploadBtn->setEnabled(true);
			QMessageBox::critical(this, "错误", "元数据写入失败，已清理入库数据\n" + errMsg);
			return;
		}

		// 新建产品时创建初始版本记录
		VersionRecord verRec;
		verRec.productId = productId;
		verRec.versionNumber = 1;
		verRec.filePath = storagePath;
		verRec.fileHash = meta.fileHash;
		verRec.fileSize = meta.fileSize;
		verRec.fileFormat = meta.fileFormat;
		verRec.layerTableName = meta.layerTableName;
		verRec.changeNote = meta.versionNote.isEmpty() ? "初始版本" : meta.versionNote;
		verRec.changedBy = meta.createdBy;
		verRec.parentVersion = 0;
		mDAO->insertVersionRecord(verRec);
	}

	// ===== 第三步：注册图层 =====
	QString importMsg;
	if (meta.productType == ProductType::Vector && featureCount > 0)
	{
		mDAO->registerLayer(productId, layerTableName, meta.geometryType, 4490, featureCount);
		importMsg = QString("，已导入 %1 个空间要素到图层表 %2")
			.arg(featureCount).arg(layerTableName);
	}
	else if (meta.productType == ProductType::Raster && featureCount > 0)
	{
		mDAO->registerLayer(productId, layerTableName, "RASTER", 4490, 1);
		importMsg = QString("，栅格数据已导入到图层表 %1").arg(layerTableName);
	}

	ui.mProgressBar->setValue(100);

	QString resultMsg;
	if (isUpdate)
	{
		resultMsg = QString("成果更新成功！\n产品ID: %1\n版本号: %2\nUUID: %3%4")
			.arg(productId).arg(meta.currentVersion).arg(meta.dataId).arg(importMsg);
	}
	else
	{
		resultMsg = QString("成果上传成功！\n产品ID: %1\nUUID: %2%3")
			.arg(productId).arg(meta.dataId).arg(importMsg);
	}
	QMessageBox::information(this, "成功", resultMsg);
	clearForm();
	loadProductList();

	ui.mProgressBar->setVisible(false);
	ui.mUploadBtn->setEnabled(true);
}

void ProductStorageDialog::onDeleteProduct()
{
	int row = ui.mProductTable->currentRow();
	if (row < 0) return;

	int productId = ui.mProductTable->item(row, 0)->text().toInt();

	auto reply = QMessageBox::question(this, "确认删除",
		QString("确定要删除产品 ID=%1 吗？此操作不可恢复。").arg(productId),
		QMessageBox::Yes | QMessageBox::No);

	if (reply == QMessageBox::Yes)
	{
		if (mDAO->deleteProduct(productId))
		{
			loadProductList();
			clearForm();
		}
	}
}

void ProductStorageDialog::onRefreshList()
{
	loadProductList();
}

void ProductStorageDialog::onProductSelected(int row, int /*col*/)
{
	if (row < 0 || row >= mProducts.size()) return;

	ui.mDeleteBtn->setEnabled(true);
	ui.mAddToMapBtn->setEnabled(true);
	const auto& meta = mProducts[row];
	fillForm(meta);
}

void ProductStorageDialog::onExtractionProgress(int percent, const QString& message)
{
	ui.mProgressBar->setValue(percent);
	ui.mProgressBar->setFormat(message + " (%p%)");
}

void ProductStorageDialog::loadProductList()
{
	mProducts = mDAO->getAllProducts();

	ui.mProductTable->setRowCount(0);
	for (int i = 0; i < mProducts.size(); ++i)
	{
		const auto& meta = mProducts[i];
		ui.mProductTable->insertRow(i);
		ui.mProductTable->setItem(i, 0, new QTableWidgetItem(QString::number(meta.id)));
		ui.mProductTable->setItem(i, 1, new QTableWidgetItem(meta.productName));
		ui.mProductTable->setItem(i, 2, new QTableWidgetItem(productTypeToString(meta.productType)));
		ui.mProductTable->setItem(i, 3, new QTableWidgetItem(meta.fileFormat));
		ui.mProductTable->setItem(i, 4, new QTableWidgetItem(formatFileSize(meta.fileSize)));
		ui.mProductTable->setItem(i, 5, new QTableWidgetItem(QString::number(meta.currentVersion)));
		ui.mProductTable->setItem(i, 6, new QTableWidgetItem(securityLevelToString(meta.securityLevel)));
		ui.mProductTable->setItem(i, 7, new QTableWidgetItem(meta.updatedAt.toString("yyyy-MM-dd hh:mm")));
	}
}

void ProductStorageDialog::clearForm()
{
	ui.mFilePathEdit->clear();
	ui.mProductNameEdit->clear();
	ui.mFileFormatEdit->clear();
	ui.mFileSizeLabel->clear();
	ui.mFileHashLabel->clear();
	ui.mMinXEdit->clear();
	ui.mMinYEdit->clear();
	ui.mMaxXEdit->clear();
	ui.mMaxYEdit->clear();
	ui.mCRSEdit->clear();
	ui.mGeometryTypeEdit->clear();
	ui.mBandCountEdit->clear();
	ui.mResolutionEdit->clear();
	ui.mVersionEdit->setText("1");
	ui.mVersionNoteEdit->clear();
	mCurrentMeta = ProductMetadata();
	ui.mDeleteBtn->setEnabled(false);
	ui.mAddToMapBtn->setEnabled(false);
	ui.mUploadBtn->setEnabled(false);

	// 重置 GDB 多图层状态
	mIsGdbMultiLayer = false;
	mSelectedGdbLayers.clear();
	mSelectedGdbPath.clear();

	// 隐藏进度信息
	ui.mProgressBar->setVisible(false);
	ui.mProgressLabel->setVisible(false);
}

void ProductStorageDialog::fillForm(const ProductMetadata& meta)
{
	mCurrentMeta = meta;
	ui.mProductNameEdit->setText(meta.productName);
	ui.mProductTypeCombo->setCurrentText(productTypeToString(meta.productType));
	ui.mFileFormatEdit->setText(meta.fileFormat);
	ui.mFileSizeLabel->setText(formatFileSize(meta.fileSize));
	ui.mFileHashLabel->setText(meta.fileHash);
	ui.mMinXEdit->setText(QString::number(meta.minX, 'f', 6));
	ui.mMinYEdit->setText(QString::number(meta.minY, 'f', 6));
	ui.mMaxXEdit->setText(QString::number(meta.maxX, 'f', 6));
	ui.mMaxYEdit->setText(QString::number(meta.maxY, 'f', 6));
	ui.mCRSEdit->setText(meta.crs);
	ui.mGeometryTypeEdit->setText(meta.geometryType);
	ui.mBandCountEdit->setText(QString::number(meta.bandCount));
	ui.mResolutionEdit->setText(QString::number(meta.pixelResolution, 'f', 6));
	ui.mVersionEdit->setText(QString::number(meta.currentVersion));
	ui.mVersionNoteEdit->setText(meta.versionNote);

	updateRasterFieldsVisibility();
}

void ProductStorageDialog::updateRasterFieldsVisibility()
{
	bool isRaster = (ui.mProductTypeCombo->currentText() == "栅格数据");
	ui.bandCountLabel->setVisible(isRaster);
	ui.mBandCountEdit->setVisible(isRaster);
	ui.resolutionLabel->setVisible(isRaster);
	ui.mResolutionEdit->setVisible(isRaster);
}

// ============================================================================
// 选择文件夹 — 批量入库
// ============================================================================

void ProductStorageDialog::onSelectFolder()
{
	QString folderPath = QFileDialog::getExistingDirectory(
		this, "选择包含成果文件的文件夹", QString(),
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (folderPath.isEmpty())
		return;

	// 支持的扩展名列表
	static const QStringList supportedExts = {
		"shp", "geojson", "kml", "gdb", "mdb",
		"tif", "tiff", "img", "jpg", "jp2", "png",
		"pdf", "ai", "cdr", "dwg", "dxf"
	};

	// 收集所有支持的文件
	QStringList files;
	
	// 先检查目录本身是否为 GDB（以 .gdb 结尾的目录）
	if (folderPath.endsWith(".gdb", Qt::CaseInsensitive))
	{
		// 弹出图层选择对话框
		GDBLayerSelectorDialog dlg(folderPath, this);
		if (dlg.exec() != QDialog::Accepted)
			return;

		QStringList selectedLayers = dlg.selectedLayers();
		if (selectedLayers.isEmpty())
		{
			QMessageBox::information(this, "提示", "未选择任何图层，导入已取消。");
			return;
		}

		// 直接调用 GDB 图层批量导入
		QFileInfo gdbFi(folderPath);
		processGdbLayers(folderPath, selectedLayers, "矢量数据",
			QString("从文件夹批量导入 GDB: %1").arg(gdbFi.baseName()));

		clearForm();
		loadProductList();
		return;
	}
	else
	{
		QDirIterator it(folderPath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
						QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			it.next();
			QFileInfo fi = it.fileInfo();

			if (fi.isDir())
			{
				// GDB 目录格式
				if (fi.suffix().compare("gdb", Qt::CaseInsensitive) == 0)
					files << fi.absoluteFilePath();
			}
			else if (fi.isFile())
			{
				QString ext = fi.suffix().toLower();
				if (supportedExts.contains(ext))
					files << fi.absoluteFilePath();
			}
		}
	}

	if (files.isEmpty())
	{
		QMessageBox::information(this, "提示",
			QString("在文件夹「%1」中未找到支持的成果文件。\n\n"
					"支持格式: %2")
				.arg(folderPath)
				.arg(supportedExts.join(", ")));
		return;
	}

	// 确认批量导入
	auto reply = QMessageBox::question(this, "批量导入确认",
		QString("在文件夹「%1」中找到 %2 个支持的文件，是否批量入库？")
			.arg(folderPath)
			.arg(files.size()),
		QMessageBox::Yes | QMessageBox::No);

	if (reply != QMessageBox::Yes)
		return;

	// 显示进度条
	ui.mProgressBar->setVisible(true);
	ui.mProgressLabel->setVisible(true);
	ui.mProgressBar->setRange(0, files.size());
	ui.mProgressBar->setValue(0);
	ui.mUploadBtn->setEnabled(false);

	int importedCount = 0;
	int skippedCount = 0;

	for (int i = 0; i < files.size(); ++i)
	{
		ui.mProgressBar->setValue(i);
		QFileInfo fi(files[i]);
		updateProgressText(
			QString("批量导入中... (%1/%2) %3").arg(i + 1).arg(files.size()).arg(fi.fileName()));
		QApplication::processEvents();

		processBatchFile(files[i], importedCount, skippedCount);
	}

	ui.mProgressBar->setValue(files.size());
	ui.mProgressBar->setVisible(false);
	ui.mProgressLabel->setVisible(false);
	ui.mUploadBtn->setEnabled(true);

	QMessageBox::information(this, "批量导入完成",
		QString("成功导入 %1 个文件，跳过 %2 个文件。")
			.arg(importedCount)
			.arg(skippedCount));

	clearForm();
	loadProductList();
}

// ============================================================================
// 处理单个批量文件
// ============================================================================

void ProductStorageDialog::processBatchFile(const QString& filePath, int& importedCount, int& skippedCount)
{
	QApplication::processEvents();

	ProductMetadata meta = mExtractor->extractMetadata(filePath);

	QFileInfo fi(filePath);
	meta.productName = fi.baseName();

	// 检测产品类型
	ProductType detected = MetadataExtractor::detectProductType(filePath);
	if (detected == ProductType::Other)
	{
		// GDB/MDB 可能返回 Vector
		QString ext = fi.suffix().toLower();
		if (ext == "gdb" || ext == "mdb")
			meta.productType = ProductType::Vector;
		else
			meta.productType = detected;
	}
	else
	{
		meta.productType = detected;
	}

	meta.versionNote = "";
	meta.createdBy = "postgres";
	meta.updatedBy = "postgres";

	// 检查同名产品
	ProductMetadata existingMeta;
	bool isUpdate = false;

	// 先查本地缓存
	for (const auto& p : mProducts)
	{
		if (p.productName == meta.productName)
		{
			existingMeta = p;
			isUpdate = true;
			break;
		}
	}

	// 再查数据库
	if (!isUpdate)
	{
		ProductDAO::SearchCondition nameCond;
		nameCond.keyword = meta.productName;
		nameCond.limit = 100;
		auto existingProducts = mDAO->searchProducts(nameCond);
		for (const auto& p : existingProducts)
		{
			if (p.productName == meta.productName)
			{
				existingMeta = p;
				isUpdate = true;
				break;
			}
		}
	}

	if (isUpdate)
	{
		// 同名产品已存在 — 比较哈希
		if (existingMeta.fileHash == meta.fileHash)
		{
			skippedCount++;
			return;
		}

		// 版本号自动 +1，弹出版本说明对话框
		int newVersion = existingMeta.currentVersion + 1;
		meta.currentVersion = newVersion;
		meta.id = existingMeta.id;
		meta.dataId = existingMeta.dataId;

		bool ok = false;
		QString versionNote = QInputDialog::getText(this, "版本说明",
			QString("产品「%1」已存在（当前版本: %2），将自动升级到版本 %3。\n请输入版本说明：")
				.arg(meta.productName)
				.arg(existingMeta.currentVersion)
				.arg(newVersion),
			QLineEdit::Normal,
			QString("批量导入 - 版本更新至 %1").arg(newVersion), &ok);

		if (!ok)
		{
			skippedCount++;
			return;
		}
		meta.versionNote = versionNote;
	}
	else
	{
		meta.currentVersion = 1;
		meta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}

	// 分配到目录
	QString typeDirName = productTypeToString(meta.productType);
	int dirId = mDAO->findOrCreateDirectory(typeDirName, 0);
	if (dirId > 0)
	{
		meta.parentDirId = dirId;
		meta.directoryPath = typeDirName;
	}

	// 导入文件到存储
	QString storagePath = mStorageManager->importFile(filePath, meta.dataId, meta.currentVersion);
	if (storagePath.isEmpty())
	{
		skippedCount++;
		return;
	}
	meta.filePath = storagePath;

	// 生成缩略图
	QString thumbnailPath = mStorageManager->getThumbnailPath(meta.dataId);
	if (MetadataExtractor::generateThumbnail(filePath, thumbnailPath))
	{
		meta.thumbnailPath = thumbnailPath;
	}

	// 生成图层表名
	QString layerTableName;
	if (meta.productType == ProductType::Vector || meta.productType == ProductType::Raster)
	{
		QString safeName = fi.baseName();
		safeName.replace(QRegularExpression("[^a-zA-Z0-9_\\u4e00-\\u9fff]"), "_");
		if (!safeName.isEmpty() && !safeName.at(0).isLetter())
			safeName = "l_" + safeName;
		if (safeName.isEmpty())
			safeName = "layer_data";

		// 更新版本时，表名带版本后缀避免冲突
		if (isUpdate)
			layerTableName = safeName + "_v" + QString::number(meta.currentVersion);
		else
			layerTableName = safeName;

		meta.layerTableName = layerTableName;
	}

	// 数据入库
	bool dataImported = true;
	int featureCount = -1;
	QString importError;

	if (meta.productType == ProductType::Vector)
	{
		int srcSrid = 0;
		if (meta.crs.startsWith("EPSG:"))
			srcSrid = meta.crs.mid(5).toInt();

		featureCount = mImporter->importVectorToPostGIS(filePath, layerTableName, srcSrid, 4490);
		if (featureCount < 0)
		{
			dataImported = false;
			importError = mImporter->lastError();
		}
		else if (featureCount == 0)
		{
			dataImported = false;
			SchemaManager schemaMgr;
			schemaMgr.dropDataLayerTable(layerTableName);
		}
	}
	else if (meta.productType == ProductType::Raster)
	{
		if (mImporter->importRasterToPostGIS(filePath, layerTableName, 4490))
			featureCount = 1;
		else
		{
			dataImported = false;
			importError = mImporter->lastError();
		}
	}

	if (!dataImported)
	{
		skippedCount++;
		return;
	}

	// 写入元数据
	int productId = -1;
	if (isUpdate)
	{
		if (!mDAO->updateProduct(meta))
		{
			skippedCount++;
			return;
		}
		productId = meta.id;
	}
	else
	{
		productId = mDAO->insertProduct(meta);
		if (productId <= 0)
		{
			if (meta.productType == ProductType::Vector || meta.productType == ProductType::Raster)
			{
				SchemaManager schemaMgr;
				schemaMgr.dropDataLayerTable(layerTableName);
			}
			skippedCount++;
			return;
		}

		VersionRecord verRec;
		verRec.productId = productId;
		verRec.versionNumber = 1;
		verRec.filePath = storagePath;
		verRec.fileHash = meta.fileHash;
		verRec.fileSize = meta.fileSize;
		verRec.fileFormat = meta.fileFormat;
		verRec.layerTableName = meta.layerTableName;
		verRec.changeNote = meta.versionNote.isEmpty() ? "初始版本" : meta.versionNote;
		verRec.changedBy = meta.createdBy;
		verRec.parentVersion = 0;
		mDAO->insertVersionRecord(verRec);
	}

	// 注册图层
	if (meta.productType == ProductType::Vector && featureCount > 0)
	{
		mDAO->registerLayer(productId, layerTableName, meta.geometryType, 4490, featureCount);
	}
	else if (meta.productType == ProductType::Raster && featureCount > 0)
	{
		mDAO->registerLayer(productId, layerTableName, "RASTER", 4490, 1);
	}

	importedCount++;

	// 刷新本地产品列表缓存
	mProducts = mDAO->getAllProducts();
}

// ============================================================================
// 添加到地图
// ============================================================================

void ProductStorageDialog::onAddToMap()
{
	int row = ui.mProductTable->currentRow();
	if (row < 0 || row >= mProducts.size())
		return;

	const ProductMetadata& meta = mProducts[row];

	// 检查格式是否支持
	QString ext = meta.fileFormat.toLower();
	static const QStringList unsupportedFormats = {"ai", "cdr", "pdf"};
	if (unsupportedFormats.contains(ext))
	{
		QMessageBox::information(this, "提示",
			QString("「%1」格式不支持添加到地图，该格式属于制图源文件，不含空间数据。")
				.arg(ext.toUpper()));
		return;
	}

	// 非矢量/栅格类型也不支持
	if (meta.productType != ProductType::Vector && meta.productType != ProductType::Raster)
	{
		QMessageBox::information(this, "提示",
			QString("产品「%1」（类型: %2）不支持添加到地图，仅矢量和栅格数据支持。")
				.arg(meta.productName)
				.arg(productTypeToString(meta.productType)));
		return;
	}

	// 检查是否有图层表
	if (meta.layerTableName.isEmpty())
	{
		QMessageBox::warning(this, "提示",
			QString("产品「%1」没有对应的空间图层表，无法添加到地图。")
				.arg(meta.productName));
		return;
	}

	if (!loadLayerToMap(meta))
	{
		QMessageBox::warning(this, "错误",
			QString("产品「%1」添加到地图失败，请检查数据库连接和图层表「%2」是否存在。")
				.arg(meta.productName)
				.arg(meta.layerTableName));
	}
}

// ============================================================================
// 从 PostGIS 加载图层到 QGIS 地图画布
// ============================================================================

bool ProductStorageDialog::loadLayerToMap(const ProductMetadata& meta)
{
	if (!mQGisIface)
		return false;

	PostgisConnector* connector = PostgisConnector::instance();
	if (!connector || !connector->isConnected())
		return false;

	PGconn* pg = connector->nativeConnection();
	if (!pg)
		return false;

	QString host = QString::fromUtf8(PQhost(pg));
	QString port = QString::fromUtf8(PQport(pg));
	QString dbName = QString::fromUtf8(PQdb(pg));
	QString user = QString::fromUtf8(PQuser(pg));
	QString password = QString::fromUtf8(PQpass(pg));
	QString schema = connector->searchPath();
	if (schema.isEmpty()) schema = "public";

	// 兼容旧数据：去掉 _vN 版本后缀（当前版本号仅用于文件存储，不体现在表名中）
	QString baseName = meta.layerTableName;
	baseName.replace(QRegularExpression("_v\\d+$"), "");

	// 大小写不敏感查找实际表名（兼容新旧导入数据：旧数据可能是小写，新数据保留原始大小写）
	QString tableName = resolveTableName(schema, baseName);
	if (tableName.isEmpty() && baseName != meta.layerTableName)
	{
		// 如果去掉后缀后没找到，尝试带版本后缀的原始名称
		tableName = resolveTableName(schema, meta.layerTableName);
	}
	if (tableName.isEmpty())
	{
		// 数据库中确实没有这张表
		tableName = baseName;
	}

	QgsMessageLog::logMessage(
		QString("[加载图层] 最终表名: \"%1\".\"%2\"").arg(schema, tableName),
		"成果存储", Qgis::Info);

	// 构建 PostGIS URI 并加载到 QGIS
	if (meta.productType == ProductType::Vector)
	{
		QString geomType = meta.geometryType.toUpper();
		if (geomType.isEmpty()) geomType = "GEOMETRY";

		QString uri = QString(
			"dbname='%1' host='%2' port='%3' user='%4' password='%5' "
			"sslmode=disable key='id' srid=4490 type=%6 table=\"%7\".\"%8\" (geom)")
			.arg(dbName, host, port, user, password)
			.arg(geomType)
			.arg(schema, tableName);

		QgsVectorLayer* layer = new QgsVectorLayer(uri, meta.productName, "postgres");
		if (!layer->isValid())
		{
			delete layer;
			return false;
		}
		QgsProject::instance()->addMapLayer(layer);
		return true;
	}
	else if (meta.productType == ProductType::Raster)
	{
		// 栅格数据：BLOB导出到本地temp文件，再用QgsRasterLayer加载
		// （当前不必用 PG: URI，因为栅格以 Large Object 形式存储而非 PostGIS raster 格式）
		QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
		QDir().mkpath(tempDir);

		QString localRasterPath;
		if (meta.fileOid > 0)
		{
			localRasterPath = tempDir + "/" + meta.productName + "_oid"
				+ QString::number(meta.fileOid) + "." + meta.fileFormat.toLower();
		}
		else if (!meta.filePath.isEmpty())
		{
			QFileInfo fi(meta.filePath);
			QString targetFileName = fi.fileName();
			if (targetFileName.isEmpty())
				targetFileName = meta.productName + "." + meta.fileFormat.toLower();
			localRasterPath = tempDir + "/" + targetFileName;
		}
		else
		{
			QgsMessageLog::logMessage(
				QString("[加载图层] 栅格数据无可用来源: %1").arg(meta.productName),
				"成果存储", Qgis::Warning);
			return false;
		}

		// 如果缓存文件不存在，从 PG BLOB 导出或从本地文件复制
		if (!QFile::exists(localRasterPath))
		{
			if (meta.fileOid > 0)
			{
				PGresult* beginRes = PQexec(pg, "BEGIN");
				if (PQresultStatus(beginRes) != PGRES_COMMAND_OK)
				{
					QgsMessageLog::logMessage(
						QString("[加载图层] BLOB导出事务开启失败: %1").arg(PQresultErrorMessage(beginRes)),
						"成果存储", Qgis::Critical);
					PQclear(beginRes);
					return false;
				}
				PQclear(beginRes);

				int exportRet = lo_export(pg, meta.fileOid, localRasterPath.toUtf8().constData());
				PGresult* endRes = PQexec(pg, "COMMIT");
				PQclear(endRes);

				if (exportRet != 1)
				{
					QgsMessageLog::logMessage(
						QString("[加载图层] BLOB导出失败: %1").arg(PQerrorMessage(pg)),
						"成果存储", Qgis::Critical);
					return false;
				}
				QgsMessageLog::logMessage(
					QString("[加载图层] BLOB导出成功: %1").arg(localRasterPath),
					"成果存储", Qgis::Info);
			}
			else
			{
				if (!QFile::exists(meta.filePath))
				{
					QgsMessageLog::logMessage(
						QString("[加载图层] 源文件不存在: %1").arg(meta.filePath),
						"成果存储", Qgis::Warning);
					return false;
				}
				if (!QFile::copy(meta.filePath, localRasterPath))
				{
					QgsMessageLog::logMessage(
						QString("[加载图层] 文件复制失败: %1 -> %2").arg(meta.filePath, localRasterPath),
						"成果存储", Qgis::Critical);
					return false;
				}
			}
		}

		QgsRasterLayer* layer = new QgsRasterLayer(localRasterPath, meta.productName);
		if (!layer->isValid())
		{
			delete layer;
			return false;
		}
		QgsProject::instance()->addMapLayer(layer);
		return true;
	}

	return false;
}

// ============================================================================
// 大小写不敏感查找 PostgreSQL 中实际的表名
// 背景：PostgreSQL 对不带引号的标识符自动折叠为小写。
// 之前未加引号入库的数据以小写存储（如 j48g064093_resa），
// 但 QGIS URI 中带引号的表名是大小写敏感的。
// 此方法通过查询 pg_tables 做大小写不敏感匹配，返回实际存储的表名。
// 返回空字符串表示未找到。
// ============================================================================

QString ProductStorageDialog::resolveTableName(const QString& schema, const QString& candidateName)
{
	PostgisConnector* connector = PostgisConnector::instance();
	if (!connector || !connector->isConnected())
		return QString();

	PGconn* pg = connector->nativeConnection();
	if (!pg || PQstatus(pg) != CONNECTION_OK)
		return QString();

	// 用 LOWER() 做大小写不敏感匹配，找到 pg_tables 中实际存在的表名
	QString query = QString(
		"SELECT tablename FROM pg_tables "
		"WHERE schemaname = '%1' AND LOWER(tablename) = LOWER('%2') "
		"LIMIT 1")
		.arg(schema, candidateName);

	PGresult* res = PQexec(pg, query.toUtf8().constData());
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
	{
		QString found = QString::fromUtf8(PQgetvalue(res, 0, 0));
		PQclear(res);
		QgsMessageLog::logMessage(
			QString("[解析表名] 找到: \"%1\" (候选: \"%2\")").arg(found, candidateName),
			"成果存储", Qgis::Info);
		return found;
	}
	PQclear(res);

	QgsMessageLog::logMessage(
		QString("[解析表名] 未找到: \"%1\"").arg(candidateName),
		"成果存储", Qgis::Warning);

	return QString();  // 返回空字符串表示未找到
}

// ============================================================================
// GDB 多图层入库处理
// ============================================================================

void ProductStorageDialog::processGdbLayers(const QString& gdbPath, const QStringList& layerNames,
											 const QString& productType, const QString& versionNote)
{
	int total = layerNames.size();
	int success = 0;
	int failed = 0;

	QFileInfo gdbFi(gdbPath);
	QString gdbBaseName = gdbFi.baseName();

	QgsMessageLog::logMessage(
		QString("========================================"),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("[GDB批量导入] 开始导入 GDB: %1").arg(gdbPath),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("[GDB批量导入] GDB 基础名称: %1").arg(gdbBaseName),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("[GDB批量导入] 产品类型: %1, 版本说明: %2").arg(productType, versionNote),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("[GDB批量导入] 待导入图层 (%1 个): %2").arg(total).arg(layerNames.join(", ")),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("[GDB批量导入] GDB 文件大小: %1 字节").arg(gdbFi.size()),
		"GDB导入", Qgis::Info);

	ui.mProgressBar->setVisible(true);
	ui.mProgressBar->setRange(0, total);
	ui.mProgressBar->setValue(0);
	ui.mProgressLabel->setVisible(true);
	ui.mUploadBtn->setEnabled(false);

	for (int i = 0; i < total; ++i)
	{
		const QString& layerName = layerNames[i];

		QgsMessageLog::logMessage(
			QString("----------------------------------------"),
			"GDB导入", Qgis::Info);
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] [%1/%2] 开始处理图层: %3").arg(i + 1).arg(total).arg(layerName),
			"GDB导入", Qgis::Info);

		// 更新进度
		ui.mProgressBar->setValue(i);
		updateProgressText(
			QString("正在入库图层 [%1/%2]: %3...").arg(i + 1).arg(total).arg(layerName));

		QApplication::processEvents();

		// ===== 为每个图层创建独立的元数据 =====
		ProductMetadata meta;
		meta.productName = gdbBaseName + "_" + layerName;
		meta.productType = stringToProductType(productType);
		meta.fileFormat = "gdb";
		meta.versionNote = versionNote;
		meta.createdBy = "postgres";
		meta.updatedBy = "postgres";
		meta.currentVersion = 1;
		meta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		meta.filePath = gdbPath;
		meta.fileSize = gdbFi.size();

		// 提取该图层的空间元数据（CRS、几何类型、空间范围）
		{
			ProductMetadata spatialMeta = mExtractor->extractLayerMetadata(gdbPath, layerName);
			meta.crs = spatialMeta.crs;
			meta.geometryType = spatialMeta.geometryType;
			meta.minX = spatialMeta.minX;
			meta.minY = spatialMeta.minY;
			meta.maxX = spatialMeta.maxX;
			meta.maxY = spatialMeta.maxY;
			meta.spatialExtentWKT = spatialMeta.spatialExtentWKT;
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 空间元数据: crs=%1, geomType=%2, extent=(%3,%4 -> %5,%6)")
					.arg(meta.crs, meta.geometryType)
					.arg(meta.minX, 0, 'f', 4).arg(meta.minY, 0, 'f', 4)
					.arg(meta.maxX, 0, 'f', 4).arg(meta.maxY, 0, 'f', 4),
				"GDB导入", Qgis::Info);
		}

		QgsMessageLog::logMessage(
			QString("[GDB批量导入] 产品名: %1, UUID: %2, 初始版本: %3")
				.arg(meta.productName, meta.dataId).arg(meta.currentVersion),
			"GDB导入", Qgis::Info);

		// 检查同名产品是否存在
		ProductMetadata existingMeta;
		bool isUpdate = false;
		for (const auto& p : mProducts)
		{
			if (p.productName == meta.productName)
			{
				existingMeta = p;
				isUpdate = true;
				break;
			}
		}
		if (!isUpdate)
		{
			ProductDAO::SearchCondition nameCond;
			nameCond.keyword = meta.productName;
			nameCond.limit = 100;
			auto existingProducts = mDAO->searchProducts(nameCond);
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 数据库搜索同名产品 \"%1\" 返回 %2 条")
					.arg(meta.productName).arg(existingProducts.size()),
				"GDB导入", Qgis::Info);
			for (const auto& p : existingProducts)
			{
				if (p.productName == meta.productName)
				{
					existingMeta = p;
					isUpdate = true;
					break;
				}
			}
		}

		if (isUpdate)
		{
			int newVersion = existingMeta.currentVersion + 1;
			meta.currentVersion = newVersion;
			meta.id = existingMeta.id;
			meta.dataId = existingMeta.dataId;

			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 产品 \"%1\" 已存在(ID=%2), 更新到版本 %3")
					.arg(meta.productName).arg(existingMeta.id).arg(newVersion),
				"GDB导入", Qgis::Info);
		}
		else
		{
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 产品 \"%1\" 为新产品").arg(meta.productName),
				"GDB导入", Qgis::Info);
		}

		// 分配目录
		QString typeDirName = productTypeToString(meta.productType);
		int dirId = mDAO->findOrCreateDirectory(typeDirName, 0);
		if (dirId > 0)
		{
			meta.parentDirId = dirId;
			meta.directoryPath = typeDirName;

			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 目录分配: ID=%1, 路径=%2").arg(dirId).arg(typeDirName),
				"GDB导入", Qgis::Info);
		}
		else
		{
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 警告: 目录创建失败, typeDirName=%1").arg(typeDirName),
				"GDB导入", Qgis::Warning);
		}

		// 文件存储
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] 开始文件存储: %1 -> uuid=%2, ver=%3")
				.arg(gdbPath, meta.dataId).arg(meta.currentVersion),
			"GDB导入", Qgis::Info);

		QString storagePath = mStorageManager->importFile(gdbPath, meta.dataId, meta.currentVersion);
		if (storagePath.isEmpty())
		{
			failed++;
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 失败: 图层 %1 文件存储失败").arg(layerName),
				"GDB导入", Qgis::Critical);
			updateProgressText(
				QString("图层 %1 文件存储失败，已跳过。").arg(layerName));
			continue;
		}
		meta.filePath = storagePath;
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] 文件存储成功: %1").arg(storagePath),
			"GDB导入", Qgis::Info);

		// 缩略图
		QString thumbnailPath = mStorageManager->getThumbnailPath(meta.dataId);
		bool thumbOk = MetadataExtractor::generateThumbnail(gdbPath, thumbnailPath);
		if (thumbOk)
		{
			meta.thumbnailPath = thumbnailPath;
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 缩略图生成成功: %1").arg(thumbnailPath),
				"GDB导入", Qgis::Info);
		}
		else
		{
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 缩略图生成失败/跳过: %1").arg(thumbnailPath),
				"GDB导入", Qgis::Warning);
		}

		// 图层表名
		QString safeLayerName = layerName;
		safeLayerName.replace(QRegularExpression("[^a-zA-Z0-9_\\u4e00-\\u9fff]"), "_");
		if (!safeLayerName.isEmpty() && !safeLayerName.at(0).isLetter())
			safeLayerName = "l_" + safeLayerName;
		if (safeLayerName.isEmpty())
			safeLayerName = "gdb_layer";

		// PostGIS 目标表名：始终使用基础名称
		// 更新时 importVectorLayerToPostGIS 内部会先 DROP 再 CREATE，直接覆盖旧数据
		// 版本号仅用于文件存储，不体现在数据库表名中
		QString layerTableName = safeLayerName;

		meta.layerTableName = layerTableName;

		QgsMessageLog::logMessage(
			QString("[GDB批量导入] 目标表名: %1 (原始图层名: %2, 安全名: %3, isUpdate=%4)")
				.arg(layerTableName, layerName, safeLayerName)
				.arg(isUpdate ? "true" : "false"),
			"GDB导入", Qgis::Info);

		// 数据入库：仅导入指定图层
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] ★ 调用 importVectorLayerToPostGIS: path=%1, layer=%2, table=%3, srcSrid=0, targetSrid=4490")
				.arg(gdbPath, layerName, layerTableName),
			"GDB导入", Qgis::Info);

		updateProgressText(
			QString("正在将图层 [%1/%2] %3 导入 PostGIS...").arg(i + 1).arg(total).arg(layerName));

		int featureCount = mImporter->importVectorLayerToPostGIS(
			gdbPath, layerName, layerTableName, 0, 4490);

		QgsMessageLog::logMessage(
			QString("[GDB批量导入] importVectorLayerToPostGIS 返回: featureCount=%1").arg(featureCount),
			"GDB导入", Qgis::Info);

		if (featureCount < 0)
		{
			failed++;
			QString errMsg = mImporter->lastError();
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] ★ 失败: 图层 %1 导入失败. 错误: %2")
					.arg(layerName, errMsg),
				"GDB导入", Qgis::Critical);
			updateProgressText(
				QString("图层 %1 导入失败: %2").arg(layerName, errMsg));

			// 清理失败时创建的表
			SchemaManager schemaMgr;
			schemaMgr.dropDataLayerTable(layerTableName);
			continue;
		}

		if (featureCount == 0)
		{
			failed++;
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 失败: 图层 %1 无有效要素").arg(layerName),
				"GDB导入", Qgis::Warning);
			SchemaManager schemaMgr;
			schemaMgr.dropDataLayerTable(layerTableName);
			updateProgressText(
				QString("图层 %1 无有效要素，已跳过。").arg(layerName));
			continue;
		}

		// 写入元数据
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] 开始写入元数据: productName=%1, isUpdate=%2, featureCount=%3")
				.arg(meta.productName).arg(isUpdate ? "true" : "false").arg(featureCount),
			"GDB导入", Qgis::Info);

		int productId;
		if (isUpdate)
		{
			if (!mDAO->updateProduct(meta))
			{
				QgsMessageLog::logMessage(
					QString("[GDB批量导入] ★ 失败: updateProduct 失败, productName=%1").arg(meta.productName),
					"GDB导入", Qgis::Critical);
				SchemaManager schemaMgr;
				schemaMgr.dropDataLayerTable(layerTableName);
				failed++;
				continue;
			}
			productId = meta.id;
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 元数据更新成功: productId=%1").arg(productId),
				"GDB导入", Qgis::Info);
		}
		else
		{
			productId = mDAO->insertProduct(meta);
			if (productId <= 0)
			{
				QgsMessageLog::logMessage(
					QString("[GDB批量导入] ★ 失败: insertProduct 失败, productName=%1, 返回=%2")
						.arg(meta.productName).arg(productId),
					"GDB导入", Qgis::Critical);
				SchemaManager schemaMgr;
				schemaMgr.dropDataLayerTable(layerTableName);
				failed++;
				continue;
			}
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 元数据插入成功: productId=%1").arg(productId),
				"GDB导入", Qgis::Info);
		}

		// 注册图层
		bool regOk = mDAO->registerLayer(productId, layerTableName, meta.geometryType, 4490, featureCount);
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] 图层注册: productId=%1, table=%2, geomType=%3, result=%4")
				.arg(productId).arg(layerTableName, meta.geometryType)
				.arg(regOk ? "成功" : "失败"),
			"GDB导入", Qgis::Info);

		// 创建版本记录
		if (!isUpdate)
		{
			VersionRecord verRec;
			verRec.productId = productId;
			verRec.versionNumber = 1;
			verRec.filePath = storagePath;
			verRec.fileHash = meta.fileHash;
			verRec.fileSize = meta.fileSize;
			verRec.fileFormat = "gdb";
			verRec.layerTableName = layerTableName;
			verRec.changeNote = versionNote.isEmpty()
				? QString("从 GDB 导入图层 %1").arg(layerName)
				: versionNote;
			verRec.changedBy = meta.createdBy;
			verRec.parentVersion = 0;

			int verId = mDAO->insertVersionRecord(verRec);
			QgsMessageLog::logMessage(
				QString("[GDB批量导入] 版本记录: productId=%1, versionNumber=1, result=%2")
					.arg(productId).arg(verId),
				"GDB导入", Qgis::Info);
		}

		success++;
		QgsMessageLog::logMessage(
			QString("[GDB批量导入] ✓ 图层 %1 入库成功 (%2 个要素)").arg(layerName).arg(featureCount),
			"GDB导入", Qgis::Info);

		updateProgressText(
			QString("图层 %1 入库成功 (%2 个要素)").arg(layerName).arg(featureCount));

		// 刷新本地缓存
		mProducts = mDAO->getAllProducts();
	}

	// 最终进度
	ui.mProgressBar->setValue(total);
	updateProgressText(
		QString("GDB 图层导入完成！成功: %1, 失败: %2").arg(success).arg(failed));

	QgsMessageLog::logMessage(
		QString("========================================"),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("[GDB批量导入] ====== 导入完成 ====== 成功: %1/%2, 失败: %3/%2")
			.arg(success).arg(total).arg(failed),
		"GDB导入", Qgis::Info);
	QgsMessageLog::logMessage(
		QString("========================================"),
		"GDB导入", Qgis::Info);

	QMessageBox::information(this, "GDB 导入完成",
		QString("GDB 图层导入完成。\n成功: %1 个图层\n失败: %2 个图层")
			.arg(success).arg(failed));
}

// ============================================================================
// 更新进度条文字说明
// ============================================================================

void ProductStorageDialog::updateProgressText(const QString& text, int value)
{
	ui.mProgressLabel->setText(text);
	ui.mProgressLabel->setVisible(true);

	if (value >= 0)
		ui.mProgressBar->setValue(value);
}

