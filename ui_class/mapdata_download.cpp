#include "mapdata_download.h"

#include <algorithm>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QSpinBox>
#include <QTableWidgetItem>

#include <qgsdatasourceuri.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include "ui_fit_helper.h"

namespace
{
const QString kVectorType = QStringLiteral("矢量数据");
const QString kRasterType = QStringLiteral("栅格数据");
const QString kCadType = QStringLiteral("CAD");
const QString kAiType = QStringLiteral("AI");
const QString kCdrType = QStringLiteral("CDR");
const QString kPdfType = QStringLiteral("PDF");
const QString kDocumentType = QStringLiteral("文档文件");
const QString kArchiveType = QStringLiteral("压缩包");
const QString kOtherType = QStringLiteral("Other");

QString uniqueFilePath(const QString& directory, const QString& fileName)
{
	const QFileInfo info(fileName);
	const QString baseName = info.completeBaseName().isEmpty() ? QStringLiteral("download") : info.completeBaseName();
	const QString suffix = info.suffix();
	QString candidate = QDir(directory).filePath(fileName);
	for (int i = 1; QFileInfo::exists(candidate); ++i)
	{
		candidate = QDir(directory).filePath(
			QStringLiteral("%1_%2%3").arg(baseName).arg(i).arg(suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix));
	}
	return candidate;
}
}

mapdata_download::mapdata_download(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
	, mConnectionName(QStringLiteral("MapDataDownload_%1").arg(reinterpret_cast<quintptr>(this)))
{
	ui.setupUi(this);
	DialogFitHelper::install(this);
	mDatabase = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), mConnectionName);

	setWindowTitle(QStringLiteral("地图数据下载"));
	ui.mPasswordEdit->setEchoMode(QLineEdit::Password);
	ui.lineEdit_filepath->setReadOnly(true);
	ui.mSpatialFilterCheck->setChecked(false);
	ui.mSpatialFilterCheck->setEnabled(false);
	ui.mSpatialFilterCheck->setToolTip(QStringLiteral("当前界面未提供空间范围输入，暂不支持空间筛选。"));

	setupTable();
	loadSettings();
	setupConnections();
	updatePagination();
	updateSelectionUi();
}

mapdata_download::~mapdata_download()
{
	if (mDatabase.isValid())
		mDatabase.close();
	mDatabase = QSqlDatabase();
	QSqlDatabase::removeDatabase(mConnectionName);
}

void mapdata_download::setupTable()
{
	ui.mResultTable->setColumnCount(8);
	ui.mResultTable->setHorizontalHeaderLabels({
		QStringLiteral("选择"), QStringLiteral("ID"), QStringLiteral("产品名称"), QStringLiteral("类型"),
		QStringLiteral("格式"), QStringLiteral("密级"), QStringLiteral("版本"), QStringLiteral("更新时间")
	});
	ui.mResultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	ui.mResultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	ui.mResultTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	for (int column = 3; column < ui.mResultTable->columnCount(); ++column)
		ui.mResultTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	ui.mResultTable->verticalHeader()->setVisible(false);
}

void mapdata_download::setupConnections()
{
	connect(ui.mTestBtn, &QPushButton::clicked, this, &mapdata_download::onTestConnection);
	connect(ui.mConnectBtn, &QPushButton::clicked, this, &mapdata_download::onConnect);
	connect(ui.mSearchBtn, &QPushButton::clicked, this, &mapdata_download::onSearch);
	connect(ui.mResetBtn, &QPushButton::clicked, this, &mapdata_download::onReset);
	connect(ui.pushButton_savefile, &QPushButton::clicked, this, &mapdata_download::onChooseOutputDir);
	connect(ui.mSelectAllBtn, &QPushButton::clicked, this, &mapdata_download::onSelectAllToggled);
	connect(ui.mDownloadBtn, &QPushButton::clicked, this, &mapdata_download::onDownloadSelected);
	connect(ui.mPrevPageBtn, &QPushButton::clicked, this, [this]() {
		if (mCurrentPage > 1) { --mCurrentPage; performSearch(); }
	});
	connect(ui.mNextPageBtn, &QPushButton::clicked, this, [this]() {
		if (mCurrentPage < mTotalPages) { ++mCurrentPage; performSearch(); }
	});
	connect(ui.mPageSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int page) {
		if (page != mCurrentPage && page >= 1 && page <= mTotalPages)
		{
			mCurrentPage = page;
			performSearch();
		}
	});
	connect(ui.mResultTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
		if (mUpdatingChecks || !item || item->column() != 0)
			return;
		const int row = item->row();
		if (row < 0 || row >= mSearchResults.size())
			return;
		const DownloadItem& selected = mSearchResults.at(row);
		if (item->checkState() == Qt::Checked)
			mSelectedItems.insert(selected.id, selected);
		else
			mSelectedItems.remove(selected.id);
		updateSelectionUi();
	});
}

