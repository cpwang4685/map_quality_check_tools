/*--------------QT---------------*/
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QRadioButton>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <QListWidget>
#include <QSpacerItem>
#include <QFont>
#include <QApplication>
#include <QDesktopWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QSet>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QProcessEnvironment>
#include <QTextStream>

/*--------------XML parsing---------------*/
#include <QDomDocument>
#include <QDomElement>

/*--------------QGIS---------------*/
#include "qgsproject.h"
#include "qgsmaplayer.h"
#include "qgsmessagelog.h"
#include "qgsvectorlayer.h"
#include "qgsdatasourceuri.h"
#include "qgisinterface.h"

/*--------------SE---------------*/
#include "se_data_import.h"
#include "se_metadata_viewer.h"

#include "ui_fit_helper.h"

// ====================================================================
//  构造 & 析构
// ====================================================================

CSE_DataImportDialog::CSE_DataImportDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , m_pProcess(nullptr)
    , m_iTotalItems(0)
    , m_iSuccessCount(0)
    , m_iFailCount(0)
    , m_bCancelBatchImport(false)
    , m_pGrpGdbLayers(nullptr)
    , m_pBtnMetadataViewer(nullptr)
{
    InitUI();
    DialogFitHelper::install(this);
    LoadSettings();
}

CSE_DataImportDialog::~CSE_DataImportDialog()
{
    SaveSettings();
    if (m_pProcess && m_pProcess->state() != QProcess::NotRunning)
    {
        m_pProcess->kill();
        m_pProcess->waitForFinished(3000);
    }
}

// ====================================================================
//  UI 初始化 (纯代码构建)
// ====================================================================

void CSE_DataImportDialog::InitUI()
{
    // ---- 窗口属性 ----
    this->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowTitleHint);
    this->setWindowTitle(tr("导入数据到PostGIS"));
    this->setMinimumSize(900, 780);
    this->resize(960, 820);

    // 居中
    QRect screenRect = QApplication::desktop()->availableGeometry();
    this->move(screenRect.center().x() - 480, screenRect.center().y() - 410);

    // 主布局
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(12, 12, 12, 12);

    // ================================================================
    //  第一部分：数据库连接
    // ================================================================
    QGroupBox* grpConn = new QGroupBox(tr("数据库连接"), this);
    QGridLayout* connLayout = new QGridLayout(grpConn);
    connLayout->setSpacing(8);

    // 主机
    m_pLabelHost = new QLabel(tr("主机:"), grpConn);
    m_pLabelHost->setFixedWidth(70);
    m_pEditHost = new QLineEdit(grpConn);
    m_pEditHost->setPlaceholderText("localhost");
    m_pEditHost->setMinimumWidth(190);
    connLayout->addWidget(m_pLabelHost, 0, 0);
    connLayout->addWidget(m_pEditHost, 0, 1);

    // 端口
    m_pLabelPort = new QLabel(tr("端口:"), grpConn);
    m_pLabelPort->setFixedWidth(70);
    m_pSpinPort = new QSpinBox(grpConn);
    m_pSpinPort->setRange(1, 65535);
    m_pSpinPort->setValue(5432);
    m_pSpinPort->setMinimumWidth(110);
    connLayout->addWidget(m_pLabelPort, 0, 2);
    connLayout->addWidget(m_pSpinPort, 0, 3);

    // 数据库
    m_pLabelDatabase = new QLabel(tr("数据库:"), grpConn);
    m_pLabelDatabase->setFixedWidth(70);
    m_pEditDatabase = new QLineEdit(grpConn);
    m_pEditDatabase->setPlaceholderText("gis_db");
    m_pEditDatabase->setMinimumWidth(190);
    connLayout->addWidget(m_pLabelDatabase, 1, 0);
    connLayout->addWidget(m_pEditDatabase, 1, 1);

    // Schema
    m_pLabelSchema = new QLabel(tr("Schema:"), grpConn);
    m_pLabelSchema->setFixedWidth(70);
    m_pEditSchema = new QLineEdit(grpConn);
    m_pEditSchema->setPlaceholderText("public");
    m_pEditSchema->setMinimumWidth(110);
    connLayout->addWidget(m_pLabelSchema, 1, 2);
    connLayout->addWidget(m_pEditSchema, 1, 3);

    // 用户名
    m_pLabelUsername = new QLabel(tr("用户名:"), grpConn);
    m_pLabelUsername->setFixedWidth(70);
    m_pEditUsername = new QLineEdit(grpConn);
    m_pEditUsername->setPlaceholderText("postgres");
    m_pEditUsername->setMinimumWidth(190);
    connLayout->addWidget(m_pLabelUsername, 2, 0);
    connLayout->addWidget(m_pEditUsername, 2, 1);

    // 密码
    m_pLabelPassword = new QLabel(tr("密码:"), grpConn);
    m_pLabelPassword->setFixedWidth(70);
    m_pEditPassword = new QLineEdit(grpConn);
    m_pEditPassword->setEchoMode(QLineEdit::Password);
    m_pEditPassword->setMinimumWidth(190);
    connLayout->addWidget(m_pLabelPassword, 2, 2);
    connLayout->addWidget(m_pEditPassword, 2, 3);

    // 测试连接按钮
    m_pBtnTestConn = new QPushButton(tr("测试连接"), grpConn);
    m_pBtnTestConn->setFixedWidth(110);
    QHBoxLayout* testBtnLayout = new QHBoxLayout();
    testBtnLayout->addSpacerItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum));
    testBtnLayout->addWidget(m_pBtnTestConn);
    connLayout->addLayout(testBtnLayout, 3, 0, 1, 4);

    // 让输入框列自动拉伸以利用空间
    connLayout->setColumnStretch(0, 0);
    connLayout->setColumnStretch(1, 1);
    connLayout->setColumnStretch(2, 0);
    connLayout->setColumnStretch(3, 1);

    mainLayout->addWidget(grpConn);

    // ================================================================
    //  第二部分：数据类型
    // ================================================================
    QGroupBox* grpType = new QGroupBox(tr("数据类型"), this);
    QGridLayout* typeLayout = new QGridLayout(grpType);
    typeLayout->setSpacing(8);

    m_pRadioShp = new QRadioButton(tr("Shapefile (.shp)"), grpType);
    m_pRadioGeoJson = new QRadioButton(tr("GeoJSON (.geojson)"), grpType);
    m_pRadioGpkg = new QRadioButton(tr("GeoPackage (.gpkg)"), grpType);
    m_pRadioGdb = new QRadioButton(tr("File Geodatabase (.gdb)"), grpType);
    m_pRadioTif = new QRadioButton(tr("TIF 栅格 (.tif)"), grpType);
    m_pRadioFolder = new QRadioButton(tr("文件夹批量导入"), grpType);

    m_pRadioShp->setChecked(true);

    typeLayout->addWidget(m_pRadioShp, 0, 0);
    typeLayout->addWidget(m_pRadioGeoJson, 0, 1);
    typeLayout->addWidget(m_pRadioGpkg, 1, 0);
    typeLayout->addWidget(m_pRadioGdb, 1, 1);
    typeLayout->addWidget(m_pRadioTif, 2, 0);
    typeLayout->addWidget(m_pRadioFolder, 2, 1);

    mainLayout->addWidget(grpType);

    // ================================================================
    //  第三部分：导入选项 (SRID / 编码 / 空间索引 / 元数据)
    // ================================================================
    QGroupBox* grpOptions = new QGroupBox(tr("导入选项"), this);
    QGridLayout* optLayout = new QGridLayout(grpOptions);
    optLayout->setSpacing(8);

    // SRID
    m_pLabelSrid = new QLabel(tr("目标坐标系 SRID:"), grpOptions);
    m_pLabelSrid->setMinimumWidth(135);
    m_pComboSrid = new QComboBox(grpOptions);
    m_pComboSrid->setEditable(true);
    m_pComboSrid->addItem(tr("WGS84 (4326)"), 4326);
    m_pComboSrid->addItem(tr("CGCS2000 (4490)"), 4490);
    m_pComboSrid->addItem(tr("Web Mercator (3857)"), 3857);
    m_pComboSrid->addItem(tr("北京1954 (4214)"), 4214);
    m_pComboSrid->addItem(tr("西安1980 (4610)"), 4610);
    m_pComboSrid->setCurrentIndex(0);
    m_pComboSrid->setMinimumWidth(180);
    m_pComboSrid->setToolTip(tr("选择预设坐标系或手动输入自定义EPSG编码"));
    optLayout->addWidget(m_pLabelSrid, 0, 0);
    optLayout->addWidget(m_pComboSrid, 0, 1);

    // 编码
    m_pLabelEncoding = new QLabel(tr("字符编码:"), grpOptions);
    m_pLabelEncoding->setMinimumWidth(135);
    m_pComboEncoding = new QComboBox(grpOptions);
    m_pComboEncoding->addItems({"UTF-8", "GBK", "LATIN1"});
    m_pComboEncoding->setCurrentIndex(0);
    m_pComboEncoding->setMinimumWidth(120);
    m_pComboEncoding->setToolTip(tr("Shapefile 常为 GBK，GeoJSON 一般为 UTF-8"));
    optLayout->addWidget(m_pLabelEncoding, 0, 2);
    optLayout->addWidget(m_pComboEncoding, 0, 3);

    // 栅格分块大小（仅TIF时可见）
    m_pLabelTileSize = new QLabel(tr("栅格分块大小:"), grpOptions);
    m_pLabelTileSize->setMinimumWidth(135);
    m_pSpinTileSize = new QSpinBox(grpOptions);
    m_pSpinTileSize->setRange(32, 2048);
    m_pSpinTileSize->setSingleStep(32);
    m_pSpinTileSize->setValue(256);
    m_pSpinTileSize->setSuffix(" px");
    m_pSpinTileSize->setMinimumWidth(120);
    m_pSpinTileSize->setToolTip(tr("raster2pgsql -t 参数，默认256x256"));
    optLayout->addWidget(m_pLabelTileSize, 1, 0);
    optLayout->addWidget(m_pSpinTileSize, 1, 1);

    // 空间索引 + 写入元数据（同一行）
    m_pChkSpatialIndex = new QCheckBox(tr("自动创建空间索引 (GIST)"), grpOptions);
    m_pChkSpatialIndex->setChecked(true);
    m_pChkSpatialIndex->setToolTip(tr("矢量: GDAL自动建GIST索引; 栅格: raster2pgsql -I 参数"));
    optLayout->addWidget(m_pChkSpatialIndex, 2, 0, 1, 2);

    m_pChkWriteMetadata = new QCheckBox(tr("写入元数据表"), grpOptions);
    m_pChkWriteMetadata->setChecked(true);
    m_pChkWriteMetadata->setToolTip(tr("导入成功后自动写入 public.gis_metadata 表"));
    optLayout->addWidget(m_pChkWriteMetadata, 2, 2, 1, 2);

    // 覆盖 + 加载到地图（与上面同一 grid，精确对齐）
    m_pChkOverwrite = new QCheckBox(tr("覆盖已有表 (-overwrite)"), grpOptions);
    m_pChkOverwrite->setToolTip(tr("目标表已存在时先删除再导入"));
    optLayout->addWidget(m_pChkOverwrite, 3, 0, 1, 2);

    m_pChkLoadAfter = new QCheckBox(tr("导入完成后加载到地图"), grpOptions);
    m_pChkLoadAfter->setChecked(true);
    optLayout->addWidget(m_pChkLoadAfter, 3, 2, 1, 2);

    // 让输入控件列自动拉伸
    optLayout->setColumnStretch(0, 0);
    optLayout->setColumnStretch(1, 1);
    optLayout->setColumnStretch(2, 0);
    optLayout->setColumnStretch(3, 1);

    mainLayout->addWidget(grpOptions);

    // ================================================================
    //  第四部分：输入设置
    // ================================================================
    QGroupBox* grpInput = new QGroupBox(tr("输入设置"), this);
    QVBoxLayout* inputLayout = new QVBoxLayout(grpInput);
    inputLayout->setSpacing(8);

    // 数据路径行（标签 + 编辑框(stretch) + 浏览按钮 紧排）
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->setSpacing(6);
    m_pLabelDataPath = new QLabel(tr("数据路径:"), grpInput);
    m_pLabelDataPath->setFixedWidth(80);
    m_pEditDataPath = new QLineEdit(grpInput);
    m_pEditDataPath->setReadOnly(true);
    m_pBtnBrowse = new QPushButton(tr("浏览"), grpInput);
    m_pBtnBrowse->setFixedWidth(80);
    pathLayout->addWidget(m_pLabelDataPath);
    pathLayout->addWidget(m_pEditDataPath, 1);
    pathLayout->addWidget(m_pBtnBrowse);
    inputLayout->addLayout(pathLayout);

    // 目标表名行（标签 + 编辑框(stretch) 紧排）
    QHBoxLayout* tableLayout = new QHBoxLayout();
    tableLayout->setSpacing(6);
    m_pLabelTableName = new QLabel(tr("目标表名:"), grpInput);
    m_pLabelTableName->setFixedWidth(80);
    m_pEditTableName = new QLineEdit(grpInput);
    m_pEditTableName->setPlaceholderText(tr("为空则自动使用文件名"));
    tableLayout->addWidget(m_pLabelTableName);
    tableLayout->addWidget(m_pEditTableName, 1);
    inputLayout->addLayout(tableLayout);

    mainLayout->addWidget(grpInput);

    // ================================================================
    //  第五部分：GDB图层列表（仅GDB模式显示）
    // ================================================================
    m_pGrpGdbLayers = new QGroupBox(tr("GDB 图层选择"), this);
    QVBoxLayout* gdbLayout = new QVBoxLayout(m_pGrpGdbLayers);
    gdbLayout->setSpacing(6);

    m_pLabelGdbLayers = new QLabel(tr("勾选需导入的图层:"), m_pGrpGdbLayers);
    gdbLayout->addWidget(m_pLabelGdbLayers);

    m_pListGdbLayers = new QListWidget(m_pGrpGdbLayers);
    m_pListGdbLayers->setMinimumHeight(100);
    m_pListGdbLayers->setMaximumHeight(150);
    m_pListGdbLayers->setSelectionMode(QAbstractItemView::NoSelection);
    gdbLayout->addWidget(m_pListGdbLayers);

    m_pBtnRefreshLayers = new QPushButton(tr("刷新图层列表"), m_pGrpGdbLayers);
    m_pBtnRefreshLayers->setFixedWidth(140);
    QHBoxLayout* refreshBtnLayout = new QHBoxLayout();
    refreshBtnLayout->addWidget(m_pBtnRefreshLayers);
    refreshBtnLayout->addSpacerItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum));
    gdbLayout->addLayout(refreshBtnLayout);

    // 默认隐藏整个分组
    m_pGrpGdbLayers->setVisible(false);
    mainLayout->addWidget(m_pGrpGdbLayers);

    // ================================================================
    //  第六部分：元数据信息
    // ================================================================
    QGroupBox* grpMetadata = new QGroupBox(tr("元数据信息（可选）"), this);
    QGridLayout* metaLayout = new QGridLayout(grpMetadata);
    metaLayout->setSpacing(8);

    m_pLabelDataSource = new QLabel(tr("数据来源:"), grpMetadata);
    m_pLabelDataSource->setFixedWidth(80);
    m_pEditDataSource = new QLineEdit(grpMetadata);
    m_pEditDataSource->setPlaceholderText(tr("如: 某部门/某项目/某某系统"));
    m_pEditDataSource->setMinimumWidth(240);
    metaLayout->addWidget(m_pLabelDataSource, 0, 0);
    metaLayout->addWidget(m_pEditDataSource, 0, 1);

    m_pLabelDescription = new QLabel(tr("描述:"), grpMetadata);
    m_pLabelDescription->setFixedWidth(80);
    m_pEditDescription = new QLineEdit(grpMetadata);
    m_pEditDescription->setPlaceholderText(tr("如: 2024年全国行政区划数据"));
    m_pEditDescription->setMinimumWidth(240);
    metaLayout->addWidget(m_pLabelDescription, 1, 0);
    metaLayout->addWidget(m_pEditDescription, 1, 1);

    // 让输入框列自动拉伸
    metaLayout->setColumnStretch(0, 0);
    metaLayout->setColumnStretch(1, 1);

    mainLayout->addWidget(grpMetadata);

    // ================================================================
    //  第七部分：进度条 & 状态
    // ================================================================
    m_pProgressBar = new QProgressBar(this);
    m_pProgressBar->setValue(0);
    mainLayout->addWidget(m_pProgressBar);

    m_pLabelStatus = new QLabel(tr("就绪"), this);
    mainLayout->addWidget(m_pLabelStatus);

    // ================================================================
    //  第八部分：操作按钮
    // ================================================================
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(6);

    m_pBtnImport = new QPushButton(tr("开始导入"), this);
    m_pBtnImport->setMinimumWidth(130);
    m_pBtnImport->setStyleSheet("QPushButton { font-weight: bold; }");
    btnLayout->addWidget(m_pBtnImport);

    m_pBtnCancel = new QPushButton(tr("停止"), this);
    m_pBtnCancel->setMinimumWidth(100);
    m_pBtnCancel->setVisible(false);
    m_pBtnCancel->setStyleSheet("QPushButton { color: red; font-weight: bold; }");
    btnLayout->addWidget(m_pBtnCancel);

    m_pBtnMetadataViewer = new QPushButton(tr("元数据管理"), this);
    m_pBtnMetadataViewer->setMinimumWidth(110);
    m_pBtnMetadataViewer->setToolTip(tr("查看和编辑已入库数据的元数据表"));
    btnLayout->addWidget(m_pBtnMetadataViewer);

    btnLayout->addSpacerItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum));

    m_pBtnClose = new QPushButton(tr("关闭"), this);
    m_pBtnClose->setMinimumWidth(100);
    btnLayout->addWidget(m_pBtnClose);

    mainLayout->addLayout(btnLayout);

    // ================================================================
    //  信号连接
    // ================================================================
    connect(m_pBtnBrowse,        &QPushButton::clicked, this, &CSE_DataImportDialog::slotBrowse);
    connect(m_pBtnTestConn,      &QPushButton::clicked, this, &CSE_DataImportDialog::slotTestConnection);
    connect(m_pBtnImport,        &QPushButton::clicked, this, &CSE_DataImportDialog::slotStartImport);
    connect(m_pBtnCancel,        &QPushButton::clicked, this, &CSE_DataImportDialog::slotCancelBatchImport);
    connect(m_pBtnMetadataViewer,&QPushButton::clicked, this, &CSE_DataImportDialog::slotOpenMetadataViewer);
    connect(m_pBtnClose,         &QPushButton::clicked, this, &CSE_DataImportDialog::slotClose);
    connect(m_pBtnRefreshLayers, &QPushButton::clicked, this, &CSE_DataImportDialog::slotRefreshGdbLayers);

    // 切换数据类型时，更新UI控件可见性
    connect(m_pRadioShp,      &QRadioButton::toggled, this, &CSE_DataImportDialog::slotUpdateDataType);
    connect(m_pRadioGeoJson,  &QRadioButton::toggled, this, &CSE_DataImportDialog::slotUpdateDataType);
    connect(m_pRadioGpkg,     &QRadioButton::toggled, this, &CSE_DataImportDialog::slotUpdateDataType);
    connect(m_pRadioGdb,      &QRadioButton::toggled, this, &CSE_DataImportDialog::slotUpdateDataType);
    connect(m_pRadioTif,      &QRadioButton::toggled, this, &CSE_DataImportDialog::slotUpdateDataType);
    connect(m_pRadioFolder,   &QRadioButton::toggled, this, &CSE_DataImportDialog::slotUpdateDataType);

    // 初始化控件可见性
    UpdateControlVisibility();
}

