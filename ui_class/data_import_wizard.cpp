#include "data_import_wizard.h"
#include "core/data_importer.h"
#include "core/directory_helper.h"
#include "database/postgis_connector.h"
#include "database/product_dao.h"
#include "core/metadata_extractor.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QUuid>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
#include <qgsmessagelog.h>
#include <QMessageBox>
#include <QApplication>
#include <QHeaderView>
#include <QScrollBar>
#include <QSplitter>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <qgsmessagelog.h>

// GDAL headers（GDB/MDB 图层枚举）
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

// ============================================================================
// CSV 元数据解析
// ============================================================================

static QVector<MetadataFieldDef> parseMetadataCSV(const QString& csvPath)
{
	QVector<MetadataFieldDef> fields;
	QFile file(csvPath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		QgsMessageLog::logMessage(
			QStringLiteral("[导入向导] 无法打开元数据文件: %1").arg(csvPath),
			"MapProductTools", Qgis::Warning);
		return fields;
	}

	QTextStream in(&file);
	in.setCodec("UTF-8");

	// 跳过标题行
	QString headerLine = in.readLine();
	if (headerLine.isEmpty())
	{
		file.close();
		return fields;
	}

	while (!in.atEnd())
	{
		QString line = in.readLine();
		// 仅去除行尾换行符，保留末尾 \t（防止备注列为空时列数不足被跳过）
		while (line.endsWith('\n') || line.endsWith('\r'))
			line.chop(1);
		if (line.isEmpty())
			continue;

		// CSV 分隔符：制表符或逗号
		QStringList parts;
		if (line.contains('\t'))
			parts = line.split('\t');
		else
			parts = line.split(',');

		// 需要至少 6 列：序号, 数据元素名称, 数据元素简称, 数据类型, 长度, 备注
		if (parts.size() < 6)
			continue;

		MetadataFieldDef field;
		field.name   = parts[1].trimmed();  // 数据元素名称
		field.key    = parts[2].trimmed();  // 数据元素简称
		field.type   = parts[3].trimmed();  // 数据类型
		field.length = parts[4].trimmed();  // 长度
		field.remark = parts[5].trimmed();  // 备注

		if (!field.name.isEmpty() && !field.key.isEmpty())
			fields.append(field);
	}

	file.close();
	return fields;
}

// 获取元数据目录路径（相对于 exe 所在目录）
static QString metadataDir()
{
	return QCoreApplication::applicationDirPath() + QStringLiteral("/metadata");
}

// ============================================================================
// 构造与析构
// ============================================================================

DataImportWizard::DataImportWizard(QWidget* parent)
	: QDialog(parent)
	, mStackedWidget(nullptr)
	, mTypeGroup(nullptr)
	, mRadioVector(nullptr)
	, mRadioRaster(nullptr)
	, mRadioOther(nullptr)
	, mRadioAll(nullptr)
	, mDirEdit(nullptr)
	, mBtnBrowse(nullptr)
	, mDirInfoLabel(nullptr)
	, mMetadataScroll(nullptr)
	, mBasicMetaGroupBox(nullptr)
	, mBasicMetaFormLayout(nullptr)
	, mSpecificMetaGroupBox(nullptr)
	, mSpecificMetaFormLayout(nullptr)
	, mProgressBar(nullptr)
	, mProgressLabel(nullptr)
	, mBtnImport(nullptr)
	, mBtnBack(nullptr)
	, mBtnNext(nullptr)
	, mUserInfoLabel(nullptr)
	, mImporter(nullptr)
{
	setWindowTitle(QString::fromUtf8("数据导入向导"));
	resize(1080, 930);
	setMinimumSize(900, 720);

	mImporter = new DataImporter(this);

	connect(mImporter, &DataImporter::importProgress,
		this, &DataImportWizard::onImportProgress);
	connect(mImporter, &DataImporter::importCompleted,
		this, &DataImportWizard::onImportFinished);
	connect(mImporter, &DataImporter::importFailed,
		this, &DataImportWizard::onImportFailed);

	setupUi();
	loadMetadataFields(ImportDataType::Vector);
}

DataImportWizard::~DataImportWizard()
{
}

// ============================================================================
// UI 构建
// ============================================================================

