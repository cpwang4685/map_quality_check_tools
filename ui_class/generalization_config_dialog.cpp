/**
 * @file    generalization_config_dialog.cpp
 * @brief   综合缩编配置对话框 — LoadLibrary + 解析 Link 容器 + DoXMLFile
 */

#include "generalization_config_dialog.h"
#include "layer_type_select_dialog.h"
#include "param_config_dialog.h"
#include "scale_selector_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
// 【2026-09-10】数据源页签（QTabWidget）已去除，SHP / GDB 输入合并到同一个"矢量数据目录"
#include <QSpacerItem>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QMap>
// 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复），QSql 不再使用
// #include <QSqlDatabase>
// #include <QSqlError>
// #include <QSqlQuery>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QXmlStreamReader>
#include <QTextStream>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <qgsmessagelog.h>

#ifdef _WIN32
#include <windows.h>
#endif

 //////////////////////////////////////////////////////////////////////////
#if !defined(SE_NMO_NO_SDK)
#include <Nmo/IO/StringConvert.h>
#include <Nmo/IO/FileHelper.h>
#include <Nmo/IO/StringHelper.h>
#include <Nmo/IO/DirectoryHelper.h>
#include <Nmo/IO/PathHelper.h>
#include <os_define.h>

#include <rapidxml/rapidxml.hpp>
#include <rapidxml/rapidxml_print.hpp>
#include <rapidxml/rapidxml_utils.hpp>

#include <MapBatchProcessing/FunctionsProcessing.h>
#include <MapBatchProcessing/FunctionsProcessingExt.h>
#include <MapBatchProcessing/GeneralFunctions.h>
#endif
//////////////////////////////////////////////////////////////////////////

#include <algorithm>

#include "ui_fit_helper.h"

#define LOG_TAG QStringLiteral("综合缩编")

// 诊断用文件日志：LTZK 内嵌 QGIS 未必能打开"日志消息"面板，QgsMessageLog 的
// 内容用户看不到。此文件无条件写、按行 flush，失败原因不丢。
// 麒麟: /tmp/generalization_debug.log   Windows: %TEMP%\generalization_debug.log
static void genDebugLog(const QString& msg)
{
    QFile f(QDir::tempPath() + QStringLiteral("/generalization_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
    {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"))
           << QLatin1Char(' ') << msg << QLatin1Char('\n');
        ts.flush();
    }
}

GeneralizationConfigDialog::GeneralizationConfigDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QString::fromUtf8("地图综合"));
    // 去掉默认的问号帮助按钮（未实现帮助内容，多余）
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(700, 560);
    setMinimumSize(580, 480);
    DialogFitHelper::install(this);

    // 平台 QSS 给 QPushButton 统一加了 padding:7px 16px，横向 32px 会把固定 80px
    // 宽的"浏览.../参数配置"按钮内容区压窄到 ~46px，导致文字被截断显示不全。
    // 这里用局部样式削减横向内边距（widget 级样式表不会被平台对顶层窗口的
    // setStyleSheet 覆盖），其余背景/边框/悬停效果保持平台默认。
    const QString kNarrowBtnStyle = QStringLiteral("QPushButton { padding: 7px 8px; }");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    // ====================================================================
    // 1. 数据源
    // ====================================================================
    auto* groupSource = new QGroupBox(QString::fromUtf8(" 数据源"), this);
    groupSource->setStyleSheet(
        QString::fromUtf8("QGroupBox::title { padding-top: 6px; } "
                          "QGroupBox { padding-top: 12px; }"));
    auto* vSource = new QVBoxLayout(groupSource);

    // 【2026-09-10】原 "SHP 数据" / "GDB 数据" 两个页签已去除：矢量数据目录现在同时
    // 接受 SHP 目录与 FileGDB 目录（GDB 走"先 gdb→shp 再综合"两阶段流程，入口合并），
    // GDB 数据页（独立跑 gdb→shp 的测试入口）不再需要。
    m_widgetFileInput = new QWidget(groupSource);
    auto* vFile = new QVBoxLayout(m_widgetFileInput);
    vFile->setContentsMargins(6,8,6,6);
    vFile->setSpacing(4);

    // 第1行: 矢量数据目录
    auto* hFileRow1 = new QHBoxLayout();
    hFileRow1->addWidget(new QLabel(QString::fromUtf8("矢量数据目录:"), this));
    m_lineEditShpDir = new QLineEdit(this);
    // 【2026-09-10】同一入口同时接受 SHP 目录与 FileGDB 目录（GDB 会先转 SHP 再综合）
    m_lineEditShpDir->setPlaceholderText(
        QString::fromUtf8("选择包含SHP文件的目录，或FileGDB"));
    hFileRow1->addWidget(m_lineEditShpDir, 1);
    auto* btnBrowseShpDir = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseShpDir->setFixedWidth(80);
    btnBrowseShpDir->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseShpDir, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseShpDir);
    hFileRow1->addWidget(btnBrowseShpDir);
    vFile->addLayout(hFileRow1);

    // // 第2行: 图层类型过滤 (与 DB 面板一致的 LayerTypeSelectDialog)
    // auto* hFileRow2 = new QHBoxLayout();
    // hFileRow2->addWidget(new QLabel(QString::fromUtf8("选择缩编的图层类型:"), this));
    // m_btnSelectFsLayerTypes = new QPushButton(QString::fromUtf8("\xF0\x9F\x93\x8B 选择图层类型..."), this);
    // m_btnSelectFsLayerTypes->setMinimumHeight(30);
    // m_labelFsSelectedTypes = new QLabel(QString::fromUtf8("(未选择 = 全部)"), this);
    // m_labelFsSelectedTypes->setStyleSheet("QLabel { color: #888; padding-left: 8px; }");
    // hFileRow2->addWidget(m_btnSelectFsLayerTypes);
    // hFileRow2->addWidget(m_labelFsSelectedTypes, 1);
    // vFile->addLayout(hFileRow2);

    vSource->addWidget(m_widgetFileInput);

    // ----- 2b. 数据库输入 (默认隐藏) -----
    // 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复），DB 面板整块去掉
    // m_widgetDbInput = new QWidget(this);
    // m_widgetDbInput->setVisible(false);
    // auto* vDb = new QVBoxLayout(m_widgetDbInput);
    // vDb->setContentsMargins(0,0,0,0);
    //
    // auto* hDbRow1 = new QHBoxLayout();
    // hDbRow1->addWidget(new QLabel(QString::fromUtf8("主机:"), this));
    // m_lineEditDbHost = new QLineEdit("localhost", this);
    // m_lineEditDbHost->setMaximumWidth(130);
    // hDbRow1->addWidget(m_lineEditDbHost);
    // hDbRow1->addWidget(new QLabel(QString::fromUtf8("端口:"), this));
    // m_spinDbPort = new QSpinBox(this);
    // m_spinDbPort->setRange(1,65535); m_spinDbPort->setValue(5432);
    // m_spinDbPort->setMaximumWidth(75);
    // hDbRow1->addWidget(m_spinDbPort);
    // hDbRow1->addWidget(new QLabel(QString::fromUtf8("数据库:"), this));
    // m_lineEditDbName = new QLineEdit("mpqis", this);
    // hDbRow1->addWidget(m_lineEditDbName, 1);
    //
    // auto* hDbRow2 = new QHBoxLayout();
    // hDbRow2->addWidget(new QLabel("Schema:", this));
    // m_lineEditDbSchema = new QLineEdit("mpqis", this);
    // hDbRow2->addWidget(m_lineEditDbSchema, 1);
    //
    // auto* hDbRow3 = new QHBoxLayout();
    // hDbRow3->addWidget(new QLabel(QString::fromUtf8("用户:"), this));
    // m_lineEditDbUser = new QLineEdit("mpqis_app", this);
    // hDbRow3->addWidget(m_lineEditDbUser, 1);
    // hDbRow3->addWidget(new QLabel(QString::fromUtf8("密码:"), this));
    // m_lineEditDbPassword = new QLineEdit(this);
    // m_lineEditDbPassword->setEchoMode(QLineEdit::Password);
    // hDbRow3->addWidget(m_lineEditDbPassword, 1);
    //
    // auto* hDbBtns = new QHBoxLayout();
    // m_btnTestDbConn   = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8C 测试连接"), this);
    // m_btnFetchData    = new QPushButton(QString::fromUtf8("\xF0\x9F\x93\xA5 调用数据"), this);
    // m_btnFetchData->setToolTip(QString::fromUtf8("将数据库空间表导出为 SHP 并自动加载到文件系统数据源"));
    // m_labelDbStatus   = new QLabel(QString::fromUtf8("未连接"), this);
    // m_labelDbStatus->setStyleSheet("QLabel { color: #888; }");
    // hDbBtns->addWidget(m_btnTestDbConn);
    // hDbBtns->addWidget(m_btnFetchData);
    // hDbBtns->addStretch();
    // hDbBtns->addWidget(m_labelDbStatus);
    //
    // // auto* hDbLayerSelect = new QHBoxLayout();
    // // hDbLayerSelect->addWidget(new QLabel(QString::fromUtf8("选择缩编的图层类型:"), this));
    // // m_btnSelectLayerTypes = new QPushButton(QString::fromUtf8("\xF0\x9F\x93\x8B 选择图层类型..."), this);
    // // m_btnSelectLayerTypes->setMinimumHeight(30);
    // // m_labelSelectedTypes = new QLabel(QString::fromUtf8("(未选择)"), this);
    // // m_labelSelectedTypes->setStyleSheet("QLabel { color: #888; padding-left: 8px; }");
    // // hDbLayerSelect->addWidget(m_btnSelectLayerTypes);
    // // hDbLayerSelect->addWidget(m_labelSelectedTypes, 1);
    //
    // vDb->addLayout(hDbRow1);
    // vDb->addLayout(hDbRow2);
    // vDb->addLayout(hDbRow3);
    // vDb->addLayout(hDbBtns);
    // // vDb->addLayout(hDbLayerSelect);
    //
    // vSource->addWidget(m_widgetDbInput);
    mainLayout->addWidget(groupSource);

    // ====================================================================
    // 2. XML 配置
    // ====================================================================
    auto* groupCfg = new QGroupBox(QString::fromUtf8(" XML 配置"), this);
    groupCfg->setStyleSheet(
        QString::fromUtf8("QGroupBox::title { padding-top: 6px; } "
                          "QGroupBox { padding-top: 12px; }"));
    auto* vCfg = new QVBoxLayout(groupCfg);
    vCfg->setSpacing(6);

    // 第1行: 配置文件选择
    auto* hCfg = new QHBoxLayout();
    hCfg->addWidget(new QLabel(QString::fromUtf8("配置文件:"), this));
    m_lineEditConfigXml = new QLineEdit(this);
    m_lineEditConfigXml->setPlaceholderText(QString::fromUtf8("选择xml文件 ..."));
    hCfg->addWidget(m_lineEditConfigXml, 1);
    auto* btnBrowseConfigXml = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseConfigXml->setFixedWidth(80);
    btnBrowseConfigXml->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseConfigXml, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseConfigXml);
    hCfg->addWidget(btnBrowseConfigXml);
    auto* btnParamConfig = new QPushButton(QString::fromUtf8("参数配置"), this);
    btnParamConfig->setFixedWidth(80);
    btnParamConfig->setStyleSheet(kNarrowBtnStyle);
    connect(btnParamConfig, &QPushButton::clicked, this, [this]() {
        QString xmlPath = m_lineEditConfigXml->text().trimmed();
        if (xmlPath.isEmpty()) {
            QMessageBox::information(this, QString::fromUtf8("提示"),
                QString::fromUtf8("请先在 XML 配置框中选择配置文件"));
            return;
        }
        // 【2026-09-10】打开参数配置时自动带入本对话框里已选的 XML 配置文件，免去用户
        // 进对话框后再手动"打开XML"一遍。加载刻意放在 show() 之前：ParamConfigDialog 装了
        // DialogFitHelper，首次 Show 时按内容最小尺寸撑大窗口，先加载再显示窗口才会按已加载
        // 的内容定尺寸；反过来则可能定成一个空窗口的尺寸，看起来就像"没加载"。
        ParamConfigDialog* dlg = new ParamConfigDialog(this, Qt::WindowCloseButtonHint);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        const bool loaded = dlg->loadXmlFile(xmlPath);
        genDebugLog(QStringLiteral("[参数配置] 自动加载 XML: %1 → %2")
                    .arg(xmlPath, loaded ? QStringLiteral("成功") : QStringLiteral("失败")));
        if (!loaded) {
            // 空窗口没有解释会让人误以为"根本没自动加载"，这里把原因说清楚再照常打开，
            // 用户仍可在对话框里手动打开别的文件。
            QMessageBox::warning(this, QString::fromUtf8("参数配置"),
                QString::fromUtf8("已选的 XML 配置文件加载失败：\n%1\n\n"
                                  "该文件可能不是知识库/任务 XML，或已被移动、删除。\n"
                                  "参数配置对话框仍会打开，可在其中手动打开其它文件。")
                .arg(xmlPath));
        }
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    hCfg->addWidget(btnParamConfig);
    vCfg->addLayout(hCfg);

    // 第2行: 自定义综合比例尺按钮
    auto* hScale = new QHBoxLayout();
    auto* btnScaleSelector = new QPushButton(QString::fromUtf8("自定义综合比例尺"), this);
    btnScaleSelector->setMinimumHeight(32);
    // 原样式硬编码了浅色主题的 color:#333（深灰），在平台深色主题下文字近乎不可见，
    // 与其它按钮（平台 QPushButton color:#f8feff）不一致。这里移除 color 覆盖，让文字
    // 颜色回落到平台全局 QSS；仅保留加粗强调与内边距，悬停白色与平台一致。
    btnScaleSelector->setStyleSheet(
        "QPushButton { font-weight: bold; padding: 4px 16px; }"
        "QPushButton:hover { color: #ffffff; }");
    connect(btnScaleSelector, &QPushButton::clicked, this, [this]() {
        ScaleSelectorDialog dlg(this);
        dlg.setXmlFilePath(m_lineEditConfigXml->text().trimmed());
        dlg.setScale(10000);  // 默认 1:10000
        if (dlg.exec() == QDialog::Accepted) {
            int scale = dlg.selectedScale();
            Q_UNUSED(scale);
        }
    });
    hScale->addWidget(btnScaleSelector);
    hScale->addStretch();  // 按钮靠左，右侧填充弹性空间
    vCfg->addLayout(hScale);

    mainLayout->addWidget(groupCfg);

    // ====================================================================
    // 3. 输出目录
    // ====================================================================
    auto* groupOutput = new QGroupBox(QString::fromUtf8(" 输出"), this);
    groupOutput->setStyleSheet(
        QString::fromUtf8("QGroupBox::title { padding-top: 6px; } "
                          "QGroupBox { padding-top: 12px; }"));
    auto* hOutput = new QHBoxLayout(groupOutput);
    hOutput->addWidget(new QLabel(QString::fromUtf8("结果目录:"), this));
    m_lineEditOutputDir = new QLineEdit(this);
    m_lineEditOutputDir->setPlaceholderText(QString::fromUtf8("选择综合缩编结果输出目录..."));
    hOutput->addWidget(m_lineEditOutputDir, 1);
    auto* btnBrowseOutputDir = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseOutputDir->setFixedWidth(80);
    btnBrowseOutputDir->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseOutputDir, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseOutputDir);
    hOutput->addWidget(btnBrowseOutputDir);
    mainLayout->addWidget(groupOutput);

    // ====================================================================
    // 4. 底部按钮
    // ====================================================================
    auto* hButtons = new QHBoxLayout();
    hButtons->addStretch();
    m_btnExecute = new QPushButton(QString::fromUtf8("▶ 执行综合缩编"), this);
    m_btnExecute->setStyleSheet(
        "QPushButton { font-weight: bold; background-color: #2196F3; color: white; "
        "padding: 8px 28px; border-radius: 4px; font-size: 14px; }"
        "QPushButton:hover { background-color: #1976D2; }");
    hButtons->addWidget(m_btnExecute);
    auto* btnCancel = new QPushButton(QString::fromUtf8("关闭"), this);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::close);
    hButtons->addWidget(btnCancel);
    mainLayout->addLayout(hButtons);

    connectSignals();

    // 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复）：
    // 自动加载数据库连接配置 + 开对话框即自动测试连接，一并去掉
    // loadDbConfigSettings();

    // // ---- 自动测试连接并显示状态 ----
    // {
    //     QString connName = "generalization_auto_test";
    //     {
    //         QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
    //         db.setHostName(m_lineEditDbHost->text().trimmed());
    //         db.setPort(m_spinDbPort->value());
    //         db.setDatabaseName(m_lineEditDbName->text().trimmed());
    //         db.setUserName(m_lineEditDbUser->text().trimmed());
    //         db.setPassword(m_lineEditDbPassword->text());
    //         if (db.open())
    //         {
    //             m_labelDbStatus->setText(QString::fromUtf8(
    //                 "\xE2\x9C\x93 连接成功 (%1:%2/%3)")
    //                 .arg(db.hostName()).arg(db.port()).arg(db.databaseName()));
    //             m_labelDbStatus->setStyleSheet("QLabel { color: #4CAF50; }");
    //             db.close();
    //         }
    //         else
    //         {
    //             m_labelDbStatus->setText(QString::fromUtf8(
    //                 "\xE2\x9C\x97 %1").arg(db.lastError().text().left(60)));
    //             m_labelDbStatus->setStyleSheet("QLabel { color: red; }");
    //         }
    //     }
    //     QSqlDatabase::removeDatabase(connName);
    // }

    QgsMessageLog::logMessage(QStringLiteral("初始化完成"), LOG_TAG, Qgis::Info);
}