// ====================================================================
//  设置持久化
// ====================================================================

void CSE_DataImportDialog::LoadSettings()
{
    // ---- 从数据库连接配置 UI（DbConfigDialog）共享的注册表配置加载连接参数 ----
    // 配置 UI 连接成功后把 host/port/database/schema/user/password 写入
    // QSettings("GarMap","MapProductManager")，这里读取同一批键，实现与配置 UI
    // 共享同一个数据库（与地图数据下载 UI mapdata_download 的做法一致）。
    // 不再读取 db_config.json（新配置 UI 已不再输出该文件）。
    QSettings dbSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
    QString host   = dbSettings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString();
    int     port   = dbSettings.value(QStringLiteral("db/port"), 5432).toInt();
    QString dbname = dbSettings.value(QStringLiteral("db/database"), QStringLiteral("map_products")).toString();
    QString schema = dbSettings.value(QStringLiteral("db/schema"), QStringLiteral("public")).toString();
    QString user   = dbSettings.value(QStringLiteral("db/user"), QStringLiteral("postgres")).toString();
    // 仅当配置 UI 勾选“保存密码”时才持久化密码，未勾选则留空由用户手动输入
    QString password;
    if (dbSettings.value(QStringLiteral("db/savePassword"), false).toBool())
        password = dbSettings.value(QStringLiteral("db/password")).toString();

    m_pEditHost->setText(host);
    m_pSpinPort->setValue(port);
    m_pEditDatabase->setText(dbname);
    m_pEditSchema->setText(schema);
    m_pEditUsername->setText(user);
    m_pEditPassword->setText(password);

    // ---- 自动测试连接并显示状态 ----
    {
        QString connName = "import_auto_test";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
            db.setHostName(host);
            db.setPort(port);
            db.setDatabaseName(dbname);
            db.setUserName(user);
            db.setPassword(password);
            if (db.open())
            {
                m_pLabelStatus->setText(tr("✓ 连接成功 (%1:%2/%3)")
                    .arg(db.hostName()).arg(db.port()).arg(db.databaseName()));
                m_pLabelStatus->setStyleSheet("QLabel { color: #4CAF50; }");
                db.close();
            }
            else
            {
                m_pLabelStatus->setText(tr("✗ 连接失败: %1").arg(db.lastError().text()));
                m_pLabelStatus->setStyleSheet("QLabel { color: red; }");
            }
        }
        QSqlDatabase::removeDatabase(connName);
    }

    // ---- QSettings 加载其他 UI 状态（SRID、编码等） ----
    QSettings settings;
    int savedSrid = settings.value("DataImport/SRID", 4326).toInt();
    int sridIdx = m_pComboSrid->findData(savedSrid);
    if (sridIdx >= 0)
        m_pComboSrid->setCurrentIndex(sridIdx);
    else {
        m_pComboSrid->setCurrentIndex(-1);
        m_pComboSrid->setEditText(QString::number(savedSrid));
    }
    // 国内 GIS 数据以 GBK 为主，默认选 GBK (index=1)，UTF-8=0, LATIN1=2
    m_pComboEncoding->setCurrentIndex(settings.value("DataImport/Encoding", 1).toInt());
    m_pSpinTileSize->setValue(settings.value("DataImport/TileSize", 256).toInt());
    m_pChkSpatialIndex->setChecked(settings.value("DataImport/SpatialIndex", true).toBool());
    m_pChkWriteMetadata->setChecked(settings.value("DataImport/WriteMetadata", true).toBool());
    m_pEditDataSource->setText(settings.value("DataImport/DataSource", "").toString());
    // 密码从共享配置读取，且仅当配置 UI 勾选“保存密码”时才写入

    int dataType = settings.value("DataImport/DataType", 0).toInt();
    switch (dataType)
    {
    case 0: m_pRadioShp->setChecked(true);     break;
    case 1: m_pRadioGeoJson->setChecked(true); break;
    case 2: m_pRadioGpkg->setChecked(true);    break;
    case 3: m_pRadioGdb->setChecked(true);     break;
    case 4: m_pRadioTif->setChecked(true);     break;
    case 5: m_pRadioFolder->setChecked(true);  break;
    default: m_pRadioShp->setChecked(true);    break;
    }

    UpdateControlVisibility();
}

void CSE_DataImportDialog::SaveSettings()
{
    // 数据库连接字段写回共享配置（与数据库连接配置 UI 同源），
    // 密码仅在配置 UI 勾选“保存密码”时才写入，与地图数据下载 UI 的 saveSettings 一致
    QSettings dbSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
    dbSettings.setValue("db/host",     m_pEditHost->text().trimmed());
    dbSettings.setValue("db/port",     m_pSpinPort->value());
    dbSettings.setValue("db/database", m_pEditDatabase->text().trimmed());
    dbSettings.setValue("db/schema",   m_pEditSchema->text().trimmed());
    dbSettings.setValue("db/user",     m_pEditUsername->text().trimmed());
    if (dbSettings.value("db/savePassword", false).toBool())
        dbSettings.setValue("db/password", m_pEditPassword->text());

    QSettings settings;
    settings.setValue("DataImport/SRID",         GetSrid());
    settings.setValue("DataImport/Encoding",     m_pComboEncoding->currentIndex());
    settings.setValue("DataImport/TileSize",     m_pSpinTileSize->value());
    settings.setValue("DataImport/SpatialIndex", m_pChkSpatialIndex->isChecked());
    settings.setValue("DataImport/WriteMetadata",m_pChkWriteMetadata->isChecked());
    settings.setValue("DataImport/DataSource",   m_pEditDataSource->text().trimmed());

    if (m_pRadioShp->isChecked())      settings.setValue("DataImport/DataType", 0);
    if (m_pRadioGeoJson->isChecked())  settings.setValue("DataImport/DataType", 1);
    if (m_pRadioGpkg->isChecked())     settings.setValue("DataImport/DataType", 2);
    if (m_pRadioGdb->isChecked())      settings.setValue("DataImport/DataType", 3);
    if (m_pRadioTif->isChecked())      settings.setValue("DataImport/DataType", 4);
    if (m_pRadioFolder->isChecked())   settings.setValue("DataImport/DataType", 5);
}

// ====================================================================
//  控件可见性控制
// ====================================================================

void CSE_DataImportDialog::UpdateControlVisibility()
{
    bool isGdb = m_pRadioGdb->isChecked();
    bool isTif = m_pRadioTif->isChecked();
    bool isFolder = m_pRadioFolder->isChecked();

    // GDB图层列表
    m_pGrpGdbLayers->setVisible(isGdb);

    // 栅格分块大小
    m_pLabelTileSize->setVisible(isTif);
    m_pSpinTileSize->setVisible(isTif);

    // 表格名：批量模式下隐藏（每个文件自动命名），GDB模式下隐藏（每个图层自动命名）
    m_pLabelTableName->setVisible(!isFolder && !isGdb);
    m_pEditTableName->setVisible(!isFolder && !isGdb);

    // 编码选项：SHP 和 GDB 时显示
    m_pLabelEncoding->setVisible(!isTif);
    m_pComboEncoding->setVisible(!isTif);

    // 根据数据类型自动切换编码默认值
    // SHP: 国内以 GBK 为主 | GeoJSON/GPKG: UTF-8 标准 | GDB: GDAL 输出 UTF-8 | 文件夹: GBK（多含 SHP）
    if (m_pRadioShp->isChecked())
        m_pComboEncoding->setCurrentIndex(0);  // UTF-8
    else if (m_pRadioFolder->isChecked())
        m_pComboEncoding->setCurrentIndex(1);  // GBK（文件夹批量多为 SHP）
    else
        m_pComboEncoding->setCurrentIndex(0);  // UTF-8（GeoJSON/GPKG/GDB/TIF）

    // 根据内容自适应大小，避免 GDB 模式下控件挤在一起
    this->adjustSize();
    QSize cur = this->size();
    if (cur.width() < 960)  cur.setWidth(960);
    if (cur.height() < 780) cur.setHeight(780);
    this->resize(cur);
}

// ====================================================================
//  辅助函数
// ====================================================================

/// 从SRID下拉框获取当前EPSG编码：预设项返回关联的data值，手动输入则解析文本
int CSE_DataImportDialog::GetSrid() const
{
    QVariant data = m_pComboSrid->currentData();
    if (data.isValid() && data.toInt() > 0)
        return data.toInt();
    bool ok = false;
    int srid = m_pComboSrid->currentText().trimmed().toInt(&ok);
    return ok ? srid : 4326;  // 解析失败降级为 WGS84
}

QString CSE_DataImportDialog::BuildPGConnString() const
{
    QString host     = m_pEditHost->text().trimmed();
    int     port     = m_pSpinPort->value();
    QString database = m_pEditDatabase->text().trimmed();
    QString schema   = m_pEditSchema->text().trimmed();
    QString username = m_pEditUsername->text().trimmed();
    QString password = m_pEditPassword->text();

    QString conn = QString("PG:host=%1 port=%2 dbname=%3 user=%4")
        .arg(host.isEmpty() ? "localhost" : host)
        .arg(port)
        .arg(database.isEmpty() ? "gis_db" : database)
        .arg(username.isEmpty() ? "postgres" : username);

    if (!password.isEmpty())
        conn += QString(" password=%1").arg(password);

    // 强制 IPv4，避免 localhost 解析为 ::1 导致 pg_hba.conf 不匹配
    conn += " hostaddr=127.0.0.1";

    if (!schema.isEmpty())
        conn += QString(" schemas=%1").arg(schema);

    conn += " active_schema=" + (schema.isEmpty() ? "public" : schema);

    return conn;
}

