/**
 * @file    generalization_config_dialog.cpp
 * @brief   综合缩编配置对话框 — LoadLibrary + 解析 Link 容器 + DoXMLFile
 */

#include "generalization_config_dialog.h"
#include "product_source_dialog.h"
#include "layer_type_select_dialog.h"
#include "param_config_dialog.h"
#include "scale_selector_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
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
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QTextStream>
// 【2026-11-07】综合结果补 .cpg：需要判断 dbf 里中文的实际字节编码
#include <QDirIterator>
#include <QTextCodec>
#include <gdal_priv.h>
#include <ogr_api.h>
// 【2026-10-23】路线B：插件自己把 GDB 转成 SHP 工作副本，需要 OGR 的 C++ 接口
#include <ogrsf_frmts.h>
#include <cpl_string.h>
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
    // 【2026-10-18】改用网格布局：第二行的「从数据库获取数据」放在第 1 列，
    // 与上一行输入框共用同一列，左边缘自动对齐（原先用 addSpacing(96) 估算，
    // 字体或 DPI 一变就错位）。
    auto* gridFile = new QGridLayout();
    gridFile->setContentsMargins(0, 0, 0, 0);
    gridFile->setHorizontalSpacing(6);
    gridFile->setVerticalSpacing(4);

    gridFile->addWidget(new QLabel(QString::fromUtf8("矢量数据目录:"), this), 0, 0);
    m_lineEditShpDir = new QLineEdit(this);
    // 【2026-09-10】同一入口同时接受 SHP 目录与 FileGDB 目录（GDB 会先转 SHP 再综合）
    m_lineEditShpDir->setPlaceholderText(
        QString::fromUtf8("选择包含SHP文件的目录，或FileGDB"));
    gridFile->addWidget(m_lineEditShpDir, 0, 1);
    auto* btnBrowseShpDir = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseShpDir->setFixedWidth(80);
    btnBrowseShpDir->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseShpDir, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseShpDir);
    gridFile->addWidget(btnBrowseShpDir, 0, 2);

    // 【2026-10-18】矢量数据目录下方：从数据库获取数据。
    // 弹出成果库目录树（带复选框/右键导出），实现见 product_source_dialog.*
    auto* btnProductSource = new QPushButton(
        QString::fromUtf8("从数据库获取数据"), this);
    btnProductSource->setFixedWidth(160);
    btnProductSource->setStyleSheet(kNarrowBtnStyle);
    connect(btnProductSource, &QPushButton::clicked,
        this, &GeneralizationConfigDialog::onOpenProductSource);
    gridFile->addWidget(btnProductSource, 1, 1, Qt::AlignLeft);

    gridFile->setColumnStretch(1, 1);
    vFile->addLayout(gridFile);

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
    // 【2026-10-18】改用网格布局：下面「自定义综合比例尺」按钮与输入框同列，
    // 左边缘自动对齐（原先按钮行从最左边开始，比输入框靠左一个标签宽度）。
    auto* gridCfg = new QGridLayout();
    gridCfg->setContentsMargins(0, 0, 0, 0);
    gridCfg->setHorizontalSpacing(6);
    gridCfg->setVerticalSpacing(6);

    gridCfg->addWidget(new QLabel(QString::fromUtf8("配置文件:"), this), 0, 0);
    m_lineEditConfigXml = new QLineEdit(this);
    m_lineEditConfigXml->setPlaceholderText(QString::fromUtf8("选择xml文件 ..."));
    gridCfg->addWidget(m_lineEditConfigXml, 0, 1);
    auto* btnBrowseConfigXml = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseConfigXml->setFixedWidth(80);
    btnBrowseConfigXml->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseConfigXml, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseConfigXml);
    gridCfg->addWidget(btnBrowseConfigXml, 0, 2);
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
    gridCfg->addWidget(btnParamConfig, 0, 3);

    // 第2行: 自定义综合比例尺按钮
    auto* btnScaleSelector = new QPushButton(QString::fromUtf8("自定义综合比例尺"), this);
    btnScaleSelector->setMinimumHeight(32);
    // 原样式硬编码了浅色主题的 color:#333（深灰），在平台深色主题下文字近乎不可见，
    // 与其它按钮（平台 QPushButton color:#f8feff）不一致。这里移除 color 覆盖，让文字
    // 颜色回落到平台全局 QSS；仅保留加粗强调与内边距，悬停白色与平台一致。
    btnScaleSelector->setStyleSheet(
        "QPushButton { font-weight: bold; padding: 4px 16px; }"
        "QPushButton:hover { color: #ffffff; }");
    connect(btnScaleSelector, &QPushButton::clicked, this, [this]() {
        // 【2026-10-09】初始比例尺改成“从主 XML 里识别出来的当前比例尺”，
        // 不再是写死的 10000；识别结果在对话框里还能手改。
        const QString xmlPath = m_lineEditConfigXml->text().trimmed();
        ScaleSelectorDialog dlg(this);
        dlg.setXmlFilePath(xmlPath);

        const int curScale = xmlPath.isEmpty()
                                 ? -1
                                 : ScaleSelectorDialog::detectCurrentScale(xmlPath);
        dlg.setScale(curScale > 0 ? curScale : 10000);

        if (dlg.exec() == QDialog::Accepted) {
            // 【2026-10-09 之七】保存 / 另存为都可能换掉当前引用的 XML：保存可能在知识库
            // 重命名里改了主 XML 的文件名，另存为则把配置换成了新目录下的副本。两者都按
            // **完整路径**收回来，否则配置框会继续指着一个已经不存在（或仍是旧的）文件。
            const QString newPath = dlg.xmlFilePath();
            const QString normNew = QDir::cleanPath(newPath);
            const QString normOld = xmlPath.isEmpty() ? QString() : QDir::cleanPath(xmlPath);
            if (!normNew.isEmpty()
                && normNew.compare(normOld, Qt::CaseInsensitive) != 0) {
                m_lineEditConfigXml->setText(QDir::toNativeSeparators(newPath));
            }
        }
    });
    // 【2026-11-02】「自定义综合比例尺」右侧新增「制图主区名称」，整行放在网格
    // 第 3 行、第 1 列（与上方「配置文件」「注记工程目录」输入框同列）。
    // 名称由数据里的 BOUA5（县级行政区划面）自动识别，用户可手改；执行前据此校验
    // 知识库里对应县的 XML 是否存在，并把主 XML 的县级 Link 切到该县。
    auto* hScaleRow = new QHBoxLayout();
    hScaleRow->setContentsMargins(0, 0, 0, 0);
    hScaleRow->setSpacing(6);
    hScaleRow->addWidget(btnScaleSelector);
    hScaleRow->addWidget(new QLabel(QString::fromUtf8("制图主区名称:"), this));
    m_lineEditMainRegion = new QLineEdit(this);
    m_lineEditMainRegion->setPlaceholderText(QString::fromUtf8("自动识别自数据，可修改"));
    m_lineEditMainRegion->setToolTip(QString::fromUtf8(
        "从矢量数据中的县级行政区划面（BOUA5）自动识别，例如「永登县」。\n"
        "执行前会用该名称校验知识库中对应县的 XML，并切换主 XML 的链接。"));
    hScaleRow->addWidget(m_lineEditMainRegion, 1);
    // 第 2 行控件多（按钮 + 标签 + 识别），只占第 1 列时输入框会被压到只剩几十像素，
    // 所以整行跨到第 3 列（与上一行「参数配置」按钮右边缘对齐），并给一个最小宽度保底。
    m_lineEditMainRegion->setMinimumWidth(180);
    auto* btnDetectRegion = new QPushButton(QString::fromUtf8("识别"), this);
    btnDetectRegion->setFixedWidth(60);
    btnDetectRegion->setStyleSheet(kNarrowBtnStyle);
    btnDetectRegion->setToolTip(QString::fromUtf8("按当前矢量数据重新识别主区名称"));
    connect(btnDetectRegion, &QPushButton::clicked,
            this, &GeneralizationConfigDialog::onDetectMainRegion);
    hScaleRow->addWidget(btnDetectRegion);
    gridCfg->addLayout(hScaleRow, 2, 1, 1, 3);   // 跨到第 3 列，给输入框留够宽度
    gridCfg->setColumnStretch(1, 1);

    // 第2行: 【2026-11-07】注记工程目录——原先放在「输出」区域，按需求移到 XML 配置区，
    // 紧跟「配置文件」那一行下面。
    // 甲方给的 .2dm 地图工程文件（道路代码/河流名称/道路名称等注记的定义）都放在一个
    // 文件夹里。知识库里 <Mission id="405"> 的 <ProjectFile> 只写文件名，引擎按 DoXMLFile
    // 的 relativePath（本插件传的就是"结果目录"）去解析，所以执行前要把这些 .2dm 拷进
    // 结果目录，否则报"找不到 .2dm 文件"。
    // 【2026-10-13】原先末尾挂的「复制到结果目录」复选框已按需求去掉：填了目录本来就只有
    // "拷进结果目录"这一种有意义的行为，现在只要目录非空就一律拷（见 onExecute）。
    // 【2026-11-07】标签放进网格第 0 列、输入框放第 1 列、浏览放第 2 列，与上面
    // 「配置文件」那一行同列，两行的标签左边缘、输入框左边缘、浏览按钮左边缘全部对齐。
    auto* lblLabelProj = new QLabel(QString::fromUtf8("注记工程目录:"), this);
    gridCfg->addWidget(lblLabelProj, 1, 0);
    m_lineEditLabelProjectDir = new QLineEdit(this);
    m_lineEditLabelProjectDir->setPlaceholderText(
        QString::fromUtf8("选择存放 .2dm 注记地图工程的文件夹..."));
    gridCfg->addWidget(m_lineEditLabelProjectDir, 1, 1);
    auto* btnBrowseLabelProj = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseLabelProj->setFixedWidth(80);
    btnBrowseLabelProj->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseLabelProj, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseLabelProjectDir);
    gridCfg->addWidget(btnBrowseLabelProj, 1, 2);
    // 【2026-11-08】「制作注记工程文件」——打开外部程序「无极地图工作系统」。
    // 知识库 14_静态注记生成.xml 的 Mission 405「动态注记转静态注记」要读 .2dm
    // 注记工程文件（<ProjectFile>），该文件原本由甲方在工作站里配好动态注记后另存。
    // 这个按钮把"去工作站做工程"这一步收进对话框：做完把 .2dm 存进上面的
    // 「注记工程目录」，综合执行时 copyLabelProjectFiles() 会把它拷进结果目录。
    auto* btnMakeLabelProj = new QPushButton(QString::fromUtf8("制作注记工程文件"), this);
    btnMakeLabelProj->setStyleSheet(kNarrowBtnStyle);
    btnMakeLabelProj->setToolTip(QString::fromUtf8(
        "打开无极地图工作系统，在里面对成果数据配置动态注记并另存为 .2dm，\n"
        "存到上面的「注记工程目录」即可被综合任务的 405 注记步骤引用。"));
    connect(btnMakeLabelProj, &QPushButton::clicked,
            this, &GeneralizationConfigDialog::onOpenLabelProjectMaker);