void GeneralizationConfigDialog::connectSignals()
{
    // 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复），DB 相关连接一并去掉
    // connect(m_btnGroupSource, QOverload<int>::of(&QButtonGroup::buttonClicked),
    //         this, [this](int){ onSourceTypeChanged(); });
    // connect(m_btnTestDbConn,     &QPushButton::clicked, this, &GeneralizationConfigDialog::onTestDbConnection);
    // connect(m_btnFetchData,      &QPushButton::clicked, this, &GeneralizationConfigDialog::onFetchData);
    // connect(m_btnSelectLayerTypes, &QPushButton::clicked, this, &GeneralizationConfigDialog::onSelectLayerTypes);
    // connect(m_btnSelectFsLayerTypes, &QPushButton::clicked, this, &GeneralizationConfigDialog::onSelectFsLayerTypes);
    connect(m_btnExecute,        &QPushButton::clicked, this, &GeneralizationConfigDialog::onExecute);
}

// ====== 数据源切换 ======
// 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复），
// 仅保留文件系统数据源，切换逻辑不再需要
// void GeneralizationConfigDialog::onSourceTypeChanged()
// {
//     bool isFile = (m_btnGroupSource->checkedId() == 0);
//     m_widgetFileInput->setVisible(isFile);
//     m_widgetDbInput->setVisible(!isFile);
//     // if (!isFile && m_selectedDbLayerTypes.isEmpty()) {
//     //     m_selectedDbLayerTypes << "a_bld";
//     //     m_labelSelectedTypes->setText(QString::fromUtf8("a_bld (建构筑物及设施)"));
//     //     m_labelSelectedTypes->setStyleSheet("QLabel { color: #4CAF50; padding-left: 8px; font-weight: bold; }");
//     // }
// }

// ====== 属性访问器 ======