/// 导入专用：不再设 client_encoding=GBK，改由 GDAL 内部做 GBK→UTF-8 转码。
/// GDAL 遇到源数据中损坏的 GBK 字节会替换为 ? 而非报错；
/// PG 之前的 server-side 转码过于严格，坏字节直接炸。
QString CSE_DataImportDialog::BuildPGConnStringForImport() const
{
    return BuildPGConnString();  // 不附加 client_encoding，PG 默认 UTF-8
}

QString CSE_DataImportDialog::BuildDataSourceUri(const QString& schema, const QString& tableName) const
{
    QString host     = m_pEditHost->text().trimmed();
    QString dbname   = m_pEditDatabase->text().trimmed();
    QString user     = m_pEditUsername->text().trimmed();
    QString password = m_pEditPassword->text();
    int     port     = m_pSpinPort->value();

    QgsDataSourceUri uri;
    // localhost 会在某些 Windows 版本解析为 IPv6 ::1，导致 pg_hba.conf 不匹配
    // 直接用 127.0.0.1 强制 IPv4，与 ogr2ogr 连接行为一致
    uri.setConnection(
        host.isEmpty()     ? "127.0.0.1" : (host == "localhost" ? "127.0.0.1" : host),
        QString::number(port),
        dbname.isEmpty()   ? "gis_db"    : dbname,
        user.isEmpty()     ? "postgres"  : user,
        password,
        QgsDataSourceUri::SslDisable
    );
    uri.setDataSource(schema, tableName, "geom");
    uri.setUseEstimatedMetadata(true);

    return uri.uri(false);
}

QStringList CSE_DataImportDialog::BuildPsqlArgs() const
{
    QStringList args;
    QString host     = m_pEditHost->text().trimmed();
    int     port     = m_pSpinPort->value();
    QString database = m_pEditDatabase->text().trimmed();
    QString username = m_pEditUsername->text().trimmed();
    QString password = m_pEditPassword->text();

    if (!host.isEmpty())
    {
        args << "-h" << host;
    }
    args << "-p" << QString::number(port);
    if (!database.isEmpty()) args << "-d" << database;
    if (!username.isEmpty()) args << "-U" << username;
    // 有密码时用 -w + PGPASSWORD，没有密码时让 psql 自己处理（trust/peer 认证）
    if (!password.isEmpty())
        args << "-w";
    args << "--no-psqlrc";

    return args;
}

/// 将 PGPASSWORD 同时写入进程环境变量和父进程环境
void CSE_DataImportDialog::SetPsqlPassword() const
{
    QString password = m_pEditPassword->text();
    if (!password.isEmpty())
    {
        qputenv("PGPASSWORD", password.toUtf8());
    }
}

/// 为 QProcess 设置 PGPASSWORD / PGCLIENTENCODING 环境变量
/// ogr2ogr 进程需要 client_encoding 让 PG 转码；psql 读导入表时也需匹配编码
void CSE_DataImportDialog::SetupProcessEnv(QProcess& proc, const QString& clientEncoding) const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString password = m_pEditPassword->text();
    if (!password.isEmpty())
    {
        env.insert("PGPASSWORD", password);
    }
    if (!clientEncoding.isEmpty())
    {
        env.insert("PGCLIENTENCODING", clientEncoding);
    }

    // ====== 自动探测 PostGIS/GDAL 工具路径，追加到 PATH ======
    // 解决用户未将 PostgreSQL bin 加入系统 PATH 导致
    // raster2pgsql / ogr2ogr / psql 找不到的问题
    QString pathEnv = env.value("PATH");
    QStringList foundBins;
    const QString listSep = QDir::listSeparator();

#ifdef Q_OS_WIN
    // 1) 从 Windows 注册表读取 PostgreSQL 安装路径
    {
        QSettings pgReg("HKEY_LOCAL_MACHINE\\SOFTWARE\\PostgreSQL\\Installations",
                         QSettings::NativeFormat);
        foreach (const QString& key, pgReg.childGroups())
        {
            pgReg.beginGroup(key);
            QString baseDir = pgReg.value("Base Directory").toString();
            if (!baseDir.isEmpty())
            {
                QString binDir = QDir(baseDir).filePath("bin");
                if (QDir(binDir).exists() && !foundBins.contains(binDir))
                    foundBins << binDir;
            }
            pgReg.endGroup();
        }
    }

    // 2) 常见默认安装路径
    for (int ver = 12; ver <= 18; ++ver)
    {
        QString binDir = QString("C:\\Program Files\\PostgreSQL\\%1\\bin").arg(ver);
        if (QDir(binDir).exists() && !foundBins.contains(binDir))
            foundBins << binDir;
    }

    // 3) OSGeo4W / QGIS 自带工具链（项目捆绑的 extern/OSGeo4W_32815/bin）
    {
        // 从插件 DLL 位置向上查找 OSGeo4W
        QString pluginDir = QCoreApplication::applicationDirPath();
        QStringList candidates = {
            pluginDir + "/osgeo4w/bin",
            pluginDir + "/../../extern/OSGeo4W_32815/bin",
            pluginDir + "/../../../extern/OSGeo4W_32815/bin",
            "C:\\OSGeo4W64\\bin",
            "C:\\Program Files\\GDAL",
            "C:\\Program Files\\QGIS 3.28.15\\bin",
        };
        for (const QString& d : candidates)
        {
            QString canonical = QDir(d).canonicalPath();
            if (!canonical.isEmpty() && QDir(canonical).exists()
                && !foundBins.contains(canonical))
                foundBins << canonical;
        }
    }
#else  // Linux
    // 1) Linux：扫描系统 PostgreSQL 各版本 bin（/usr/lib/postgresql/<版本>/bin）
    //    psql/raster2pgsql 一般在这里；麒麟 PostgreSQL 12 即 /usr/lib/postgresql/12/bin
    {
        QDir pgRoot("/usr/lib/postgresql");
        if (pgRoot.exists())
        {
            const QStringList versions = pgRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString& v : versions)
            {
                QString binDir = pgRoot.filePath(v + "/bin");
                if (QDir(binDir).exists() && !foundBins.contains(binDir))
                    foundBins << binDir;
            }
        }
    }

    // 2) 系统常用工具目录（GDAL 工具 ogr2ogr/ogrinfo 在 /usr/bin）
    {
        QStringList candidates = {
            "/usr/bin",
            "/usr/lib/gdal-bin",
            "/usr/lib/postgresql/bin",
        };
        for (const QString& d : candidates)
        {
            if (QDir(d).exists() && !foundBins.contains(d))
                foundBins << d;
        }
    }
#endif

    // 将找到的 bin 目录追加到 PATH 最前面（优先级最高）
    QStringList currentPaths = pathEnv.split(listSep, QString::SkipEmptyParts);
    for (const QString& bin : foundBins)
    {
        // 检查该目录下是否有我们需要的工具（按平台区分可执行文件名）
#ifdef Q_OS_WIN
        bool hasTools = QFileInfo::exists(bin + "/raster2pgsql.exe")
                     || QFileInfo::exists(bin + "/ogr2ogr.exe")
                     || QFileInfo::exists(bin + "/psql.exe");
#else
        bool hasTools = QFileInfo::exists(bin + "/raster2pgsql")
                     || QFileInfo::exists(bin + "/ogr2ogr")
                     || QFileInfo::exists(bin + "/psql");
#endif
        if (hasTools && !currentPaths.contains(bin, Qt::CaseInsensitive))
        {
            pathEnv = bin + listSep + pathEnv;
            currentPaths.prepend(bin);
        }
    }

    env.insert("PATH", pathEnv);
    // ====== 自动探测结束 ======

    proc.setProcessEnvironment(env);
}

QString CSE_DataImportDialog::GetFileFilter() const
{
    if (m_pRadioShp->isChecked())
        return tr("Shapefile (*.shp)");
    if (m_pRadioGeoJson->isChecked())
        return tr("GeoJSON (*.geojson *.json)");
    if (m_pRadioGpkg->isChecked())
        return tr("GeoPackage (*.gpkg)");
    if (m_pRadioGdb->isChecked())
        return tr("File Geodatabase (*.gdb)");
    if (m_pRadioTif->isChecked())
        return tr("TIF 栅格 (*.tif *.tiff)");
    return QString(); // 文件夹
}

QString CSE_DataImportDialog::GetDataTypeName() const
{
    if (m_pRadioShp->isChecked())     return tr("Shapefile");
    if (m_pRadioGeoJson->isChecked()) return tr("GeoJSON");
    if (m_pRadioGpkg->isChecked())    return tr("GeoPackage");
    if (m_pRadioGdb->isChecked())     return tr("File Geodatabase");
    if (m_pRadioTif->isChecked())     return tr("TIF 栅格");
    if (m_pRadioFolder->isChecked())  return tr("文件夹批量");
    return tr("未知");
}

QString CSE_DataImportDialog::GetDataTypeId() const
{
    if (m_pRadioShp->isChecked())     return "SHP";
    if (m_pRadioGeoJson->isChecked()) return "GeoJSON";
    if (m_pRadioGpkg->isChecked())    return "GPKG";
    if (m_pRadioGdb->isChecked())     return "GDB";
    if (m_pRadioTif->isChecked())     return "TIF";
    if (m_pRadioFolder->isChecked())  return "BATCH";
    return "UNKNOWN";
}