void DataImportWizard::setupUi()
{
	auto* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	auto* topLayout = new QVBoxLayout();
	topLayout->setContentsMargins(0, 0, 0, 0);

	// ── 数据库连接信息 ──
	{
		auto* dbInfoFrame = new QFrame(this);
		dbInfoFrame->setObjectName(QStringLiteral("dbInfoFrame"));
		dbInfoFrame->setFixedHeight(28);

		auto* dbHBox = new QHBoxLayout(dbInfoFrame);
		dbHBox->setContentsMargins(12, 2, 12, 2);

		mUserInfoLabel = new QLabel(dbInfoFrame);

		// 查询当前数据库连接信息
		auto* db = PostgisConnector::instance();
		if (db->isConnected())
		{
			auto rows = db->executeQuery("SELECT current_user as username, current_database() as dbname");
			if (!rows.isEmpty())
			{
				auto m = rows[0].toMap();
				QString userName = m.value("username").toString();
				QString dbName   = m.value("dbname").toString();
				mUserInfoLabel->setText(
					QString::fromUtf8("数据库: %1  |  用户: %2  |  角色: %3")
						.arg(dbName, userName,
							 gCurrentUserSession.isLoggedIn
								 ? accessRoleToString(gCurrentUserSession.role)
								 : QString::fromUtf8("未登录")));
			}
		}
		else
		{
			mUserInfoLabel->setText(QString::fromUtf8("数据库未连接"));
		}

		dbHBox->addWidget(mUserInfoLabel);
		dbHBox->addStretch();
		topLayout->addWidget(dbInfoFrame);
	}

	mainLayout->addLayout(topLayout);

	// ── Stacked Widget ──
	mStackedWidget = new QStackedWidget(this);
	mStackedWidget->setObjectName(QStringLiteral("mStackedWidget"));

	// ====================== Page 0: 选择数据类型 ======================
	mPageType = new QWidget();
	{
		auto* pageLayout = new QVBoxLayout(mPageType);
		pageLayout->setContentsMargins(40, 30, 40, 30);
		pageLayout->setSpacing(16);

		// 标题
		auto* titleLabel = new QLabel(QString::fromUtf8("选择入库数据类型"));
		QFont titleFont = titleLabel->font();
		titleFont.setBold(true);
		titleLabel->setFont(titleFont);
		pageLayout->addWidget(titleLabel);

		auto* subtitleLabel = new QLabel(QString::fromUtf8("请选择要导入的数据类型，系统将根据类型提供对应的元数据表单。"));
		subtitleLabel->setWordWrap(true);
		pageLayout->addWidget(subtitleLabel);

		// 分隔线
		auto* sep = new QFrame();
		sep->setFrameShape(QFrame::HLine);
		pageLayout->addWidget(sep);

		// 数据类型选项组
		auto* typeGroupBox = new QGroupBox();
		auto* typeLayout = new QVBoxLayout(typeGroupBox);
		typeLayout->setSpacing(10);

		auto makeRadio = [&](const QString& text, const QString& hint) -> QRadioButton* {
			auto* radio = new QRadioButton(text);

			auto* hintLabel = new QLabel(hint);
			// 使用布局辅助
			typeLayout->addWidget(radio);
			typeLayout->addWidget(hintLabel);
			return radio;
		};

		mRadioVector  = makeRadio(QString::fromUtf8("矢量数据"),    QString::fromUtf8("支持的格式: .shp .geojson .kml .gdb .mdb"));
		mRadioRaster  = makeRadio(QString::fromUtf8("栅格数据"),    QString::fromUtf8("支持的格式: .tif .tiff .img .jpg .jp2"));
		mRadioOther   = makeRadio(QString::fromUtf8("制图文件"),    QString::fromUtf8("支持的格式: .pdf .ai .cdr .dwg .dxf"));
		mRadioAll     = makeRadio(QString::fromUtf8("其它类型文件"), QString::fromUtf8("支持的格式: .doc .docx .xls .xlsx .xml .txt .csv .zip .rar .7z 等"));

		mTypeGroup = new QButtonGroup(this);
		mTypeGroup->addButton(mRadioVector, static_cast<int>(ImportDataType::Vector));
		mTypeGroup->addButton(mRadioRaster, static_cast<int>(ImportDataType::Raster));
		mTypeGroup->addButton(mRadioOther, static_cast<int>(ImportDataType::Other));
		mTypeGroup->addButton(mRadioAll, static_cast<int>(ImportDataType::All));
		mRadioVector->setChecked(true);

		connect(mTypeGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked),
			this, [this](QAbstractButton*) {
				ImportDataType dt = selectedDataType();
				loadMetadataFields(dt);
				// 栅格子类别仅栅格类型可见
				bool isRaster = (dt == ImportDataType::Raster);
				if (mRasterSubcategoryLabel)
					mRasterSubcategoryLabel->setVisible(isRaster);
				if (mRasterSubcategoryCombo)
					mRasterSubcategoryCombo->setVisible(isRaster);
			});

		pageLayout->addWidget(typeGroupBox);
		pageLayout->addStretch();
	}
	mStackedWidget->addWidget(mPageType);

	// ====================== Page 1: 数据目录 + 元数据 + 入库 ======================
	mPageImport = new QWidget();
	{
		auto* pageLayout = new QVBoxLayout(mPageImport);
		pageLayout->setContentsMargins(24, 20, 24, 16);
		pageLayout->setSpacing(12);

		// 标题
		auto* titleLabel = new QLabel(QString::fromUtf8("设置数据目录与元数据"));
		QFont titleFont = titleLabel->font();
		titleFont.setBold(true);
		titleLabel->setFont(titleFont);
		pageLayout->addWidget(titleLabel);

		// 目录选择行
		auto* dirLayout = new QHBoxLayout();
		auto* dirLabel = new QLabel(QString::fromUtf8("数据目录:"));
		dirLabel->setMinimumWidth(70);
		mDirEdit = new QLineEdit();
		mDirEdit->setPlaceholderText(QString::fromUtf8("请选择或输入数据所在目录..."));

		mBtnBrowse = new QPushButton(QString::fromUtf8("浏览..."));
		connect(mBtnBrowse, &QPushButton::clicked, this, &DataImportWizard::onBrowseDir);

		dirLayout->addWidget(dirLabel);
		dirLayout->addWidget(mDirEdit, 1);
		dirLayout->addWidget(mBtnBrowse);

		mDirInfoLabel = new QLabel();

		// 栅格子类别选择（仅栅格类型显示）
		auto* rasterLayout = new QHBoxLayout();
		mRasterSubcategoryLabel = new QLabel(QString::fromUtf8("栅格子类别:"));
		mRasterSubcategoryLabel->setMinimumWidth(70);
		mRasterSubcategoryCombo = new QComboBox();
		mRasterSubcategoryCombo->addItem(QString::fromUtf8("影像"));
		mRasterSubcategoryCombo->addItem(QString::fromUtf8("晕渲"));
		mRasterSubcategoryCombo->setCurrentIndex(0);
		rasterLayout->addWidget(mRasterSubcategoryLabel);
		rasterLayout->addWidget(mRasterSubcategoryCombo, 1);
		// 默认仅栅格类型可见
		mRasterSubcategoryLabel->setVisible(selectedDataType() == ImportDataType::Raster);
		mRasterSubcategoryCombo->setVisible(selectedDataType() == ImportDataType::Raster);

		pageLayout->addLayout(dirLayout);
		pageLayout->addWidget(mDirInfoLabel);
		pageLayout->addLayout(rasterLayout);

	// 元数据表单区域（可滚动，内含两个分组）
	mMetadataScroll = new QScrollArea();
	mMetadataScroll->setWidgetResizable(true);
	mMetadataScroll->setFrameShape(QFrame::StyledPanel);

	auto* metaContainer = new QWidget();
	auto* metaContainerLayout = new QVBoxLayout(metaContainer);
	metaContainerLayout->setContentsMargins(12, 8, 12, 8);
	metaContainerLayout->setSpacing(10);

	// 基础元数据 GroupBox
	mBasicMetaGroupBox = new QGroupBox(QString::fromUtf8("基础元数据"));
	mBasicMetaFormLayout = new QFormLayout(mBasicMetaGroupBox);
	mBasicMetaFormLayout->setContentsMargins(12, 8, 12, 8);
	mBasicMetaFormLayout->setSpacing(6);
	mBasicMetaFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	mBasicMetaFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	// 专有元数据 GroupBox
	mSpecificMetaGroupBox = new QGroupBox(QString::fromUtf8("专有元数据"));
	mSpecificMetaFormLayout = new QFormLayout(mSpecificMetaGroupBox);
	mSpecificMetaFormLayout->setContentsMargins(12, 8, 12, 8);
	mSpecificMetaFormLayout->setSpacing(6);
	mSpecificMetaFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	mSpecificMetaFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	metaContainerLayout->addWidget(mBasicMetaGroupBox);
	metaContainerLayout->addWidget(mSpecificMetaGroupBox);
	metaContainerLayout->addStretch();

	mMetadataScroll->setWidget(metaContainer);
	pageLayout->addWidget(mMetadataScroll, 1);

		// ── 入库日志（可收缩） ──
		mLogGroupBox = new QGroupBox();
		auto* logHeaderLayout = new QHBoxLayout();
		logHeaderLayout->setContentsMargins(0, 0, 0, 0);

		mBtnToggleLog = new QPushButton(QString::fromUtf8("▼ 入库日志"));
		mBtnToggleLog->setCursor(Qt::PointingHandCursor);
		connect(mBtnToggleLog, &QPushButton::clicked, this, &DataImportWizard::onToggleLog);

		logHeaderLayout->addWidget(mBtnToggleLog);
		logHeaderLayout->addStretch();

		mLogTextEdit = new QPlainTextEdit();
		mLogTextEdit->setReadOnly(true);
		mLogTextEdit->setMaximumBlockCount(2000);
		mLogTextEdit->setMinimumHeight(60);
		mLogTextEdit->setMaximumHeight(220);

		mLogExpanded = false;
		mLogTextEdit->setVisible(false);
		mBtnToggleLog->setText(QString::fromUtf8("▶ 入库日志"));
		mBtnToggleLog->setToolTip(QString::fromUtf8("点击展开入库日志"));

		auto* logLayout = new QVBoxLayout(mLogGroupBox);
		logLayout->setContentsMargins(4, 2, 4, 4);
		logLayout->setSpacing(2);
		logLayout->addLayout(logHeaderLayout);
		logLayout->addWidget(mLogTextEdit);

		pageLayout->addWidget(mLogGroupBox);

		// 进度条 + 入库按钮
		auto* progressLayout = new QHBoxLayout();

		mProgressBar = new QProgressBar();
		mProgressBar->setRange(0, 100);
		mProgressBar->setValue(0);
		mProgressBar->setVisible(false);
		mProgressBar->setTextVisible(true);

		mProgressLabel = new QLabel();
		mProgressLabel->setVisible(false);

		progressLayout->addWidget(mProgressBar, 1);
		progressLayout->addWidget(mProgressLabel);

		mBtnImport = new QPushButton(QString::fromUtf8("开始入库"));
		mBtnImport->setEnabled(false);
		connect(mBtnImport, &QPushButton::clicked, this, &DataImportWizard::onStartImport);

		pageLayout->addLayout(progressLayout);
		pageLayout->addWidget(mBtnImport, 0, Qt::AlignRight);
	}
	mStackedWidget->addWidget(mPageImport);

	mainLayout->addWidget(mStackedWidget, 1);

	// ── 底部：导航按钮栏 ──
	auto* navLayout = new QHBoxLayout();
	navLayout->setContentsMargins(20, 8, 20, 10);

	mBtnBack = new QPushButton(QString::fromUtf8("< 上一步"));
	mBtnBack->setVisible(false);
	connect(mBtnBack, &QPushButton::clicked, this, &DataImportWizard::onPrevPage);

	mBtnNext = new QPushButton(QString::fromUtf8("下一步 >"));
	connect(mBtnNext, &QPushButton::clicked, this, &DataImportWizard::onNextPage);

	auto* cancelBtn = new QPushButton(QString::fromUtf8("取消"));
	connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

	navLayout->addWidget(cancelBtn);
	navLayout->addStretch();
	navLayout->addWidget(mBtnBack);
	navLayout->addWidget(mBtnNext);

	mainLayout->addLayout(navLayout);

	// 初始状态：显示第一页
	mStackedWidget->setCurrentIndex(0);
}