QString GeneralizationConfigDialog::shpDirectory() const    { return m_lineEditShpDir->text().trimmed(); }
QString GeneralizationConfigDialog::gdbDirectory() const    { return m_lineEditGdbDir->text().trimmed(); }
QString GeneralizationConfigDialog::convXmlPath() const      { return m_lineEditConvXml->text().trimmed(); }
QString GeneralizationConfigDialog::configXmlPath() const    { return m_lineEditConfigXml->text().trimmed(); }
QString GeneralizationConfigDialog::outputDirectory() const  { return m_lineEditOutputDir->text().trimmed(); }
bool    GeneralizationConfigDialog::isFileSystemSource() const { return true; }   // 仅文件系统数据源
// bool GeneralizationConfigDialog::isDatabaseSource() const { return false; }     // 数据库源已注释

// ====== DB 工具 ======
// 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复），DB 工具不再使用
// QString GeneralizationConfigDialog::pgConnString() const
// {
//     return QString("PG:host=%1 port=%2 dbname=%3 user=%4 password=%5 schemas=%6")
//         .arg(m_lineEditDbHost->text().trimmed())
//         .arg(m_spinDbPort->value())
//         .arg(m_lineEditDbName->text().trimmed())
//         .arg(m_lineEditDbUser->text().trimmed())
//         .arg(m_lineEditDbPassword->text())
//         .arg(m_lineEditDbSchema->text().trimmed());
// }
//
// QString GeneralizationConfigDialog::findOgr2ogr() const
// {
//     QString appDir = QCoreApplication::applicationDirPath();
//     QStringList candidates = {
//         // LTZK: 相对于 exe 目录的 OSGeo4W
//         appDir + "/../OSGeo4W_32815/bin/ogr2ogr.exe",
//         // GarMap 环境
//         "D:/GarMap/extern/OSGeo4W_32815/bin/ogr2ogr.exe",
//         "C:/OSGeo4W/bin/ogr2ogr.exe",
//         "/usr/bin/ogr2ogr",
//     };
//     for (const auto& c : candidates) {
//         if (QFile::exists(c)) return c;
//     }
//     return "ogr2ogr";
// }
//
// void GeneralizationConfigDialog::loadDbConfigSettings()
// {
//     // 与数据库连接配置 UI（DbConfigDialog）共享注册表配置：
//     // 读取其连接成功后写入的 QSettings("GarMap","MapProductManager") 键，
//     // 填充数据库面板字段（与地图数据下载 UI mapdata_download 的做法一致）。
//     // 不再读取 db_config.json（新配置 UI 已不再输出该文件）。
//     QSettings dbSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
//     m_lineEditDbHost->setText(dbSettings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString());
//     m_spinDbPort->setValue(dbSettings.value(QStringLiteral("db/port"), 5432).toInt());
//     m_lineEditDbName->setText(dbSettings.value(QStringLiteral("db/database"), QStringLiteral("map_products")).toString());
//     m_lineEditDbSchema->setText(dbSettings.value(QStringLiteral("db/schema"), QStringLiteral("public")).toString());
//     m_lineEditDbUser->setText(dbSettings.value(QStringLiteral("db/user"), QStringLiteral("postgres")).toString());
//     // 仅当配置 UI 勾选“保存密码”时才持久化密码，未勾选则留空由用户手动输入
//     if (dbSettings.value(QStringLiteral("db/savePassword"), false).toBool())
//         m_lineEditDbPassword->setText(dbSettings.value(QStringLiteral("db/password")).toString());
//     else
//         m_lineEditDbPassword->clear();
// }

// ====== 事件处理 ======

void GeneralizationConfigDialog::onBrowseShpDir()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择矢量数据目录"), m_lineEditShpDir->text());
    if (!dir.isEmpty()) m_lineEditShpDir->setText(dir);
}

void GeneralizationConfigDialog::onBrowseGdbDir()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择 GDB 目录"), m_lineEditGdbDir->text());
    if (!dir.isEmpty()) m_lineEditGdbDir->setText(dir);
}

void GeneralizationConfigDialog::onBrowseConvXml()
{
    QString startDir;
    if (!m_lineEditConvXml->text().trimmed().isEmpty())
        startDir = QFileInfo(m_lineEditConvXml->text()).absolutePath();
    else {
        // 默认定位到知识库目录（与插件部署的 know20260313 一致）
        const QString appDir = QCoreApplication::applicationDirPath();
        startDir = QDir::cleanPath(appDir + "/../plugins/know20260313");
    }
    QString file = QFileDialog::getOpenFileName(this,
        QString::fromUtf8("选择转换知识库 XML"), startDir, "XML 文件 (*.xml);;所有文件 (*)");
    if (!file.isEmpty()) m_lineEditConvXml->setText(file);
}

void GeneralizationConfigDialog::onRunGdb2Shp()
{
    // ====== 1. 校验输入 ======
    const QString xmlPath = convXmlPath();
    if (xmlPath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("GDB→SHP 转换"),
            QString::fromUtf8("请先在 GDB 数据页选择转换知识库 XML。"));
        return;
    }
    if (!QFile::exists(xmlPath)) {
        QMessageBox::warning(this, QString::fromUtf8("GDB→SHP 转换"),
            QString::fromUtf8("转换知识库 XML 不存在:\n%1").arg(xmlPath));
        return;
    }
    if (m_workerThread && m_workerThread->isRunning()) {
        QMessageBox::warning(this, QString::fromUtf8("GDB→SHP 转换"),
            QString::fromUtf8("已有任务在执行中，请等待完成后再试。"));
        return;
    }

    genDebugLog(QStringLiteral("=== GDB→SHP 转换开始 XML=%1 GDB目录=%2")
        .arg(xmlPath, gdbDirectory()));

    // ====== 2. 解析所选 XML：取输入 GDB / 输出 SHP 的 ConnectionString ======
    // 用途: 输出目录（SHP 输出=一个目录）绝对路径时预建; 输入为绝对路径时预检存在
    QString inputConn, outputConn;
    {
        QFile f(xmlPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QXmlStreamReader xml(&f);
            QString ds;   // 当前所在数据源块: "in" / "out"
            while (!xml.atEnd() && !xml.hasError()) {
                xml.readNext();
                if (xml.isStartElement()) {
                    const QStringRef n = xml.name();
                    if      (n == QStringLiteral("InputDataSource"))   ds = QStringLiteral("in");
                    else if (n == QStringLiteral("OutputDataSource"))  ds = QStringLiteral("out");
                    else if (n == QStringLiteral("ConnectionString")) {
                        const QString v = xml.readElementText().trimmed();
                        if      (ds == QStringLiteral("in"))  inputConn  = v;
                        else if (ds == QStringLiteral("out")) outputConn = v;
                    }
                }
                else if (xml.isEndElement()) {
                    const QStringRef n = xml.name();
                    if (n == QStringLiteral("InputDataSource") || n == QStringLiteral("OutputDataSource"))
                        ds.clear();
                }
            }
            f.close();
        }
    }

    if (!inputConn.isEmpty()) {
        const QFileInfo inFi(inputConn);
        if (inFi.isAbsolute() && !QFileInfo::exists(inputConn)) {
            QMessageBox::warning(this, QString::fromUtf8("GDB→SHP 转换"),
                QString::fromUtf8("XML 中输入 GDB 不存在:\n%1\n\n"
                                  "请检查所选转换知识库 XML 的 InputDataSource ConnectionString。")
                    .arg(inputConn));
            return;
        }
    }
    if (!outputConn.isEmpty()) {
        const QFileInfo outFi(outputConn);
        if (outFi.isAbsolute())
            QDir().mkpath(outputConn);   // SHP 输出目录预建，避免 SDK 侧因父目录缺失失败
    }
    genDebugLog(QStringLiteral("解析 XML: 输入GDB=%1 输出SHP=%2").arg(inputConn, outputConn));

    // ====== 3. 子线程执行（直接格式单 XML，links 为空） ======
    // dataDir = DoXMLFile 的 relativePath 基准目录: 优先 GDB 目录，空则回退 XML 所在目录
    QString dataDir = gdbDirectory();
    if (dataDir.isEmpty())
        dataDir = QFileInfo(xmlPath).absolutePath();

    const QString appDir     = QCoreApplication::applicationDirPath();
    const QString pluginsDir = QDir::cleanPath(appDir + "/../plugins");

    startWorkerRun(xmlPath, dataDir, QVector<GeneralizationWorker::LinkEntry>(),
                   pluginsDir, QString::fromUtf8("GDB→SHP 转换"));
}

void GeneralizationConfigDialog::onBrowseConfigXml()
{
    QString file = QFileDialog::getOpenFileName(this,
        QString::fromUtf8("选择综合缩编 XML"),
        m_lineEditConfigXml->text(), "XML 文件 (*.xml);;所有文件 (*)");
    if (!file.isEmpty()) m_lineEditConfigXml->setText(file);
}

void GeneralizationConfigDialog::onBrowseOutputDir()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择结果输出目录"), m_lineEditOutputDir->text());
    if (!dir.isEmpty()) m_lineEditOutputDir->setText(dir);
}