QStringList CSE_DataImportDialog::ScanSupportedFiles(const QString& dirPath) const
{
    QDir dir(dirPath);
    if (!dir.exists()) return {};

    QStringList result;

    // 如果选中的目录本身就是 .gdb，直接加入结果
    // （用户在文件夹模式下直接选了 GDB 目录的情况）
    if (dirPath.toLower().endsWith(".gdb"))
    {
        result << QDir::toNativeSeparators(dir.absolutePath());
        return result;  // .gdb 内部不再扫描
    }

    // 递归扫描文件（SHP/GeoJSON/GPKG/TIF），支持按图幅/年份等子文件夹组织的数据
    QStringList filters;
    filters << "*.shp" << "*.geojson" << "*.json" << "*.gpkg" << "*.tif" << "*.tiff";
    QDirIterator itFiles(dirPath, filters, QDir::Files, QDirIterator::Subdirectories);
    while (itFiles.hasNext())
    {
        QString f = itFiles.next();
        // 跳过 .gdb 目录内部的文件（GDB 作为整体导入，不拆散）
        QString lower = QDir::fromNativeSeparators(f).toLower();
        if (lower.contains(".gdb/"))
            continue;
        result << f;
    }

    // 递归扫描 .gdb 目录（GDB 是目录而非单文件）
    // 不用 entryList 的 nameFilter + QDir::Readable 组合，
    // Windows 上 QDir::Readable 可能错误过滤目录；改为手动过滤后缀
    QDirIterator itDirs(dirPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (itDirs.hasNext())
    {
        QString d = itDirs.next();
        if (d.toLower().endsWith(".gdb"))
            result << d;
    }

    result.sort(Qt::CaseInsensitive);
    return result;
}

QString CSE_DataImportDialog::DefaultTableName(const QString& filePath) const
{
    QFileInfo fi(filePath);
    QString name = fi.completeBaseName();

    // 替换不合法的 PostGIS 表名字符：只保留字母、数字、下划线
    QString result;
    for (int i = 0; i < name.length(); i++)
    {
        QChar c = name.at(i);
        if (c.isLetterOrNumber() || c == '_')
            result += c;
        else
            result += '_';
    }

    // 确保不以数字开头
    if (!result.isEmpty() && result.at(0).isDigit())
        result.prepend('t');

    // PostgreSQL 默认将未加引号的标识符转为小写，统一 lowercase 避免大小写不匹配
    return (result.isEmpty() ? "imported_data" : result).toLower();
}

QStringList CSE_DataImportDialog::BuildOgr2ogrArgs(const QString& inputPath, const QString& tableName,
                                                     const QStringList& dateFields) const
{
    QStringList args;

    // 输出格式：PostgreSQL
    args << "-f" << "PostgreSQL";

    // 连接字符串（导入专用，带 client_encoding 参数）
    args << BuildPGConnStringForImport();

    // 目标表名
    if (!tableName.isEmpty())
        args << "-nln" << tableName;

    // 坐标系
    int srid = GetSrid();
    if (srid > 0)
        args << "-t_srs" << QString("EPSG:%1").arg(srid);

    // 几何字段名
    args << "-lco" << "GEOMETRY_NAME=geom";

    // 编码处理：不依赖 -lco (GDAL PostGIS 驱动不支持 ENCODING/CLIENT_ENCODING)
    // SHAPE_ENCODING 告诉 GDAL 以什么编码读源文件；PGCLIENTENCODING 由 SetupProcessEnv 注入
    QString encoding = m_pComboEncoding->currentText();
    if (!encoding.isEmpty() && (m_pRadioShp->isChecked() || m_pRadioGdb->isChecked() || m_pRadioFolder->isChecked()))
    {
        args << "--config" << "SHAPE_ENCODING" << encoding;
        args << "--config" << "OGR_FORCE_ASCII" << "NO";
    }

    // 覆盖模式
    if (m_pChkOverwrite->isChecked())
        args << "-overwrite";

    // 跳过失败要素继续导入（避免个别编码问题整批失败）
    args << "-skipfailures";

    // 日期字段先按 VARCHAR 导入，避免 0000/00/00 被 PostgreSQL 拒绝
    // -lco COLUMN_TYPES 直接告诉 PG 驱动将指定列建为 VARCHAR（比 -fieldTypeToString 更可靠）
    if (!dateFields.isEmpty())
    {
        QStringList colTypes;
        for (const QString& fld : dateFields)
            colTypes << QString("%1=VARCHAR").arg(fld);
        args << "-lco" << QString("COLUMN_TYPES=%1").arg(colTypes.join(","));
    }

    // 进度输出
    args << "-progress";

    // 输入数据
    args << inputPath;

    return args;
}

QStringList CSE_DataImportDialog::BuildRaster2pgsqlArgs(const QString& inputPath, const QString& tableName) const
{
    QStringList args;

    // 坐标系
    int srid = GetSrid();
    if (srid > 0)
        args << "-s" << QString::number(srid);

    // 分块大小
    int tileSize = m_pSpinTileSize->value();
    args << "-t" << QString("%1x%2").arg(tileSize).arg(tileSize);

    // 建约束（自动推断栅格波段类型）
    args << "-C";

    // 建空间索引
    if (m_pChkSpatialIndex->isChecked())
        args << "-I";

    // 覆盖已有表
    if (m_pChkOverwrite->isChecked())
        args << "-F";

    // 输入文件
    args << inputPath;

    // 目标表（schema.table）
    QString schema = m_pEditSchema->text().trimmed();
    if (schema.isEmpty()) schema = "public";
    args << QString("%1.%2").arg(schema, tableName);

    return args;
}

QStringList CSE_DataImportDialog::ListGdbLayers(const QString& gdbPath) const
{
    QStringList layers;

    QProcess proc;
    SetupProcessEnv(proc);
    // --config GDAL_SKIP FileGDB: 跳过ESRI专有FileGDB驱动，使用GDAL内置OpenFileGDB（只读、跨平台）
    proc.start("ogrinfo", { "--config", "GDAL_SKIP", "FileGDB", gdbPath });
    if (!proc.waitForFinished(15000))
    {
        QgsMessageLog::logMessage(tr("ogrinfo 超时: %1").arg(gdbPath),
            tr("数据导入"), Qgis::Warning);
        return layers;
    }

    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    if (output.isEmpty())
        output = QString::fromUtf8(proc.readAllStandardError());

    // 解析 ogrinfo 输出，兼容两种图层列表格式:
    //   数字格式: "1: roads (Multi Line String)"
    //   GDB格式:  "Layer: roads (Multi Line String)"
    QStringList lines = output.split('\n');
    for (const QString& line : lines)
    {
        int colonPos = line.indexOf(':');
        if (colonPos > 0)
        {
            QString prefix = line.left(colonPos).trimmed();
            bool isNumber = false;
            prefix.toInt(&isNumber);

            // 匹配 "N: layername" 或 "Layer: layername" 格式的行
            if (isNumber || prefix == "Layer")
            {
                QString rest = line.mid(colonPos + 1).trimmed();
                // 提取图层名（可能是 "layername" 或 "layername (type)"）
                int spaceParen = rest.indexOf(" (");
                QString layerName = (spaceParen > 0) ? rest.left(spaceParen) : rest;
                if (!layerName.isEmpty())
                    layers << layerName;
            }
        }
    }

    return layers;
}

// ====================================================================
//  日期字段检测与修复（解决 0000/00/00 日期 PostGIS 拒绝写入问题）
// ====================================================================

QStringList CSE_DataImportDialog::ScanDateFields(const QString& filePath, const QString& layerName) const
{
    QStringList dateFields;

    QProcess proc;
    SetupProcessEnv(proc);

    QStringList args;
    args << "-so" << "-al";

    // GDB 需要用 OpenFileGDB 驱动并指定图层名
    bool isGdb = filePath.toLower().endsWith(".gdb");
    if (isGdb)
    {
        args << "--config" << "GDAL_SKIP" << "FileGDB";
        args << filePath;
        if (!layerName.isEmpty())
            args << layerName;
    }
    else
    {
        args << filePath;
    }

    proc.start("ogrinfo", args);
    if (!proc.waitForFinished(15000))
    {
        QgsMessageLog::logMessage(tr("ScanDateFields: ogrinfo 超时"),
            tr("数据导入"), Qgis::Warning);
        return dateFields;
    }

    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    if (output.isEmpty())
        output = QString::fromUtf8(proc.readAllStandardError());

    // 解析字段定义行，匹配 "field_name: Date (10.0)" 或 "field_name: DateTime (19.0)"
    // 不同 GDAL 驱动输出可能有细微差异，统一 lowercase 后做宽松匹配
    QStringList lines = output.split('\n');
    for (const QString& line : lines)
    {
        QString trimmed = line.trimmed();
        QString lower = trimmed.toLower();

        // 跳过不含日期类型关键字的行（date / datetime）
        if (!lower.contains(": date") && !lower.contains(": datetime"))
            continue;

        int colonPos = trimmed.indexOf(':');
        if (colonPos <= 0)
            continue;

        QString fieldName = trimmed.left(colonPos).trimmed();
        if (!fieldName.isEmpty())
            dateFields << fieldName;
    }

    if (!dateFields.isEmpty())
    {
        QgsMessageLog::logMessage(
            tr("检测到日期字段 (%1): %2")
                .arg(filePath).arg(dateFields.join(", ")),
            tr("数据导入"), Qgis::Info);
    }

    return dateFields;
}

bool CSE_DataImportDialog::FixDateColumns(const QString& schema, const QString& tableName,
                                           const QString& filePath, const QString& layerName)
{
    QStringList dateFields = ScanDateFields(filePath, layerName);
    if (dateFields.isEmpty())
        return true;  // 无日期字段，无需修复

    QString qualifiedName = QString("%1.%2").arg(schema).arg(tableName);

    // 用 pg_temp 安全转换函数替代 CASE WHEN 枚举白名单。
    // pg_temp 函数在会话结束后自动清理，无需手动 DROP。
    // 任何无法转换的值（0000/00/00、N/A、非法日期等）由 EXCEPTION 捕获 → 返回 NULL。
    for (const QString& field : dateFields)
    {
        // ====== 先尝试 DATE ======
        QString sqlDate = QString(
            "CREATE OR REPLACE FUNCTION pg_temp.safe_date_cast(t text) RETURNS date AS $$\n"
            "BEGIN\n"
            "    RETURN t::date;\n"
            "EXCEPTION WHEN OTHERS THEN\n"
            "    RETURN NULL;\n"
            "END;\n"
            "$$ LANGUAGE plpgsql IMMUTABLE;\n"
            "ALTER TABLE %1 ALTER COLUMN \"%2\" TYPE DATE USING pg_temp.safe_date_cast(\"%2\");")
            .arg(qualifiedName).arg(field);

        if (ExecuteSql(sqlDate))
        {
            QgsMessageLog::logMessage(
                tr("日期列转换完成: %1.%2 → DATE").arg(qualifiedName).arg(field),
                tr("数据导入"), Qgis::Info);
            continue;
        }

        // ====== DATE 失败，尝试 TIMESTAMP ======
        QString sqlTs = QString(
            "CREATE OR REPLACE FUNCTION pg_temp.safe_ts_cast(t text) RETURNS timestamp AS $$\n"
            "BEGIN\n"
            "    RETURN t::timestamp;\n"
            "EXCEPTION WHEN OTHERS THEN\n"
            "    RETURN NULL;\n"
            "END;\n"
            "$$ LANGUAGE plpgsql IMMUTABLE;\n"
            "ALTER TABLE %1 ALTER COLUMN \"%2\" TYPE TIMESTAMP USING pg_temp.safe_ts_cast(\"%2\");")
            .arg(qualifiedName).arg(field);

        if (ExecuteSql(sqlTs))
        {
            QgsMessageLog::logMessage(
                tr("日期列转换完成: %1.%2 → TIMESTAMP").arg(qualifiedName).arg(field),
                tr("数据导入"), Qgis::Info);
        }
        else
        {
            QgsMessageLog::logMessage(
                tr("日期列转换失败，保留为TEXT: %1.%2").arg(qualifiedName).arg(field),
                tr("数据导入"), Qgis::Warning);
            return false;
        }
    }

    return true;
}

// ====================================================================
//  元数据 XML 配置加载
// ====================================================================

/// 查找 XML 配置文件路径
/// 搜索顺序：1) 程序目录/resource/  2) 程序目录/
///           3) 程序目录/../apps/qgis/resource/ (OSGeo4W)
///           4) 程序目录/../resource/
static QString FindMetadataConfigFile()
{
    QString appDir = QCoreApplication::applicationDirPath();

    // 候选路径列表（按优先级，覆盖 QGIS 独立版 / OSGeo4W / 源码编译 等部署场景）
    QStringList candidates = {
        appDir + "/resource/gis_metadata_config.xml",
        appDir + "/gis_metadata_config.xml",
        appDir + "/../apps/qgis/resource/gis_metadata_config.xml",
        appDir + "/../resource/gis_metadata_config.xml",
    };

    for (const QString& path : candidates)
    {
        QString canonical = QDir(path).canonicalPath();
        if (!canonical.isEmpty() && QFileInfo::exists(canonical))
            return canonical;
        if (QFileInfo::exists(path))
            return path;
    }

    return QString(); // 未找到
}

bool CSE_DataImportDialog::LoadMetadataConfig(QList<SeMetadataFieldDef>& outFields)
{
    outFields.clear();

    QString configPath = FindMetadataConfigFile();
    if (configPath.isEmpty())
    {
        QgsMessageLog::logMessage(
            tr("未找到元数据XML配置文件 gis_metadata_config.xml，"
               "请将文件放在程序目录的 resource/ 子目录下。"),
            tr("数据导入"), Qgis::Warning);
        return false;
    }

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        QgsMessageLog::logMessage(
            tr("无法打开元数据配置文件: %1").arg(configPath),
            tr("数据导入"), Qgis::Warning);
        return false;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorCol;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorCol))
    {
        file.close();
        QgsMessageLog::logMessage(
            tr("XML解析错误 [%1] 行%2 列%3: %4")
                .arg(configPath).arg(errorLine).arg(errorCol).arg(errorMsg),
            tr("数据导入"), Qgis::Warning);
        return false;
    }
    file.close();

    QDomElement root = doc.documentElement();
    if (root.tagName() != "gis_metadata_config")
    {
        QgsMessageLog::logMessage(
            tr("XML根节点不是 gis_metadata_config: %1").arg(configPath),
            tr("数据导入"), Qgis::Warning);
        return false;
    }

    // 遍历所有 <fields> 分组，解析每个 <field> 节点
    QDomNodeList fieldGroups = root.elementsByTagName("fields");
    for (int g = 0; g < fieldGroups.size(); ++g)
    {
        QDomElement groupElem = fieldGroups.at(g).toElement();
        QDomNodeList fieldNodes = groupElem.elementsByTagName("field");

        for (int i = 0; i < fieldNodes.size(); ++i)
        {
            QDomElement f = fieldNodes.at(i).toElement();
            SeMetadataFieldDef def;
            def.name       = f.attribute("name");
            def.type       = f.attribute("type");
            def.defaultVal = f.attribute("default");
            def.notNull    = (f.attribute("not_null") == "true");
            def.primaryKey = (f.attribute("primary_key") == "true");
            def.value      = f.attribute("value");

            if (!def.name.isEmpty() && !def.type.isEmpty())
                outFields.append(def);
        }
    }

    QgsMessageLog::logMessage(
        tr("成功加载元数据配置: %1 (%2 个字段)")
            .arg(configPath).arg(outFields.size()),
        tr("数据导入"), Qgis::Info);

    return !outFields.isEmpty();
}

// ====================================================================
//  元数据表创建（基于XML配置动态生成SQL）
// ====================================================================

bool CSE_DataImportDialog::EnsureMetadataTable()
{
    QString schema = m_pEditSchema->text().trimmed();
    if (schema.isEmpty()) schema = "public";

    // ========== 从XML配置文件加载字段定义 ==========
    QList<SeMetadataFieldDef> fields;
    if (!LoadMetadataConfig(fields))
    {
        // 加载失败时输出警告并回退，不阻塞导入流程
        QgsMessageLog::logMessage(
            tr("元数据配置加载失败，跳过元数据表创建。"),
            tr("数据导入"), Qgis::Warning);
        return false;
    }

    // ========== 动态拼接 CREATE TABLE SQL ==========
    QStringList columnDefs;
    QStringList pkColumns;

    for (const SeMetadataFieldDef& f : fields)
    {
        QString colDef = QString("  %1 %2").arg(f.name, f.type);

        // 主键
        if (f.primaryKey)
        {
            pkColumns.append(f.name);
        }

        // 默认值
        if (!f.defaultVal.isEmpty())
        {
            colDef += QString(" DEFAULT %1").arg(f.defaultVal);
        }

        // NOT NULL 约束（主键默认 NOT NULL，其他按XML配置）
        if (f.notNull || f.primaryKey)
        {
            colDef += " NOT NULL";
        }

        columnDefs.append(colDef);
    }

    // 主键约束（在字段列表末尾）
    if (!pkColumns.isEmpty())
    {
        columnDefs.append(QString("  PRIMARY KEY (%1)").arg(pkColumns.join(", ")));
    }

    QString sql = QString(
        "CREATE TABLE IF NOT EXISTS %1.gis_metadata (\n%2\n);"
    ).arg(schema, columnDefs.join(",\n"));

    QgsMessageLog::logMessage(
        tr("根据XML配置动态生成元数据表 (%1 个字段)").arg(fields.size()),
        tr("数据导入"), Qgis::Info);

    return ExecuteSql(sql);
}

// ====================================================================
//  栅格元数据提取 — 通过 gdalinfo 解析 TIF/GeoTIFF 的详细属性
// ====================================================================

struct RasterMeta
{
    bool valid = false;
    int  bandCount = 0;
    double resolutionX = 0.0;
    double resolutionY = 0.0;
    int  colCount = 0;
    int  rowCount = 0;
    QString pixelType;
    int  pixelDepth = 0;
    QString compressionType;
    QString colorInterpretation;
    double noDataValue = 0.0;
    bool hasNoData = false;
    QString projectionWkt;
    QString geotransformJson;
    QString bandInfoJson;
    bool hasOverviews = false;
};