// ============================================================================
// 元数据加载（根据所选数据类型组合 CSV）
// ============================================================================

void DataImportWizard::loadMetadataFields(ImportDataType dataType)
{
	// 清除旧表单
	while (mBasicMetaFormLayout->rowCount() > 0)
		mBasicMetaFormLayout->removeRow(0);
	while (mSpecificMetaFormLayout->rowCount() > 0)
		mSpecificMetaFormLayout->removeRow(0);
	mMetadataLineEdits.clear();
	mCurrentFields.clear();

	// —— 辅助：将字段列表添加到指定 QFormLayout ——
	// 规则：备注含「暂未存储」→ 隐藏；含「系统自动」→ 只读展示；其余 → 可编辑
	auto addFieldsToLayout = [&](const QVector<MetadataFieldDef>& fields, QFormLayout* layout)
	{
		for (const auto& field : fields)
		{
			// 暂未实现的不展示在表单中
			if (field.remark.contains(QStringLiteral("暂未存储")))
				continue;

			bool isAuto = field.remark.contains(QStringLiteral("系统自动"));

			auto* label = new QLabel(field.name + ":");
			if (isAuto)
			{
				QFont font = label->font();
				font.setItalic(true);
				label->setFont(font);
			}

			auto* edit = new QLineEdit();
			QString placeholder = isAuto
				? QString::fromUtf8("（%1）").arg(field.remark)
				: (field.remark.isEmpty() ? field.name : field.remark);

			edit->setPlaceholderText(placeholder);
			edit->setReadOnly(isAuto);
			edit->setMinimumWidth(200);

			layout->addRow(label, edit);

			// 只读字段不纳入收集（不会覆盖自动生成的值）
			if (!isAuto)
				mMetadataLineEdits[field.key] = edit;
		}
	};

	// 1. 基础元数据（所有类型共用）
	QVector<MetadataFieldDef> basicFields = parseMetadataCSV(metadataDir() + "/基础元数据.csv");
	QVector<MetadataFieldDef> specificFields;

	// 2. 按类型加载专属元数据
	switch (dataType)
	{
	case ImportDataType::Vector:
		specificFields = parseMetadataCSV(metadataDir() + "/矢量数据元数据.csv");
		break;
	case ImportDataType::Raster:
		specificFields = parseMetadataCSV(metadataDir() + "/栅格数据元数据.csv");
		break;
	case ImportDataType::Other:
		specificFields.append(parseMetadataCSV(metadataDir() + "/制图文件元数据.csv"));
		break;
	case ImportDataType::All:
		// 其它类型文件（文档、压缩包等）使用文档数据元数据
		specificFields = parseMetadataCSV(metadataDir() + "/文档数据元数据.csv");
		break;
	}

	// 3. 填入表单
	addFieldsToLayout(basicFields, mBasicMetaFormLayout);
	addFieldsToLayout(specificFields, mSpecificMetaFormLayout);

	// 显示/隐藏专有元数据分组
	mSpecificMetaGroupBox->setVisible(!specificFields.isEmpty());

	QVector<MetadataFieldDef> allFields;
	allFields.append(basicFields);
	allFields.append(specificFields);
	mCurrentFields = allFields;

	// 刷新入库按钮状态
	updateImportButton();

	QgsMessageLog::logMessage(
		QStringLiteral("[导入向导] 加载元数据字段 基础=%1 专有=%2 合计=%3")
			.arg(basicFields.size()).arg(specificFields.size()).arg(allFields.size()),
		"MapProductTools", Qgis::Info);
}

// ============================================================================
// 页面导航
// ============================================================================

void DataImportWizard::onNextPage()
{
	if (mStackedWidget->currentIndex() == 0)
	{
		// 从类型页 → 导入页：重新加载对应元数据
		loadMetadataFields(selectedDataType());
		mStackedWidget->setCurrentIndex(1);
		mBtnBack->setVisible(true);
		mBtnNext->setVisible(false);
	}
}

void DataImportWizard::onPrevPage()
{
	if (mStackedWidget->currentIndex() == 1)
	{
		mStackedWidget->setCurrentIndex(0);
		mBtnBack->setVisible(false);
		mBtnNext->setVisible(true);
	}
}

// ============================================================================
// 辅助函数
// ============================================================================

ImportDataType DataImportWizard::selectedDataType() const
{
	int id = mTypeGroup->checkedId();
	if (id >= 0 && id <= 3)
		return static_cast<ImportDataType>(id);
	return ImportDataType::Vector;
}