// 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复）：
// onTestDbConnection / onFetchData / onSelectLayerTypes 三个 DB 槽函数不再使用
// void GeneralizationConfigDialog::onTestDbConnection()
// {
//     m_labelDbStatus->setText(QString::fromUtf8("正在连接..."));
//     m_labelDbStatus->setStyleSheet("QLabel { color: #FF9800; }");
//     QApplication::processEvents();
// 
//     if (!QSqlDatabase::isDriverAvailable("QPSQL")) {
//         m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x97 QPSQL 驱动不可用"));
//         m_labelDbStatus->setStyleSheet("QLabel { color: red; }");
//         return;
//     }
//     QString connName = "mpqis_gen_test_" + QString::number((quintptr)this);
//     {
//         QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
//         db.setHostName(m_lineEditDbHost->text().trimmed());
//         db.setPort(m_spinDbPort->value());
//         db.setDatabaseName(m_lineEditDbName->text().trimmed());
//         db.setUserName(m_lineEditDbUser->text().trimmed());
//         db.setPassword(m_lineEditDbPassword->text());
//         if (db.open()) {
//             if (!m_lineEditDbSchema->text().trimmed().isEmpty()) {
//                 QSqlQuery q(db);
//                 q.exec(QString("SET search_path TO %1, public").arg(m_lineEditDbSchema->text().trimmed()));
//             }
//             m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x93 连接成功"));
//             m_labelDbStatus->setStyleSheet("QLabel { color: #4CAF50; }");
//             db.close();
//         } else {
//             m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x97 %1").arg(db.lastError().text().left(60)));
//             m_labelDbStatus->setStyleSheet("QLabel { color: red; }");
//         }
//     }
//     QSqlDatabase::removeDatabase(connName);
// }
// 
// void GeneralizationConfigDialog::onFetchData()
// {
//     QString host = m_lineEditDbHost->text().trimmed();
//     int port = m_spinDbPort->value();
//     QString dbname = m_lineEditDbName->text().trimmed();
//     QString schema = m_lineEditDbSchema->text().trimmed();
//     QString user = m_lineEditDbUser->text().trimmed();
//     QString password = m_lineEditDbPassword->text();
// 
//     if (dbname.isEmpty()) {
//         QMessageBox::warning(this, QString::fromUtf8("调用数据"),
//             QString::fromUtf8("请先填写数据库连接参数。"));
//         return;
//     }
// 
//     m_labelDbStatus->setText(QString::fromUtf8("正在查询空间表..."));
//     m_labelDbStatus->setStyleSheet("QLabel { color: #FF9800; }");
//     QApplication::processEvents();
// 
//     // Step 1: 连接数据库，搜索空间表
//     QMap<QString, QString> spatialTableGeomCol;  // tableName -> ""=geometry, "colname"=bytea列
//     {
//         QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", "mpqis_fetch_list");
//         db.setHostName(host); db.setPort(port);
//         db.setDatabaseName(dbname); db.setUserName(user); db.setPassword(password);
// 
//         if (!db.open()) {
//             m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x97 连接失败"));
//             m_labelDbStatus->setStyleSheet("QLabel { color: red; }");
//             QSqlDatabase::removeDatabase("mpqis_fetch_list");
//             QMessageBox::warning(this, QString::fromUtf8("调用数据"),
//                 QString::fromUtf8("数据库连接失败:\n%1").arg(db.lastError().text()));
//             return;
//         }
// 
//         QSqlQuery q(db);
// 
//         // geometry_columns 视图
//         QString sqlA = QString("SELECT DISTINCT f_table_name FROM geometry_columns"
//                                " WHERE f_table_schema = '%1' AND f_geometry_column IS NOT NULL"
//                                " ORDER BY f_table_name").arg(schema);
//         if (q.exec(sqlA)) {
//             while (q.next()) spatialTableGeomCol[q.value(0).toString()] = QString();
//         }
// 
//         // pg_type typname
//         if (spatialTableGeomCol.isEmpty()) {
//             q.exec(QString("SELECT DISTINCT c.relname FROM pg_class c "
//                 "JOIN pg_attribute a ON a.attrelid = c.oid "
//                 "JOIN pg_type t ON t.oid = a.atttypid "
//                 "JOIN pg_namespace n ON n.oid = c.relnamespace "
//                 "WHERE n.nspname = '%1' AND c.relkind = 'r' AND NOT a.attisdropped "
//                 "AND (t.typname = 'geometry' OR t.typname = 'geography') "
//                 "ORDER BY c.relname").arg(schema));
//             while (q.next()) spatialTableGeomCol[q.value(0).toString()] = QString();
//         }
// 
//         // bytea + 列名匹配（独立于 geometry_columns，始终执行）
//         {
//             q.exec(QString("SELECT DISTINCT c.relname, a.attname FROM pg_class c "
//                 "JOIN pg_attribute a ON a.attrelid = c.oid "
//                 "JOIN pg_namespace n ON n.oid = c.relnamespace "
//                 "WHERE n.nspname = '%1' AND c.relkind = 'r' AND NOT a.attisdropped "
//                 "AND a.atttypid = (SELECT oid FROM pg_type WHERE typname = 'bytea') "
//                 "AND (lower(a.attname) LIKE '%%geom%%' OR lower(a.attname) LIKE '%%wkb%%' "
//                 "     OR lower(a.attname) LIKE '%%geo%%' OR lower(a.attname) LIKE '%%wkt%%') "
//                 "ORDER BY c.relname").arg(schema));
//             while (q.next()) {
//                 QString tname = q.value(0).toString();
//                 if (!spatialTableGeomCol.contains(tname))
//                     spatialTableGeomCol[tname] = q.value(1).toString();
//             }
//         }
// 
//         db.close();
//         QSqlDatabase::removeDatabase("mpqis_fetch_list");
//     }
// 
//     if (spatialTableGeomCol.isEmpty()) {
//         m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x97 未找到空间表"));
//         m_labelDbStatus->setStyleSheet("QLabel { color: red; }");
//         QMessageBox::information(this, QString::fromUtf8("调用数据"),
//             QString::fromUtf8("在 schema \"%1\" 中未找到任何空间表。\n\n"
//             "请检查:\n"
//             "1. PostGIS 是否已安装\n"
//             "2. 表中是否有 geometry 或 bytea(geom) 类型的列\n"
//             "3. Schema 名称是否正确").arg(schema));
//         return;
//     }
// 
//     // Step 2: 确认导出
//     QMessageBox::StandardButton btn = QMessageBox::question(this,
//         QString::fromUtf8("调用数据"),
//         QString::fromUtf8("找到 %1 个空间表。\n\n"
//             "将导出到 D:\\Temp\\mpqis_export_<时间戳>\\\n"
//             "并自动加载为文件系统数据源。\n\n是否继续?")
//             .arg(spatialTableGeomCol.size()),
//         QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
// 
//     if (btn != QMessageBox::No) {
//         // Step 3: 创建目标目录
//         QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
//         QString targetDir = QString("D:/Temp/mpqis_export_%1").arg(timestamp);
//         QDir().mkpath(targetDir);
// 
//         // Step 4: 查找 ogr2ogr
//         QString ogr2ogr = findOgr2ogr();
//         QString pgConn = pgConnString();
// 
//         // Step 5: 进度对话框
//         int total = spatialTableGeomCol.size();
//         QProgressDialog progress(QString::fromUtf8("正在导出空间表..."),
//             QString::fromUtf8("取消"), 0, total, this);
//         progress.setWindowModality(Qt::WindowModal);
//         progress.setMinimumDuration(0);
// 
//         int successCount = 0, failCount = 0;
//         QStringList failedTables;
//         int idx = 0;
// 
//         for (auto it = spatialTableGeomCol.begin(); it != spatialTableGeomCol.end(); ++it, ++idx) {
//             if (progress.wasCanceled()) break;
// 
//             const QString& table = it.key();
//             const QString& byteaCol = it.value();
//             progress.setLabelText(QString::fromUtf8("正在导出 (%1/%2): %3").arg(idx+1).arg(total).arg(table));
//             progress.setValue(idx);
//             QApplication::processEvents();
// 
//             QString shpPath = targetDir + "/" + table + ".shp";
//             QProcess proc;
//             QStringList args;
//             args << "-f" << "ESRI Shapefile"
//                  << "-lco" << "ENCODING=UTF-8"
//                  << "-overwrite"
//                  << shpPath << pgConn;
// 
//             if (byteaCol.isEmpty()) {
//                 QString sql = QString("SELECT * FROM \"%1\".\"%2\"")
//                     .arg(schema, table);
//                 args << "-sql" << sql;
//             } else {
//                 // 获取非 bytea 列名
//                 QStringList nonGeomCols;
//                 {
//                     QSqlDatabase tmpDb = QSqlDatabase::addDatabase("QPSQL", "mpqis_fetch_cols");
//                     tmpDb.setHostName(host); tmpDb.setPort(port);
//                     tmpDb.setDatabaseName(dbname); tmpDb.setUserName(user); tmpDb.setPassword(password);
//                     if (tmpDb.open()) {
//                         QSqlQuery tq(tmpDb);
//                         tq.exec(QString("SELECT a.attname FROM pg_attribute a "
//                             "JOIN pg_class c ON c.oid = a.attrelid "
//                             "JOIN pg_namespace n ON n.oid = c.relnamespace "
//                             "WHERE n.nspname = '%1' AND c.relname = '%2' "
//                             "AND NOT a.attisdropped AND a.attnum > 0 AND a.attname != '%3' "
//                             "ORDER BY a.attnum").arg(schema, table, byteaCol));
//                         while (tq.next()) nonGeomCols << "\"" + tq.value(0).toString() + "\"";
//                         tmpDb.close();
//                     }
//                     QSqlDatabase::removeDatabase("mpqis_fetch_cols");
//                 }
//                 QString colList = nonGeomCols.isEmpty() ? "*" : nonGeomCols.join(", ");
//                 QString sql = QString("SELECT %1, ST_GeomFromWKB(\"%2\") AS geom FROM \"%3\".\"%4\"")
//                     .arg(colList, byteaCol, schema, table);
//                 args << "-sql" << sql;
//             }
// 
//             QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
//             env.insert("PGPASSWORD", password);
//             proc.setProcessEnvironment(env);
//             proc.start(ogr2ogr, args);
// 
//             if (proc.waitForStarted(10000) && proc.waitForFinished(120000)) {
//                 if (proc.exitCode() == 0) successCount++;
//                 else { failCount++; failedTables << (table + ": " + proc.readAllStandardError().left(150)); }
//             } else { failCount++; failedTables << table; }
//         }
// 
//         progress.setValue(total);
// 
//         // Step 6: 报告结果 & 自动加载
//         QString msg = QString::fromUtf8("导出完成:\n  成功: %1 个表\n  失败: %2 个表\n\n目录: %3")
//             .arg(successCount).arg(failCount).arg(targetDir);
//         if (!failedTables.isEmpty()) {
//             QStringList first5;
//             for (int i = 0; i < qMin(5, failedTables.size()); ++i) first5 << failedTables[i];
//             msg += QString::fromUtf8("\n\n失败的表 (前5):\n") + first5.join("\n");
//         }
// 
//         if (successCount > 0) {
//             // 切换到文件系统模式并自动加载目录
//             m_radioFileSystem->setChecked(true);
//             m_lineEditShpDir->setText(targetDir);
// 
//             m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x93 已加载 %1 个表").arg(successCount));
//             m_labelDbStatus->setStyleSheet("QLabel { color: #4CAF50; }");
// 
//             msg += QString::fromUtf8("\n\n已自动切换到文件系统数据源并加载该目录。");
//         } else {
//             m_labelDbStatus->setText(QString::fromUtf8("\xE2\x9C\x97 导出失败"));
//             m_labelDbStatus->setStyleSheet("QLabel { color: red; }");
//         }
// 
//         QMessageBox::information(this, QString::fromUtf8("调用数据"), msg);
//     }
// }