#if !defined(Q_OS_WIN)
    // 无极工作站目前只有 Windows 版（build_hubei 是 MSVC 产物），麒麟上先置灰。
    btnMakeLabelProj->setEnabled(false);
    btnMakeLabelProj->setToolTip(QString::fromUtf8(
        "无极地图工作系统当前仅提供 Windows 版本"));
#endif
    gridCfg->addWidget(btnMakeLabelProj, 1, 3);

    vCfg->addLayout(gridCfg);

    mainLayout->addWidget(groupCfg);

    // ====================================================================
    // 3. 输出目录
    // ====================================================================
    auto* groupOutput = new QGroupBox(QString::fromUtf8(" 输出"), this);
    groupOutput->setStyleSheet(
        QString::fromUtf8("QGroupBox::title { padding-top: 6px; } "
                          "QGroupBox { padding-top: 12px; }"));
    auto* vOutput = new QVBoxLayout(groupOutput);
    vOutput->setSpacing(6);

    auto* hOutput = new QHBoxLayout();
    auto* lblOutputDir = new QLabel(QString::fromUtf8("结果目录:"), this);
    lblOutputDir->setMinimumWidth(96);
    hOutput->addWidget(lblOutputDir);
    m_lineEditOutputDir = new QLineEdit(this);
    m_lineEditOutputDir->setPlaceholderText(QString::fromUtf8("选择综合缩编结果输出目录..."));
    hOutput->addWidget(m_lineEditOutputDir, 1);
    auto* btnBrowseOutputDir = new QPushButton(QString::fromUtf8("浏览"), this);
    btnBrowseOutputDir->setFixedWidth(80);
    btnBrowseOutputDir->setStyleSheet(kNarrowBtnStyle);
    connect(btnBrowseOutputDir, &QPushButton::clicked, this, &GeneralizationConfigDialog::onBrowseOutputDir);
    hOutput->addWidget(btnBrowseOutputDir);
    vOutput->addLayout(hOutput);

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

    // 【2026-11-02】恢复上一次使用的制图主区名称（选择数据源后会被重新识别覆盖）
    {
        QSettings st(QStringLiteral("GarMap"), QStringLiteral("MapGeneralization"));
        m_lineEditMainRegion->setText(st.value(QStringLiteral("mainRegion")).toString());
    }

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

void GeneralizationConfigDialog::setQgisInterface(QgisInterface* iface)
{
    m_pQgisIface = iface;
}

// 【2026-10-18】从数据库获取数据：非模态打开，保证「按范围导出」手动绘制时
// 地图画布可用（模态子对话框会挡住画布交互）。
void GeneralizationConfigDialog::onOpenProductSource()
{
    auto* dlg = new ProductSourceDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setModal(false);
    dlg->setQgisInterface(m_pQgisIface);
    // 【2026-10-18】右键「…并设置为数据源」导出成功后，直接把结果填进矢量数据目录
    connect(dlg, &ProductSourceDialog::dataSourceReady, this,
        [this](const QString& path)
        {
            if (path.isEmpty()) return;
            m_lineEditShpDir->setText(QDir::toNativeSeparators(path));
            // 【2026-11-02】数据库侧导出的数据源同样触发主区名称识别
            onDetectMainRegion();
        });
    // 【2026-11-07】「按范围导出」里点「开始绘制」时，数据源对话框会先藏起来让用户
    // 在地图上框选。它本身是本对话框的子窗口，父窗口不藏的话会一直挡在地图前面，
    // 所以这里跟着一起 hide()，绘制完（drawingFinished）再一起弹回来。
    ProductSourceDialog* srcDlg = dlg;
    connect(dlg, &ProductSourceDialog::drawingStarted, this, [this]() { hide(); });
    connect(dlg, &ProductSourceDialog::drawingFinished, this, [this, srcDlg]() {
        show();
        raise();
        activateWindow();
        if (srcDlg)
        {
            srcDlg->show();
            srcDlg->raise();
            srcDlg->activateWindow();
        }
    });
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

QString GeneralizationConfigDialog::shpDirectory() const    { return m_lineEditShpDir->text().trimmed(); }
QString GeneralizationConfigDialog::gdbDirectory() const    { return m_lineEditGdbDir->text().trimmed(); }
QString GeneralizationConfigDialog::convXmlPath() const      { return m_lineEditConvXml->text().trimmed(); }
QString GeneralizationConfigDialog::configXmlPath() const    { return m_lineEditConfigXml->text().trimmed(); }
QString GeneralizationConfigDialog::outputDirectory() const  { return m_lineEditOutputDir->text().trimmed(); }
// 【2026-10-10】注记工程目录（.2dm 所在文件夹）
// 【2026-10-13】原来配套的「复制到结果目录」开关（copyLabelProjectToResult）已按需求
// 连同复选框一起去掉：只要这个目录非空，执行前就一律把 *.2dm 拷进结果目录。
QString GeneralizationConfigDialog::labelProjectDir() const
{
    return m_lineEditLabelProjectDir ? m_lineEditLabelProjectDir->text().trimmed() : QString();
}
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
    if (!dir.isEmpty()) {
        m_lineEditShpDir->setText(dir);
        // 【2026-11-02】换了数据源就重新识别主区名称（覆盖上一次的自动/手工值）
        onDetectMainRegion();
    }
}