void mapdata_download::loadSettings()
{
	QSettings settings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
	ui.mHostEdit->setText(settings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString());
	ui.mPortSpin->setValue(settings.value(QStringLiteral("db/port"), 5432).toInt());
	ui.mDbNameEdit->setText(settings.value(QStringLiteral("db/database"), QStringLiteral("map_products")).toString());
	ui.mSchemaCombo->setCurrentText(settings.value(QStringLiteral("db/schema"), QStringLiteral("public")).toString());
	ui.mUserEdit->setText(settings.value(QStringLiteral("db/user"), QStringLiteral("postgres")).toString());
	ui.mSavePwdCheck->setChecked(settings.value(QStringLiteral("db/savePassword"), false).toBool());
	if (ui.mSavePwdCheck->isChecked())
		ui.mPasswordEdit->setText(settings.value(QStringLiteral("db/password")).toString());
	ui.lineEdit_filepath->setText(settings.value(QStringLiteral("download/outputDir")).toString());
}

void mapdata_download::saveSettings()
{
	QSettings settings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
	settings.setValue(QStringLiteral("db/host"), ui.mHostEdit->text().trimmed());
	settings.setValue(QStringLiteral("db/port"), ui.mPortSpin->value());
	settings.setValue(QStringLiteral("db/database"), ui.mDbNameEdit->text().trimmed());
	settings.setValue(QStringLiteral("db/schema"), ui.mSchemaCombo->currentText().trimmed());
	settings.setValue(QStringLiteral("db/user"), ui.mUserEdit->text().trimmed());
	settings.setValue(QStringLiteral("db/savePassword"), ui.mSavePwdCheck->isChecked());
	settings.setValue(QStringLiteral("download/outputDir"), ui.lineEdit_filepath->text().trimmed());
	if (ui.mSavePwdCheck->isChecked())
		settings.setValue(QStringLiteral("db/password"), ui.mPasswordEdit->text());
	else
		settings.remove(QStringLiteral("db/password"));
}

void mapdata_download::setStatus(const QString& message, const QString& color)
{
	ui.mStatusLabel->setText(message);
	ui.mStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; padding: 4px; }").arg(color));
}

void mapdata_download::onTestConnection()
{
	const QString testConnectionName = mConnectionName + QStringLiteral("_test");
	if (QSqlDatabase::contains(testConnectionName))
		QSqlDatabase::removeDatabase(testConnectionName);
	{
		QSqlDatabase testDatabase = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), testConnectionName);
		testDatabase.setHostName(ui.mHostEdit->text().trimmed());
		testDatabase.setPort(ui.mPortSpin->value());
		testDatabase.setDatabaseName(ui.mDbNameEdit->text().trimmed());
		testDatabase.setUserName(ui.mUserEdit->text().trimmed());
		testDatabase.setPassword(ui.mPasswordEdit->text());
		if (!testDatabase.open())
		{
			setStatus(QStringLiteral("连接失败：") + testDatabase.lastError().text(), QStringLiteral("red"));
			testDatabase.close();
		}
		else
		{
			QSqlQuery query(testDatabase);
			if (query.exec(QStringLiteral("SELECT PostGIS_Version()")) && query.next())
				setStatus(QStringLiteral("测试连接成功，PostGIS 版本：") + query.value(0).toString(), QStringLiteral("green"));
			else
				setStatus(QStringLiteral("数据库已连接，但未检测到 PostGIS：") + query.lastError().text(), QStringLiteral("orange"));
			testDatabase.close();
		}
	}
	QSqlDatabase::removeDatabase(testConnectionName);
}

bool mapdata_download::openDatabase(QString* errorMessage)
{
	if (mDatabase.isOpen())
		mDatabase.close();
	mDatabase.setHostName(ui.mHostEdit->text().trimmed());
	mDatabase.setPort(ui.mPortSpin->value());
	mDatabase.setDatabaseName(ui.mDbNameEdit->text().trimmed());
	mDatabase.setUserName(ui.mUserEdit->text().trimmed());
	mDatabase.setPassword(ui.mPasswordEdit->text());
	if (mDatabase.open())
		return true;
	if (errorMessage)
		*errorMessage = mDatabase.lastError().text();
	return false;
}