// 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复）：
// onTestDbConnection / onFetchData / onSelectLayerTypes 三个 DB 槽函数不再使用
// void GeneralizationConfigDialog::onSelectLayerTypes()
// {
//     LayerTypeSelectDialog dlg(m_selectedDbLayerTypes, this);
//     if (dlg.exec() == QDialog::Accepted) {
//         m_selectedDbLayerTypes = dlg.selectedTypes();
//         if (m_selectedDbLayerTypes.isEmpty()) {
//             m_labelSelectedTypes->setText(QString::fromUtf8("(未选择)"));
//             m_labelSelectedTypes->setStyleSheet("QLabel { color: #888; padding-left: 8px; }");
//         } else {
//             QString summary = m_selectedDbLayerTypes.join(", ");
//             if (summary.length() > 80)
//                 summary = QString::fromUtf8("已选 %1 种图层").arg(m_selectedDbLayerTypes.size());
//             m_labelSelectedTypes->setText(summary);
//             m_labelSelectedTypes->setStyleSheet("QLabel { color: #4CAF50; padding-left: 8px; font-weight: bold; }");
//         }
//     }
// }

// ====== 核心: 执行综合缩编 ======

// ====== 文件系统图层类型选择 (复用 LayerTypeSelectDialog) ======
void GeneralizationConfigDialog::onSelectFsLayerTypes()
{
    LayerTypeSelectDialog dlg(m_selectedFsLayerTypes, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_selectedFsLayerTypes = dlg.selectedTypes();
        if (m_selectedFsLayerTypes.isEmpty()) {
            m_labelFsSelectedTypes->setText(QString::fromUtf8("(未选择 = 全部)"));
            m_labelFsSelectedTypes->setStyleSheet("QLabel { color: #888; padding-left: 8px; }");
        } else {
            QString summary = m_selectedFsLayerTypes.join(", ");
            if (summary.length() > 80)
                summary = QString::fromUtf8("已选 %1 种图层").arg(m_selectedFsLayerTypes.size());
            m_labelFsSelectedTypes->setText(summary);
            m_labelFsSelectedTypes->setStyleSheet("QLabel { color: #4CAF50; padding-left: 8px; font-weight: bold; }");
        }
    }
}

// ====== 从 SHP 文件名提取图层类型码 (如 "A_BLD_AFC_A.shp" → "a_bld") ======
static QString extractLayerTypeFromShp(const QString& shpFileName)
{
    // 取前两个下划线分隔的段 → 转小写
    QStringList parts = shpFileName.split('_');
    if (parts.size() < 2) return QString();
    QString code = parts[0].toLower() + "_" + parts[1].toLower();
    // 验证是否为合法图层类型码 (N/A/M 开头 + 3字母)
    if (code.length() == 5 && (code[0] == 'n' || code[0] == 'a' || code[0] == 'm')
        && code[1] == '_' && code[2].isLetter() && code[3].isLetter() && code[4].isLetter())
        return code;
    return QString();
}

// ====== GDAL 拷贝 SHP: 返回值 0=成功 1=目录不存在 2=无SHP 3=全部打不开
// layerTypeFilter: 为空则拷贝全部, 非空则只拷贝匹配的图层类型
static int copyShapefilesToDir(const QString& srcDir, const QString& dstDir,
                                const QStringList& layerTypeFilter = {})
{
    QDir src(srcDir);
    if (!src.exists()) {
        QgsMessageLog::logMessage(QStringLiteral("目录不存在: %1").arg(srcDir), LOG_TAG, Qgis::Warning);
        return 1;
    }

    QStringList nameFilters;
    nameFilters << "*.shp" << "*.SHP" << "*.Shp";
    QStringList shpFiles = src.entryList(nameFilters, QDir::Files);

    QgsMessageLog::logMessage(
        QStringLiteral("搜索目录: %1 | 找到 %2 个 SHP").arg(srcDir).arg(shpFiles.size()),
        LOG_TAG, Qgis::Info);

    if (shpFiles.isEmpty()) {
        QStringList allFiles = src.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        QStringList preview;
        for (int i = 0; i < qMin(allFiles.size(), 30); ++i)
            preview << allFiles[i];
        QgsMessageLog::logMessage(
            QStringLiteral("目录有 %1 个条目（非SHP），前30个: %2")
                .arg(allFiles.size()).arg(preview.join(", ")),
            LOG_TAG, Qgis::Warning);
        return 2;
    }

    // 如果设置了图层类型过滤，过滤掉不匹配的 SHP
    QSet<QString> filterSet = layerTypeFilter.toSet();   // Qt 5.14+ 才有区间构造，5.12 用 toSet()
    if (!filterSet.isEmpty()) {
        QStringList filtered;
        int skipped = 0;
        for (const QString& shp : shpFiles) {
            QString code = extractLayerTypeFromShp(shp);
            if (code.isEmpty() || filterSet.contains(code))
                filtered << shp;
            else
                skipped++;
        }
        QgsMessageLog::logMessage(
            QStringLiteral("图层类型过滤: 保留 %1 个 / 跳过 %2 个 (已选类型: %3)")
                .arg(filtered.size()).arg(skipped).arg(layerTypeFilter.join(",")),
            LOG_TAG, Qgis::Info);
        shpFiles = filtered;
    }

    if (shpFiles.isEmpty()) {
        QgsMessageLog::logMessage(
            QStringLiteral("过滤后无匹配图层类型的 SHP 文件"), LOG_TAG, Qgis::Warning);
        return 2;
    }

    QDir().mkpath(dstDir);
    int copied = 0, failed = 0;
    for (const QString& shp : shpFiles) {
        QString srcShp = src.filePath(shp);
        QString dstShp = dstDir + "/" + shp;
        GDALDatasetH hSrc = GDALOpenEx(
            srcShp.toUtf8().constData(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!hSrc) {
            QgsMessageLog::logMessage(
                QStringLiteral("GDAL 无法打开: %1").arg(srcShp), LOG_TAG, Qgis::Warning);
            failed++;
            continue;
        }
        GDALDriverH hDrv = GDALGetDatasetDriver(hSrc);
        GDALDatasetH hDst = GDALCreateCopy(hDrv,
            dstShp.toUtf8().constData(), hSrc, FALSE, nullptr, nullptr, nullptr);
        if (hDst) GDALClose(hDst);
        GDALClose(hSrc);
        copied++;
    }
    const QString copySummary =
        QStringLiteral("复制完成: %1 成功 / %2 失败 → %3").arg(copied).arg(failed).arg(dstDir);
    QgsMessageLog::logMessage(copySummary, LOG_TAG, Qgis::Info);
    genDebugLog(copySummary);
    return (copied > 0) ? 0 : 3;
}

#ifndef _WIN32
// Linux 专用：SDK 把知识库 XML 里的反斜杠路径当普通字符，输出是"文件名含\"的
// 单个实体（如 1w\result\A_BLD_AFC_A.shp）而非嵌套目录。这里把所有这样的文件
// 按 \ 拆成目录段 + 文件名，建目录后移过去，恢复与 Windows 一致的输出结构。
// 递归扫描，返回整理的文件数。
static int moveBackslashFilesToDirsRec(const QString& dir)
{
    int moved = 0;
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries) {
        if (fi.isDir()) {
            moved += moveBackslashFilesToDirsRec(fi.absoluteFilePath());
            continue;
        }
        const QString name = fi.fileName();
        if (!name.contains(QLatin1Char('\\')))
            continue;
        const QStringList parts = name.split(QLatin1Char('\\'));
        const QString fileName = parts.last();
        QString dirPath = dir;
        for (int i = 0; i < parts.size() - 1; ++i)
            dirPath += QLatin1Char('/') + parts[i];
        QDir().mkpath(dirPath);
        const QString target = dirPath + QLatin1Char('/') + fileName;
        const QString src = fi.absoluteFilePath();
        if (QFile::exists(target))
            QFile::remove(target);
        if (QFile::rename(src, target))
            moved++;
    }
    return moved;
}
static int moveBackslashFilesToDirs(const QString& rootDir)
{
    return moveBackslashFilesToDirsRec(rootDir);
}
#endif // _WIN32

// ============================================================================
// 【2026-09-10】输入类型判定 + GDB→SHP 转换准备（地图综合支持 FileGDB 输入）
// ============================================================================

// ---- 输入目录类型：SHP 目录 / FileGDB / 名为 .gdb 但不是有效 FileGDB ----
enum class InputKind { Shp, Gdb, GdbInvalid };