QString DataImportWizard::dataTypeFilter() const
{
	switch (selectedDataType())
	{
	case ImportDataType::Vector:
		return QString::fromUtf8("矢量文件 (*.shp *.geojson *.kml *.gdb *.mdb);;其它类型文件 (*.*)");
	case ImportDataType::Raster:
		return QString::fromUtf8("栅格文件 (*.tif *.tiff *.img *.jpg *.jp2);;其它类型文件 (*.*)");
	case ImportDataType::Other:
		return QString::fromUtf8("制图文件 (*.pdf *.ai *.cdr *.dwg *.dxf);;其它类型文件 (*.*)");
	case ImportDataType::All:
		return QString::fromUtf8(
			"其它类型文件 (*.doc *.docx *.xls *.xlsx *.xml *.txt *.csv *.zip *.rar *.7z);;"
			"所有文件 (*.*)");
	}
	return QString();
}

void DataImportWizard::updateImportButton()
{
	bool hasDir = !mDirEdit->text().trimmed().isEmpty();
	mBtnImport->setEnabled(hasDir);
}

void DataImportWizard::onBrowseDir()
{
	// 使用 QFileDialog 选择目录
	QString dir = QFileDialog::getExistingDirectory(this,
		QString::fromUtf8("选择数据所在目录"),
		mDirEdit->text().isEmpty() ? QDir::homePath() : mDirEdit->text(),
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (!dir.isEmpty())
	{
		mDirEdit->setText(QDir::toNativeSeparators(dir));

		// 扫描目录文件
		QStringList files = scanFiles(dir);

		if (files.isEmpty())
		{
			mDirInfoLabel->setText(
				QString::fromUtf8("未找到匹配该类型的文件"));
		}
		else
		{
			mDirInfoLabel->setText(
				QString::fromUtf8("扫描到 %1 个文件").arg(files.size()));
		}

		updateImportButton();
	}
}

QStringList DataImportWizard::scanFiles(const QString& dirPath) const
{
	QStringList exts;
	switch (selectedDataType())
	{
	case ImportDataType::Vector:
		exts << "*.shp" << "*.geojson" << "*.kml" << "*.gdb" << "*.mdb";
		break;
	case ImportDataType::Raster:
		exts << "*.tif" << "*.tiff" << "*.img" << "*.jpg" << "*.jp2";
		break;
	case ImportDataType::Other:
		exts << "*.pdf" << "*.ai" << "*.cdr" << "*.dwg" << "*.dxf";
		break;
	case ImportDataType::All:
		exts << "*.doc" << "*.docx" << "*.xls" << "*.xlsx" << "*.ppt" << "*.pptx"
		     << "*.xml" << "*.txt" << "*.csv" << "*.json" << "*.rtf" << "*.odt" << "*.ods" << "*.odp"
		     << "*.zip" << "*.rar" << "*.7z" << "*.tar" << "*.gz" << "*.bz2"
		     << "*.*";  // 最后兜底，覆盖所有无法识别的格式
		break;
	}

	QDir dir(dirPath);
	QStringList files;
	for (const auto& ext : exts)
	{
		QStringList found = dir.entryList(QStringList() << ext, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
		for (const auto& f : found)
			files.append(dir.absoluteFilePath(f));
	}
	files.removeDuplicates();  // 防止 *.* 等通配与具体扩展名重复匹配

	// 如果目录内未扫描到文件，检查所选目录本身是否就是一个数据源
	// 例如 .gdb、.mdb 本身就是目录格式的空间数据，直接作为导入目标
	if (files.isEmpty())
	{
		QString dirSuffix = QFileInfo(dirPath).suffix().toLower();
		if (!dirSuffix.isEmpty())
		{
			QString dirPattern = "*." + dirSuffix;
			if (exts.contains(dirPattern, Qt::CaseInsensitive))
				files.append(dirPath);
		}
	}
	return files;
}

// ============================================================================
// 入库执行
// ============================================================================

void DataImportWizard::onStartImport()
{
	QString dirPath = mDirEdit->text().trimmed();
	if (dirPath.isEmpty())
	{
		QMessageBox::warning(this, QString::fromUtf8("提示"),
			QString::fromUtf8("请先选择数据目录。"));
		return;
	}

	QDir dir(dirPath);
	if (!dir.exists())
	{
		QMessageBox::warning(this, QString::fromUtf8("提示"),
			QString::fromUtf8("所选目录不存在。"));
		return;
	}

	auto* db = PostgisConnector::instance();
	if (!db->isConnected())
	{
		QMessageBox::critical(this, QString::fromUtf8("错误"),
			QString::fromUtf8("数据库未连接，请先配置数据库连接。"));
		return;
	}

	QStringList files = scanFiles(dirPath);
	if (files.isEmpty())
	{
		QMessageBox::information(this, QString::fromUtf8("提示"),
			QString::fromUtf8("未找到匹配的文件。"));
		return;
	}

	QgsMessageLog::logMessage(
		QStringLiteral("[导入向导] 开始入库，文件数=%1 目录=%2")
			.arg(files.size()).arg(dirPath),
		"MapProductTools", Qgis::Info);

	// 清空日志
	mLogTextEdit->clear();
	appendLog(QString::fromUtf8("══════════════════════════════════"));
	appendLog(QString::fromUtf8("  数据导入开始  —  %1 个文件").arg(files.size()));
	appendLog(QString::fromUtf8("══════════════════════════════════"));

	// 禁用 UI
	mBtnImport->setEnabled(false);
	mBtnBrowse->setEnabled(false);
	mBtnBack->setEnabled(false);
	mDirEdit->setEnabled(false);

	// 显示进度
	mProgressBar->setVisible(true);
	mProgressBar->setValue(0);
	mProgressLabel->setVisible(true);

	ImportDataType dt = selectedDataType();

	// 地图成果固定目录上下文
	mDirIds = DirectoryHelper::ensureFixedDirectories(mDirDao);
	mImportUsedDirs.clear();
	mImportPopulatedDirs.clear();
	mIsSingleVectorSource = false;
	if (dt == ImportDataType::Vector)
	{
		// 统计目录下独立矢量数据源（shp/geojson/kml 单文件）数量
		// 用于区分"单个矢量文件"（用文件名建节点）与"矢量文件夹"（用目录名建节点）
		QDir srcDir(dirPath);
		int shpCount = srcDir.entryList(QStringList() << "*.shp" << "*.geojson" << "*.kml",
			QDir::Files | QDir::NoDotAndDotDot).size();
		mIsSingleVectorSource = (shpCount == 1);
	}

	// 构建目标表名前缀（用于安全生成 PostGIS 表名）
	auto safeTableName = [](const QString& fileName) -> QString {
		QFileInfo fi(fileName);
		QString base = fi.completeBaseName().toLower();
		base.replace(QRegularExpression("[^a-z0-9_\\x{4e00}-\\x{9fff}]"), "_");
		if (base.isEmpty()) base = "imported";
		if (base.at(0).isDigit()) base.prepend("t_");
		return base;
	};

	// 汇集元数据
	QVariantMap metaValues;
	for (auto it = mMetadataLineEdits.constBegin(); it != mMetadataLineEdits.constEnd(); ++it)
	{
		QString val = it.value()->text().trimmed();
		if (!val.isEmpty())
			metaValues[it.key()] = val;
	}

	// 将表单元数据字段映射到 ProductMetadata
	auto applyFormMetadata = [&](ProductMetadata& meta) {
		if (metaValues.contains("product_name"))
			meta.productName = metaValues["product_name"].toString();
		if (metaValues.contains("description"))
			meta.description = metaValues["description"].toString();
		if (metaValues.contains("source"))
			meta.source = metaValues["source"].toString();
		if (metaValues.contains("version_note"))
			meta.versionNote = metaValues["version_note"].toString();
		if (metaValues.contains("security_level"))
			meta.securityLevel = stringToSecurityLevel(metaValues["security_level"].toString());
		if (metaValues.contains("tags"))
			meta.tags = metaValues["tags"].toString();
		if (metaValues.contains("city"))
			meta.city = metaValues["city"].toString();
		if (metaValues.contains("scale"))
			meta.scale = metaValues["scale"].toString();
		if (metaValues.contains("project_name"))
			meta.projectName = metaValues["project_name"].toString();
		if (metaValues.contains("producer"))
			meta.producer = metaValues["producer"].toString();
		if (metaValues.contains("production_date"))
			meta.productionDate = QDate::fromString(metaValues["production_date"].toString(), Qt::ISODate);
		if (metaValues.contains("crs"))
			meta.crs = metaValues["crs"].toString();
		if (metaValues.contains("compilation_info"))
			meta.compilationInfo = metaValues["compilation_info"].toString();
		if (metaValues.contains("approval_number"))
			meta.approvalNumber = metaValues["approval_number"].toString();
		if (metaValues.contains("delivery_status"))
			meta.deliveryStatus = metaValues["delivery_status"].toString();
		if (metaValues.contains("delivery_time"))
			meta.deliveryTime = QDateTime::fromString(metaValues["delivery_time"].toString(), Qt::ISODate);
	};

	// 统计
	int importedCount = 0;
	int skippedCount = 0;
	int versionUpCount = 0;
	int failedCount = 0;

	// 逐文件导入
	for (int i = 0; i < files.size(); ++i)
	{
		const QString& filePath = files[i];
		QFileInfo fi(filePath);
		QString fileName = fi.fileName();
		QString ext = fi.suffix().toLower();

		// ── 目录挂载上下文（地图成果固定目录树）──
		// 计算该文件应挂载的目录节点；版本更新时保留原目录
		int importDirId = DirectoryHelper::resolveDirectoryIdByExt(ext, false,
			DirectoryHelper::RasterSubcategory::Image, mDirIds);

		if (dt == ImportDataType::Vector)
		{
			// gdb / mdb：在要素集下建以"gdb/mdb 文件名"命名的子节点，图层挂其下
			if (ext == "gdb" || ext == "mdb")
			{
				QString base = fi.completeBaseName().isEmpty() ? fileName : fi.completeBaseName();
				importDirId = DirectoryHelper::createNamedChildDirectory(
					mDirDao, mDirIds.elementFeature, base);
				if (importDirId > 0) mImportUsedDirs.insert(importDirId);
			}
			else
			{
				// 单个 shp/geojson/kml 源用文件名建子节点；否则用目录名建子节点（矢量文件夹）
				QString containerName = mIsSingleVectorSource ? fileName : QDir(dirPath).dirName();
				// 单源文件时该节点即图层节点，标记为图层类型（矢量图层图标）
				importDirId = DirectoryHelper::createNamedChildDirectory(
					mDirDao, mDirIds.elementFeature, containerName, mIsSingleVectorSource ? 1 : 0);
				if (importDirId > 0) mImportUsedDirs.insert(importDirId);
			}
		}
		else if (dt == ImportDataType::Raster)
		{
			DirectoryHelper::RasterSubcategory sub =
				(mRasterSubcategoryCombo && mRasterSubcategoryCombo->currentIndex() == 1)
					? DirectoryHelper::RasterSubcategory::Shading
					: DirectoryHelper::RasterSubcategory::Image;
			importDirId = DirectoryHelper::resolveDirectoryIdByExt(ext, true, sub, mDirIds);
		}

		int percent = static_cast<int>((static_cast<double>(i) / files.size()) * 100.0);
		mProgressBar->setValue(percent);
		mProgressLabel->setText(
			QString::fromUtf8("正在处理 %1/%2: %3").arg(i + 1).arg(files.size()).arg(fileName));
		QApplication::processEvents();

		appendLog(QString::fromUtf8("──────────────────────────────────"));
		appendLog(QString::fromUtf8("[%1/%2] %3").arg(i + 1).arg(files.size()).arg(fileName), "#9fd8e6");

		// ── 1. 自动提取文件元数据 ──
		MetadataExtractor extractor;
		ProductMetadata autoMeta = extractor.extractMetadata(filePath);
		QString fileHash = autoMeta.fileHash;

		// 确保 hash 已计算（部分文件类型 extractMetadata 可能不计算）
		if (fileHash.isEmpty())
		{
			QFile f(filePath);
			if (f.open(QIODevice::ReadOnly))
			{
				QCryptographicHash hash(QCryptographicHash::Sha256);
				hash.addData(&f);
				f.close();
				fileHash = hash.result().toHex();
			}
		}

		appendLog(QString::fromUtf8("  SHA256: %1").arg(fileHash.left(16) + "..."), "#8aa4b5");

		// ── 2. 检测文件大小 ──
		qint64 fileSize = fi.size();
		appendLog(QString::fromUtf8("  大小: %1 KB").arg(fileSize / 1024), "#8aa4b5");

		// ── 3. 去重检查：查找数据库中同名产品 ──
		bool isNewProduct = true;   // 是否为新入库产品
		int  targetPid      = 0;    // 目标产品 ID
		ProductMetadata existing;

		{
			existing = findExistingProductByName(fileName);
			if (!existing.dataId.isEmpty())
			{
				if (!existing.fileHash.isEmpty() && !fileHash.isEmpty() && existing.fileHash == fileHash)
				{
					// 哈希相同 → 跳过
					appendLog(QString::fromUtf8("  ⏭ 跳过: 文件哈希与已入库记录一致 (id=%1)").arg(existing.id), "#e67e22");
					skippedCount++;
					continue;
				}
				else if (!existing.fileHash.isEmpty() && existing.fileHash != fileHash)
				{
					// 哈希不同 → 更新已有记录（updateProduct 自动版本 +1）
					appendLog(QString::fromUtf8("  ⬆ 版本更新: 检测到内容变化 (id=%1)").arg(existing.id), "#3498db");
					isNewProduct = false;
					targetPid    = existing.id;
				}
			}
		}

		// ── 4. 确定目标表名（含版本后缀 _v{N}）──
		int importVersion = isNewProduct ? 1 : (existing.currentVersion + 1);
		QString targetTable = safeTableName(fileName) + "_v" + QString::number(importVersion);

		// ── 5. 按类型导入 ──
	if (dt == ImportDataType::Vector)
	{
			// ── 5a. GDB / MDB：多图层数据集，哈希去重 + 每图层一条产品记录 ──
			if (ext == "gdb" || ext == "mdb")
			{
				// 哈希去重：GDB/MDB 按图层拆分为多条记录，用文件哈希精确定位
				if (!fileHash.isEmpty())
				{
					ProductDAO daoCheck;
					ProductMetadata hashExisting = daoCheck.findByHash(fileHash);
					if (!hashExisting.dataId.isEmpty())
					{
						appendLog(QString::fromUtf8("  ⏭ 跳过: %1 文件哈希与已入库记录一致 (原记录 id=%2)")
							.arg(ext.toUpper()).arg(hashExisting.id), "#e67e22");
						skippedCount++;
						continue;
					}
				}

				int count = mImporter->importVectorToPostGIS(filePath, targetTable, 0, 4490, "UTF-8");
				if (count < 0)
				{
					appendLog(QString::fromUtf8("  ✗ %1 导入失败: %2").arg(ext.toUpper()).arg(mImporter->lastError()), "#e74c3c");
					failedCount++;
				}
				else
				{
					appendLog(QString::fromUtf8("  ✓ %1 导入成功 (%2 个要素)").arg(ext.toUpper()).arg(count), "#27ae60");

					// 重新打开 GDB/MDB，枚举图层，为每个图层创建独立产品记录
					GDALDatasetUniquePtr poDS(GDALDataset::Open(filePath.toUtf8().constData(), GDAL_OF_VECTOR));
					if (poDS)
					{
						ProductDAO dao;
						int layerCount = poDS->GetLayerCount();
						appendLog(QString::fromUtf8("  检测到 %1 个图层").arg(layerCount), "#8aa4b5");

						for (int iLayer = 0; iLayer < layerCount; ++iLayer)
						{
							OGRLayer* poLayer = poDS->GetLayer(iLayer);
							QString layerName = QString::fromUtf8(poLayer->GetName());
							QString safeLayer = layerName.toLower();
							safeLayer.replace(QRegularExpression("[^a-z0-9_\\x{4e00}-\\x{9fff}]"), "_");
							QString layerTableName = targetTable + "_" + safeLayer;

							// 几何类型
							QString geomType = QString::fromUtf8("Geometry");
							OGRFeatureDefn* defn = poLayer->GetLayerDefn();
							if (defn && defn->GetGeomFieldCount() > 0)
							{
								OGRGeomFieldDefn* gfd = defn->GetGeomFieldDefn(0);
								if (gfd)
									geomType = QString::fromUtf8(OGRGeometryTypeToName(gfd->GetType()));
							}

							int featCount = poLayer->GetFeatureCount();

							ProductMetadata vecMeta = autoMeta;
							vecMeta.productName = layerTableName;       // 例如 "testgdb_roads"
							vecMeta.productType = ProductType::Vector;
							vecMeta.fileFormat = ext;
							vecMeta.filePath = filePath;
							vecMeta.fileSize = fileSize;
							vecMeta.fileHash = fileHash;
							vecMeta.layerTableName = layerTableName;
							vecMeta.geometryType = geomType;
							applyFormMetadata(vecMeta);
							vecMeta.createdBy = "postgres";
							vecMeta.updatedBy = "postgres";
							vecMeta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
							vecMeta.currentVersion = 1;
							// gdb/mdb：在 gdb 名节点下为每个图层建"图层名"子节点，产品挂其下
							int layerDirId = DirectoryHelper::createNamedChildDirectory(mDirDao, importDirId, layerName, 1);
							if (layerDirId > 0)
							{
								mImportUsedDirs.insert(layerDirId);
								vecMeta.parentDirId = layerDirId;
							}
							else
							{
								vecMeta.parentDirId = importDirId;
							}

							int pid = dao.insertProduct(vecMeta);
							if (pid > 0)
							{
								if (vecMeta.parentDirId > 0) mImportPopulatedDirs.insert(vecMeta.parentDirId);
								dao.registerLayer(pid, layerTableName, geomType, 4490,
									featCount >= 0 ? featCount : 0);
								dao.enrichWithTypeMeta(pid);
								importedCount++;
								appendLog(QString::fromUtf8("  ✓ 图层 [%1] → %2 (id=%3, %4 要素)")
									.arg(layerName).arg(layerTableName).arg(pid)
									.arg(featCount >= 0 ? QString::number(featCount) : QString::fromUtf8("未知")), "#27ae60");
							}
							else
							{
								appendLog(QString::fromUtf8("  ✗ 图层 [%1] 元数据写入失败").arg(layerName), "#e74c3c");
								failedCount++;
							}
						}
					}
					else
					{
						// 无法重新打开 GDB → 回退为单条记录
						appendLog(QString::fromUtf8("  ⚠ 无法枚举图层，回退为单条产品记录"), "#e67e22");

						ProductMetadata vecMeta = autoMeta;
						vecMeta.productName = fileName;
						vecMeta.productType = ProductType::Vector;
						vecMeta.fileFormat = ext;
						vecMeta.filePath = filePath;
						vecMeta.fileSize = fileSize;
						vecMeta.fileHash = fileHash;
						vecMeta.layerTableName = targetTable;
						applyFormMetadata(vecMeta);
						vecMeta.createdBy = "postgres";
						vecMeta.updatedBy = "postgres";
						vecMeta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
						vecMeta.currentVersion = 1;
						vecMeta.parentDirId = importDirId;

						ProductDAO dao;
						int pid = dao.insertProduct(vecMeta);
						if (pid > 0)
						{
							if (vecMeta.parentDirId > 0) mImportPopulatedDirs.insert(vecMeta.parentDirId);
							dao.registerLayer(pid, targetTable, vecMeta.geometryType, 4490, count);
							dao.enrichWithTypeMeta(pid);
							importedCount++;
							appendLog(QString::fromUtf8("  ✓ 新纪录写入 (id=%1)").arg(pid), "#27ae60");
						}
						else
						{
							appendLog(QString::fromUtf8("  ✗ 元数据写入失败"), "#e74c3c");
							failedCount++;
						}
					}
				}
				continue;
			}

			// ── 5b. 单层矢量文件（SHP / GeoJSON / KML） ──
			if (ext == "shp" || ext == "geojson" || ext == "kml")
			{
				int count = mImporter->importVectorToPostGIS(filePath, targetTable, 0, 4490, "UTF-8");
				if (count < 0)
				{
					appendLog(QString::fromUtf8("  ✗ 矢量导入失败: %1").arg(mImporter->lastError()), "#e74c3c");
					failedCount++;
				}
				else
				{
					appendLog(QString::fromUtf8("  ✓ 矢量导入成功 (%1 个要素)").arg(count), "#27ae60");

					ProductMetadata vecMeta = autoMeta;
					vecMeta.productName = fileName;
					vecMeta.productType = ProductType::Vector;
					vecMeta.fileFormat = ext;
					vecMeta.filePath = filePath;
					vecMeta.fileSize = fileSize;
					vecMeta.fileHash = fileHash;
					vecMeta.layerTableName = targetTable;
					applyFormMetadata(vecMeta);
					vecMeta.createdBy = "postgres";
					vecMeta.updatedBy = "postgres";
					vecMeta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);

					ProductDAO dao;
					bool ok = false;
					int  pid = 0;

					if (isNewProduct)
					{
						vecMeta.currentVersion = 1;
						// 单层矢量：若是文件夹场景（非单源），在文件夹名节点下为每个文件(图层)建子节点；
						// 单源文件时 importDirId 已是文件名节点，无需再嵌套。
						int effectiveDirId = importDirId;
						if (!mIsSingleVectorSource)
						{
							int layerDirId = DirectoryHelper::createNamedChildDirectory(mDirDao, importDirId, fileName, 1);
							if (layerDirId > 0)
							{
								mImportUsedDirs.insert(layerDirId);
								effectiveDirId = layerDirId;
							}
						}
						vecMeta.parentDirId = effectiveDirId;
						pid = dao.insertProduct(vecMeta);
						ok = (pid > 0);
						if (ok)
						{
							if (vecMeta.parentDirId > 0) mImportPopulatedDirs.insert(vecMeta.parentDirId);
							appendLog(QString::fromUtf8("  ✓ 新纪录写入 (id=%1)").arg(pid), "#27ae60");
						}
					}
					else
					{
						vecMeta.id = targetPid;
						ok = dao.updateProduct(vecMeta);
						pid = targetPid;
						if (ok)
						{
							versionUpCount++;
							appendLog(QString::fromUtf8("  ✓ 产品更新 (id=%1, 版本已自增)").arg(pid), "#27ae60");
						}
					}

					if (ok)
					{
						// 刷新 layer_registry（使用 UPSERT 方式已有或新建）
						dao.registerLayer(pid, targetTable, vecMeta.geometryType, 4490, count);
						dao.enrichWithTypeMeta(pid);
						importedCount++;
					}
					else
					{
						appendLog(QString::fromUtf8("  ✗ 元数据写入失败"), "#e74c3c");
						failedCount++;
					}
				}
				continue;
			}
		}

	if (dt == ImportDataType::Raster)
	{
			if (ext == "tif" || ext == "tiff" || ext == "img" || ext == "jpg" || ext == "jp2")
			{
				bool impOk = mImporter->importRasterToPostGIS(filePath, targetTable, 4490);
				if (!impOk)
				{
					appendLog(QString::fromUtf8("  ✗ 栅格导入失败: %1").arg(mImporter->lastError()), "#e74c3c");
					failedCount++;
				}
				else
				{
					appendLog(QString::fromUtf8("  ✓ 栅格导入成功"), "#27ae60");

					ProductMetadata rasMeta = autoMeta;
					rasMeta.productName = fileName;
					rasMeta.productType = ProductType::Raster;
					rasMeta.fileFormat = ext;
					rasMeta.filePath = filePath;
					rasMeta.fileSize = fileSize;
					rasMeta.fileHash = fileHash;
					rasMeta.layerTableName = targetTable;
					applyFormMetadata(rasMeta);
					rasMeta.createdBy = "postgres";
					rasMeta.updatedBy = "postgres";
					rasMeta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);

					ProductDAO dao;
					bool ok = false;
					int  pid = 0;

					if (isNewProduct)
					{
						rasMeta.currentVersion = 1;
						rasMeta.parentDirId = importDirId;
						pid = dao.insertProduct(rasMeta);
						ok = (pid > 0);
						if (ok)
						{
							if (rasMeta.parentDirId > 0) mImportPopulatedDirs.insert(rasMeta.parentDirId);
							appendLog(QString::fromUtf8("  ✓ 新纪录写入 (id=%1)").arg(pid), "#27ae60");
						}
					}
					else
					{
						rasMeta.id = targetPid;
						ok = dao.updateProduct(rasMeta);
						pid = targetPid;
						if (ok)
						{
							versionUpCount++;
							appendLog(QString::fromUtf8("  ✓ 产品更新 (id=%1, 版本已自增)").arg(pid), "#27ae60");
						}
					}

					if (ok)
					{
						dao.registerLayer(pid, targetTable, "Raster", 4490, 0);
						dao.enrichWithTypeMeta(pid);
						importedCount++;
					}
					else
					{
						appendLog(QString::fromUtf8("  ✗ 元数据写入失败"), "#e74c3c");
						failedCount++;
					}
				}
				continue;
			}
		}

	if (dt == ImportDataType::Other)
	{
			if (ext == "pdf" || ext == "ai" || ext == "cdr" || ext == "dwg" || ext == "dxf")
			{
				ProductMetadata meta = autoMeta;
				meta.productName = fileName;
				// 根据扩展名设置正确的产品类型
				if (ext == "ai")
					meta.productType = ProductType::AI;
				else if (ext == "cdr")
					meta.productType = ProductType::CDR;
				else if (ext == "dwg" || ext == "dxf")
					meta.productType = ProductType::CAD;
				else if (ext == "pdf")
					meta.productType = ProductType::PDF;
				else
					meta.productType = ProductType::Other;
				meta.fileFormat = ext;
				meta.fileSize = fileSize;
				meta.fileHash = fileHash;
				applyFormMetadata(meta);
				meta.createdBy = "postgres";
				meta.updatedBy = "postgres";
				meta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
				// 确定版本号
				int ver = isNewProduct ? 1 : (meta.currentVersion > 0 ? meta.currentVersion : 1);
				meta.currentVersion = ver;

				// 直接上传到 PostgreSQL BLOB（无需本地 product_storage 双写）
				meta.filePath = filePath;
				int oid = PostgisConnector::instance()->loImport(meta.filePath);
				if (oid > 0)
				{
					meta.fileOid = oid;
				}
				else
				{
					appendLog(QString::fromUtf8("  ✗ BLOB上传失败(其他机器将无法导出): %1")
						.arg(PostgisConnector::instance()->lastError()), "#e74c3c");
				}

				ProductDAO dao;
				bool ok = false;
				int  pid = 0;

				if (isNewProduct)
				{
					meta.parentDirId = importDirId;
					pid = dao.insertProduct(meta);
					ok = (pid > 0);
					if (ok)
					{
						if (meta.parentDirId > 0) mImportPopulatedDirs.insert(meta.parentDirId);
						appendLog(QString::fromUtf8("  ✓ 制图文件注册成功 (id=%1)").arg(pid), "#27ae60");
					}
				}
				else
				{
					meta.id = targetPid;
					ok = dao.updateProduct(meta);
					pid = targetPid;
					if (ok)
					{
						versionUpCount++;
						appendLog(QString::fromUtf8("  ✓ 制图文件更新 (id=%1, 版本已自增)").arg(pid), "#27ae60");
					}
				}

				if (ok)
				{
					dao.enrichWithTypeMeta(pid);
					importedCount++;
				}
				else
				{
					appendLog(QString::fromUtf8("  ✗ 制图文件注册失败"), "#e74c3c");
					failedCount++;
				}
				continue;
			}
		}

		if (dt == ImportDataType::All)
		{
			// 其它类型文件：文档(doc/xlsx/xml等)、压缩包(zip/rar/7z等)、无法识别格式
			// 保留 autoMeta 自动检测的类型（.ai→AI, .docx→Document, .zip→Archive 等），其余归为 Other
			ProductMetadata meta = autoMeta;
			meta.productName = fileName;
			ProductType autoType = MetadataExtractor::detectProductType(filePath);
			meta.productType = (autoType == ProductType::Vector || autoType == ProductType::Raster)
				? ProductType::Other : autoType;
			meta.fileFormat = ext;
			meta.fileSize = fileSize;
			meta.fileHash = fileHash;
			applyFormMetadata(meta);
			meta.createdBy = "postgres";
			meta.updatedBy = "postgres";
			meta.dataId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			int ver = isNewProduct ? 1 : (meta.currentVersion > 0 ? meta.currentVersion : 1);
			meta.currentVersion = ver;

			// 直接上传到 PostgreSQL BLOB（无需本地 product_storage 双写）
			meta.filePath = filePath;
			int oid = PostgisConnector::instance()->loImport(meta.filePath);
			if (oid > 0)
			{
				meta.fileOid = oid;
			}
			else
			{
				appendLog(QString::fromUtf8("  ✗ BLOB上传失败(其他机器将无法导出): %1")
					.arg(PostgisConnector::instance()->lastError()), "#e74c3c");
			}

			ProductDAO dao;
			bool ok = false;
			int  pid = 0;

			if (isNewProduct)
			{
				meta.parentDirId = importDirId;
				pid = dao.insertProduct(meta);
				ok = (pid > 0);
				if (ok)
				{
					if (meta.parentDirId > 0) mImportPopulatedDirs.insert(meta.parentDirId);
					appendLog(QString::fromUtf8("  ✓ 其它文件注册成功 (id=%1)").arg(pid), "#27ae60");
				}
			}
			else
			{
				meta.id = targetPid;
				ok = dao.updateProduct(meta);
				pid = targetPid;
				if (ok)
				{
					versionUpCount++;
					appendLog(QString::fromUtf8("  ✓ 其它文件更新 (id=%1, 版本已自增)").arg(pid), "#27ae60");
				}
			}

			if (ok)
			{
				dao.enrichWithTypeMeta(pid);
				importedCount++;
			}
			else
			{
				appendLog(QString::fromUtf8("  ✗ 其它文件注册失败"), "#e74c3c");
				failedCount++;
			}
			continue;
		}
	}

	// ── 清理本次导入产生的空目录节点（入库失败时不保留节点）──
	// 仅清理"动态命名子节点"中既无产品也无子目录的空节点；固定的地图成果目录始终保留。
	int removedDirs = DirectoryHelper::cleanupEmptyCreatedDirs(
		mDirDao, mImportUsedDirs, mImportPopulatedDirs,
		[this](const QString& msg) { appendLog(msg, "#e67e22"); });

	// ── 完成 ──
	mProgressBar->setValue(100);
	mProgressLabel->setText(QString::fromUtf8("入库完成"));

	appendLog("");
	appendLog(QString::fromUtf8("══════════════════════════════════"));
	appendLog(QString::fromUtf8("  导入完成汇总"), "#27ae60");
	appendLog(QString::fromUtf8("══════════════════════════════════"));
	appendLog(QString::fromUtf8("  新入库: %1  |  跳过(哈希相同): %2  |  版本自增: %3  |  失败: %4")
		.arg(importedCount).arg(skippedCount).arg(versionUpCount).arg(failedCount), "#9fd8e6");
	if (removedDirs > 0)
		appendLog(QString::fromUtf8("  🗑 已清理 %1 个空目录节点（入库失败，未保留目录）").arg(removedDirs), "#e67e22");

	if (failedCount > 0)
		appendLog(QString::fromUtf8("  ⚠ 有 %1 个文件入库失败，请检查日志").arg(failedCount), "#e74c3c");

	// 恢复 UI
	mBtnImport->setEnabled(true);
	mBtnBrowse->setEnabled(true);
	mBtnBack->setEnabled(true);
	mDirEdit->setEnabled(true);

	QString summary = QString::fromUtf8(
		"数据导入完成！\n\n"
		"  新入库: %1 个\n"
		"  跳过(哈希相同): %2 个\n"
		"  版本自增: %3 个\n"
		"  失败: %4 个\n\n"
		"详细信息请查看入库日志窗口。")
		.arg(importedCount).arg(skippedCount).arg(versionUpCount).arg(failedCount);
	QMessageBox::information(this, QString::fromUtf8("入库完成"), summary);
}