static RasterMeta ExtractRasterMeta(const QString& filePath)
{
    RasterMeta meta;
    QFileInfo fi(filePath);
    if (!fi.exists()) return meta;

    QProcess proc;
    proc.start("gdalinfo", { filePath });
    if (!proc.waitForFinished(15000)) return meta;

    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    if (output.isEmpty())
        output = QString::fromUtf8(proc.readAllStandardError());
    if (output.isEmpty()) return meta;

    QStringList lines = output.split('\n');

    double   originX = 0.0, originY = 0.0;
    bool     hasOrigin = false;
    QStringList bandTypes, bandColorInterps;
    bool     inCoordSys = false;
    QString  coordSysWkt;

    for (int i = 0; i < lines.size(); i++)
    {
        QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;

        // ---- Size is W, H ----
        if (line.startsWith("Size is "))
        {
            QStringList parts = line.mid(8).split(',');
            if (parts.size() >= 2)
            {
                meta.colCount = parts[0].trimmed().toInt();
                meta.rowCount = parts[1].trimmed().toInt();
            }
            continue;
        }

        // ---- Pixel Size = (X, Y) ----
        if (line.startsWith("Pixel Size = ("))
        {
            QString content = line.mid(14);
            content.remove(')');
            QStringList parts = content.split(',');
            if (parts.size() >= 2)
            {
                meta.resolutionX = qAbs(parts[0].trimmed().toDouble());
                meta.resolutionY = qAbs(parts[1].trimmed().toDouble());
            }
            continue;
        }

        // ---- Origin = (X, Y) ----
        if (line.startsWith("Origin = ("))
        {
            QString content = line.mid(10);
            content.remove(')');
            QStringList parts = content.split(',');
            if (parts.size() >= 2)
            {
                originX = parts[0].trimmed().toDouble();
                originY = parts[1].trimmed().toDouble();
                hasOrigin = true;
            }
            continue;
        }

        // ---- Band N ... Type=XXX, ColorInterp=YYY ----
        if (line.startsWith("Band ") && line.contains("Type="))
        {
            meta.bandCount++;
            int tp = line.indexOf("Type=");
            if (tp >= 0)
            {
                QString typeStr = line.mid(tp + 5);
                int commaPos = typeStr.indexOf(',');
                if (commaPos >= 0) typeStr = typeStr.left(commaPos);
                QString t = typeStr.trimmed();
                bandTypes << t;
                // 常见类型 → bit 深度
                if (meta.pixelDepth == 0)
                {
                    QString tl = t.toLower();
                    if (tl == "byte")                    meta.pixelDepth = 8;
                    else if (tl == "uint16" || tl == "int16") meta.pixelDepth = 16;
                    else if (tl == "uint32" || tl == "int32" || tl == "float32") meta.pixelDepth = 32;
                    else if (tl == "float64")            meta.pixelDepth = 64;
                }
            }
            int ci = line.indexOf("ColorInterp=");
            if (ci >= 0)
                bandColorInterps << line.mid(ci + 12).trimmed();
            continue;
        }

        // ---- NoData Value ----
        if (line.startsWith("  NoData Value="))
        {
            meta.noDataValue = line.mid(16).trimmed().toDouble();
            meta.hasNoData = true;
            continue;
        }

        // ---- COMPRESSION ----
        if (line.startsWith("  COMPRESSION="))
        {
            meta.compressionType = line.mid(15).trimmed();
            continue;
        }

        // ---- Coordinate System is: (多行 WKT) ----
        if (line == "Coordinate System is:")
        {
            inCoordSys = true;
            continue;
        }
        if (inCoordSys)
        {
            if (line.startsWith("Data axis") || line.startsWith("Origin") ||
                line.startsWith("GCP") || line.startsWith("Metadata"))
            {
                inCoordSys = false;
            }
            else if (!line.startsWith("PROJCS") && !line.startsWith("GEOGCS") && coordSysWkt.isEmpty())
            {
                continue; // 跳过 WKT 之前的空行
            }
            else
            {
                if (!coordSysWkt.isEmpty()) coordSysWkt += "\n";
                coordSysWkt += line;
            }
            continue;
        }

        // ---- Overviews ----
        if (line.startsWith("Overviews:") || line.startsWith("Overview:") ||
            line.startsWith("  Overviews:"))
        {
            meta.hasOverviews = true;
            continue;
        }
    }

    // ---- 后处理 ----
    if (meta.bandCount == 0 && !bandTypes.isEmpty())
        meta.bandCount = bandTypes.size();
    if (!bandTypes.isEmpty())
        meta.pixelType = bandTypes.first();
    if (!bandColorInterps.isEmpty())
        meta.colorInterpretation = bandColorInterps.first();
    meta.projectionWkt = coordSysWkt;

    // 构建 geotransform JSON: [originX, resX, 0, originY, 0, -resY]
    if (hasOrigin && meta.resolutionX > 0)
    {
        meta.geotransformJson = QString("[%1, %2, 0.0, %3, 0.0, %4]")
            .arg(originX, 0, 'f', 10)
            .arg(meta.resolutionX, 0, 'f', 10)
            .arg(originY, 0, 'f', 10)
            .arg(-qAbs(meta.resolutionY), 0, 'f', 10);
    }

    // 构建 band_info JSON
    if (!bandTypes.isEmpty())
    {
        QStringList objs;
        for (int i = 0; i < bandTypes.size(); i++)
        {
            QString ci = (i < bandColorInterps.size()) ? bandColorInterps[i] : "Unknown";
            objs << QString("{\"band\":%1,\"type\":\"%2\",\"colorInterp\":\"%3\"}")
                .arg(i + 1).arg(bandTypes[i]).arg(ci);
        }
        meta.bandInfoJson = "[" + objs.join(",") + "]";
    }

    meta.valid = true;
    return meta;
}

// ====================================================================
//  元数据值解析 — 将 XML 中的 {token} 替换为实际的 SQL 值
// ====================================================================

QString CSE_DataImportDialog::ResolveMetadataValue(const QString& token, const MetadataContext& ctx) const
{
    // ---- 辅助：按类型格式化值 ----
    // 字符串 → PostgreSQL dollar-quote 包裹 ($$...$$)，避免转义问题
    auto fmtStr = [](const QString& s) -> QString {
        if (s.isEmpty()) return QString();
        // 如果字符串中已包含 $$，改用 $tag$...$tag$ 包裹
        if (s.contains("$$"))
            return QString("$md$%1$md$").arg(s);
        return QString("$$%1$$").arg(s);
    };

    // 数值 → 直接输出（或 0 兜底）
    auto fmtNum = [](double v, int precision = 2) -> QString {
        return QString::number(v, 'f', precision);
    };

    // 布尔 → TRUE / FALSE
    auto fmtBool = [](bool v) -> QString {
        return v ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
    };

    // ---- 令牌映射表：{token} → raw value 字符串 ----
    // 返回空字符串表示该令牌在当前上下文中无值（字段将被跳过）
    QString result = token;  // 先复制原始模板

    // 替换 {token} 占位符
    struct TokenEntry { QString token; QString replacement; };
    QList<TokenEntry> replacements;

    // 文件/路径
    replacements.append({"{file_name}",      fmtStr(ctx.fileName)});
    replacements.append({"{file_path}",      fmtStr(ctx.filePath)});
    replacements.append({"{file_size_mb}",   fmtNum(ctx.fileSizeMB)});
    replacements.append({"{table_name}",     fmtStr(ctx.tableName)});

    // 数据类型
    replacements.append({"{data_type}",      fmtStr(ctx.dataType)});
    replacements.append({"{meta_data_type}", fmtStr(ctx.metaDataType)});
    replacements.append({"{srid}",           QString::number(ctx.srid)});
    replacements.append({"{encoding}",       fmtStr(ctx.encoding)});
    replacements.append({"{data_source}",    fmtStr(ctx.dataSource.isEmpty() ? QStringLiteral("未知") : ctx.dataSource)});
    replacements.append({"{description}",    fmtStr(ctx.description.isEmpty() ? QStringLiteral("无") : ctx.description)});
    replacements.append({"{upload_user}",    fmtStr(ctx.uploadUser.isEmpty() ? QStringLiteral("未知") : ctx.uploadUser)});

    // 矢量字段（矢量数据有值，栅格数据为 0/空/Unknown）
    replacements.append({"{feature_count}",       fmtNum(ctx.featureCount, 0)});
    replacements.append({"{geom_type}",           fmtStr(ctx.geometryType)});
    replacements.append({"{west_longitude}",      fmtNum(ctx.westLongitude, 10)});
    replacements.append({"{east_longitude}",      fmtNum(ctx.eastLongitude, 10)});
    replacements.append({"{south_latitude}",      fmtNum(ctx.southLatitude, 10)});
    replacements.append({"{north_latitude}",      fmtNum(ctx.northLatitude, 10)});
    replacements.append({"{field_count}",         QString::number(ctx.fieldCount)});
    replacements.append({"{attribute_fields}",    ctx.attributeFields.isEmpty() ? QString() : fmtStr(ctx.attributeFields)});
    replacements.append({"{has_z_value}",         fmtBool(ctx.hasZValue)});
    replacements.append({"{has_m_value}",         fmtBool(ctx.hasMValue)});
    replacements.append({"{shp_type}",            ctx.shpType.isEmpty() ? QString() : fmtStr(ctx.shpType)});

    // 栅格字段（仅 TIF 导入时有值）
    if (ctx.rasterValid)
    {
        replacements.append({"{band_count}",           QString::number(ctx.bandCount)});
        replacements.append({"{resolution_x}",         fmtNum(ctx.resolutionX, 10)});
        replacements.append({"{resolution_y}",         fmtNum(ctx.resolutionY, 10)});
        replacements.append({"{col_count}",            QString::number(ctx.colCount)});
        replacements.append({"{row_count}",            QString::number(ctx.rowCount)});
        replacements.append({"{pixel_type}",           fmtStr(ctx.pixelType)});
        replacements.append({"{pixel_depth}",          QString::number(ctx.pixelDepth)});
        replacements.append({"{compression_type}",     ctx.compressionType.isEmpty() ? QString() : fmtStr(ctx.compressionType)});
        replacements.append({"{color_interpretation}", ctx.colorInterpretation.isEmpty() ? QString() : fmtStr(ctx.colorInterpretation)});
        replacements.append({"{no_data_value}",        ctx.hasNoData ? fmtNum(ctx.noDataValue, 6) : QString()});
        replacements.append({"{geotransform}",         ctx.geotransform.isEmpty() ? QString() : fmtStr(ctx.geotransform)});
        replacements.append({"{projection_wkt}",       ctx.projectionWkt.isEmpty() ? QString() : fmtStr(ctx.projectionWkt)});
        replacements.append({"{band_info}",            ctx.bandInfo.isEmpty() ? QString() : fmtStr(ctx.bandInfo)});
        replacements.append({"{has_overviews}",        fmtBool(ctx.hasOverviews)});
    }
    else
    {
        // 栅格令牌无值时置空（插入时将跳过该字段）
        replacements.append({"{band_count}",           QString()});
        replacements.append({"{resolution_x}",         QString()});
        replacements.append({"{resolution_y}",         QString()});
        replacements.append({"{col_count}",            QString()});
        replacements.append({"{row_count}",            QString()});
        replacements.append({"{pixel_type}",           QString()});
        replacements.append({"{pixel_depth}",          QString()});
        replacements.append({"{compression_type}",     QString()});
        replacements.append({"{color_interpretation}", QString()});
        replacements.append({"{no_data_value}",        QString()});
        replacements.append({"{geotransform}",         QString()});
        replacements.append({"{projection_wkt}",       QString()});
        replacements.append({"{band_info}",            QString()});
        replacements.append({"{has_overviews}",        QString()});
    }

    // 执行替换
    for (const auto& entry : replacements)
    {
        result.replace(entry.token, entry.replacement);
    }

    // 如果替换后结果为空（所有令牌均无值），返回空字符串 → 跳过该字段
    if (result.trimmed().isEmpty())
        return QString();

    return result;
}

// ====================================================================
//  元数据写入（完全由 XML 配置驱动）
// ====================================================================

bool CSE_DataImportDialog::WriteMetadata(const QString& tableName, const QString& originalFile,
                                          const QString& dataType, qint64 fileSizeBytes)
{
    if (!m_pChkWriteMetadata->isChecked())
        return true;

    // ========== 1. 加载 XML 字段定义 ==========
    QList<SeMetadataFieldDef> fields;
    if (!LoadMetadataConfig(fields))
        return false;

    // ========== 2. 构建 MetadataContext ==========
    bool isRaster = (dataType == "TIF");

    QString schema = m_pEditSchema->text().trimmed();
    if (schema.isEmpty()) schema = "public";

    MetadataContext ctx;
    ctx.fileName    = QFileInfo(originalFile).fileName();
    ctx.filePath    = originalFile;
    ctx.fileSizeMB  = fileSizeBytes / (1024.0 * 1024.0);
    ctx.tableName   = tableName;
    ctx.schema      = schema;
    ctx.dataType    = dataType;
    ctx.dataFormat  = dataType;
    ctx.metaDataType = isRaster ? "raster" : "vector";
    ctx.srid        = GetSrid();
    ctx.encoding    = m_pComboEncoding->currentText();
    ctx.dataSource  = m_pEditDataSource->text().trimmed();
    ctx.description = m_pEditDescription->text().trimmed();
#ifdef Q_OS_WIN
    ctx.uploadUser  = qgetenv("USERNAME");
#else
    ctx.uploadUser  = qgetenv("USER");   // Linux 无 USERNAME 环境变量
#endif

    // --- 矢量：查询 PostGIS 获取统计信息 ---
    if (!isRaster)
    {
        int     fc = 0;
        QString gt = "Unknown";
        QString bboxWkt;
        GetTableInfo(tableName, fc, gt, bboxWkt);

        ctx.featureCount = fc;
        ctx.geometryType = gt;

        // 解析 bbox
        if (!bboxWkt.isEmpty())
        {
            QString c = bboxWkt;
            c.remove("BOX("); c.remove(")");
            c.replace(",", " ");
            QStringList p = c.split(" ", QString::SkipEmptyParts);
            if (p.size() >= 4)
            {
                ctx.westLongitude  = p[0].toDouble();
                ctx.southLatitude  = p[1].toDouble();  // BOX(xmin ymin, xmax ymax)
                ctx.eastLongitude  = p[2].toDouble();
                ctx.northLatitude  = p[3].toDouble();
            }
        }

        // 字段列表
        {
            QString fullTbl = QString("\"%1\".\"%2\"").arg(schema, tableName);
            QString fieldSql = QString(
                "SELECT column_name, data_type FROM information_schema.columns "
                "WHERE table_schema = '%1' AND table_name = '%2' "
                "  AND column_name NOT IN ('geom','rid') "
                "ORDER BY ordinal_position;"
            ).arg(schema, tableName);

            SetPsqlPassword();
            QProcess fp;
            SetupProcessEnv(fp);
            QStringList fargs = BuildPsqlArgs();
            fargs << "-t" << "-A" << "-F" << "|" << "-c" << fieldSql;
            fp.start("psql", fargs);
            if (fp.waitForFinished(10000) && fp.exitCode() == 0)
            {
                QString result = QString::fromUtf8(fp.readAllStandardOutput()).trimmed();
                if (!result.isEmpty())
                {
                    QStringList rows = result.split('\n', QString::SkipEmptyParts);
                    ctx.fieldCount = rows.size();
                    QStringList objs;
                    for (const QString& row : rows)
                    {
                        QStringList parts = row.split('|');
                        if (parts.size() >= 2)
                            objs << QString("{\"name\":\"%1\",\"type\":\"%2\"}")
                                .arg(parts[0].trimmed()).arg(parts[1].trimmed());
                    }
                    if (!objs.isEmpty())
                        ctx.attributeFields = "[" + objs.join(",") + "]";
                }
            }
        }

        // Z / M 检测
        {
            QString fullTbl = QString("\"%1\".\"%2\"").arg(schema, tableName);
            QString zmSql = QString(
                "SELECT DISTINCT ST_NDims(geom) FROM %1 "
                "WHERE geom IS NOT NULL LIMIT 1;"
            ).arg(fullTbl);

            QProcess zp;
            SetupProcessEnv(zp);
            QStringList zargs = BuildPsqlArgs();
            zargs << "-t" << "-A" << "-c" << zmSql;
            zp.start("psql", zargs);
            int ndims = 0;
            if (zp.waitForFinished(10000) && zp.exitCode() == 0)
                ndims = QString::fromUtf8(zp.readAllStandardOutput()).trimmed().toInt();
            ctx.hasZValue = (ndims >= 3);
            ctx.hasMValue = (ndims >= 4);
        }

        // SHP 类型推断
        if (dataType == "SHP")
        {
            QString st = ctx.geometryType;
            if (st.startsWith("ST_")) st = st.mid(3);
            if (ctx.hasZValue) st += " Z";
            if (ctx.hasMValue) st += " M";
            ctx.shpType = st;
        }
    }

    // --- 栅格：gdalinfo 提取 ---
    if (isRaster)
    {
        RasterMeta rm = ExtractRasterMeta(originalFile);
        if (rm.valid)
        {
            ctx.rasterValid          = true;
            ctx.bandCount            = rm.bandCount;
            ctx.resolutionX          = rm.resolutionX;
            ctx.resolutionY          = rm.resolutionY;
            ctx.colCount             = rm.colCount;
            ctx.rowCount             = rm.rowCount;
            ctx.pixelType            = rm.pixelType;
            ctx.pixelDepth           = rm.pixelDepth;
            ctx.compressionType      = rm.compressionType;
            ctx.colorInterpretation  = rm.colorInterpretation;
            ctx.noDataValue          = rm.noDataValue;
            ctx.hasNoData            = rm.hasNoData;
            ctx.geotransform         = rm.geotransformJson;
            ctx.projectionWkt        = rm.projectionWkt;
            ctx.bandInfo             = rm.bandInfoJson;
            ctx.hasOverviews         = rm.hasOverviews;
        }
    }

    // ========== 3. XML 驱动：遍历字段，解析 value 令牌 → 构建 INSERT ==========
    QStringList cols, vals;
    for (const SeMetadataFieldDef& f : fields)
    {
        if (f.value.isEmpty())
            continue;  // 无 value 属性的字段跳过（留给手动填写）

        QString sqlVal = ResolveMetadataValue(f.value, ctx);
        if (sqlVal.isEmpty())
            continue;  // 令牌无法解析（如矢量数据遇到栅格令牌），跳过

        cols << f.name;
        vals << sqlVal;
    }

    if (cols.isEmpty())
    {
        QgsMessageLog::logMessage(
            tr("元数据写入：没有可填充的字段，跳过。"),
            tr("数据导入"), Qgis::Warning);
        return false;
    }

    // ========== 4. 组装并执行 SQL ==========
    QString sql = QString("INSERT INTO %1.gis_metadata (%2) VALUES (%3);")
        .arg(schema)
        .arg(cols.join(", "))
        .arg(vals.join(", "));

    return ExecuteSql(sql);
}