// FileGDB 在磁盘上是一个目录，标志是内含 *.gdbtable 文件（实测本机样本：
// 甘肃陇图 TemplateData.gdb 顶层 101 个条目、其中 22 个 .gdbtable；
// 混合文件类型 test_data.gdb 10 个）。只看 ".gdb" 后缀不够——实测存在名字像
// GDB 但里面是空的目录（D:\集成测试\数据备份模块\1.gdb，0 个条目），
// 那种要单独报错，而不是掉进 SHP 分支去报"未找到 .shp 文件"。
static InputKind detectInputKind(const QString& dir, QString& detail)
{
    const QDir d(dir);
    const QStringList tables = d.entryList(
        QStringList() << "*.gdbtable" << "*.GDBTABLE", QDir::Files);
    if (!tables.isEmpty()) {
        detail = QStringLiteral("FileGDB（%1 个 .gdbtable）").arg(tables.size());
        return InputKind::Gdb;
    }
    if (QFileInfo(dir).fileName().endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive)) {
        detail = QStringLiteral("目录名以 .gdb 结尾，但未找到 *.gdbtable（不是有效的 FileGDB）");
        return InputKind::GdbInvalid;
    }
    detail = QStringLiteral("按 SHP 目录处理");
    return InputKind::Shp;
}

// ---- 在 Link 列表里找 gdb→shp 转换子 XML，返回下标（-1 = 未找到）----
// 判据：子 XML 的 InputDataSource/Provider == "DatastoreType_EsriGDB"。
// 实测全知识库只有 gdb2shp 转换 XML 用该 Provider（如 1格式转换_gdb2shp_天津.xml），
// 而综合子 XML 连 InputDataSource 节点都没有（用的是 <Layers><FilePath> /
// <ParaIn><FilePath>），所以不会误判。注意 ProviderInfo 是另一个节点名，不匹配。
static int findGdb2ShpLinkIndex(const QString& xmlDir,
                                const QVector<GeneralizationWorker::LinkEntry>& links)
{
    for (int i = 0; i < links.size(); ++i) {
        if (!links[i].run) continue;

        // 容器 XML 里的子路径是 Windows 风格反斜杠，Linux 上必须转正斜杠
        QString rel = links[i].path;
        rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString subXml = QDir::cleanPath(xmlDir + "/" + rel);

        QFile f(subXml);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QXmlStreamReader xml(&f);
        bool inInputDs = false;
        while (!xml.atEnd() && !xml.hasError()) {
            xml.readNext();
            if (xml.isStartElement()) {
                const QStringRef n = xml.name();
                if (n == QStringLiteral("InputDataSource")) {
                    inInputDs = true;
                }
                else if (inInputDs && n == QStringLiteral("Provider")) {
                    if (xml.readElementText().trimmed()
                            == QStringLiteral("DatastoreType_EsriGDB"))
                        return i;
                    inInputDs = false;   // 该输入源不是 GDB，继续找下一条 Link
                }
            }
            else if (xml.isEndElement()
                     && xml.name() == QStringLiteral("InputDataSource")) {
                inInputDs = false;
            }
        }
    }
    return -1;
}

// ---- XML 文本值的最小转义（路径里出现 & < > 的极端情况）----
static QString xmlEscaped(const QString& v)
{
    QString r = v;
    r.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    r.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    r.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return r;
}

// ---- 把 s 里 <openTag>…<closeTag> 之间的内容整段替换为 newValue ----
static bool replaceTagBody(QString& s, const QString& openTag,
                           const QString& closeTag, const QString& newValue)
{
    const int p = s.indexOf(openTag);
    if (p < 0) return false;
    const int bodyStart = p + openTag.length();
    const int q = s.indexOf(closeTag, bodyStart);
    if (q < 0) return false;
    s.replace(bodyStart, q - bodyStart, newValue);
    return true;
}

// ---- 生成 gdb→shp 临时转换 XML ----
// 复制知识库模板，只替换运行时才知道的三处路径，其余（LayerNameList 要素集映射、
// StringFieldLengthScale、中文注释、缩进）原样保留。不用 QXmlStreamWriter 重写
// 全文——那会丢掉模板里的注释。
// 与实测可用的样式一致：<RelativePath> 置空 + 两个 ConnectionString 用绝对路径。
static QString writeGdb2ShpTempXml(const QString& tplPath, const QString& gdbDir,
                                   const QString& outDir, QString& errMsg)
{
    QFile f(tplPath);
    if (!f.open(QIODevice::ReadOnly)) {
        errMsg = QStringLiteral("无法读取转换知识库模板: %1").arg(tplPath);
        return QString();
    }
    const QByteArray raw = f.readAll();
    f.close();

    // 交付模板是 UTF-8 + BOM，读时剥掉、写时补回，保持原编码风格
    const QByteArray bom = QByteArrayLiteral("\xEF\xBB\xBF");
    const bool hasBom = raw.startsWith(bom);
    QString text = QString::fromUtf8(hasBom ? raw.mid(bom.size()) : raw);

    // 在 <secOpen>…<secClose> 区间内替换第一个 <ConnectionString>
    const auto replaceConnIn = [&text, &errMsg](const QString& secOpen, const QString& secClose,
                                                const QString& value, const QString& label) -> bool {
        const int s = text.indexOf(secOpen);
        if (s < 0) {
            errMsg = QStringLiteral("模板缺少 %1 节点").arg(label);
            return false;
        }
        const int e = text.indexOf(secClose, s);
        if (e < 0) {
            errMsg = QStringLiteral("模板 %1 节点未闭合").arg(label);
            return false;
        }
        QString section = text.mid(s, e - s);
        if (!replaceTagBody(section, QStringLiteral("<ConnectionString>"),
                            QStringLiteral("</ConnectionString>"), xmlEscaped(value))) {
            errMsg = QStringLiteral("模板 %1 内未找到 <ConnectionString>").arg(label);
            return false;
        }
        text.replace(s, e - s, section);
        return true;
    };

    if (!replaceConnIn(QStringLiteral("<InputDataSource"), QStringLiteral("</InputDataSource>"),
                       gdbDir, QStringLiteral("InputDataSource")))
        return QString();
    if (!replaceConnIn(QStringLiteral("<OutputDataSource"), QStringLiteral("</OutputDataSource>"),
                       outDir, QStringLiteral("OutputDataSource")))
        return QString();

    // <RelativePath> 置空：路径已全部绝对化，避免模板里的旧路径参与解析。
    // 模板可能写成 <RelativePath></RelativePath>（空）也可能有值，两种都能替换。
    replaceTagBody(text, QStringLiteral("<RelativePath>"),
                   QStringLiteral("</RelativePath>"), QString());

    QDir().mkpath(outDir);
    const QString outPath = outDir + QStringLiteral("/_temp_gdb2shp.xml");
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errMsg = QStringLiteral("无法写入临时 XML: %1").arg(outPath);
        return QString();
    }
    QByteArray data = text.toUtf8();
    if (hasBom) data.prepend(bom);
    out.write(data);
    out.close();
    return outPath;
}

// ====== GeneralizationWorker::process() — 在子线程中执行 ======
void GeneralizationWorker::process()
{
    // DELAYLOAD: 首次调用 Nmo 函数前，必须设置 DLL 搜索路径（仅 Windows）
#ifdef _WIN32
    SetDllDirectoryW((const wchar_t*)m_dllDir.utf16());
#else
    // Linux: NMO SDK 从"当前工作目录"读取配置文件(AppConfigInfo.xml / MachineCode.txt /
    // dataproviders.xml)并加载算法 .so；与 se_nmo_sdk_bridge.cpp::executeMission 一致，
    // 调用前把 CWD 切到插件目录(= NMO SDK 目录)，结束后恢复，避免影响 QGIS 运行。
    // 【2026-08-19】合并/接边走 bridge 会切 CWD 所以正常；综合缩编此前不切，
    // SDK 在 LTZK 启动目录(/opt/ltzk)找不到 AppConfigInfo.xml → DoXMLFile 恒 false。
    m_savedCwd = QDir::currentPath();
    QDir::setCurrent(m_dllDir);
    emit logMessage(QStringLiteral("CWD 已切换到插件目录: %1").arg(m_dllDir), 0);
#endif
    emit logMessage(QStringLiteral("Worker 线程已启动，开始处理..."), 0);

    const int total = m_links.isEmpty() ? 1 : m_links.size();
    int success = 0;
    int current = 0;

    emit logMessage(
        QStringLiteral("Link 数=%1, 数据目录=%2, XML目录=%3")
            .arg(m_links.size()).arg(m_dataDir).arg(m_xmlDir), 0);

#if !defined(SE_NMO_NO_SDK)
    // 构造 relativePath:必须用 UTF-8 编码传给 SDK。
    // 平台 ANSI 代码页是 GBK(936),toLocal8Bit() 会把含中文的结果目录转成 GBK 字节;
    // SDK 打开数据 shapefile 走 GDAL 数据流,GDAL 在 Windows 上要求文件名 UTF-8
    // (进程内 GDAL_FILENAME_IS_UTF8 默认开启),GBK 字节的中文路径解析不出文件,
    // 表现为"综合缩编未执行、只复制了原数据"。ASCII 路径在 GBK/UTF-8 下字节相同,
    // 所以此问题只在结果目录含中文时暴露。
    QByteArray dataDirBytes = m_dataDir.toUtf8();
    char relativePathBuf[10240] = {};
    strncpy(relativePathBuf, dataDirBytes.constData(), sizeof(relativePathBuf) - 1);

    // missionPara 是 SDK 的状态/错误输出缓冲。此前传 nullptr，SDK 自己报的错被
    // 直接丢弃，导致"综合缩编失败"看不到原因；这里传出并在日志中显示（与
    // se_nmo_sdk_bridge.cpp 的 missionBuf 用法一致）。
    char missionBuf[4096] = {};

    if (m_links.isEmpty()) {
        // 直接 <MapGeneBatchProcessing> 格式: 只处理一个 XML
        emit progressChanged(1, 1, QString::fromUtf8("执行中..."));

        QByteArray xmlBytes = m_xmlDir.toLocal8Bit();  // m_xmlDir 即完整路径
        char xmlBuf[10240] = {};
        strncpy(xmlBuf, xmlBytes.constData(), sizeof(xmlBuf) - 1);

        bool ok = Nmo::MapBatchProcessing::FunctionsProcessing::DoXMLFile(
            xmlBuf, relativePathBuf, true, false, missionBuf);
        {
            const QString sdkMsg = QString::fromUtf8(missionBuf).trimmed();
            if (!sdkMsg.isEmpty())
                emit logMessage(QStringLiteral("SDK 输出: %1").arg(sdkMsg), ok ? 0 : 1);
        }
        success = ok ? 1 : 0;
        emit progressChanged(1, 1, ok ? QString::fromUtf8("完成") : QString::fromUtf8("失败"));
    }
    else {
        // Link 容器格式: 遍历每个 <Link run="true">
        for (const auto& link : m_links) {
            if (!link.run) continue;

            // 容器 XML 里的子路径是 Windows 风格反斜杠，Linux 上必须转正斜杠，
            // 否则 QFile::exists 找不到（麒麟实测 24 个 Link 全被跳过 → 0/24 失败）。
            QString linkPath = link.path;
            linkPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
            QString subXmlPath = QDir::cleanPath(m_xmlDir + "/" + linkPath);
            if (!QFile::exists(subXmlPath)) {
                emit logMessage(
                    QStringLiteral("子 XML 不存在，跳过: %1").arg(subXmlPath), 1);
                continue;
            }

            current++;
            emit progressChanged(current, total, link.path);

            QByteArray xmlBytes = subXmlPath.toLocal8Bit();
            char xmlBuf[10240] = {};
            strncpy(xmlBuf, xmlBytes.constData(), sizeof(xmlBuf) - 1);

            bool ok = Nmo::MapBatchProcessing::FunctionsProcessing::DoXMLFile(
                xmlBuf, relativePathBuf, true, false, missionBuf);
            {
                const QString sdkMsg = QString::fromUtf8(missionBuf).trimmed();
                if (!sdkMsg.isEmpty())
                    emit logMessage(QStringLiteral("SDK 输出: %1").arg(sdkMsg), ok ? 0 : 1);
            }
            if (ok) success++;

            emit logMessage(
                QStringLiteral("[%1/%2] %3 %4")
                    .arg(current).arg(total).arg(link.path)
                    .arg(ok ? QStringLiteral("✓ 完成") : QStringLiteral("✗ 失败")),
                ok ? 0 : 1);
        }
    }
#else
    // NMO SDK 未就绪：不执行综合缩编（对应菜单已在 initGui 中置灰，此处兜底）
    Q_UNUSED(current);
    emit logMessage(QStringLiteral("NMO SDK 未就绪，综合缩编未执行"), 1);
#endif

#ifdef _WIN32
    SetDllDirectoryW(nullptr);  // 恢复默认搜索路径
#else
    QDir::setCurrent(m_savedCwd);  // 恢复原工作目录（不影响 QGIS）
    // Linux: SDK 把 XML 里的反斜杠当普通字符，输出是"文件名含\"的单个实体而非
    // 目录（麒麟实测 /qgis/rrr1/1w\result\A_BLD_AFC_A.shp）。整理成与 Windows
    // 一致的嵌套目录（1w/result/A_BLD_AFC_A.shp）。
    const int moved = moveBackslashFilesToDirs(m_dataDir);
    if (moved > 0)
        emit logMessage(QStringLiteral("已整理 %1 个反斜杠文件名 → 嵌套目录").arg(moved), 0);
#endif
    emit finished(success, total);
}