// ====== 【2026-11-02】制图主区名称 ======

void GeneralizationConfigDialog::onDetectMainRegion()
{
    if (!m_lineEditMainRegion) return;
    const QString name = detectMainRegionName(shpDirectory());
    if (!name.isEmpty())
        m_lineEditMainRegion->setText(name);
}

// 从数据源的 BOUA5（县级行政区划面）识别主区名称。
// 原始 GDB 里没有「县级行政区划面」这个图层，它是知识库任务 40 把 BOUA5 改名后的
// 中间名（见 1数据预处理_区县\预处理新增.xml），所以这里直接读 BOUA5。
QString GeneralizationConfigDialog::detectMainRegionName(const QString& dataPath) const
{
    if (dataPath.isEmpty()) return QString();

    static bool gdalRegistered = false;
    if (!gdalRegistered) { GDALAllRegister(); gdalRegistered = true; }

    // 1. 定位 GDB：路径本身就是 .gdb 目录 → 直接用；否则在目录下找第一个 .gdb
    QString gdbPath;
    if (dataPath.endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive)
        && QFileInfo(dataPath).isDir()) {
        gdbPath = dataPath;
    } else {
        const QDir d(dataPath);
        const QStringList subs = d.entryList(QStringList() << QStringLiteral("*.gdb"),
                                             QDir::Dirs | QDir::NoDotAndDotDot);
        if (!subs.isEmpty())
            gdbPath = d.absoluteFilePath(subs.first());
    }
    if (gdbPath.isEmpty()) return QString();

    // 2. 打开 GDB，取县级行政区划面图层
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(gdbPath.toUtf8().constData(), GDAL_OF_VECTOR,
                   nullptr, nullptr, nullptr));
    if (!ds) return QString();

    OGRLayer* countyLayer = ds->GetLayerByName("BOUA5");
    if (!countyLayer) {
        const QString cnName = QString::fromUtf8("县级行政区划面");
        for (int i = 0; i < ds->GetLayerCount() && !countyLayer; ++i) {
            OGRLayer* l = ds->GetLayer(i);
            if (l && QString::fromUtf8(l->GetName()).contains(cnName))
                countyLayer = l;
        }
    }

    // 3. 过滤 + 按几何面积取最大
    QString best;
    double  bestArea = -1.0;
    if (countyLayer) {
        countyLayer->ResetReading();
        while (OGRFeature* f = countyLayer->GetNextFeature()) {
            const QString name = QString::fromUtf8(f->GetFieldAsString("NAME")).trimmed();
            do {
                // 裁剪切出来的邻县碎片，NAME 形如「属民和回族土族自治县」
                if (name.isEmpty() || name.startsWith(QString::fromUtf8("属"))) break;
                // 只认甘肃的县：跨省范围里青海的县（AreaCode=630000）也会被裁进来
                const int areaCode = f->GetFieldAsInteger("AreaCode");
                if (areaCode != 0 && areaCode != 620000) break;
                OGRGeometry* g = f->GetGeometryRef();
                if (!g) break;
                // 用 C API 求面积：OGRGeometry 基类没有 get_Area()。
                // 地理坐标下单位是度²、投影坐标下是米²，但只在同一图层内比大小，无影响。
                const double a = OGR_G_Area(reinterpret_cast<OGRGeometryH>(g));
                if (a > bestArea) { bestArea = a; best = name; }
            } while (false);
            OGRFeature::DestroyFeature(f);
        }
    }
    GDALClose(ds);
    return best;
}