// ============================================================================
// 入库过程中的进度回调（实时更新）
// ============================================================================

void DataImportWizard::onImportProgress(int percent, const QString& message)
{
	mProgressBar->setValue(percent);
	if (!message.isEmpty())
		mProgressLabel->setText(message);
}

void DataImportWizard::onImportFinished(const QString& tableName, int featureCount)
{
	QgsMessageLog::logMessage(
		QStringLiteral("[导入向导] 导入完成: 表=%1 要素数=%2")
			.arg(tableName).arg(featureCount),
		"MapProductTools", Qgis::Info);
}

void DataImportWizard::onImportFailed(const QString& tableName, const QString& error)
{
	QgsMessageLog::logMessage(
		QStringLiteral("[导入向导] 导入失败: 表=%1 错误=%2")
			.arg(tableName, error),
		"MapProductTools", Qgis::Critical);
}

// ============================================================================
// 入库日志
// ============================================================================

void DataImportWizard::appendLog(const QString& message, const QString& color)
{
	Q_UNUSED(color);
	QString timeStamp = QDateTime::currentDateTime().toString("hh:mm:ss");
	QString html = QString("[%1] %2").arg(timeStamp, message.toHtmlEscaped());
	mLogTextEdit->appendHtml(html);
	// 自动滚动到底部
	QTextCursor cursor = mLogTextEdit->textCursor();
	cursor.movePosition(QTextCursor::End);
	mLogTextEdit->setTextCursor(cursor);
	QApplication::processEvents();
}