bool CSE_DataImportDialog::GetTableInfo(const QString& tableName, int& featureCount,
                                         QString& geometryType, QString& bboxWkt)
{
    featureCount = 0;
    geometryType = "Unknown";
    bboxWkt.clear();

    // COUNT(*) 和 GeometryType/ST_Extent 只涉及几何元数据，
    // 不读取属性文本，无论源数据是什么编码都能安全执行
    QString schema = m_pEditSchema->text().trimmed();
    if (schema.isEmpty()) schema = "public";
    QString fullTable = QString("\"%1\".\"%2\"").arg(schema, tableName);

    // 获取要素数
    QString countSql = QString("SELECT COUNT(*) FROM %1;").arg(fullTable);
    SetPsqlPassword();

    QProcess proc;
    SetupProcessEnv(proc);
    QStringList psqlArgs = BuildPsqlArgs();
    psqlArgs << "-t" << "-A" << "-c" << countSql;

    proc.start("psql", psqlArgs);
    if (proc.waitForFinished(10000) && proc.exitCode() == 0)
    {
        QString result = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        featureCount = result.toInt();
    }

    // 获取几何类型和范围（仅矢量数据），分开查询避免聚合函数 GROUP BY 问题
    QString geomTypeSql = QString(
        "SELECT GeometryType(geom) FROM %1 "
        "WHERE geom IS NOT NULL LIMIT 1;"
    ).arg(fullTable);

    QProcess proc2;
    SetupProcessEnv(proc2);
    QStringList psqlArgs2 = BuildPsqlArgs();
    psqlArgs2 << "-t" << "-A" << "-c" << geomTypeSql;

    proc2.start("psql", psqlArgs2);
    if (proc2.waitForFinished(10000) && proc2.exitCode() == 0)
    {
        geometryType = QString::fromUtf8(proc2.readAllStandardOutput()).trimmed();
    }

    // 获取空间范围
    QString bboxSql = QString(
        "SELECT ST_AsText(ST_Extent(geom)) FROM %1 "
        "WHERE geom IS NOT NULL;"
    ).arg(fullTable);

    QProcess proc3;
    SetupProcessEnv(proc3);
    QStringList psqlArgs3 = BuildPsqlArgs();
    psqlArgs3 << "-t" << "-A" << "-c" << bboxSql;

    proc3.start("psql", psqlArgs3);
    if (proc3.waitForFinished(10000) && proc3.exitCode() == 0)
    {
        bboxWkt = QString::fromUtf8(proc3.readAllStandardOutput()).trimmed();
    }

    return true;
}

bool CSE_DataImportDialog::ExecuteSql(const QString& sql)
{
    // 将 SQL 写入临时 UTF-8 文件，避免 Windows 命令行传参时编码被转为 GBK
    QString tempFile = QDir::temp().absoluteFilePath("qgis_import_temp.sql");
    QFile f(tempFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QgsMessageLog::logMessage(tr("无法创建临时 SQL 文件"), tr("数据导入"), Qgis::Warning);
        return false;
    }
    QTextStream ts(&f);
    ts.setCodec("UTF-8");
    ts << sql;
    f.close();

    SetPsqlPassword();

    QProcess proc;
    SetupProcessEnv(proc);
    QStringList psqlArgs = BuildPsqlArgs();
    psqlArgs << "-f" << tempFile;

    proc.start("psql", psqlArgs);
    if (!proc.waitForStarted(5000))
    {
        f.remove();
        QgsMessageLog::logMessage(tr("无法启动 psql"), tr("数据导入"), Qgis::Warning);
        return false;
    }

    if (!proc.waitForFinished(30000))
    {
        f.remove();
        QgsMessageLog::logMessage(tr("psql 执行超时"), tr("数据导入"), Qgis::Warning);
        return false;
    }

    f.remove();

    if (proc.exitCode() != 0)
    {
        QString err = QString::fromUtf8(proc.readAllStandardError());
        if (!err.isEmpty())
        {
            QgsMessageLog::logMessage(tr("SQL 执行失败: %1").arg(err),
                tr("数据导入"), Qgis::Warning);
        }
        return false;
    }

    return true;
}

void CSE_DataImportDialog::UpdateStatus(const QString& message)
{
    m_pLabelStatus->setText(message);
}

void CSE_DataImportDialog::UpdateProgress(int current, int total)
{
    if (total > 0)
    {
        m_pProgressBar->setRange(0, total);
        m_pProgressBar->setValue(current);
    }
}

// ====================================================================
//  槽函数
// ====================================================================

void CSE_DataImportDialog::slotBrowse()
{
    QString curPath = QCoreApplication::applicationDirPath();

    if (m_pRadioFolder->isChecked())
    {
        // 选择文件夹
        QString dir = QFileDialog::getExistingDirectory(
            this,
            tr("选择包含数据的文件夹"),
            curPath,
            QFileDialog::ShowDirsOnly);
        if (!dir.isEmpty())
            m_pEditDataPath->setText(dir);
    }
    else if (m_pRadioGdb->isChecked())
    {
        // 选择 GDB 目录
        QString dir = QFileDialog::getExistingDirectory(
            this,
            tr("选择 File Geodatabase 目录 (.gdb)"),
            curPath,
            QFileDialog::ShowDirsOnly);
        if (!dir.isEmpty())
        {
            m_pEditDataPath->setText(dir);
            // 自动刷新图层列表
            slotRefreshGdbLayers();
        }
    }
    else
    {
        // 选择文件
        QString filter = GetFileFilter();
        QString filePath = QFileDialog::getOpenFileName(
            this,
            tr("选择数据文件"),
            curPath,
            filter);
        if (!filePath.isEmpty())
        {
            m_pEditDataPath->setText(filePath);

            // 自动填充表名
            if (m_pEditTableName->text().isEmpty())
                m_pEditTableName->setText(DefaultTableName(filePath));
        }
    }
}

void CSE_DataImportDialog::slotRefreshGdbLayers()
{
    m_pListGdbLayers->clear();

    QString gdbPath = m_pEditDataPath->text().trimmed();
    if (gdbPath.isEmpty()) return;

    QFileInfo fi(gdbPath);
    if (!fi.exists() || !fi.isDir())
    {
        QMessageBox::warning(this, tr("GDB 图层"),
            tr("请先选择有效的 File Geodatabase 目录 (.gdb)。"));
        return;
    }

    UpdateStatus(tr("正在读取 GDB 图层列表..."));
    QApplication::processEvents();

    QStringList layers = ListGdbLayers(gdbPath);

    if (layers.isEmpty())
    {
        QMessageBox::warning(this, tr("GDB 图层"),
            tr("未能从 %1 中读取到任何图层。\n"
               "请确认该目录是有效的 File Geodatabase。").arg(gdbPath));
        UpdateStatus(tr("就绪"));
        return;
    }

    for (const QString& layer : layers)
    {
        QListWidgetItem* item = new QListWidgetItem(layer, m_pListGdbLayers);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);  // 默认全选
    }

    UpdateStatus(tr("已读取 %1 个图层").arg(layers.size()));
    QgsMessageLog::logMessage(
        tr("GDB 图层列表 (%1): %2").arg(gdbPath).arg(layers.join(", ")),
        tr("数据导入"), Qgis::Info);
}

void CSE_DataImportDialog::slotTestConnection()
{
    QString connStr = BuildPGConnString();

    // 使用 ogrinfo 测试连接
    QProcess proc;
    SetupProcessEnv(proc);
    proc.start("ogrinfo", { connStr, "-so" });

    if (!proc.waitForStarted(5000))
    {
        QMessageBox::warning(this, tr("测试连接"),
            tr("无法启动 ogrinfo。\n请确认 GDAL/OGR 已正确安装并配置环境变量。"));
        return;
    }

    if (!proc.waitForFinished(10000))
    {
        QMessageBox::warning(this, tr("测试连接"),
            tr("连接超时。请检查主机地址和端口是否正确。"));
        proc.kill();
        return;
    }

    QString stdOut = QString::fromUtf8(proc.readAllStandardOutput());
    QString stdErr = QString::fromUtf8(proc.readAllStandardError());

    if (proc.exitCode() == 0)
    {
        QMessageBox::information(this, tr("测试连接"),
            tr("✓ 数据库连接成功！"));
    }
    else
    {
        QString errMsg = stdErr.isEmpty() ? stdOut : stdErr;
        QMessageBox::critical(this, tr("测试连接"),
            tr("✗ 数据库连接失败！\n\n%1").arg(errMsg.trimmed()));
    }
}

// ====================================================================
//  开始导入（核心逻辑）
// ====================================================================