QString mapdata_download::quotedSchema() const
{
	const QString schema = ui.mSchemaCombo->currentText().trimmed();
	static const QRegularExpression validSchema(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
	return validSchema.match(schema).hasMatch() ? schema : QString();
}

QString mapdata_download::metadataTable() const
{
	return QStringLiteral("\"%1\".\"product_metadata\"").arg(mConnectedSchema);
}

bool mapdata_download::verifyProductTable(QString* errorMessage) const
{
	const QString schema = quotedSchema();
	if (schema.isEmpty())
	{
		if (errorMessage) *errorMessage = QStringLiteral("Schema 名称只能包含字母、数字和下划线，且不能以数字开头。");
		return false;
	}
	QSqlQuery query(mDatabase);
	query.prepare(QStringLiteral("SELECT 1 FROM information_schema.tables "
		"WHERE table_schema = :schema AND table_name = 'product_metadata'"));
	query.bindValue(QStringLiteral(":schema"), schema);
	if (!query.exec())
	{
		if (errorMessage) *errorMessage = query.lastError().text();
		return false;
	}
	if (!query.next())
	{
		if (errorMessage) *errorMessage = QStringLiteral("未找到 product_metadata 表。");
		return false;
	}
	return true;
}

void mapdata_download::onConnect()
{
	ui.mTestBtn->setEnabled(false);
	ui.mConnectBtn->setEnabled(false);
	setStatus(QStringLiteral("正在连接数据库…"), QStringLiteral("orange"));
	QApplication::processEvents();

	QString error;
	if (!openDatabase(&error))
	{
		setStatus(QStringLiteral("连接失败：") + error, QStringLiteral("red"));
	}
	else if (!verifyProductTable(&error))
	{
		mDatabase.close();
		setStatus(QStringLiteral("连接失败：") + error, QStringLiteral("red"));
	}
	else
	{
		mConnectedSchema = quotedSchema();
		saveSettings();
		setStatus(QStringLiteral("数据库已连接，可以检索和下载成果。"), QStringLiteral("green"));
	}

	ui.mTestBtn->setEnabled(true);
	ui.mConnectBtn->setEnabled(true);
}

QString mapdata_download::buildWhereClause(QMap<QString, QVariant>* bindings) const
{
	QStringList where;
	const QString keyword = ui.mKeywordEdit->text().trimmed();
	if (!keyword.isEmpty())
	{
		where << QStringLiteral("(product_name ILIKE :keyword OR description ILIKE :keyword OR producer ILIKE :keyword "
			"OR approval_number ILIKE :keyword OR compilation_info ILIKE :keyword OR scale ILIKE :keyword OR file_format ILIKE :keyword)");
		bindings->insert(QStringLiteral(":keyword"), QStringLiteral("%%1%").arg(keyword));
	}

	const QString productType = ui.mProductTypeCombo->currentText();
	if (productType == QStringLiteral("矢量数据") || productType == QStringLiteral("栅格数据"))
	{
		where << QStringLiteral("product_type = :product_type");
		bindings->insert(QStringLiteral(":product_type"), productType);
	}
	else if (productType == QStringLiteral("制图文件"))
	{
		where << QStringLiteral("product_type IN (:type_cad, :type_ai, :type_cdr, :type_pdf)");
		bindings->insert(QStringLiteral(":type_cad"), kCadType);
		bindings->insert(QStringLiteral(":type_ai"), kAiType);
		bindings->insert(QStringLiteral(":type_cdr"), kCdrType);
		bindings->insert(QStringLiteral(":type_pdf"), kPdfType);
	}
	else if (productType == QStringLiteral("其它类型文件"))
	{
		where << QStringLiteral("product_type IN (:type_document, :type_archive, :type_other)");
		bindings->insert(QStringLiteral(":type_document"), kDocumentType);
		bindings->insert(QStringLiteral(":type_archive"), kArchiveType);
		bindings->insert(QStringLiteral(":type_other"), kOtherType);
	}

	if (ui.mSecurityLevelCombo->currentIndex() > 0)
	{
		where << QStringLiteral("security_level = :security_level");
		bindings->insert(QStringLiteral(":security_level"), ui.mSecurityLevelCombo->currentText());
	}
	const QString dateFrom = ui.mDateFromEdit->text().trimmed();
	if (!dateFrom.isEmpty())
	{
		where << QStringLiteral("created_at >= CAST(:date_from AS date)");
		bindings->insert(QStringLiteral(":date_from"), dateFrom);
	}
	const QString dateTo = ui.mDateToEdit->text().trimmed();
	if (!dateTo.isEmpty())
	{
		where << QStringLiteral("created_at < CAST(:date_to AS date) + INTERVAL '1 day'");
		bindings->insert(QStringLiteral(":date_to"), dateTo);
	}
	return where.isEmpty() ? QString() : QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
}

void mapdata_download::onSearch()
{
	if (!mDatabase.isOpen())
	{
		QMessageBox::warning(this, QStringLiteral("未连接数据库"), QStringLiteral("请先连接数据库，再执行检索。"));
		return;
	}
	mCurrentPage = 1;
	mSelectedItems.clear();
	performSearch();
}

void mapdata_download::performSearch()
{
	if (!mDatabase.isOpen() || mConnectedSchema.isEmpty())
		return;

	QMap<QString, QVariant> bindings;
	const QString whereClause = buildWhereClause(&bindings);
	QSqlQuery countQuery(mDatabase);
	if (!countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM ") + metadataTable() + whereClause))
	{
		QMessageBox::critical(this, QStringLiteral("检索失败"), countQuery.lastError().text());
		return;
	}
	for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
		countQuery.bindValue(it.key(), it.value());
	if (!countQuery.exec() || !countQuery.next())
	{
		QMessageBox::critical(this, QStringLiteral("检索失败"), countQuery.lastError().text());
		return;
	}
	mTotalResults = countQuery.value(0).toInt();
	mTotalPages = qMax(1, (mTotalResults + mPageSize - 1) / mPageSize);
	if (mCurrentPage > mTotalPages)
		mCurrentPage = mTotalPages;

	QSqlQuery query(mDatabase);
	const QString sql = QStringLiteral("SELECT id, product_name, product_type, file_format, security_level, "
		"current_version, updated_at, file_oid, layer_table_name FROM ") + metadataTable() + whereClause
		+ QStringLiteral(" ORDER BY created_at DESC LIMIT :limit OFFSET :offset");
	if (!query.prepare(sql))
	{
		QMessageBox::critical(this, QStringLiteral("检索失败"), query.lastError().text());
		return;
	}
	for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
		query.bindValue(it.key(), it.value());
	query.bindValue(QStringLiteral(":limit"), mPageSize);
	query.bindValue(QStringLiteral(":offset"), (mCurrentPage - 1) * mPageSize);
	if (!query.exec())
	{
		QMessageBox::critical(this, QStringLiteral("检索失败"), query.lastError().text());
		return;
	}

	mSearchResults.clear();
	while (query.next())
	{
		DownloadItem item;
		item.id = query.value(0).toInt();
		item.productName = query.value(1).toString();
		item.productType = query.value(2).toString();
		item.fileFormat = query.value(3).toString();
		item.securityLevel = query.value(4).toString();
		item.currentVersion = query.value(5).toInt();
		item.updatedAt = query.value(6).toDateTime();
		item.fileOid = query.value(7).toInt();
		item.layerTableName = query.value(8).toString();
		mSearchResults.append(item);
	}

	mUpdatingChecks = true;
	ui.mResultTable->setRowCount(0);
	for (int row = 0; row < mSearchResults.size(); ++row)
	{
		const DownloadItem& item = mSearchResults.at(row);
		ui.mResultTable->insertRow(row);
		auto* checkedItem = new QTableWidgetItem();
		checkedItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		checkedItem->setCheckState(mSelectedItems.contains(item.id) ? Qt::Checked : Qt::Unchecked);
		ui.mResultTable->setItem(row, 0, checkedItem);
		ui.mResultTable->setItem(row, 1, new QTableWidgetItem(QString::number(item.id)));
		ui.mResultTable->setItem(row, 2, new QTableWidgetItem(item.productName));
		ui.mResultTable->setItem(row, 3, new QTableWidgetItem(item.productType));
		ui.mResultTable->setItem(row, 4, new QTableWidgetItem(item.fileFormat));
		ui.mResultTable->setItem(row, 5, new QTableWidgetItem(item.securityLevel));
		ui.mResultTable->setItem(row, 6, new QTableWidgetItem(QString::number(item.currentVersion)));
		ui.mResultTable->setItem(row, 7, new QTableWidgetItem(item.updatedAt.toString(QStringLiteral("yyyy-MM-dd hh:mm"))));
	}
	mUpdatingChecks = false;
	updatePagination();
	updateSelectionUi();
}

void mapdata_download::updatePagination()
{
	ui.mPageSpinBox->blockSignals(true);
	ui.mPageSpinBox->setMaximum(mTotalPages);
	ui.mPageSpinBox->setValue(mCurrentPage);
	ui.mPageSpinBox->blockSignals(false);
	ui.mTotalPageLabel->setText(QStringLiteral("/ %1 页").arg(mTotalPages));
	ui.mPrevPageBtn->setEnabled(mCurrentPage > 1);
	ui.mNextPageBtn->setEnabled(mCurrentPage < mTotalPages);
}

void mapdata_download::updateSelectionUi()
{
	ui.mResultCountLabel->setText(QStringLiteral("共找到 %1 条结果，已选 %2 条").arg(mTotalResults).arg(mSelectedItems.size()));
	const bool allSelected = !mSearchResults.isEmpty() && std::all_of(mSearchResults.cbegin(), mSearchResults.cend(),
		[this](const DownloadItem& item) { return mSelectedItems.contains(item.id); });
	ui.mSelectAllBtn->setText(allSelected ? QStringLiteral("取消全选本页") : QStringLiteral("全选本页"));
	ui.mDownloadBtn->setEnabled(!mSelectedItems.isEmpty());
}

void mapdata_download::setAllCurrentPageChecked(bool checked)
{
	mUpdatingChecks = true;
	for (int row = 0; row < mSearchResults.size(); ++row)
	{
		const DownloadItem& item = mSearchResults.at(row);
		if (checked) mSelectedItems.insert(item.id, item);
		else mSelectedItems.remove(item.id);
		if (QTableWidgetItem* checkItem = ui.mResultTable->item(row, 0))
			checkItem->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
	}
	mUpdatingChecks = false;
	updateSelectionUi();
}

void mapdata_download::onSelectAllToggled()
{
	const bool allSelected = !mSearchResults.isEmpty() && std::all_of(mSearchResults.cbegin(), mSearchResults.cend(),
		[this](const DownloadItem& item) { return mSelectedItems.contains(item.id); });
	setAllCurrentPageChecked(!allSelected);
}

void mapdata_download::onReset()
{
	ui.mKeywordEdit->clear();
	ui.mProductTypeCombo->setCurrentIndex(0);
	ui.mSecurityLevelCombo->setCurrentIndex(0);
	ui.mDateFromEdit->clear();
	ui.mDateToEdit->clear();
	mCurrentPage = 1;
	mTotalPages = 1;
	mTotalResults = 0;
	mSearchResults.clear();
	mSelectedItems.clear();
	ui.mResultTable->setRowCount(0);
	updatePagination();
	updateSelectionUi();
}

void mapdata_download::onChooseOutputDir()
{
	const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择下载保存路径"), ui.lineEdit_filepath->text().trimmed());
	if (!directory.isEmpty())
	{
		ui.lineEdit_filepath->setText(QDir::toNativeSeparators(directory));
		saveSettings();
	}
}

QString mapdata_download::safeFileName(const QString& name) const
{
	QString cleaned = name.trimmed();
	cleaned.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
	return cleaned.isEmpty() ? QStringLiteral("product") : cleaned;
}

bool mapdata_download::exportVector(const DownloadItem& item, const QString& itemDir, QString* errorMessage)
{
	if (item.layerTableName.isEmpty())
	{
		if (errorMessage) *errorMessage = QStringLiteral("未找到矢量图层表名。");
		return false;
	}
	QgsDataSourceUri uri;
	uri.setConnection(mDatabase.hostName(), QString::number(mDatabase.port()), mDatabase.databaseName(),
		mDatabase.userName(), mDatabase.password());
	uri.setDataSource(mConnectedSchema, item.layerTableName, QStringLiteral("geom"), QString(), QString());
	uri.setUseEstimatedMetadata(true);
	QgsVectorLayer layer(uri.uri(), item.productName, QStringLiteral("postgres"));
	if (!layer.isValid())
	{
		if (errorMessage) *errorMessage = QStringLiteral("无法加载 PostGIS 矢量图层：") + item.layerTableName;
		return false;
	}
	QgsVectorFileWriter::SaveVectorOptions options;
	options.driverName = QStringLiteral("ESRI Shapefile");
	options.fileEncoding = QStringLiteral("UTF-8");
	options.layerName = safeFileName(item.productName);
	options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
	QString writerError;
	QString outputFile;
	const QString targetPath = uniqueFilePath(itemDir, safeFileName(item.productName) + QStringLiteral(".shp"));
	const QgsVectorFileWriter::WriterError result = QgsVectorFileWriter::writeAsVectorFormatV3(
		&layer, targetPath, layer.transformContext(), options, &writerError, &outputFile);
	if (result != QgsVectorFileWriter::NoError)
	{
		if (errorMessage) *errorMessage = writerError;
		return false;
	}
	return true;
}

bool mapdata_download::exportLargeObject(const DownloadItem& item, const QString& itemDir, QString* errorMessage)
{
	if (item.fileOid <= 0)
	{
		if (errorMessage) *errorMessage = QStringLiteral("该成果没有可下载的文件对象（file_oid）。");
		return false;
	}
	QSqlQuery query(mDatabase);
	query.prepare(QStringLiteral("SELECT lo_get(:oid)"));
	query.bindValue(QStringLiteral(":oid"), item.fileOid);
	if (!query.exec() || !query.next())
	{
		if (errorMessage) *errorMessage = query.lastError().text();
		return false;
	}
	const QByteArray content = query.value(0).toByteArray();
	if (content.isEmpty())
	{
		if (errorMessage) *errorMessage = QStringLiteral("数据库返回的文件内容为空。");
		return false;
	}
	QString suffix = item.fileFormat.trimmed().toLower();
	if (suffix.startsWith(QLatin1Char('.')))
		suffix.remove(0, 1);
	const QString fileName = safeFileName(item.productName) + (suffix.isEmpty() ? QStringLiteral(".bin") : QStringLiteral(".") + suffix);
	QFile output(uniqueFilePath(itemDir, fileName));
	if (!output.open(QIODevice::WriteOnly))
	{
		if (errorMessage) *errorMessage = output.errorString();
		return false;
	}
	if (output.write(content) != content.size())
	{
		if (errorMessage) *errorMessage = output.errorString();
		output.close();
		return false;
	}
	return true;
}

bool mapdata_download::exportItem(const DownloadItem& item, const QString& outputDir, QString* errorMessage)
{
	const QString itemDir = QDir(outputDir).filePath(
		safeFileName(item.productName) + QStringLiteral("_") + QString::number(item.id));
	if (!QDir().mkpath(itemDir))
	{
		if (errorMessage) *errorMessage = QStringLiteral("无法创建下载目录：") + itemDir;
		return false;
	}
	return item.productType == kVectorType
		? exportVector(item, itemDir, errorMessage)
		: exportLargeObject(item, itemDir, errorMessage);
}

void mapdata_download::onDownloadSelected()
{
	if (mSelectedItems.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("未选择成果"), QStringLiteral("请先勾选要下载的成果。"));
		return;
	}
	const QString outputDir = ui.lineEdit_filepath->text().trimmed();
	if (outputDir.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("未选择保存路径"), QStringLiteral("请先选择下载保存路径。"));
		return;
	}
	if (!QDir().mkpath(outputDir))
	{
		QMessageBox::critical(this, QStringLiteral("下载失败"), QStringLiteral("无法创建保存路径：") + outputDir);
		return;
	}

	const QList<DownloadItem> items = mSelectedItems.values();
	QProgressDialog progress(QStringLiteral("正在准备下载…"), QStringLiteral("取消"), 0, items.size(), this);
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(0);
	ui.mDownloadBtn->setEnabled(false);
	int successCount = 0;
	QStringList failures;
	for (int i = 0; i < items.size(); ++i)
	{
		if (progress.wasCanceled())
			break;
		const DownloadItem& item = items.at(i);
		progress.setLabelText(QStringLiteral("正在下载 (%1/%2)：%3").arg(i + 1).arg(items.size()).arg(item.productName));
		QString error;
		if (exportItem(item, outputDir, &error))
			++successCount;
		else
			failures << QStringLiteral("%1（ID:%2）：%3").arg(item.productName).arg(item.id).arg(error);
		progress.setValue(i + 1);
		QApplication::processEvents();
	}
	ui.mDownloadBtn->setEnabled(!mSelectedItems.isEmpty());

	QString message = QStringLiteral("下载完成。\n保存路径：%1\n成功：%2 条").arg(QDir::toNativeSeparators(outputDir)).arg(successCount);
	if (!failures.isEmpty())
		message += QStringLiteral("\n失败：%1 条\n\n%2").arg(failures.size()).arg(failures.join(QLatin1Char('\n')));
	QMessageBox::information(this, QStringLiteral("下载结果"), message);
}