// 【2026-11-07】把主区名称写进「县预处理 XML 的 SQL」里，而不是去切 Link。
// 步骤：
//   1) 读主 XML，找到第一条 1数据预处理_区县\<县名>.xml 的 Link（跳过公共文件）；
//   2) 打开它指向的那个县 XML；
//   3) 只改 <Mission id="11" note="属性查询:县级行政区划面"> 块里
//      <SQL>NAME LIKE '%<县名>'</SQL> 的县名，其它 Mission / 其它内容不动；
//   4) 写回原文件（保持原 BOM 与换行），按需求不做 .bak 备份。
// 县名用正则从 SQL 里现抓再换（不是写死"会宁县"），所以换成任何县、反复执行都幂等。
int GeneralizationConfigDialog::applyMainRegionToXml(const QString& xmlPath,
                                                     const QString& region)
{
    if (xmlPath.isEmpty() || region.isEmpty()) return 0;

    // ---- 1. 主 XML：找 Link 指向的县预处理文件 ----
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray raw = f.readAll();
    f.close();

    const bool hasBom = raw.startsWith("\xEF\xBB\xBF");
    const QString mainText = QString::fromUtf8(hasBom ? raw.mid(3) : raw);

    // 预处理目录里的公共文件不是县文件，不作数
    const QStringList skipPre = {
        QString::fromUtf8("预处理"),
        QString::fromUtf8("预处理新增"),
        QString::fromUtf8("分层改名")
    };

    QString countyName;
    {
        const QRegularExpression reLink(QStringLiteral(
            "1数据预处理_区县[\\\\/]([^<\\\\/]+)\\.xml"));
        auto it = reLink.globalMatch(mainText);
        while (it.hasNext()) {
            const QString n = it.next().captured(1);
            if (!skipPre.contains(n)) { countyName = n; break; }
        }
    }
    if (countyName.isEmpty()) return 0;

    // ---- 2. 打开被 Link 指向的县 XML ----
    const QString kbDir = QFileInfo(xmlPath).absolutePath();
    const QString countyPath = QDir(kbDir).absoluteFilePath(
        QStringLiteral("1数据预处理_区县/%1.xml").arg(countyName));

    QFile cf(countyPath);
    if (!cf.open(QIODevice::ReadOnly)) return 0;
    const QByteArray craw = cf.readAll();
    cf.close();

    const bool cBom = craw.startsWith("\xEF\xBB\xBF");
    const QString text = QString::fromUtf8(cBom ? craw.mid(3) : craw);

    // ---- 3. 只在「属性查询:县级行政区划面」这个 Mission 块内改 SQL ----
    // 注意引擎不认 XML 注释，但这里仍按 note 精确匹配，省/市/乡镇级那些块一概不碰。
    const QRegularExpression reMission(QStringLiteral(
        "<Mission\\s+id=\"11\"\\s+note=\"属性查询:县级行政区划面\"[^>]*>(.*?)</Mission>"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = reMission.match(text);
    if (!m.hasMatch()) return 0;

    const QString block = m.captured(1);
    // &apos; 与普通单引号两种写法都认
    const QRegularExpression reSql(QStringLiteral(
        "(<SQL>\\s*NAME\\s+LIKE\\s+&apos;%)([^&<]*)(&apos;\\s*</SQL>)"));

    QString newBlock;
    newBlock.reserve(block.size());
    int last = 0, changed = 0;
    auto it2 = reSql.globalMatch(block);
    while (it2.hasNext()) {
        const QRegularExpressionMatch sm = it2.next();
        if (sm.captured(2) == region) continue;   // 已经是目标县名，不动
        newBlock += block.mid(last, sm.capturedStart(2) - last);
        newBlock += region;
        last = sm.capturedEnd(2);
        ++changed;
    }
    if (changed == 0) return 0;
    newBlock += block.mid(last);

    const QString newText = text.left(m.capturedStart(1))
                          + newBlock
                          + text.mid(m.capturedEnd(1));

    // ---- 4. 写回县 XML（保持原 BOM；按需求不做备份） ----
    QFile out(countyPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 0;
    QByteArray bytes = newText.toUtf8();
    if (cBom) bytes.prepend("\xEF\xBB\xBF");
    const qint64 w = out.write(bytes);
    out.close();
    return w == bytes.size() ? changed : 0;
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

// 【2026-10-10】注记工程目录浏览：选的是"文件夹"（甲方把 .2dm 打包成一个目录给），
// 不是单个文件——样例里一次要用两个 .2dm，选文件夹才能整包拷过去。
void GeneralizationConfigDialog::onBrowseLabelProjectDir()
{
    QString startDir = m_lineEditLabelProjectDir->text().trimmed();
    if (startDir.isEmpty()) startDir = m_lineEditOutputDir->text().trimmed();
    QString dir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择注记工程目录（存放 .2dm 的文件夹）"), startDir);
    if (!dir.isEmpty()) m_lineEditLabelProjectDir->setText(dir);
}

// 【2026-11-08】打开无极地图工作系统（只启动，不带工程参数）。
// 目录约定：<LTZK 根>\无极工作站\无极地图工作系统.exe
//   appDir 一般是 <LTZK 根>\bin（与 pluginsDir = appDir/../plugins 同源），故先按
//   appDir/.. 拼；再退一步试 appDir 本身，兼容插件被平铺到根目录的部署。
//   两个候选都找不到就把它们一起弹出来，现场改目录名/放错位置能一眼看出。
// 为什么只启动不带参数：工作站的命令行只认一个位置参数（.2dm 工程路径），
// 传不了数据目录；而 .2dm 里的符号与注记规则是几百行 DrawObject，程序生成不了，
// 只能由人在工作站里配。
void GeneralizationConfigDialog::onOpenLabelProjectMaker()
{
    const QString exeName = QString::fromUtf8("无极地图工作系统.exe");
    const QString subDir  = QString::fromUtf8("无极工作站");
    const QString appDir  = QCoreApplication::applicationDirPath();

    QStringList candidates;
    candidates << QDir::cleanPath(appDir + "/../" + subDir + "/" + exeName)
               << QDir::cleanPath(appDir + "/" + subDir + "/" + exeName);

    QString exePath;
    for (const QString& cand : candidates) {
        if (QFileInfo::exists(cand)) { exePath = cand; break; }
    }

    if (exePath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("制作注记工程文件"),
            QString::fromUtf8("没有找到无极地图工作系统：\n\n%1\n\n"
                              "请把工作站整个目录拷贝到 LTZK 安装目录下，命名为「%2」，\n"
                              "并确保其中存在「%3」。")
                .arg(candidates.join(QString::fromUtf8("\n或\n")), subDir, exeName));
        genDebugLog(QStringLiteral("[注记工程] 未找到工作站，候选路径: %1")
                        .arg(candidates.join(QStringLiteral(" | "))));
        return;
    }

    const QString workDir = QFileInfo(exePath).absolutePath();
    if (!QProcess::startDetached(exePath, QStringList(), workDir)) {
        QMessageBox::warning(this, QString::fromUtf8("制作注记工程文件"),
            QString::fromUtf8("启动无极地图工作系统失败：\n%1").arg(exePath));
        genDebugLog(QStringLiteral("[注记工程] 启动失败: %1").arg(exePath));
        return;
    }

    genDebugLog(QStringLiteral("[注记工程] 已启动工作站: %1").arg(exePath));
    QgsMessageLog::logMessage(
        QStringLiteral("已启动无极地图工作系统: %1").arg(exePath), LOG_TAG, Qgis::Info);
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

// 【2026-11-03】Linux 专用：执行前把子 XML 里 <FilePath> 内容中的 Windows 反斜杠
// 换成正斜杠，再生成一份临时副本交给 DoXMLFile。
//
// 背景：知识库 XML 的 <ParaOut>/<ParaIn> 用 Windows 习惯写路径
// （如 <FilePath>10w\del2村道合_备份.shp</FilePath>）。Windows 上 \ 是分隔符；
// Linux 上它只是普通文件名字符，SDK 会把整串当成一个文件名，于是在结果根目录
// 产出"名字里带反斜杠"的单文件（麒麟实测 综合结果/10w\del2村道合_备份.shp）。
// 输出侧表现为目录结构丢失，输入侧更隐蔽——ParaIn 读不到文件时该 Mission
// 可能空跑而不报错。全库统计 978 处（ParaOut 642 / ParaIn 336）。
//
// 只动 <FilePath> 标签内的内容：SQL 标签里也有反斜杠（54 处，如 NAME='\'），
// 那是转义字符，全局替换会改查询语义。
//
// 临时副本放在原 XML 同目录（文件名前加 '.' 隐藏、后加 __kylin__+pid 防撞），
// 这样 XML 之间的相对引用不受影响；调用方用完删除。
// 返回临时文件路径；无需替换（没有反斜杠路径）或无法安全处理时返回空串。
static QString makeBackslashFixedXmlCopy(const QString& xmlPath)
{
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray raw = f.readAll();
    f.close();

    // 只处理 UTF-8 / ASCII。UTF-8 的多字节序列不会出现 0x5C，按字符替换安全；
    // GBK 的尾字节可能正好是 0x5C，按字节替换会把汉字拆坏，故非 UTF-8 一律放弃。
    // 原文件的 BOM（若有）会以 U+FEFF 保留在字符串里，写回时原样输出。
    QString text = QString::fromUtf8(raw);
    if (text.contains(QChar(0xFFFD)))                     // 解不出 UTF-8
        return QString();

    static const QRegularExpression re(
        QStringLiteral("<FilePath\\b[^>]*>([^<]*)</FilePath>"));
    QString out;
    int last = 0;
    int changed = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString body = m.captured(1);
        if (!body.contains(QLatin1Char('\\')))
            continue;
        const int bodyStart = m.capturedStart(1);
        out += text.mid(last, bodyStart - last);
        QString fixed = body;
        fixed.replace(QLatin1Char('\\'), QLatin1Char('/'));
        out += fixed;
        last = bodyStart + body.length();
        changed++;
    }
    if (changed == 0)
        return QString();
    out += text.mid(last);

    // 临时副本沿用原扩展名（.xml），避免 SDK 按后缀判断格式时出问题
    const QFileInfo fi(xmlPath);
    QString tmpPath = fi.absolutePath() + QLatin1Char('/')
        + QLatin1Char('.') + fi.completeBaseName()
        + QStringLiteral(".__kylin__")
        + QString::number(QCoreApplication::applicationPid());
    if (!fi.suffix().isEmpty())
        tmpPath += QLatin1Char('.') + fi.suffix();
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    tmp.write(out.toUtf8());
    tmp.close();
    return tmpPath;
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


// ============================================================================
// 【2026-10-23】路线 B：知识库不自带 GDB→SHP 转换步骤时，插件自己转一份 SHP 工作副本
//   背景（实测）：引擎本身能直接对 .gdb 综合——DoXMLFile 的 relativePath 指向 .gdb
//   目录时，任务里写的 "xxx.shp" 会被当作 GDB 内的图层名，读写都在库内完成。甘肃这套
//   库也正是按 GDB 写的（主 XML 的 relativePath 就是 .gdb，整链没有 gdb2shp 那一步）。
//   但实测有两处硬伤，短时间内只能绕：
//     ① 3_水系综合.xml 在 GDB 模式下必崩（RiverGeneralization.dll+0x61571，厂家自带的
//        MapBatchProcessing.exe 跑同样崩）；同一份数据转成 SHP 目录再跑则正常；
//     ② 405 注记步骤的 <ProjectFile>.2dm 与 <OutputDirectory> 都按 relativePath 解析，
//        relativePath 是 .gdb 时既要把 .2dm 塞进 .gdb 目录，它产出的 .shp 还会写进
//        .gdb 内部，之后 GDAL 会把该目录认成 Shapefile 驱动，GDB 等于废掉。
//   所以这里改成：先在结果目录里铺一份 SHP 工作副本，再按 SHP 输入跑整条链。
//   知识库一个字都不用改，也不需要它包含 gdb2shp 那一步。
// ============================================================================

// DBF 字段名上限 10 字节：按 UTF-8 字符边界截断，避免把汉字切成半个
static QByteArray truncateUtf8Bytes(const QByteArray& s, int nMaxBytes)
{
    if (nMaxBytes <= 0 || s.size() <= nMaxBytes) return s;
    int k = nMaxBytes;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return s.left(k);
}

// 列出 FileGDB 里的图层名；打开失败返回 false
static bool listGdbLayers(const QString& gdbDir, QStringList& names)
{
    names.clear();
    GDALAllRegister();
    const char* apszAllowed[] = { "OpenFileGDB", "FileGDB", nullptr };
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(gdbDir.toUtf8().constData(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY, apszAllowed, nullptr, nullptr);
    if (!poDS) return false;
    for (int i = 0; i < poDS->GetLayerCount(); ++i) {
        OGRLayer* poLayer = poDS->GetLayer(i);
        if (poLayer) names << QString::fromUtf8(poLayer->GetName());
    }
    GDALClose(poDS);
    return true;
}

// 读取知识库 XML 里 <LayerNameList> 的映射（源图层名 → 输出名）。
// 注释掉的条目 QXmlStreamReader 会自动跳过，正好符合知识库的写法。
static QVector<QPair<QString, QString> > readLayerNameList(const QString& xmlPath)
{
    QVector<QPair<QString, QString> > out;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QXmlStreamReader xml(&f);
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("LayerName")) continue;
        const QString newName =
            xml.attributes().value(QStringLiteral("newname")).toString().trimmed();
        const QString srcName = xml.readElementText().trimmed();
        if (srcName.isEmpty()) continue;
        out.append(qMakePair(srcName, newName.isEmpty() ? srcName : newName));
    }
    f.close();
    return out;
}

// 把 FileGDB 的全部图层转成 SHP 写进 dstDir（输出名 = 图层名，DBF 编码 UTF-8）。
// 口径与插件「格式转换」里的 GDB→SHP 一致：字段名按 UTF-8 边界截到 10 字节、重名补
// _1/_2；SHP 不支持 Z/M，几何先拍平到 2D；.prj 用 ESRI WKT，ArcGIS 侧读着更稳。
static bool convertGdbDirToShp(const QString& gdbDir, const QString& dstDir, QString& msg)
{
    GDALAllRegister();
    const char* apszAllowed[] = { "OpenFileGDB", "FileGDB", nullptr };
    GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(gdbDir.toUtf8().constData(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY, apszAllowed, nullptr, nullptr);
    if (!poSrcDS) {
        msg = QStringLiteral("GDAL 无法打开 GDB：%1（%2）")
                  .arg(gdbDir, QString::fromUtf8(CPLGetLastErrorMsg()));
        return false;
    }
    GDALDriver* poShpDrv = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poShpDrv) {
        GDALClose(poSrcDS);
        msg = QStringLiteral("GDAL 未找到 ESRI Shapefile 驱动");
        return false;
    }
    if (!QDir().mkpath(dstDir)) {
        GDALClose(poSrcDS);
        msg = QStringLiteral("无法创建结果目录：%1").arg(dstDir);
        return false;
    }

    const int nLayers = poSrcDS->GetLayerCount();
    int nOk = 0, nBadLayer = 0, nBadFeature = 0;
    QStringList failedLayers;

    for (int iLayer = 0; iLayer < nLayers; ++iLayer) {
        OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
        if (!poSrcLayer) { ++nBadLayer; continue; }
        const QByteArray layerName(poSrcLayer->GetName());
        if (layerName.isEmpty()) { ++nBadLayer; continue; }

        const QString outBase = dstDir + QLatin1Char('/') + QString::fromUtf8(layerName);
        // 必须带 .shp 后缀：Shapefile 驱动的 Create() 见到不带后缀的路径会当成
        // “目录型数据集”，结果是每个图层建出一个同名文件夹而不是一个 .shp
        GDALDataset* poOutDS = poShpDrv->Create(
            (outBase + QStringLiteral(".shp")).toUtf8().constData(),
            0, 0, 0, GDT_Unknown, nullptr);
        if (!poOutDS) {
            ++nBadLayer;
            failedLayers << QString::fromUtf8(layerName);
            continue;
        }

        OGRSpatialReference* poSRS = poSrcLayer->GetSpatialRef();
        char** papszLco = nullptr;
        papszLco = CSLSetNameValue(papszLco, "ENCODING", "UTF-8");
        // 几何类型原样带过去（不拍平 Z）：SHP 本身支持 Z，拍平会悄悄丢高程，
        // 而下游综合任务对 3D 面（保护区/国家公园）是照原样跑的
        OGRLayer* poOutLayer = poOutDS->CreateLayer(
            layerName.constData(), poSRS, poSrcLayer->GetGeomType(), papszLco);
        CSLDestroy(papszLco);
        if (!poOutLayer) {
            GDALClose(poOutDS);
            ++nBadLayer;
            failedLayers << QString::fromUtf8(layerName);
            continue;
        }

        // 字段：按 UTF-8 边界截到 10 字节并自己保证唯一，避免 GDAL 硬截断切坏汉字
        const OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
        QSet<QByteArray> usedFieldNames;
        for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f) {
            OGRFieldDefn oField(poSrcDefn->GetFieldDefn(f));
            QByteArray name(oField.GetNameRef() ? oField.GetNameRef() : "");
            if (name.isEmpty()) name = QByteArray("field_") + QByteArray::number(f);
            const QByteArray base = truncateUtf8Bytes(name, 10);
            QByteArray cand = base;
            int n = 1;
            while (usedFieldNames.contains(cand.toLower())) {
                const QByteArray suffix = QByteArray("_") + QByteArray::number(n++);
                cand = truncateUtf8Bytes(base, 10 - suffix.size()) + suffix;
            }
            usedFieldNames.insert(cand.toLower());
            oField.SetName(cand.constData());
            poOutLayer->CreateField(&oField);
        }

        poSrcLayer->ResetReading();
        OGRFeature* poFeature = nullptr;
        while ((poFeature = poSrcLayer->GetNextFeature()) != nullptr) {
            OGRFeature* poOut = OGRFeature::CreateFeature(poOutLayer->GetLayerDefn());
            poOut->SetFrom(poFeature, TRUE);
            if (poOutLayer->CreateFeature(poOut) != OGRERR_NONE) ++nBadFeature;
            OGRFeature::DestroyFeature(poOut);
            OGRFeature::DestroyFeature(poFeature);
        }
        GDALClose(poOutDS);

        if (poSRS) {
            OGRSpatialReference oTmp(*poSRS);
            oTmp.morphToESRI();
            char* pszWkt = nullptr;
            if (oTmp.exportToWkt(&pszWkt) == OGRERR_NONE) {
                QFile prj(outBase + QStringLiteral(".prj"));
                if (prj.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    prj.write(pszWkt);
                    prj.close();
                }
                CPLFree(pszWkt);
            }
        }
        ++nOk;
    }
    GDALClose(poSrcDS);

    msg = QStringLiteral("共 %1 个图层，成功 %2，失败 %3，失败要素 %4%5")
              .arg(nLayers).arg(nOk).arg(nBadLayer).arg(nBadFeature)
              .arg(failedLayers.isEmpty()
                       ? QString()
                       : QStringLiteral("（%1）").arg(failedLayers.join(QStringLiteral("、"))));
    return nOk > 0;
}

// 按知识库导出步骤的 <LayerNameList>，把成果层从结果目录平铺拷到 dstDir 并改名。
// GDB 模式下这步由引擎做，LayerNameList 写的是 "区县级成果_长城" 这种扁平层名；
// 换成 SHP 工作目录后，源文件其实是 结果目录\区县级成果\长城.shp，所以这里按
// "文件存在即命中"逐条解析：先按原名找，再按"前缀_后缀 → 前缀\后缀"找，
// 不写死任何目录名。返回成功拷出的层数，未命中的层名回填 missing。
static int exportResultLayers(const QString& outDir, const QString& dstDir,
                              const QVector<QPair<QString, QString> >& nameMap,
                              QStringList& missing)
{
    missing.clear();
    const QStringList sidecars = QStringList()
        << QStringLiteral("shp") << QStringLiteral("shx") << QStringLiteral("dbf")
        << QStringLiteral("prj") << QStringLiteral("cpg");
    if (!QDir().mkpath(dstDir)) return 0;

    int nCopied = 0;
    for (int i = 0; i < nameMap.size(); ++i) {
        const QString srcName = nameMap[i].first;
        const QString newName = nameMap[i].second;

        QString srcBase;
        if (QFile::exists(outDir + QLatin1Char('/') + srcName + QStringLiteral(".shp"))) {
            srcBase = outDir + QLatin1Char('/') + srcName;
        }
        else {
            const int us = srcName.indexOf(QLatin1Char('_'));
            if (us > 0) {
                const QString nested = outDir + QLatin1Char('/') + srcName.left(us)
                                     + QLatin1Char('/') + srcName.mid(us + 1);
                if (QFile::exists(nested + QStringLiteral(".shp")))
                    srcBase = nested;
            }
        }
        if (srcBase.isEmpty()) { missing << srcName; continue; }

        for (int s = 0; s < sidecars.size(); ++s) {
            const QString src = srcBase + QLatin1Char('.') + sidecars[s];
            if (!QFile::exists(src)) continue;
            const QString dst = dstDir + QLatin1Char('/') + newName
                              + QLatin1Char('.') + sidecars[s];
            if (QFile::exists(dst)) QFile::remove(dst);
            QFile::copy(src, dst);
        }
        ++nCopied;
    }
    return nCopied;
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

#ifndef _WIN32
    // 【2026-11-03】Linux: 每次交给 DoXMLFile 的 XML 先做一次 <FilePath> 反斜杠
    // 转正斜杠（见 makeBackslashFixedXmlCopy 注释）。收集临时副本，结束时删除。
    QStringList tmpXmlFiles;
#endif

    if (m_links.isEmpty()) {
        // 直接 <MapGeneBatchProcessing> 格式: 只处理一个 XML
        emit progressChanged(1, 1, QString::fromUtf8("执行中..."));

        QString execXmlPath = m_xmlDir;  // m_xmlDir 即完整路径
#ifndef _WIN32
        const QString tmpXml = makeBackslashFixedXmlCopy(execXmlPath);
        if (!tmpXml.isEmpty()) {
            execXmlPath = tmpXml;
            tmpXmlFiles << tmpXml;
            emit logMessage(QStringLiteral("已生成临时副本（<FilePath> 反斜杠→正斜杠）"), 0);
        }
#endif
        QByteArray xmlBytes = execXmlPath.toLocal8Bit();
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

            QString execXmlPath = subXmlPath;
#ifndef _WIN32
            const QString tmpXml = makeBackslashFixedXmlCopy(execXmlPath);
            if (!tmpXml.isEmpty()) {
                execXmlPath = tmpXml;
                tmpXmlFiles << tmpXml;
                emit logMessage(
                    QStringLiteral("%1: 已生成临时副本（<FilePath> 反斜杠→正斜杠）")
                        .arg(link.path), 0);
            }
#endif
            QByteArray xmlBytes = execXmlPath.toLocal8Bit();
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
#ifndef _WIN32
    // 删除本次生成的临时副本（知识库原文件始终未被修改）
    for (const QString& tmpXml : tmpXmlFiles) {
        if (QFile::exists(tmpXml))
            QFile::remove(tmpXml);
    }
    tmpXmlFiles.clear();
#endif
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

    // 【2026-10-23】路线B：整链跑完后按导出步骤的 <LayerNameList> 把成果层平铺改名，
    // 补上 GDB 模式下由引擎完成的那一步（SHP 工作目录下引擎做不了）
    if (!m_gdbExportXml.isEmpty()) {
        const QString exportXml = m_gdbExportXml;
        m_gdbExportXml.clear();
        runGdbExportStep(exportXml);
    }

    // 【2026-11-07】结果目录（含 区县级成果 / 成果数据 等子目录）里的 dbf 补 .cpg。
    // 必须放在 runGdbExportStep 之后：那一步会把成果再拷一份到 成果数据，
    // 先补的话新拷过去的那份又没 .cpg 了。
    {
        const int nCpg = writeDbfCpgFiles(outputDirectory());
        if (nCpg > 0) {
            const QString s = QStringLiteral("已为 %1 个 DBF 补写 .cpg 编码声明").arg(nCpg);
            QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
            genDebugLog(s);
        }
    }

    // 【2026-09-09】文案改用 m_taskLabel（综合 / gdb→shp 转换共用此回调），
    // 默认任务标签"综合缩编"，弹窗文案与改造前一致
    const QString title = m_taskLabel.isEmpty() ? QString::fromUtf8("综合缩编") : m_taskLabel;
    // 【2026-10-23】路线B 若做了成果导出，顺带告诉用户落在哪儿
    QString exportHint;
    if (!m_exportSummary.isEmpty()) {
        exportHint = QString::fromUtf8("\n\n成果数据已按导出步骤平铺到：\n%1")
                         .arg(m_exportSummary);
        m_exportSummary.clear();
    }

    if (success == total && total > 0) {
        QMessageBox::information(this, title,
            QString::fromUtf8("%1执行完成！\n\n 全部成功。%2").arg(title, exportHint));
    }
    else if (success > 0) {
        QMessageBox::warning(this, title,
            QString::fromUtf8("%1部分完成。\n\n成功: %2 / 失败: %3%4")
                .arg(title).arg(success).arg(total - success).arg(exportHint));
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

// ============================================================================
// 【2026-10-23】路线B：成果导出
//   知识库里那条 InputDataSource=EsriGDB 的步骤（甘肃库的 "GDB制图成果后处理"）做的事
//   其实就是"把 区县级成果_xxx 平铺并改名"，而它只在 GDB 模式下有意义。换成 SHP 工作
//   目录后源数据变成了 结果目录\区县级成果\xxx.shp，引擎那条链接打不开，所以链里剔掉，
//   改由这里用文件拷贝还原。导出失败不影响综合结果本身——成果层仍在
//   结果目录\区县级成果\ 下，只是没平铺改名。
// ============================================================================
void GeneralizationConfigDialog::runGdbExportStep(const QString& xmlPath)
{
    const QVector<QPair<QString, QString> > nameMap = readLayerNameList(xmlPath);
    if (nameMap.isEmpty()) {
        const QString s = QStringLiteral("成果导出步骤里没有 <LayerNameList>，跳过导出: %1")
                              .arg(xmlPath);
        QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Warning);
        genDebugLog(s);
        return;
    }

    const QString outDir = outputDirectory();
    const QString dstDir = QDir(outDir).absoluteFilePath(QStringLiteral("成果数据"));
    QStringList missing;
    const int nCopied = exportResultLayers(outDir, dstDir, nameMap, missing);

    const QString s = QStringLiteral("成果导出: %1/%2 层 → %3%4")
                          .arg(nCopied).arg(nameMap.size()).arg(dstDir)
                          .arg(missing.isEmpty()
                                   ? QString()
                                   : QStringLiteral("；未找到: %1")
                                         .arg(missing.join(QStringLiteral("、"))));
    QgsMessageLog::logMessage(s, LOG_TAG, nCopied > 0 ? Qgis::Info : Qgis::Warning);
    genDebugLog(s);
    if (nCopied > 0)
        m_exportSummary = QStringLiteral("%1（%2 个图层）").arg(dstDir).arg(nCopied);
}

// ============================================================================
// 【2026-11-07】综合结果补 .cpg
//   现象：结果目录里的 dbf 内容写的是 UTF-8，但文件头第 29 字节 LDID 固定是
//         0x57（ANSI 936 / CP936），而且没有 .cpg。GDAL/QGIS 打开时没有 .cpg 可参考，
//         直接按 Latin-1 解释 UTF-8 字节，属性表中文就全成 "æ»©" 这种乱码。
//   实测（ogrinfo）：无 cpg → 乱码；补一个内容为 UTF-8 的 cpg → 正常。
//   做法：递归遍历结果目录，给每个没有 .cpg 的 dbf 按它实际的字节写一个。
//         编码靠"把第一条记录里的字符字段读出来试解码"判断，不写死，
//         万一某个中间层真是 GBK 也能写对。
//   返回：新写了多少个 .cpg。
// ============================================================================
int GeneralizationConfigDialog::writeDbfCpgFiles(const QString& rootDir)
{
    if (rootDir.isEmpty() || !QDir(rootDir).exists()) return 0;

    QTextCodec* codecUtf8 = QTextCodec::codecForName("UTF-8");
    QTextCodec* codecGbk  = QTextCodec::codecForName("GBK");

    int written = 0;
    QDirIterator it(rootDir, QStringList() << QStringLiteral("*.dbf"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString dbfPath = it.next();
        // 同名 .cpg 已存在就尊重原有声明，不覆盖
        const QString cpgPath = QFileInfo(dbfPath).absolutePath() + QLatin1Char('/')
                              + QFileInfo(dbfPath).completeBaseName() + QStringLiteral(".cpg");
        if (QFile::exists(cpgPath)) continue;

        QFile df(dbfPath);
        if (!df.open(QIODevice::ReadOnly)) continue;

        QByteArray hdr = df.read(32);
        if (hdr.size() < 32) { df.close(); continue; }

        const quint32 nrec  = quint32(quint8(hdr[4])) | (quint32(quint8(hdr[5])) << 8)
                            | (quint32(quint8(hdr[6])) << 16) | (quint32(quint8(hdr[7])) << 24);
        const quint16 hlen  = quint16(quint8(hdr[8]))  | (quint16(quint8(hdr[9]))  << 8);
        const quint16 rlen  = quint16(quint8(hdr[10])) | (quint16(quint8(hdr[11])) << 8);
        if (hlen < 33 || rlen < 2 || nrec == 0) { df.close(); continue; }

        // 解析字段：只记字符型（C）的偏移与长度
        const int nField = (hlen - 33) / 32;
        QByteArray fdefs = df.read(nField * 32);
        QVector<QPair<int, int> > cFields;   // (offset, len)
        {
            int off = 1;   // 记录首字节是删除标志
            for (int i = 0; i < nField && i * 32 + 32 <= fdefs.size(); ++i)
            {
                const char type = fdefs.at(i * 32 + 11);
                const int  len  = quint8(fdefs.at(i * 32 + 16));
                if (type == 'C') cFields.append(qMakePair(off, len));
                off += len;
            }
        }
        if (cFields.isEmpty()) { df.close(); continue; }

        // 跳到第一条记录，取字符字段里的非 ASCII 字节
        if (!df.seek(hlen)) { df.close(); continue; }
        const QByteArray rec = df.read(rlen);
        df.close();
        if (rec.size() < rlen || rec.at(0) == '*') continue;

        QByteArray sample;
        sample.reserve(64);
        for (int i = 0; i < cFields.size(); ++i)
        {
            const int off = cFields.at(i).first, len = cFields.at(i).second;
            if (off + len > rec.size()) continue;
            QByteArray cell = rec.mid(off, len);
            while (!cell.isEmpty() && (cell.endsWith(' ') || cell.endsWith('\0')))
                cell.chop(1);
            for (int k = 0; k < cell.size(); ++k)
                if (quint8(cell.at(k)) >= 0x80) { sample += cell.mid(k); break; }
            if (sample.size() >= 48) break;
        }

        QString enc;
        if (sample.isEmpty())
            enc = QStringLiteral("UTF-8");   // 没有中文，写哪个都一样
        else if (codecUtf8 && codecUtf8->fromUnicode(codecUtf8->toUnicode(sample)) == sample)
            enc = QStringLiteral("UTF-8");
        else if (codecGbk && codecGbk->fromUnicode(codecGbk->toUnicode(sample)) == sample)
            enc = QStringLiteral("CP936");
        else
            continue;    // 两种都解不了，宁可不写也别写错

        QFile cf(cpgPath);
        if (!cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        cf.write(enc.toLatin1());
        cf.close();
        ++written;
    }
    return written;
}

// ============================================================================
// 【2026-10-10】注记工程文件（.2dm）复制
//   甲方把 .2dm 地图工程打包成一个文件夹给（样例里一次要两个：
//   制图注记工程去路径.2dm、挂图点要素注记GDB去路径.2dm），知识库里的
//   <Mission id="405"> 只写文件名，引擎按 DoXMLFile 的 relativePath
//   （本插件传的是"结果目录"）去解析，所以执行前必须把这些文件放到结果目录下。
//   目录留空 = 该知识库没有注记步骤 → 不拷、不报错，保持原有行为。
// ============================================================================
bool GeneralizationConfigDialog::copyLabelProjectFiles(const QString& outDir)
{
    const QString srcDir = labelProjectDir();

    // 未填注记工程目录：该知识库没有 405 注记步骤，走原有流程
    if (srcDir.isEmpty())
        return true;

    QDir dir(srcDir);
    if (!dir.exists()) {
        QMessageBox::warning(this, QString::fromUtf8("注记工程文件"),
            QString::fromUtf8("注记工程目录不存在：\n%1").arg(srcDir));
        return false;
    }

    const QStringList files = dir.entryList(QStringList() << QStringLiteral("*.2dm"),
                                            QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        const int btn = QMessageBox::warning(this, QString::fromUtf8("注记工程文件"),
            QString::fromUtf8("注记工程目录里没有找到 .2dm 文件：\n%1\n\n"
                              "继续执行的话，知识库里的注记任务（id=405 动态注记转静态注记）\n"
                              "会报「找不到 .2dm 文件」。\n\n是否仍然继续？").arg(srcDir),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return btn == QMessageBox::Yes;
    }

    if (!QDir().mkpath(outDir)) {
        QMessageBox::warning(this, QString::fromUtf8("注记工程文件"),
            QString::fromUtf8("无法创建结果目录：\n%1").arg(outDir));
        return false;
    }

    // 若用户把"注记工程目录"直接选成了结果目录，源和目标是同一个文件，跳过以免自删
    const QString srcCanon = QDir(srcDir).absolutePath();
    const QString dstCanon = QDir(outDir).absolutePath();

    QStringList copied;
    for (const QString& name : files) {
        const QString src = dir.absoluteFilePath(name);
        const QString dst = QDir(outDir).absoluteFilePath(name);   // 保留原文件名

        if (srcCanon == dstCanon) { copied << name; continue; }

        // 结果目录常被复用：以本次所选的那份为准（先删后拷，QFile::copy 目标已存在会失败）
        if (QFile::exists(dst) && !QFile::remove(dst)) {
            QMessageBox::warning(this, QString::fromUtf8("注记工程文件"),
                QString::fromUtf8("结果目录里已存在同名文件且无法覆盖：\n%1\n\n"
                                  "请关闭占用它的程序后重试。").arg(dst));
            return false;
        }
        if (!QFile::copy(src, dst)) {
            QMessageBox::warning(this, QString::fromUtf8("注记工程文件"),
                QString::fromUtf8("注记工程文件复制失败：\n%1\n→ %2\n\n"
                                  "请检查文件是否被占用、磁盘是否有空间。").arg(src, dst));
            return false;
        }
        copied << name;
    }

    const QString s = QStringLiteral("注记工程文件已复制到结果目录：%1")
                          .arg(copied.join(QStringLiteral("、")));
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

    // 【2026-10-23】清掉上一次运行遗留的路线B状态（上次中途 return 时可能没清）
    m_gdbExportXml.clear();
    m_exportSummary.clear();

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

    // ====== 1a. 【2026-11-07】制图主区名称：改县预处理 XML 里的 SQL，不切 Link ======
    // 甲方的口径：知识库只维护一份县模板（如 会宁县.xml），主 XML 的 Link 固定不动，
    // 程序按识别出的主区名称去改该 XML 里「属性查询:县级行政区划面」的 SQL 条件。
    const QString mainRegion = m_lineEditMainRegion
                                   ? m_lineEditMainRegion->text().trimmed()
                                   : QString();
    if (mainRegion.isEmpty()) {
        // 主区名称为空不阻断综合：不改任何文件，按知识库原样执行。
        genDebugLog(QStringLiteral("制图主区名称为空，跳过县 XML 的 SQL 改写"));
    } else {
        const int n = applyMainRegionToXml(xmlPath, mainRegion);
        if (n > 0) {
            const QString s =
                QStringLiteral("制图主区名称=%1，已改写县预处理 XML 里"
                               "「属性查询:县级行政区划面」的 SQL")
                    .arg(mainRegion);
            QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
            genDebugLog(s);
        } else {
            genDebugLog(QStringLiteral(
                "制图主区名称=%1，未找到可改写的县预处理 XML SQL（Link 或 Mission 11 不匹配）")
                .arg(mainRegion));
        }
        // 记住本次填写，下次打开对话框时恢复
        QSettings st(QStringLiteral("GarMap"), QStringLiteral("MapGeneralization"));
        st.setValue(QStringLiteral("mainRegion"), mainRegion);
    }

    // ====== 1b. 【2026-10-10】注记工程文件（.2dm）拷进结果目录 ======
    // 必须在起 worker 之前：知识库里 <Mission id="405"> 用 <ProjectFile>文件名.2dm</ProjectFile>
    // 引用工程，引擎按 relativePath（= outDir）解析，文件不在那儿就报"找不到 .2dm 文件"。
    // 【2026-10-13】不再有"复制到结果目录"复选框：目录非空就一律拷
    //（copyLabelProjectFiles 内部：目录留空直接返回 true，不拷不报错）
    if (!copyLabelProjectFiles(outDir))
        return;   // 已弹错 / 用户选择中止

    genDebugLog(QStringLiteral("=== 综合缩编开始 数据目录=%1 XML=%2 输出=%3")
        .arg(shpDir, xmlPath, outDir));

    // ====== 1c. 【2026-09-10】输入类型判定：SHP 目录 / FileGDB 目录 ======
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

    // ====== 3b. 【2026-10-23】GDB 输入：先分清知识库里那条 GDB 输入步骤是
    //   "源数据转换"还是"成果导出"，再决定走原有两阶段还是路线 B ======
    if (gdbInput) {
        const QString kbDir = QFileInfo(xmlPath).absolutePath();
        const int gdbLinkIdx = isLinkContainer ? findGdb2ShpLinkIndex(kbDir, links) : -1;

        // 判据：把这条 Link 的 <LayerNameList> 拿去和源 GDB 的图层名对一遍。
        // 一个都对不上 → 它是整链跑完后对成果做的导出/后处理（甘肃库的
        // "GDB制图成果后处理"就是这样，LayerNameList 写的是 区县级成果_xxx）；
        // 有对得上的 → 它确实是把源数据 GDB 转成 SHP 的转换步骤（天津库那种）。
        bool isExportStep = false;
        if (gdbLinkIdx >= 0) {
            QString subXml = links[gdbLinkIdx].path;
            subXml.replace(QLatin1Char('\\'), QLatin1Char('/'));
            const QString subXmlPath = QDir::cleanPath(kbDir + "/" + subXml);
            const QVector<QPair<QString, QString> > nameMap = readLayerNameList(subXmlPath);
            if (!nameMap.isEmpty()) {
                QStringList gdbLayers;
                if (listGdbLayers(shpDir, gdbLayers)) {
                    int nHit = 0;
                    for (int i = 0; i < nameMap.size(); ++i)
                        if (gdbLayers.contains(nameMap[i].first)) ++nHit;
                    isExportStep = (nHit == 0);
                }
            }
            const QString s = QStringLiteral("GDB 输入步骤判定: %1 | LayerName 共 %2 条 | %3")
                                  .arg(subXmlPath).arg(nameMap.size())
                                  .arg(isExportStep
                                           ? QStringLiteral("成果导出（源库里无对应图层）")
                                           : QStringLiteral("源数据转换"));
            QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
            genDebugLog(s);
        }

        // ---- 情形1：知识库自带 GDB→SHP 转换步骤 → 沿用原有两阶段流程 ----
        if (gdbLinkIdx >= 0 && !isExportStep) {
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

        // ---- 情形2（路线B）：知识库里没有转换步骤，插件自己铺 SHP 工作副本 ----
        // 如果找到的那条是成果导出，把它从链里摘出来，整链跑完后由插件执行导出。
        if (gdbLinkIdx >= 0) {
            QString subXml = links[gdbLinkIdx].path;
            subXml.replace(QLatin1Char('\\'), QLatin1Char('/'));
            m_gdbExportXml = QDir::cleanPath(kbDir + "/" + subXml);
            links.removeAt(gdbLinkIdx);
            const QString s = QStringLiteral(
                "成果导出步骤不在链里跑（SHP 工作目录下它打不开），整链结束后由插件执行: %1")
                    .arg(m_gdbExportXml);
            QgsMessageLog::logMessage(s, LOG_TAG, Qgis::Info);
            genDebugLog(s);
        }

        {
            QProgressDialog convProgress(
                QString::fromUtf8("正在把 GDB 转成 SHP 工作副本..."), QString(), 0, 0, this);
            convProgress.setWindowModality(Qt::WindowModal);
            convProgress.setCancelButton(nullptr);
            convProgress.show();
            QApplication::processEvents();

            QString convMsg;
            const bool convOk = convertGdbDirToShp(shpDir, outDir, convMsg);
            convProgress.close();

            const QString s = QStringLiteral("GDB→SHP 工作副本: %1").arg(convMsg);
            QgsMessageLog::logMessage(s, LOG_TAG, convOk ? Qgis::Info : Qgis::Critical);
            genDebugLog(s);
            if (!convOk) {
                m_gdbExportXml.clear();
                QMessageBox::warning(this, QString::fromUtf8("综合缩编"),
                    QString::fromUtf8("把 GDB 转成 SHP 工作副本失败，综合已中止。\n\n"
                                      "%1\n\nGDB: %2\n结果目录: %3")
                        .arg(convMsg, shpDir, outDir));
                return;
            }
        }

        // 转换完成后与 SHP 输入完全一致：数据铺在结果目录根，逐个 Link 跑
        startWorkerRun(xmlDir, dataDir, links, testDllDir, QString::fromUtf8("综合缩编"));
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