void CSE_DataImportDialog::slotStartImport()
{
    // ========== 验证输入 ==========

    // 数据库名必填
    QString database = m_pEditDatabase->text().trimmed();
    if (database.isEmpty())
    {
        QMessageBox::warning(this, tr("导入数据"),
            tr("请输入数据库名称。"));
        m_pEditDatabase->setFocus();
        return;
    }

    // 数据路径必填
    QString dataPath = m_pEditDataPath->text().trimmed();
    if (dataPath.isEmpty())
    {
        QMessageBox::warning(this, tr("导入数据"),
            tr("请选择要导入的数据文件或文件夹。"));
        m_pBtnBrowse->setFocus();
        return;
    }

    // GDB 模式：检查是否选择了图层
    if (m_pRadioGdb->isChecked())
    {
        int checkedCount = 0;
        for (int i = 0; i < m_pListGdbLayers->count(); i++)
        {
            if (m_pListGdbLayers->item(i)->checkState() == Qt::Checked)
                checkedCount++;
        }
        if (checkedCount == 0)
        {
            QMessageBox::warning(this, tr("导入数据"),
                tr("请在 GDB 图层列表中勾选至少一个要导入的图层。"));
            return;
        }
    }

    // 文件夹模式：检查是否有支持的文件
    if (m_pRadioFolder->isChecked())
    {
        QStringList files = ScanSupportedFiles(dataPath);
        if (files.isEmpty())
        {
            QMessageBox::warning(this, tr("导入数据"),
                tr("所选文件夹中没有找到支持的数据文件。\n"
                   "支持的格式：SHP、GeoJSON、GPKG、GDB、TIF（含子文件夹递归扫描）"));
            return;
        }
    }
    else
    {
        // 文件模式：检查是否存在
        QFileInfo fi(dataPath);
        if (!fi.exists())
        {
            QMessageBox::warning(this, tr("导入数据"),
                tr("数据文件不存在：%1").arg(dataPath));
            return;
        }
    }

    // ========== 确保元数据表存在 ==========
    if (m_pChkWriteMetadata->isChecked())
    {
        UpdateStatus(tr("检查元数据表..."));
        QApplication::processEvents();
        if (!EnsureMetadataTable())
        {
            QgsMessageLog::logMessage(tr("元数据表创建/检查失败，将继续导入但跳过元数据写入。"),
                tr("数据导入"), Qgis::Warning);
        }
    }

    // ========== 禁用UI ==========
    m_pBtnImport->setEnabled(false);
    m_pBtnCancel->setVisible(true);
    m_pBtnCancel->setEnabled(true);
    m_pBtnTestConn->setEnabled(false);
    m_pBtnBrowse->setEnabled(false);
    m_pBtnRefreshLayers->setEnabled(false);
    m_pProgressBar->setValue(0);

    // 重置计数器
    m_bCancelBatchImport = false;
    m_iTotalItems = 0;
    m_iSuccessCount = 0;
    m_iFailCount = 0;

    // ========== 按数据类型分支 ==========

    // --- GDB 导入 ---
    if (m_pRadioGdb->isChecked())
    {
        QString gdbPath = dataPath;
        QString schema = m_pEditSchema->text().trimmed();
        if (schema.isEmpty()) schema = "public";

        // 收集勾选的图层
        QStringList selectedLayers;
        for (int i = 0; i < m_pListGdbLayers->count(); i++)
        {
            QListWidgetItem* item = m_pListGdbLayers->item(i);
            if (item->checkState() == Qt::Checked)
                selectedLayers << item->text();
        }

        m_iTotalItems = selectedLayers.size();

        for (int i = 0; i < selectedLayers.size(); i++)
        {
            if (m_bCancelBatchImport) break;

            const QString& layerName = selectedLayers[i];
            QString tblName = DefaultTableName(layerName);

            UpdateProgress(i, m_iTotalItems);
            UpdateStatus(tr("[%1/%2] 正在导入 GDB 图层: %3")
                .arg(i + 1).arg(m_iTotalItems).arg(layerName));
            QApplication::processEvents();

            // 构建带图层名的 ogr2ogr 参数
            QStringList args;
            args << "-f" << "PostgreSQL";
            args << BuildPGConnStringForImport();
            args << "-nln" << tblName;

            int srid = GetSrid();
            if (srid > 0)
                args << "-t_srs" << QString("EPSG:%1").arg(srid);

            args << "-lco" << "GEOMETRY_NAME=geom";

            // 编码：通过 SHAPE_ENCODING 告诉 GDAL 源文件编码
            QString encoding = m_pComboEncoding->currentText();
            if (!encoding.isEmpty())
            {
                args << "--config" << "SHAPE_ENCODING" << encoding;
                args << "--config" << "OGR_FORCE_ASCII" << "NO";
            }

            if (m_pChkOverwrite->isChecked())
                args << "-overwrite";

            // 日期字段先按 VARCHAR 导入，避免 0000/00/00 被 PostgreSQL 拒绝
            {
                QStringList dateFlds = ScanDateFields(gdbPath, layerName);
                if (!dateFlds.isEmpty())
                {
                    QStringList colTypes;
                    for (const QString& fld : dateFlds)
                        colTypes << QString("%1=VARCHAR").arg(fld);
                    args << "-lco" << QString("COLUMN_TYPES=%1").arg(colTypes.join(","));
                }
            }

            args << "-skipfailures";
            args << "-progress";
            args << "--config" << "GDAL_SKIP" << "FileGDB";  // 使用跨平台OpenFileGDB驱动
            args << gdbPath;       // GDB 目录
            args << layerName;     // 指定图层名

            QProcess proc;
            SetupProcessEnv(proc);
            proc.start("ogr2ogr", args);

            if (proc.waitForStarted(5000)
                && proc.waitForFinished(300000)
                && proc.exitCode() == 0)
            {
                m_iSuccessCount++;
                QgsMessageLog::logMessage(
                    tr("GDB 图层导入成功: %1 → %2.%3")
                        .arg(layerName).arg(schema).arg(tblName),
                    tr("数据导入"), Qgis::Info);

                // 写入元数据（递归计算GDB目录总大小）
                if (m_pChkWriteMetadata->isChecked())
                {
                    qint64 gdbSize = 0;
                    QDirIterator it(gdbPath, QDir::Files, QDirIterator::Subdirectories);
                    while (it.hasNext()) { it.next(); gdbSize += it.fileInfo().size(); }
                    WriteMetadata(tblName, gdbPath + "/" + layerName,
                                  "GDB", gdbSize);
                }

                // 修复日期列（0000/00/00 → NULL）——在加载到地图之前
                FixDateColumns(schema, tblName, gdbPath, layerName);

                // 加载到地图
                if (m_pChkLoadAfter->isChecked())
                {
                    QString uri = BuildDataSourceUri(schema, tblName);
                    QgsVectorLayer* layer = new QgsVectorLayer(uri, tblName, "postgres");
                    if (layer->isValid())
                        QgsProject::instance()->addMapLayer(layer);
                    else
                        delete layer;
                }
            }
            else
            {
                m_iFailCount++;
                QString err = QString::fromUtf8(proc.readAllStandardError());
                QgsMessageLog::logMessage(
                    tr("GDB 图层导入失败: %1\n%2").arg(layerName).arg(err),
                    tr("数据导入"), Qgis::Warning);
            }
        }

        m_pProgressBar->setValue(m_iTotalItems);

        if (m_bCancelBatchImport)
        {
            UpdateStatus(tr("已停止: 成功 %1, 失败 %2, 剩余未导入")
                .arg(m_iSuccessCount).arg(m_iFailCount));
            QMessageBox::information(this, tr("导入数据"),
                tr("导入已停止。\n\n成功: %1 个图层\n失败: %2 个图层\n\n"
                   "剩余图层未导入。").arg(m_iSuccessCount).arg(m_iFailCount));
        }
        else
        {
            UpdateStatus(tr("完成: 成功 %1, 失败 %2").arg(m_iSuccessCount).arg(m_iFailCount));
            QMessageBox::information(this, tr("导入数据"),
                tr("GDB 批量导入完成！\n\n成功: %1 个图层\n失败: %2 个图层\n\n"
                   "详情请查看日志消息面板。").arg(m_iSuccessCount).arg(m_iFailCount));
        }

        // 恢复UI
        m_pBtnCancel->setVisible(false);
        m_pBtnImport->setEnabled(true);
        m_pBtnTestConn->setEnabled(true);
        m_pBtnBrowse->setEnabled(true);
        m_pBtnRefreshLayers->setEnabled(true);
    }

    // --- TIF 栅格导入 ---
    else if (m_pRadioTif->isChecked())
    {
        QString tblName = m_pEditTableName->text().trimmed().toLower();
        if (tblName.isEmpty())
            tblName = DefaultTableName(dataPath);

        m_iTotalItems = 1;

        UpdateStatus(tr("正在导入栅格: %1").arg(QFileInfo(dataPath).fileName()));
        QApplication::processEvents();

        // 拼接 raster2pgsql | psql 管道命令
        SetPsqlPassword();

        QProcess pipeProc;
        SetupProcessEnv(pipeProc);

        // Windows 下用 cmd /c 来执行管道
        QStringList rasterArgs = BuildRaster2pgsqlArgs(dataPath, tblName);
        QString rasterCmd = "raster2pgsql";
        for (const QString& a : rasterArgs)
            rasterCmd += " \"" + a + "\"";

        QString psqlArgs;
        {
            QString host     = m_pEditHost->text().trimmed();
            int     port     = m_pSpinPort->value();
            QString database = m_pEditDatabase->text().trimmed();
            QString username = m_pEditUsername->text().trimmed();

            psqlArgs = QString("-h %1 -p %2 -d %3 -U %4 -w --no-psqlrc")
                .arg(host.isEmpty() ? "localhost" : host)
                .arg(port)
                .arg(database.isEmpty() ? "gis_db" : database)
                .arg(username.isEmpty() ? "postgres" : username);
        }

        QString fullCmd = rasterCmd + " | psql " + psqlArgs;

#ifdef Q_OS_WIN
        pipeProc.start("cmd", { "/c", fullCmd });
#else
        pipeProc.start("sh", { "-c", fullCmd });
#endif

        QFileInfo fi(dataPath);
        qint64 fileSize = fi.size();

        if (pipeProc.waitForStarted(5000)
            && pipeProc.waitForFinished(600000)  // 栅格通常较大，最多等10分钟
            && pipeProc.exitCode() == 0)
        {
            m_iSuccessCount = 1;
            m_pProgressBar->setValue(100);
            UpdateStatus(tr("栅格导入完成"));

            QString stdOut = QString::fromUtf8(pipeProc.readAllStandardOutput());
            QgsMessageLog::logMessage(
                tr("栅格导入成功: %1 → %2\n%3").arg(dataPath).arg(tblName).arg(stdOut),
                tr("数据导入"), Qgis::Info);

            // 写入元数据
            if (m_pChkWriteMetadata->isChecked())
                WriteMetadata(tblName, dataPath, "TIF", fileSize);

            QMessageBox::information(this, tr("导入数据"),
                tr("栅格数据导入成功！\n表名: %1").arg(tblName));
        }
        else
        {
            m_iFailCount = 1;
            m_pProgressBar->setValue(0);
            UpdateStatus(tr("栅格导入失败"));

            QString errMsg = QString::fromUtf8(pipeProc.readAllStandardError());
            if (errMsg.isEmpty())
                errMsg = QString::fromUtf8(pipeProc.readAllStandardOutput());
            QMessageBox::critical(this, tr("导入数据"),
                tr("栅格导入失败！\n\n%1").arg(errMsg.trimmed()));
            QgsMessageLog::logMessage(
                tr("栅格导入失败: %1").arg(errMsg), tr("数据导入"), Qgis::Warning);
        }

        // 恢复UI
        m_pBtnCancel->setVisible(false);
        m_pBtnImport->setEnabled(true);
        m_pBtnTestConn->setEnabled(true);
        m_pBtnBrowse->setEnabled(true);
        m_pBtnRefreshLayers->setEnabled(true);
    }

    // --- 文件夹批量导入 ---
    else if (m_pRadioFolder->isChecked())
    {
        QStringList files = ScanSupportedFiles(dataPath);
        m_iTotalItems = files.size();
        QString schema = m_pEditSchema->text().trimmed();
        if (schema.isEmpty()) schema = "public";

        // 表名去重：不同子文件夹下的同名文件（如各图幅都有 dltb.shp）追加序号，避免互相覆盖
        QSet<QString> usedTableNames;
        auto uniqueTableName = [&usedTableNames](const QString& base) -> QString {
            QString name = base;
            int n = 2;
            while (usedTableNames.contains(name))
                name = QString("%1_%2").arg(base).arg(n++);
            usedTableNames.insert(name);
            return name;
        };

        QDir rootDir(dataPath);
        QStringList importedVectorTables;  // 成功导入的矢量表，导入后按实际表名加载地图

        for (int i = 0; i < files.size(); i++)
        {
            if (m_bCancelBatchImport) break;

            const QString& file = files[i];
            QFileInfo fi(file);
            QString tblName = uniqueTableName(DefaultTableName(file));
            bool isTif = file.toLower().endsWith(".tif") || file.toLower().endsWith(".tiff");

            UpdateProgress(i, m_iTotalItems);
            UpdateStatus(tr("[%1/%2] 正在导入: %3")
                .arg(i + 1).arg(m_iTotalItems).arg(rootDir.relativeFilePath(file)));
            QApplication::processEvents();

            if (isTif)
            {
                // 栅格用 raster2pgsql
                SetPsqlPassword();
                QProcess pipeProc;
                SetupProcessEnv(pipeProc);

                QStringList rArgs = BuildRaster2pgsqlArgs(file, tblName);
                QString rasterCmd = "raster2pgsql";
                for (const QString& a : rArgs)
                    rasterCmd += " \"" + a + "\"";

                QString psqlCmdPart;
                {
                    QString host     = m_pEditHost->text().trimmed();
                    int     port     = m_pSpinPort->value();
                    QString database = m_pEditDatabase->text().trimmed();
                    QString username = m_pEditUsername->text().trimmed();
                    psqlCmdPart = QString("-h %1 -p %2 -d %3 -U %4 -w --no-psqlrc")
                        .arg(host.isEmpty() ? "localhost" : host)
                        .arg(port)
                        .arg(database.isEmpty() ? "gis_db" : database)
                        .arg(username.isEmpty() ? "postgres" : username);
                }

                QString fullCmd = rasterCmd + " | psql " + psqlCmdPart;
#ifdef Q_OS_WIN
                pipeProc.start("cmd", { "/c", fullCmd });
#else
                pipeProc.start("sh", { "-c", fullCmd });
#endif

                if (pipeProc.waitForStarted(5000)
                    && pipeProc.waitForFinished(600000)
                    && pipeProc.exitCode() == 0)
                {
                    m_iSuccessCount++;
                    QgsMessageLog::logMessage(
                        tr("导入成功: %1 → %2").arg(fi.fileName()).arg(tblName),
                        tr("数据导入"), Qgis::Info);
                    if (m_pChkWriteMetadata->isChecked())
                        WriteMetadata(tblName, file, "TIF", fi.size());
                }
                else
                {
                    m_iFailCount++;
                    QString err = QString::fromUtf8(pipeProc.readAllStandardError());
                    QgsMessageLog::logMessage(
                        tr("导入失败: %1\n%2").arg(fi.fileName()).arg(err),
                        tr("数据导入"), Qgis::Warning);
                }
            }
            else if (file.toLower().endsWith(".gdb"))
            {
                // GDB 目录：列出所有图层并逐个导入
                QStringList gdbLayers = ListGdbLayers(file);
                if (gdbLayers.isEmpty())
                {
                    m_iFailCount++;
                    QgsMessageLog::logMessage(
                        tr("GDB 无图层或无法读取: %1").arg(fi.fileName()),
                        tr("数据导入"), Qgis::Warning);
                    continue;
                }

                // 计算 GDB 目录总大小（用于元数据）
                qint64 gdbSize = 0;
                QDirIterator it(file, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) { it.next(); gdbSize += it.fileInfo().size(); }

                for (const QString& layerName : gdbLayers)
                {
                    if (m_bCancelBatchImport) break;

                    QString gdbTblName = uniqueTableName(DefaultTableName(layerName));
                    UpdateStatus(tr("[%1/%2] 正在导入 GDB 图层: %3")
                        .arg(i + 1).arg(m_iTotalItems).arg(layerName));
                    QApplication::processEvents();

                    QStringList args;
                    args << "-f" << "PostgreSQL";
                    args << BuildPGConnStringForImport();
                    args << "-nln" << gdbTblName;

                    int srid = GetSrid();
                    if (srid > 0)
                        args << "-t_srs" << QString("EPSG:%1").arg(srid);

                    args << "-lco" << "GEOMETRY_NAME=geom";

                    QString encoding = m_pComboEncoding->currentText();
                    if (!encoding.isEmpty())
                    {
                        args << "--config" << "SHAPE_ENCODING" << encoding;
                        args << "--config" << "OGR_FORCE_ASCII" << "NO";
                    }

                    if (m_pChkOverwrite->isChecked())
                        args << "-overwrite";

                    // 日期字段先按 VARCHAR 导入，避免 0000/00/00 被 PostgreSQL 拒绝
                    {
                        QStringList dateFlds = ScanDateFields(file, layerName);
                        if (!dateFlds.isEmpty())
                        {
                            QStringList colTypes;
                            for (const QString& fld : dateFlds)
                                colTypes << QString("%1=VARCHAR").arg(fld);
                            args << "-lco" << QString("COLUMN_TYPES=%1").arg(colTypes.join(","));
                        }
                    }

                    args << "-skipfailures";
                    args << "-progress";
                    args << "--config" << "GDAL_SKIP" << "FileGDB";  // 使用跨平台OpenFileGDB驱动
                    args << file;        // GDB 目录
                    args << layerName;   // 指定图层

                    QProcess proc;
                    SetupProcessEnv(proc);
                    proc.start("ogr2ogr", args);

                    if (proc.waitForStarted(5000)
                        && proc.waitForFinished(300000)
                        && proc.exitCode() == 0)
                    {
                        m_iSuccessCount++;
                        QgsMessageLog::logMessage(
                            tr("GDB 图层导入成功: %1 → %2")
                                .arg(layerName).arg(gdbTblName),
                            tr("数据导入"), Qgis::Info);
                        if (m_pChkWriteMetadata->isChecked())
                            WriteMetadata(gdbTblName, file + "/" + layerName,
                                          "GDB", gdbSize);

                        // 修复日期列（0000/00/00 → NULL）——在加载到地图之前
                        FixDateColumns(schema, gdbTblName, file, layerName);

                        // 加载到地图
                        if (m_pChkLoadAfter->isChecked())
                        {
                            QString uri = BuildDataSourceUri(schema, gdbTblName);
                            QgsVectorLayer* layer = new QgsVectorLayer(uri, gdbTblName, "postgres");
                            if (layer->isValid())
                                QgsProject::instance()->addMapLayer(layer);
                            else
                                delete layer;
                        }
                    }
                    else
                    {
                        m_iFailCount++;
                        QString err = QString::fromUtf8(proc.readAllStandardError());
                        QgsMessageLog::logMessage(
                            tr("GDB 图层导入失败: %1\n%2").arg(layerName).arg(err),
                            tr("数据导入"), Qgis::Warning);
                    }
                }
            }
            else
            {
                // 矢量用 ogr2ogr
                QStringList dateFields = ScanDateFields(file);
                QStringList args = BuildOgr2ogrArgs(file, tblName, dateFields);
                QProcess proc;
                SetupProcessEnv(proc);
                proc.start("ogr2ogr", args);

                if (proc.waitForStarted(5000)
                    && proc.waitForFinished(300000)
                    && proc.exitCode() == 0)
                {
                    m_iSuccessCount++;
                    QgsMessageLog::logMessage(
                        tr("导入成功: %1 → %2").arg(fi.fileName()).arg(tblName),
                        tr("数据导入"), Qgis::Info);
                    if (m_pChkWriteMetadata->isChecked())
                    {
                        QString dtype = "UNKNOWN";
                        if (file.toLower().endsWith(".shp"))      dtype = "SHP";
                        else if (file.toLower().endsWith(".gpkg")) dtype = "GPKG";
                        else if (file.toLower().contains("geojson") || file.toLower().endsWith(".json"))
                            dtype = "GeoJSON";
                        WriteMetadata(tblName, file, dtype, fi.size());
                    }

                    // 修复日期列（0000/00/00 → NULL）
                    FixDateColumns(schema, tblName, file);

                    importedVectorTables << tblName;
                }
                else
                {
                    m_iFailCount++;
                    QString err = QString::fromUtf8(proc.readAllStandardError());
                    QgsMessageLog::logMessage(
                        tr("导入失败: %1\n%2").arg(fi.fileName()).arg(err),
                        tr("数据导入"), Qgis::Warning);
                }
            }
        }

        m_pProgressBar->setValue(m_iTotalItems);

        if (m_bCancelBatchImport)
        {
            UpdateStatus(tr("已停止: 成功 %1, 失败 %2, 剩余未导入")
                .arg(m_iSuccessCount).arg(m_iFailCount));
        }
        else
        {
            UpdateStatus(tr("完成: 成功 %1, 失败 %2").arg(m_iSuccessCount).arg(m_iFailCount));
        }

        // 加载到地图（取消时仍加载已成功部分）
        // 按实际导入成功的表名加载（去重后表名可能带 _2 后缀，不能按文件名重算）
        if (m_pChkLoadAfter->isChecked() && !importedVectorTables.isEmpty())
        {
            QgsProject* prj = QgsProject::instance();
            for (const QString& tblName : importedVectorTables)
            {
                QString uri = BuildDataSourceUri(schema, tblName);
                QgsVectorLayer* layer = new QgsVectorLayer(uri, tblName, "postgres");
                if (layer->isValid())
                    prj->addMapLayer(layer);
                else
                    delete layer;
            }
        }

        if (m_bCancelBatchImport)
        {
            QMessageBox::information(this, tr("导入数据"),
                tr("导入已停止。\n\n成功: %1 个\n失败: %2 个\n\n"
                   "剩余文件未导入。").arg(m_iSuccessCount).arg(m_iFailCount));
        }
        else
        {
            QMessageBox::information(this, tr("导入数据"),
                tr("批量导入完成！\n\n成功: %1 个\n失败: %2 个\n\n"
                   "详情请查看日志消息面板。").arg(m_iSuccessCount).arg(m_iFailCount));
        }

        // 恢复UI
        m_pBtnCancel->setVisible(false);
        m_pBtnImport->setEnabled(true);
        m_pBtnTestConn->setEnabled(true);
        m_pBtnBrowse->setEnabled(true);
        m_pBtnRefreshLayers->setEnabled(true);
    }
    else
    {
        // ========== 单文件导入（异步） ==========
        QString tableName = m_pEditTableName->text().trimmed().toLower();
        if (tableName.isEmpty())
            tableName = DefaultTableName(dataPath);

        m_qstrCurrentTableName = tableName;
        m_iTotalItems = 1;

        if (m_pRadioTif->isChecked())
        {
            // TIF 单文件同步导入
            UpdateStatus(tr("正在导入栅格: %1").arg(QFileInfo(dataPath).fileName()));
            QApplication::processEvents();

            SetPsqlPassword();

            QProcess pipeProc;
            SetupProcessEnv(pipeProc);

            QStringList rArgs = BuildRaster2pgsqlArgs(dataPath, tableName);
            QString rasterCmd = "raster2pgsql";
            for (const QString& a : rArgs)
                rasterCmd += " \"" + a + "\"";

            QString psqlCmdPart;
            {
                QString host     = m_pEditHost->text().trimmed();
                int     port     = m_pSpinPort->value();
                QString database = m_pEditDatabase->text().trimmed();
                QString username = m_pEditUsername->text().trimmed();
                psqlCmdPart = QString("-h %1 -p %2 -d %3 -U %4 -w --no-psqlrc")
                    .arg(host.isEmpty() ? "localhost" : host)
                    .arg(port)
                    .arg(database.isEmpty() ? "gis_db" : database)
                    .arg(username.isEmpty() ? "postgres" : username);
            }

            QString fullCmd = rasterCmd + " | psql " + psqlCmdPart;
#ifdef Q_OS_WIN
            pipeProc.start("cmd", { "/c", fullCmd });
#else
            pipeProc.start("sh", { "-c", fullCmd });
#endif

            if (pipeProc.waitForStarted(5000)
                && pipeProc.waitForFinished(600000)
                && pipeProc.exitCode() == 0)
            {
                m_iSuccessCount = 1;
                m_pProgressBar->setValue(100);
                UpdateStatus(tr("栅格导入完成"));

                QString stdOut = QString::fromUtf8(pipeProc.readAllStandardOutput());
                QgsMessageLog::logMessage(
                    tr("栅格导入成功: %1 → %2\n%3").arg(dataPath).arg(tableName).arg(stdOut),
                    tr("数据导入"), Qgis::Info);

                if (m_pChkWriteMetadata->isChecked())
                {
                    QFileInfo fi(dataPath);
                    WriteMetadata(tableName, dataPath, "TIF", fi.size());
                }

                QMessageBox::information(this, tr("导入数据"),
                    tr("栅格数据导入成功！\n表名: %1").arg(tableName));
            }
            else
            {
                m_iFailCount = 1;
                m_pProgressBar->setValue(0);
                UpdateStatus(tr("栅格导入失败"));

                QString errMsg = QString::fromUtf8(pipeProc.readAllStandardError());
                if (errMsg.isEmpty())
                    errMsg = QString::fromUtf8(pipeProc.readAllStandardOutput());
                QMessageBox::critical(this, tr("导入数据"),
                    tr("栅格导入失败！\n\n%1").arg(errMsg.trimmed()));
            }

            // 恢复UI
            m_pBtnImport->setEnabled(true);
            m_pBtnTestConn->setEnabled(true);
            m_pBtnBrowse->setEnabled(true);
            m_pBtnRefreshLayers->setEnabled(true);
        }
        else
        {
            // 矢量单文件异步导入
            QStringList dateFields = ScanDateFields(dataPath);
            QStringList args = BuildOgr2ogrArgs(dataPath, tableName, dateFields);

            if (!m_pProcess)
            {
                m_pProcess = new QProcess(this);
                connect(m_pProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        this, &CSE_DataImportDialog::slotProcessFinished);
                connect(m_pProcess, &QProcess::errorOccurred,
                        this, &CSE_DataImportDialog::slotProcessError);
            }

            UpdateStatus(tr("正在启动 ogr2ogr 导入..."));
            m_pProgressBar->setRange(0, 0);  // 不确定进度

            SetupProcessEnv(*m_pProcess);
            m_pProcess->start("ogr2ogr", args);
        }
    }

    SaveSettings();
}