void DataImportWizard::onToggleLog()
{
	mLogExpanded = !mLogExpanded;
	mLogTextEdit->setVisible(mLogExpanded);
	if (mLogExpanded)
	{
		mBtnToggleLog->setText(QString::fromUtf8("▼ 入库日志"));
		mBtnToggleLog->setToolTip(QString::fromUtf8("点击收起入库日志"));
		mLogGroupBox->setMaximumHeight(300);
	}
	else
	{
		mBtnToggleLog->setText(QString::fromUtf8("▶ 入库日志"));
		mBtnToggleLog->setToolTip(QString::fromUtf8("点击展开入库日志"));
		mLogGroupBox->setMaximumHeight(40);
	}
}

ProductMetadata DataImportWizard::findExistingProductByName(const QString& productName)
{
	ProductMetadata empty;
	empty.dataId.clear();

	ProductDAO dao;
	ProductDAO::SearchCondition cond;
	cond.keyword = productName;
	cond.productType = ProductType::Other;  // 不限类型
	cond.limit = 5;

	auto results = dao.searchProducts(cond);
	for (const auto& p : results)
	{
		// 精确匹配产品名称（忽略大小写）
		if (p.productName.compare(productName, Qt::CaseInsensitive) == 0)
		{
			return p;
		}
	}
	return empty;
}