// ====== 析构：等待子线程结束 ======
GeneralizationConfigDialog::~GeneralizationConfigDialog()
{
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait(3000);  // 最多等 3 秒（DoXMLFile 无法中断）
    }
}

// ====== Worker 进度回调 (主线程) ======
void GeneralizationConfigDialog::onWorkerProgress(int current, int total, const QString& stepName)
{
    if (m_execProgress) {
        m_execProgress->setMaximum(total);
        m_execProgress->setValue(current);
        m_execProgress->setLabelText(
            QStringLiteral("%1 [%2/%3]\n%4")
                .arg(m_progressLabelPrefix).arg(current).arg(total).arg(stepName));
    }
    QgsMessageLog::logMessage(
        QStringLiteral("进度 [%1/%2]: %3").arg(current).arg(total).arg(stepName),
        LOG_TAG, Qgis::Info);
}

// ====== Worker 完成回调 (主线程) ======
void GeneralizationConfigDialog::onWorkerFinished(int success, int total)
{
    // 【2026-09-10】加 null 检查并置空：GDB 输入时一次用户操作会连续跑两轮
    // （阶段1 gdb→shp、阶段2 综合），第二轮 startWorkerRun 会新建进度框，
    // 不置空的话这里 close 的是新框、旧指针也会一直留着。
    if (m_execProgress) {
        m_execProgress->close();
        m_execProgress = nullptr;
    }

    // 清理线程
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
        m_workerThread->deleteLater();
        m_workerThread = nullptr;
    }
    if (m_worker) {
        m_worker->deleteLater();
        m_worker = nullptr;
    }

    QgsMessageLog::logMessage(
        QStringLiteral("执行完毕: %1/%2 成功").arg(success).arg(total),
        LOG_TAG, Qgis::Info);
    genDebugLog(QStringLiteral("执行完毕: %1/%2 成功").arg(success).arg(total));

    // 【2026-09-10】GDB 输入的两阶段串联：阶段1（gdb→shp 转换）结束后自动接
    // 阶段2（综合）。此间不弹完成框——阶段1 失败才弹错并中止。
    if (m_pendingGdb2ShpRun) {
        m_pendingGdb2ShpRun = false;
        if (success < total) {
            QgsMessageLog::logMessage(
                QStringLiteral("GDB→SHP 转换未全部完成，综合已中止"), LOG_TAG, Qgis::Critical);
            genDebugLog(QStringLiteral("GDB→SHP 转换未全部完成，综合已中止"));
            QMessageBox::warning(this, QString::fromUtf8("GDB→SHP 转换"),
                QString::fromUtf8("GDB→SHP 转换未全部完成，综合已中止。\n\n"
                                  "请查看 QGIS「日志消息」面板的「综合缩编」标签获取详情，\n"
                                  "并检查临时转换配置与转换知识库模板。"));
            return;
        }
        QgsMessageLog::logMessage(
            QStringLiteral("GDB→SHP 转换完成，继续执行综合缩编"), LOG_TAG, Qgis::Info);
        genDebugLog(QStringLiteral("GDB→SHP 转换完成，继续执行综合缩编"));
        startWorkerRun(m_stage2XmlDir, m_stage2DataDir, m_stage2Links,
                       m_stage2DllDir, QString::fromUtf8("综合缩编"));
        return;   // 阶段2 结束时才走下面的常规完成弹框
    }

    // 【2026-09-09】文案改用 m_taskLabel（综合 / gdb→shp 转换共用此回调），
    // 默认任务标签"综合缩编"，弹窗文案与改造前一致
    const QString title = m_taskLabel.isEmpty() ? QString::fromUtf8("综合缩编") : m_taskLabel;
    if (success == total && total > 0) {
        QMessageBox::information(this, title,
            QString::fromUtf8("%1执行完成！\n\n 全部成功。").arg(title));
    }
    else if (success > 0) {
        QMessageBox::warning(this, title,
            QString::fromUtf8("%1部分完成。\n\n成功: %2 / 失败: %3")
                .arg(title).arg(success).arg(total - success));
    }
    else {
        QMessageBox::warning(this, title,
            QString::fromUtf8("%1执行失败。\n\n请查看 QGIS「日志消息」面板的「综合缩编」标签获取详情。").arg(title));
    }
}

// ====== 通用启动：创建子线程 worker + 进度条，跑一次 DoXMLFile ======
// links 空 = 直接格式单 XML（综合单 XML / gdb→shp 转换）；
// links 非空 = Link 容器格式，worker 内逐条跑。
// 【2026-09-09】从 onExecute 抽出，onExecute 与 onRunGdb2Shp 共用。
void GeneralizationConfigDialog::startWorkerRun(
    const QString& xmlDir, const QString& dataDir,
    const QVector<GeneralizationWorker::LinkEntry>& links,
    const QString& dllDir, const QString& taskLabel)
{
    m_taskLabel = taskLabel;
    m_progressLabelPrefix = QString::fromUtf8("正在执行%1...").arg(taskLabel);

    if (m_workerThread && m_workerThread->isRunning()) {
        QgsMessageLog::logMessage(
            QStringLiteral("已有任务在执行中，忽略本次启动: %1").arg(taskLabel),
            LOG_TAG, Qgis::Warning);
        return;
    }

    // 创建子线程 Worker
    m_worker = new GeneralizationWorker(this);
    m_worker->setXmlDir(xmlDir);
    m_worker->setDataDir(dataDir);
    m_worker->setDllDir(dllDir);
    m_worker->setLinks(links);

    m_workerThread = new QThread(this);
    m_worker->moveToThread(m_workerThread);

    // 信号连接 (跨线程自动使用 QueuedConnection)
    connect(m_workerThread, &QThread::started,
            m_worker,       &GeneralizationWorker::process);
    connect(m_worker, &GeneralizationWorker::progressChanged,
            this,     &GeneralizationConfigDialog::onWorkerProgress);
    connect(m_worker, &GeneralizationWorker::finished,
            this,     &GeneralizationConfigDialog::onWorkerFinished);
    connect(m_worker, &GeneralizationWorker::logMessage,
            this,     [](const QString& msg, int level) {
                QgsMessageLog::logMessage(msg, LOG_TAG,
                    level == 2 ? Qgis::Critical : (level == 1 ? Qgis::Warning : Qgis::Info));
                genDebugLog(msg);   // 同步写 /tmp 诊断日志（用户看不了 QGIS 面板）
            });

    // 显示进度条（实际执行 Link 数可能少于 links.size()，有 run=false 的项）
    int activeCount = 0;
    for (const auto& link : links) {
        if (link.run) activeCount++;
    }
    if (activeCount == 0) activeCount = 1;

    m_execProgress = new QProgressDialog(
        m_progressLabelPrefix +
            QStringLiteral("\n\n处理时间取决于数据量和算法复杂度。\n请耐心等待，不要关闭此窗口。"),
        QString(), 0, activeCount, this);
    m_execProgress->setWindowModality(Qt::WindowModal);
    m_execProgress->setCancelButton(nullptr);  // DoXMLFile 无法中断，不提供取消按钮
    m_execProgress->setMinimumDuration(0);
    m_execProgress->show();

    // 启动线程
    m_workerThread->start();
}