void CSE_DataImportDialog::slotOpenMetadataViewer()
{
    QString host     = m_pEditHost->text().trimmed();
    int     port     = m_pSpinPort->value();
    QString database = m_pEditDatabase->text().trimmed();
    QString username = m_pEditUsername->text().trimmed();
    QString password = m_pEditPassword->text();
    QString schema   = m_pEditSchema->text().trimmed();

    // 数据库名为空时给出提示
    if (database.isEmpty())
    {
        QMessageBox::warning(this, tr("元数据管理"),
            tr("请先填写数据库名称。"));
        m_pEditDatabase->setFocus();
        return;
    }

    CSEMetadataViewerDialog* pViewer = new CSEMetadataViewerDialog(
        host, port, database, username, password,
        schema.isEmpty() ? "public" : schema,
        nullptr);
    pViewer->setAttribute(Qt::WA_DeleteOnClose);
    pViewer->setModal(false);
    pViewer->show();
}

void CSE_DataImportDialog::slotCancelBatchImport()
{
    m_bCancelBatchImport = true;
    m_pBtnCancel->setEnabled(false);
    UpdateStatus(tr("正在停止...（等待当前文件完成）"));
    QApplication::processEvents();
}

void CSE_DataImportDialog::slotClose()
{
    SaveSettings();
    reject();
}

void CSE_DataImportDialog::slotProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_pProgressBar->setRange(0, 100);

    QString dataPath = m_pEditDataPath->text().trimmed();
    QString tableName = m_qstrCurrentTableName;
    if (tableName.isEmpty())
        tableName = DefaultTableName(dataPath);

    if (exitStatus == QProcess::CrashExit || exitCode != 0)
    {
        m_pProgressBar->setValue(0);
        QString errMsg = QString::fromUtf8(m_pProcess->readAllStandardError());
        if (errMsg.isEmpty())
            errMsg = QString::fromUtf8(m_pProcess->readAllStandardOutput());
        if (errMsg.isEmpty())
            errMsg = tr("ogr2ogr 进程异常退出 (exit code: %1)").arg(exitCode);

        UpdateStatus(tr("导入失败"));
        QMessageBox::critical(this, tr("导入数据"),
            tr("数据导入失败！\n\n%1").arg(errMsg.trimmed()));

        QgsMessageLog::logMessage(
            tr("导入失败: %1").arg(errMsg), tr("数据导入"), Qgis::Warning);
    }
    else
    {
        m_pProgressBar->setValue(100);
        UpdateStatus(tr("导入完成"));

        QString stdOut = QString::fromUtf8(m_pProcess->readAllStandardOutput());
        QgsMessageLog::logMessage(
            tr("导入成功。\n%1").arg(stdOut), tr("数据导入"), Qgis::Info);

        // 写入元数据
        if (m_pChkWriteMetadata->isChecked())
        {
            QFileInfo fi(dataPath);
            QString dtype = GetDataTypeId();
            WriteMetadata(tableName, dataPath, dtype, fi.size());
        }

        // 修复日期列（0000/00/00 → NULL）——在加载到地图之前
        {
            QString schema = m_pEditSchema->text().trimmed();
            if (schema.isEmpty()) schema = "public";
            FixDateColumns(schema, tableName, dataPath);
        }

        // 加载到地图
        if (m_pChkLoadAfter->isChecked())
        {
            QString schema = m_pEditSchema->text().trimmed();
            if (schema.isEmpty()) schema = "public";

            QString uri = BuildDataSourceUri(schema, tableName);
            QgsVectorLayer* layer = new QgsVectorLayer(uri, tableName, "postgres");
            if (layer->isValid())
            {
                QgsProject::instance()->addMapLayer(layer);
            }
            else
            {
                delete layer;
            }
            QMessageBox::information(this, tr("导入数据"),
                tr("数据导入成功！\n表名: %1\nSchema: %2\nSRID: %3\n空间索引: %4")
                .arg(tableName)
                .arg(schema)
                .arg(GetSrid())
                .arg(m_pChkSpatialIndex->isChecked() ? tr("已创建") : tr("未创建")));
        }
        else
        {
            QMessageBox::information(this, tr("导入数据"),
                tr("数据导入成功！\n表名: %1\nSRID: %2\n空间索引: %3")
                .arg(tableName)
                .arg(GetSrid())
                .arg(m_pChkSpatialIndex->isChecked() ? tr("已创建") : tr("未创建")));
        }
    }

    // 恢复UI
    m_pBtnCancel->setVisible(false);
    m_pBtnImport->setEnabled(true);
    m_pBtnTestConn->setEnabled(true);
    m_pBtnBrowse->setEnabled(true);
    m_pBtnRefreshLayers->setEnabled(true);
}

void CSE_DataImportDialog::slotProcessError(QProcess::ProcessError error)
{
    m_pProgressBar->setRange(0, 100);
    m_pProgressBar->setValue(0);

    QString errStr;
    switch (error)
    {
    case QProcess::FailedToStart:
        errStr = tr("无法启动 ogr2ogr。请确认 GDAL/OGR 已正确安装。");
        break;
    case QProcess::Timedout:
        errStr = tr("ogr2ogr 执行超时。");
        break;
    default:
        errStr = tr("ogr2ogr 进程错误 (code: %1)").arg(error);
        break;
    }

    UpdateStatus(tr("导入失败"));
    QMessageBox::critical(this, tr("导入数据"), errStr);

    m_pBtnCancel->setVisible(false);
    m_pBtnImport->setEnabled(true);
    m_pBtnTestConn->setEnabled(true);
    m_pBtnBrowse->setEnabled(true);
    m_pBtnRefreshLayers->setEnabled(true);
}

void CSE_DataImportDialog::slotUpdateDataType()
{
    // 切换数据类型时清除之前选择的路径
    m_pEditDataPath->clear();
    m_pEditTableName->clear();
    m_pListGdbLayers->clear();
    m_pProgressBar->setValue(0);

    UpdateControlVisibility();
}