// ====== 【2026-09-10】GDB 输入准备（阶段1：gdb→shp 转换）======
// 在知识库 Link 里找 gdb2shp 转换步骤 → 用它作模板生成临时转换 XML（把实际 GDB
// 目录与结果输出目录注入进去）→ 把该 Link 从综合步骤里剔除（阶段2 不再重复跑）。
// 返回 false 表示已弹错，调用方直接 return。
bool GeneralizationConfigDialog::prepareGdb2ShpStage(
    const QString& gdbDir, const QString& outDir, bool isLinkContainer,
    QVector<GeneralizationWorker::LinkEntry>& links, QString& tempXmlPath)
{
    if (!isLinkContainer) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("当前选择的是 GDB 目录，但所选知识库是直接格式（单个 XML）。\n\n"
                              "GDB 输入需要 Link 容器形式的综合知识库，并在其中声明\n"
                              "GDB→SHP 转换步骤（Link 指向的 XML 里 InputDataSource 的\n"
                              "Provider 为 DatastoreType_EsriGDB）。"));
        return false;
    }

    const QString xmlDir = QFileInfo(configXmlPath()).absolutePath();
    const int idx = findGdb2ShpLinkIndex(xmlDir, links);
    if (idx < 0) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("所选综合知识库未包含 GDB→SHP 转换步骤。\n\n"
                              "请在知识库容器里加入一条 Link，指向 InputDataSource 的\n"
                              "Provider 为 DatastoreType_EsriGDB 的转换 XML。"));
        return false;
    }

    const QString tplRel = links[idx].path;
    QString rel = tplRel;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString tplPath = QDir::cleanPath(xmlDir + "/" + rel);
    links.removeAt(idx);   // 转换步骤由阶段1 单独执行，不再作为综合步骤

    QString errMsg;
    tempXmlPath = writeGdb2ShpTempXml(tplPath, gdbDir, outDir, errMsg);
    if (tempXmlPath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("生成 GDB→SHP 转换配置失败：\n%1\n\n模板: %2")
                .arg(errMsg, tplPath));
        tempXmlPath.clear();
        return false;
    }

    const QString s = QStringLiteral(
        "GDB 输入准备：转换步骤=%1，临时 XML=%2，输入 GDB=%3，输出 SHP=%4，"
        "并已从综合步骤中剔除该 Link")
        .arg(tplRel, tempXmlPath, gdbDir, outDir);
    QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
    genDebugLog(s);
    return true;
}

void GeneralizationConfigDialog::onExecute()
{
    // ====== 1. 验证输入 ======
    const QString shpDir  = shpDirectory();
    const QString xmlPath = configXmlPath();
    const QString outDir  = outputDirectory();

    if (shpDir.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("请选择矢量数据目录。"));
        return;
    }
    if (xmlPath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("请选择综合缩编知识库 XML 配置文件。"));
        return;
    }
    if (!QFile::exists(xmlPath)) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("XML 配置文件不存在:\n%1").arg(xmlPath));
        return;
    }
    if (outDir.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("请选择结果输出目录。"));
        return;
    }

    genDebugLog(QStringLiteral("=== 综合缩编开始 数据目录=%1 XML=%2 输出=%3")
        .arg(shpDir, xmlPath, outDir));

    // ====== 1b. 【2026-09-10】输入类型判定：SHP 目录 / FileGDB 目录 ======
    QString kindDetail;
    const InputKind kind = detectInputKind(shpDir, kindDetail);
    {
        const QString s = QStringLiteral("输入类型判定: %1 | %2").arg(shpDir, kindDetail);
        QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
        genDebugLog(s);
    }
    if (kind == InputKind::GdbInvalid) {
        QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
            QString::fromUtf8("该目录不是有效的 FileGDB：\n%1\n\n%2").arg(shpDir, kindDetail));
        return;
    }
    const bool gdbInput = (kind == InputKind::Gdb);

    // ====== 2. 复制源数据到输出目录 (主线程，速度快) ======
    // 【2026-09-10】GDB 输入跳过本步：FileGDB 目录里没有 SHP 可拷，改由阶段1 的
    // gdb→shp 转换把 SHP 产出到 outDir 根（综合子 XML 用裸文件名从 outDir 根读数据）。
    if (!gdbInput)
    {
        QProgressDialog copyProgress(
            QString::fromUtf8("正在复制数据到输出目录..."), QString(), 0, 0, this);
        copyProgress.setWindowModality(Qt::WindowModal);
        copyProgress.setCancelButton(nullptr);
        copyProgress.show();
        QApplication::processEvents();

        int copyResult = copyShapefilesToDir(shpDir, outDir /*, m_selectedFsLayerTypes*/);
        copyProgress.close();

        if (copyResult != 0) {
            QString detail;
            if (copyResult == 1)
                detail = QString::fromUtf8("目录不存在！\n\n路径: %1").arg(shpDir);
            else if (copyResult == 2)
                detail = QString::fromUtf8(
                    "该目录下未找到 .shp 文件。\n\n路径: %1").arg(shpDir);
            else
                detail = QString::fromUtf8(
                    "GDAL 无法打开任何 SHP 文件！\n\n路径: %1").arg(shpDir);
            QMessageBox::warning(this, QString::fromUtf8("综合缩编"), detail);
            return;
        }
    }

    // ====== 3. 解析 XML (主线程) ======
    // 使用用户在对话框里选的 XML（此前误用硬编码 testXmlPath=plugins/know20260313/
    // 10000_know.xml，用户选的文件被忽略——麒麟实测日志里 XML目录 恒为 know20260313，
    // 2026-08-19 修正）。DLL/SDK 目录固定为插件目录（NMO SDK 所在，麒麟 /opt/ltzk/plugins）。
    const QString appDir       = QCoreApplication::applicationDirPath();
    const QString pluginsDir   = QDir::cleanPath(appDir + "/../plugins");
    const QString testDllDir   = pluginsDir;

    QVector<GeneralizationWorker::LinkEntry> links;
    QString containerRelativePath;
    bool isLinkContainer = false;

    {
        QFile f(xmlPath);   // 用户选择的 XML（此前是 testXmlPath，已修正）
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
                QString::fromUtf8("无法读取 XML 文件:\n%1").arg(xmlPath));
            return;
        }
        QXmlStreamReader xml(&f);
        while (!xml.atEnd() && !xml.hasError()) {
            xml.readNext();
            if (xml.isStartElement()) {
                if (xml.name() == QStringLiteral("MapGeneBatchProcessingLink")) {
                    isLinkContainer = true;
                    containerRelativePath = xml.attributes().value("relativePath").toString();
                }
                else if (xml.name() == QStringLiteral("Link") && isLinkContainer) {
                    GeneralizationWorker::LinkEntry e;
                    e.run  = (xml.attributes().value("run").toString() == QStringLiteral("true"));
                    e.path = xml.readElementText().trimmed();
                    links.append(e);
                }
            }
        }
        f.close();
    }

    QString dataDir = outDir;
    // Link 容器：xmlDir = XML 所在目录（子 XML 相对它解析）；直接格式：xmlDir = XML 完整路径
    QString xmlDir  = QFileInfo(xmlPath).absolutePath();

    const QString xmlTypeLog =
        QStringLiteral("XML 类型: %1, Link 数量: %2, 数据目录: %3, XML目录: %4")
            .arg(isLinkContainer ? QStringLiteral("Link容器") : QStringLiteral("直接"))
            .arg(links.size()).arg(dataDir).arg(xmlDir);
    QgsMessageLog::logMessage(xmlTypeLog, LOG_TAG, Qgis::Info);
    genDebugLog(xmlTypeLog);

    if (!isLinkContainer) {
        xmlDir = xmlPath;
    }

    // ====== 3b. 【2026-09-10】GDB 输入：准备阶段1（gdb→shp 转换）并分流 ======
    if (gdbInput) {
        QString tempXml;
        if (!prepareGdb2ShpStage(shpDir, outDir, isLinkContainer, links, tempXml))
            return;   // 已弹错

        // 阶段2（综合）参数先存好，等阶段1 的 finished 回调里启动
        m_stage2XmlDir      = xmlDir;
        m_stage2DataDir     = dataDir;
        m_stage2DllDir      = testDllDir;
        m_stage2Links       = links;   // prepareGdb2ShpStage 已剔除转换 Link
        m_pendingGdb2ShpRun = true;

        // 阶段1：单个 XML 直接跑（links 为空 → worker 把 xmlDir 当完整文件路径）
        startWorkerRun(tempXml, outDir, QVector<GeneralizationWorker::LinkEntry>(),
                       testDllDir, QString::fromUtf8("GDB→SHP 转换"));
        return;
    }

    // SHP 输入：若知识库声明了 gdb→shp 转换步骤，从综合步骤里剔除（输入不是 GDB，
    // 跑它没有意义且会失败）。知识库没有该 Link 时此段什么都不做 → 与改造前一致。
    {
        const int gdbLinkIdx = findGdb2ShpLinkIndex(QFileInfo(xmlPath).absolutePath(), links);
        if (gdbLinkIdx >= 0) {
            const QString removed = links[gdbLinkIdx].path;
            links.removeAt(gdbLinkIdx);
            const QString s = QStringLiteral(
                "SHP 输入：知识库含 GDB→SHP 转换 Link，已从综合步骤剔除: %1").arg(removed);
            QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
            genDebugLog(s);
        }
    }

    // ====== 4. 创建子线程 Worker 并启动 ======
    startWorkerRun(xmlDir, dataDir, links, testDllDir, QString::fromUtf8("综合缩编"));
}
