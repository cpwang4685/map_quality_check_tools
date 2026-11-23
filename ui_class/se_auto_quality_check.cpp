/*--------------QT---------------*/
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QTextStream>
#include <QProgressBar>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QCheckBox>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QFrame>
#include <QScreen>
#include <QGuiApplication>
#include <QTabWidget>
#include <QMap>
#include <QSet>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <memory>

// ---- Mission 检查模块（需在QGIS头文件之前，避免json.hpp冲突） ----
#include "se_db_manager.h"
#include "se_mission453_check.h"
#include "se_mission454_check.h"
#include "se_mission455_check.h"
#include "se_mission456_check.h"
#include "se_mission457_check.h"
#include "se_mission458_check.h"
#include "se_mission459_check.h"
#include "../ui_task/wuji_mission_runner.h"
#include "../ui_task/wuji_engine_bridge.h"

/*--------------QGIS---------------*/
#include "qgssettings.h"
#include "qgsgui.h"
#include "qgsapplication.h"

// ---- QGIS 矢量图层（内部质检用） ----
#include "qgsvectorlayer.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgsfield.h"
#include "qgsfields.h"
#include "qgsvectorfilewriter.h"
#include "qgscoordinatereferencesystem.h"
#include "qgswkbtypes.h"
#include "qgspointxy.h"
#include "qgslinestring.h"
#include "qgspolygon.h"
#include "qgsproject.h"
#include "qgslayertree.h"
#include "qgslayertreegroup.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

#include "se_auto_quality_check.h"
#include "se_check_params_dialog.h"

#include "ui_fit_helper.h"

// ================================================================
//  构造函数
// ================================================================
CSE_AutoQualityCheckDialog::CSE_AutoQualityCheckDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);
    QgsGui::enableAutoGeometryRestore(this);
    DialogFitHelper::install(this);
    this->setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);

    // ---- 浏览按钮 ----
    connect(ui.btn_BrowseOrigData,   &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onBrowseOrigData);
    connect(ui.btn_BrowseResultData, &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onBrowseResultData);
    connect(ui.btn_BrowseMissionXml, &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onBrowseMissionXml);
    connect(ui.btn_BrowseOutputDir,  &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onBrowseOutputDir);

    // ---- 隐藏字段映射UI（已移除，改用自动探测） ----
    ui.label_MappingConfig->setVisible(false);
    ui.lineEdit_MappingConfig->setVisible(false);
    ui.btn_BrowseMapping->setVisible(false);
    ui.btn_ConfigMapping->setVisible(false);

    // ---- 图层映射表（新增行，添加到数据源GroupBox中） ----
    {
        QGridLayout* gLayout = qobject_cast<QGridLayout*>(ui.groupBox_DataSource->layout());
        if (gLayout) {
            int nextRow = gLayout->rowCount();
            // 标签
            QLabel* lblLayerMap = new QLabel("图层映射表：", ui.groupBox_DataSource);
            gLayout->addWidget(lblLayerMap, nextRow, 0);
            // 路径输入框
            QLineEdit* leLayerMap = new QLineEdit(ui.groupBox_DataSource);
            leLayerMap->setObjectName("lineEdit_LayerMapping");
            leLayerMap->setPlaceholderText("图层映射CSV（自动加载）");
            gLayout->addWidget(leLayerMap, nextRow, 1);
            // 按钮
            QHBoxLayout* hLayout = new QHBoxLayout();
            QPushButton* btnBrowseLayer = new QPushButton("浏览", ui.groupBox_DataSource);
            btnBrowseLayer->setMaximumWidth(60);
            QPushButton* btnConfigLayer = new QPushButton("配置", ui.groupBox_DataSource);
            btnConfigLayer->setMaximumWidth(60);
            btnConfigLayer->setStyleSheet("QPushButton { color: #1890ff; font-weight: bold; }");
            hLayout->addWidget(btnBrowseLayer);
            hLayout->addWidget(btnConfigLayer);
            gLayout->addLayout(hLayout, nextRow, 2);

            connect(btnBrowseLayer, &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onBrowseLayerMappingConfig);
            connect(btnConfigLayer, &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onConfigureLayerMapping);
        }
    }

    // ---- 分阶段数据（2026-11-08 甲方口径）：⑤真实移位值 ----
    // 两个目录分别指向「化简后移位前」和「最终成果（区县级成果）」；
    // 勾选⑤道路过度偏移时必须同时填写，⑤只按这两套数据计算真实移位值。
    {
        QGridLayout* gLayout = qobject_cast<QGridLayout*>(ui.groupBox_DataSource->layout());
        if (gLayout) {
            int row = gLayout->rowCount();

            QLabel* lblBefore = new QLabel("移位前数据路径（化简后）：", ui.groupBox_DataSource);
            gLayout->addWidget(lblBefore, row, 0);
            QLineEdit* leBefore = new QLineEdit(ui.groupBox_DataSource);
            leBefore->setObjectName("lineEdit_ShiftBeforePath");
            leBefore->setPlaceholderText("选择「化简后移位前」数据目录（SHP）");
            gLayout->addWidget(leBefore, row, 1);
            QPushButton* btnBefore = new QPushButton("浏览...", ui.groupBox_DataSource);
            gLayout->addWidget(btnBefore, row, 2);
            connect(btnBefore, &QPushButton::clicked,
                    this, &CSE_AutoQualityCheckDialog::onBrowseShiftBefore);
            ++row;

            QLabel* lblAfter = new QLabel("移位后数据路径（最终成果）：", ui.groupBox_DataSource);
            gLayout->addWidget(lblAfter, row, 0);
            QLineEdit* leAfter = new QLineEdit(ui.groupBox_DataSource);
            leAfter->setObjectName("lineEdit_ShiftAfterPath");
            leAfter->setPlaceholderText("选择「最终成果」数据目录（SHP，通常与成果数据路径相同）");
            gLayout->addWidget(leAfter, row, 1);
            QPushButton* btnAfter = new QPushButton("浏览...", ui.groupBox_DataSource);
            gLayout->addWidget(btnAfter, row, 2);
            connect(btnAfter, &QPushButton::clicked,
                    this, &CSE_AutoQualityCheckDialog::onBrowseShiftAfter);
            ++row;

            QLabel* hint = new QLabel(
                "用于 M460「⑤道路过度偏移」：按图层对应表逐顶点计算真实移位值；"
                "勾选⑤时必须同时填写这两个路径，否则不执行⑤检查。", ui.groupBox_DataSource);
            hint->setWordWrap(true);
            hint->setStyleSheet("QLabel { color: #888; }");
            gLayout->addWidget(hint, row, 0, 1, 3);
        }
    }

    // ---- 参数入口调整：取消独立参数子窗口，参数直接放进各检查组页签 ----
    // 主界面「阈值参数」区只保留全局两项（比例尺、结点容差）；
    // 尖锐角/碎面/狭长面/小面积/节点最小距离已移入「图形规范性检查」页签。
    ui.groupBox_Params->setTitle("全局参数");
    ui.label_AcuteAngle->setVisible(false);
    ui.doubleSpinBox_AcuteAngle->setVisible(false);
    ui.label_SliverArea->setVisible(false);
    ui.doubleSpinBox_SliverArea->setVisible(false);
    ui.label_NarrowWidth->setVisible(false);
    ui.doubleSpinBox_NarrowWidth->setVisible(false);
    ui.label_MinArea->setVisible(false);
    ui.doubleSpinBox_MinArea->setVisible(false);
    ui.label_MinNodeDist->setVisible(false);
    ui.doubleSpinBox_MinNodeDist->setVisible(false);

    // ---- 开始质检 ----
    connect(ui.btn_StartChk, &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onStartCheck);

    // ---- 关闭 ----
    connect(ui.btn_Close, &QPushButton::clicked, this, &CSE_AutoQualityCheckDialog::onClose);

    // ---- 恢复上次参数 ----
    restoreState();

    // ---- 自动加载图层映射CSV ----
    loadLayerMappingCsv(m_qstrLayerMappingPath);

    // ---- 加载 mission_config.xml 并构建UI ----
    if (!loadMissionConfig(m_qstrMissionXmlPath)) {
        ui.label_Status->setText(QString("状态：未找到 mission_config.xml，使用内置默认配置"));
        loadDefaultMissionConfig();  // 兜底：用代码内置默认值
    }
    buildMissionUI();

    // ---- 动态窗口标题（与集成版一致：综合成果自动化质检: X组Mission Y项检查）----
    {
        int totalItems = 0;
        for (const auto& group : m_missionGroups) {
            totalItems += group.items.size();
        }
        setWindowTitle(QString("综合成果自动化质检: %1组Mission %2项检查")
                       .arg(m_missionGroups.size())
                       .arg(totalItems));
    }
}

CSE_AutoQualityCheckDialog::~CSE_AutoQualityCheckDialog()
{
    QgsSettings s;
    s.setValue("AutoQualityCheck/OrigDataPath",   m_qstrOrigDataPath,   QgsSettings::Plugins);
    s.setValue("AutoQualityCheck/ResultDataPath", m_qstrResultDataPath, QgsSettings::Plugins);
    s.setValue("AutoQualityCheck/MissionXml",     m_qstrMissionXmlPath, QgsSettings::Plugins);
    s.setValue("AutoQualityCheck/OutputDir",      m_qstrOutputDir,      QgsSettings::Plugins);
    s.setValue("AutoQualityCheck/LayerMappingPath",  m_qstrLayerMappingPath, QgsSettings::Plugins);
    s.setValue("AutoQualityCheck/ShiftBeforePath",   m_qstrShiftBeforePath, QgsSettings::Plugins);
    s.setValue("AutoQualityCheck/ShiftAfterPath",    m_qstrShiftAfterPath,  QgsSettings::Plugins);
}

// ================================================================
//  首次显示：把整窗尺寸定到"屏幕放得下、且给检查项区留足高度"
//  1) 收敛：窗口/下限不得超出当前屏幕可用区域，避免低分辨率/高DPI下越出屏幕；
//  2) 定高：不采用"窗口正好等于内容最小高度"（那样检查项区只能拿到自己的最小值、
//     非常挤，tab 页底部的按钮与分组框下边框会被压没），而是取期望高度(860)与
//     屏幕可用高度的较小值；多出来的高度由 QVBoxLayout 按 groupBox_Missions 的
//     verstretch=4 全部给「检查项」区。
//  检查项超高部分另有 buildMissionUI 的每页 QScrollArea 滚动兜底。
// ================================================================
void CSE_AutoQualityCheckDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    if (m_uiFitOnce)
        return;
    m_uiFitOnce = true;

    // 取本窗口所在屏幕（窗口中心落入的屏幕）的可用区域。
    // 不用 QWidget::screen()：麒麟系统 Qt 5.12.12 未提供该成员，此写法跨平台可用。
    QScreen* scr = nullptr;
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.size() > 1) {
        const QPoint c = geometry().center();
        for (QScreen* s : screens) {
            if (s->availableGeometry().contains(c)) {
                scr = s;
                break;
            }
        }
    }
    if (!scr)
        scr = QGuiApplication::primaryScreen();

    if (scr) {
        const QRect avail = scr->availableGeometry();
        // 四周留 24px，避免整窗贴住屏幕边缘/任务栏
        const int capW = qMax(320, avail.width()  - 24);
        const int capH = qMax(240, avail.height() - 24);

        // 1) 下限不得超出屏幕可用范围，否则整窗会被顶出屏幕
        if (minimumWidth()  > capW) setMinimumWidth(capW);
        if (minimumHeight() > capH) setMinimumHeight(capH);

        // 2) 定尺寸：宽不低于 920；高取"期望高度"与"屏幕可用高度"的较小值。
        //    已经比期望值大的（用户自己调过 / 上次还原的）不缩回去。
        const int kPreferredHeight = 860;
        int w = qMax(width(), 920);
        if (w > capW) w = capW;
        int h = qMin(kPreferredHeight, capH);
        if (height() > h) h = qMin(height(), capH);
        if (h < minimumHeight()) h = minimumHeight();
        resize(w, h);
    }
}

// ================================================================
//  恢复 / 保存状态
// ================================================================
void CSE_AutoQualityCheckDialog::restoreState()
{
    const QgsSettings s;
    m_qstrOrigDataPath   = s.value("AutoQualityCheck/OrigDataPath",   "", QgsSettings::Plugins).toString();
    m_qstrResultDataPath = s.value("AutoQualityCheck/ResultDataPath", "", QgsSettings::Plugins).toString();
    m_qstrMissionXmlPath = s.value("AutoQualityCheck/MissionXml",     "", QgsSettings::Plugins).toString();
    m_qstrOutputDir      = s.value("AutoQualityCheck/OutputDir",      "", QgsSettings::Plugins).toString();
    m_qstrLayerMappingPath = s.value("AutoQualityCheck/LayerMappingPath",  "", QgsSettings::Plugins).toString();
    m_qstrShiftBeforePath  = s.value("AutoQualityCheck/ShiftBeforePath",   "", QgsSettings::Plugins).toString();
    m_qstrShiftAfterPath   = s.value("AutoQualityCheck/ShiftAfterPath",    "", QgsSettings::Plugins).toString();

    if (m_qstrMissionXmlPath.isEmpty())
        m_qstrMissionXmlPath = getDefaultMissionXmlPath();

    ui.lineEdit_OrigDataPath->setText(m_qstrOrigDataPath);
    ui.lineEdit_ResultDataPath->setText(m_qstrResultDataPath);
    ui.lineEdit_MissionXml->setText(m_qstrMissionXmlPath);
    ui.lineEdit_OutputDir->setText(m_qstrOutputDir);

    // 恢复图层映射路径
    {
        QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_LayerMapping");
        if (le) le->setText(m_qstrLayerMappingPath);
    }
    // 恢复分阶段数据路径（⑤真实移位值）
    if (QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_ShiftBeforePath"))
        le->setText(m_qstrShiftBeforePath);
    if (QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_ShiftAfterPath"))
        le->setText(m_qstrShiftAfterPath);
}

bool CSE_AutoQualityCheckDialog::CheckFileOrDirExist(const QString& path)
{
    return QFileInfo::exists(path);
}

QString CSE_AutoQualityCheckDialog::getDefaultMissionXmlPath()
{
    // 按优先级尝试多个路径
    QStringList candidates;

    // 1. 插件DLL同目录下的 config/mission_config.xml
    QString dllDir = QCoreApplication::applicationDirPath();
    candidates.append(dllDir + "/config/mission_config.xml");

    // 2. 插件DLL上一层目录的 config/
    QDir dllParent(dllDir);
    dllParent.cdUp();
    candidates.append(dllParent.absolutePath() + "/config/mission_config.xml");

    // 4. 用户桌面知识库目录
    candidates.append(QDir::homePath() + "/Desktop/知识库/Release_x64_lic_20260718/mission_config.xml");

    for (const auto& p : candidates) {
        if (QFileInfo::exists(p)) return p;
    }
    return "";
}

// ================================================================
//  查找引擎 exe
// ================================================================
static QString findExe()
{
    QString p = QCoreApplication::applicationDirPath() + "/MapBatchProcessing.exe";
    if (QFileInfo::exists(p)) return p;
    // 注意：无需硬编码路径，引擎在 exe 同级目录查找即可
#ifdef Q_OS_WIN
    char buf[MAX_PATH];
    HMODULE hm = GetModuleHandleA("plugin_auto_quality_check.dll");
    if (hm) {
        GetModuleFileNameA(hm, buf, MAX_PATH);
        QFileInfo fi(QString::fromLocal8Bit(buf));
        p = fi.absolutePath() + "/MapBatchProcessing.exe";
        if (QFileInfo::exists(p)) return p;
    }
#endif
    return "";
}

// ================================================================
//  扫描 SHP 文件
// ================================================================
QStringList CSE_AutoQualityCheckDialog::scanShpFiles(const QString& dirPath)
{
    QDir dir(dirPath);
    return dir.entryList({"*.shp"}, QDir::Files, QDir::Name);
}

// ---- 从错误消息文本中提取图层名称 ----
// 消息格式约定：
//   [图层名] ...                        （M458 匹配类）
//   检查名(图层名)：详情                 （M453/454/455/456 拓扑类）
//   图形规范性检查(图层名/模式名)：详情   （M459）
static QString extractLayerNameFromMsg(const QString& msg)
{
    const int lb = msg.indexOf('[');
    const int lp = msg.indexOf('(');
    if (lb >= 0 && (lp < 0 || lb < lp)) {
        const int rb = msg.indexOf(']', lb + 1);
        if (rb > lb + 1) return msg.mid(lb + 1, rb - lb - 1);
        return QString();
    }
    if (lp >= 0) {
        const int rp = msg.indexOf(')', lp + 1);
        if (rp > lp + 1) {
            const int slash = msg.indexOf('/', lp + 1);
            const int end = (slash > lp && slash < rp) ? slash : rp;
            if (end > lp + 1) return msg.mid(lp + 1, end - lp - 1);
        }
    }
    return QString();
}

// 解析错误消息里的“（定位点 x,y）”——M455 自相交、M459 尖锐角/自相交/
// 节点最小距离会把准确位置写进消息；兼容中英文括号、半/全角逗号。
static bool parseErrorLocation(const QString& msg, QgsPointXY& pt)
{
    const int k = msg.indexOf(QStringLiteral("定位点"));
    if (k < 0) return false;
    auto readNumber = [&msg](int& i, double& v) -> bool {
        while (i < msg.size() && !msg.at(i).isDigit() && msg.at(i) != '-'
               && msg.at(i) != '+' && msg.at(i) != '.') ++i;
        const int start = i;
        while (i < msg.size()) {
            const QChar c = msg.at(i);
            if (!(c.isDigit() || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'))
                break;
            ++i;
        }
        if (i <= start) return false;
        bool ok = false;
        v = msg.mid(start, i - start).toDouble(&ok);
        return ok;
    };
    int i = k + static_cast<int>(QStringLiteral("定位点").size());
    double x = 0.0, y = 0.0;
    if (!readNumber(i, x) || !readNumber(i, y)) return false;
    pt = QgsPointXY(x, y);
    return true;
}

// ---- DBF 字符串字段（254 字节）安全截断 ----
// Shapefile 的 C 字段上限 254 **字节**，UTF-8 中文一个字 3 字节。直接超长写入
// 会被 OGR 从字节中间截断，末尾出现半个汉字（读出来是“�”）。写入前按字符
// 边界截到 251 字节以内并加省略号，保证不出现乱码。
static QString fitDbfUtf8(const QString& s)
{
    const QByteArray b = s.toUtf8();
    if (b.size() <= 254) return s;
    int cut = 251;
    while (cut > 0 && (static_cast<unsigned char>(b.at(cut)) & 0xC0) == 0x80)
        --cut;
    return QString::fromUtf8(b.left(cut)) + QStringLiteral("…");
}

// ---- 将错误列表写入 SHP 文件 ----
// 生成两个文件：
//   {outputDir}/{name}.shp    — 错误要素副本 + "error"字段 + "图层名"字段
//   {outputDir}/{name}_pt.shp — 错误位置点标记（同样带图层名）
// layerName 非空时整批写入该值；为空时逐条从错误消息中提取。
// （DBF 字段名限 10 字节，"图层名称"12 字节会被截断，故用"图层名"）
static QStringList writeErrorsToShp(const QString& outputDir, const QString& name,
    const QList<QPair<QgsFeature, QString>>& errors,
    const QgsCoordinateReferenceSystem& crs,
    const QgsFields& layerFields,
    const QString& layerName = QString())
{
    QStringList outFiles;
    if (errors.isEmpty()) return outFiles;

    // ---- 用图层字段 + 追加 error / 图层名 字段 ----
    QgsFields featFields(layerFields);
    int errIdx = featFields.indexOf("error");
    if (errIdx < 0) {
        featFields.append(QgsField("error", QVariant::String, "String", 254));
        errIdx = featFields.size() - 1;
    }
    int layIdx = featFields.indexOf("图层名");
    if (layIdx < 0) {
        featFields.append(QgsField("图层名", QVariant::String, "String", 254));
        layIdx = featFields.size() - 1;
    }
    // ===== 1. 错误要素图层 =====
    // SHP 单文件只能存一种几何类型。M459 会把面/线图层的错误汇总进同一列表，
    // 原来按第一条错误的类型建文件，其他类型会被静默丢弃。这里按大类分桶：
    // 第一类仍写原文件名（兼容既有输出），其余类型追加 _点/_线/_面 后缀。
    {
        const QString suffix[3] = {QStringLiteral("_点"), QStringLiteral("_线"),
                                   QStringLiteral("_面")};
        QList<QPair<QgsFeature, QString>> buckets[3];
        for (const auto& pair : errors) {
            const QgsWkbTypes::GeometryType t = pair.first.geometry().type();
            if (t == QgsWkbTypes::PointGeometry) buckets[0].append(pair);
            else if (t == QgsWkbTypes::LineGeometry) buckets[1].append(pair);
            else if (t == QgsWkbTypes::PolygonGeometry) buckets[2].append(pair);
        }

        // 主文件类型跟“第一条错误”走（保持原文件名与原有行为）；点错误不额外
        // 建文件（_pt 文件已覆盖所有点）。线/面混合时给其余类型补 _线/_面。
        int mainBucket = -1;
        const QgsWkbTypes::GeometryType firstType =
            errors.first().first.geometry().type();
        if (firstType == QgsWkbTypes::PointGeometry) mainBucket = 0;
        else if (firstType == QgsWkbTypes::LineGeometry) mainBucket = 1;
        else if (firstType == QgsWkbTypes::PolygonGeometry) mainBucket = 2;

        for (int k = 0; k < 3; ++k) {
            if (buckets[k].isEmpty()) continue;
            if (k == 0 && mainBucket != 0) continue;   // 点错误只在 _pt 中输出
            if (k != mainBucket && k != 1 && k != 2) continue;
            const QString featName = (k == mainBucket) ? name : name + suffix[k];
            QString featShp = outputDir + "/" + featName + ".shp";
            QgsVectorFileWriter::SaveVectorOptions opts;
            opts.driverName = "ESRI Shapefile";
            opts.fileEncoding = "UTF-8";

            std::unique_ptr<QgsVectorFileWriter> featWriter(
                QgsVectorFileWriter::create(featShp, featFields,
                    buckets[k].first().first.geometry().wkbType(), crs,
                    QgsCoordinateTransformContext(), opts));
            if (!featWriter || featWriter->hasError() != QgsVectorFileWriter::NoError) {
                qWarning() << "[ShpWrite] 创建失败:" << featShp
                    << (featWriter ? featWriter->errorMessage() : "null");
                continue;
            }
            for (const auto& pair : buckets[k]) {
                QgsFeature outFeat(featFields);
                outFeat.setGeometry(pair.first.geometry());
                // 按字段名拷贝属性，避免索引错位
                for (int f = 0; f < layerFields.size(); f++)
                    outFeat.setAttribute(f, pair.first.attribute(layerFields.at(f).name()));
                outFeat.setAttribute(errIdx, pair.second);
                outFeat.setAttribute(layIdx, layerName.isEmpty()
                    ? extractLayerNameFromMsg(pair.second) : layerName);
                featWriter->addFeature(outFeat);
            }
            featWriter.reset();
            outFiles.append(featName + ".shp");
        }
        // 空几何错误无法写入要素文件，但仍会出现在下面的 _pt 文件里
    }

    // ===== 2. 错误位置点图层 =====
    {
        QgsFields ptFields;
        ptFields.append(QgsField("feature_id", QVariant::LongLong));
        ptFields.append(QgsField("error", QVariant::String, "String", 254));
        ptFields.append(QgsField("图层名", QVariant::String, "String", 254));

        QString ptShp = outputDir + "/" + name + "_pt.shp";
        QgsVectorFileWriter::SaveVectorOptions opts;
        opts.driverName = "ESRI Shapefile";
        opts.fileEncoding = "UTF-8";

        std::unique_ptr<QgsVectorFileWriter> ptWriter(
            QgsVectorFileWriter::create(ptShp, ptFields, QgsWkbTypes::Point, crs,
                QgsCoordinateTransformContext(), opts));
        if (!ptWriter || ptWriter->hasError() != QgsVectorFileWriter::NoError) {
            qWarning() << "[ShpWrite] 创建失败:" << ptShp
                << (ptWriter ? ptWriter->errorMessage() : "null");
        } else {
            for (const auto& pair : errors) {
                // 优先用消息里的准确“定位点”；没有时才退回几何质心
                QgsGeometry ptGeom;
                QgsPointXY errLoc;
                if (parseErrorLocation(pair.second, errLoc))
                    ptGeom = QgsGeometry::fromPointXY(errLoc);
                if (ptGeom.isNull())
                    ptGeom = pair.first.geometry().centroid();
                if (ptGeom.isNull())
                    ptGeom = QgsGeometry::fromPointXY(QgsPointXY(0, 0));
                QgsFeature ptFeat(ptFields);
                ptFeat.setGeometry(ptGeom);
                ptFeat.setAttribute(0, (qlonglong)pair.first.id());
                ptFeat.setAttribute(1, fitDbfUtf8(pair.second));
                ptFeat.setAttribute(2, layerName.isEmpty()
                    ? extractLayerNameFromMsg(pair.second) : layerName);
                ptWriter->addFeature(ptFeat);
            }
            ptWriter.reset();
            outFiles.append(name + "_pt.shp");
        }
    }

    return outFiles;
}

// ---- 将综合前后拓扑检查错误写入 SHP（按几何类型分文件） ----
// 三个文件（仅在有对应类型错误时生成，不加数字前缀，供程序置顶加载）：
//   综合前后拓扑检查_点.shp / 综合前后拓扑检查_线.shp / 综合前后拓扑检查_面.shp
// 字段：检查项 / 说明 / 前后值（DBF 字段名限10字节，三者均满足）
static QStringList writeFocusErrorsToShp(const QString& outputDir,
    const QList<Mission458::FocusError>& errors,
    const QgsCoordinateReferenceSystem& crs)
{
    QStringList outFiles;
    if (errors.isEmpty()) return outFiles;

    const QgsWkbTypes::GeometryType types[3] = {QgsWkbTypes::PointGeometry,
        QgsWkbTypes::LineGeometry, QgsWkbTypes::PolygonGeometry};
    const QString names[3] = {QStringLiteral("综合前后拓扑检查_点"), QStringLiteral("综合前后拓扑检查_线"),
        QStringLiteral("综合前后拓扑检查_面")};

    for (int t = 0; t < 3; t++) {
        QList<const Mission458::FocusError*> batch;
        for (const auto& e : errors) {
            if (!e.geometry.isNull() && e.geometry.type() == types[t])
                batch.append(&e);
        }
        if (batch.isEmpty()) continue;

        QgsFields fields;
        fields.append(QgsField("检查项", QVariant::String, "String", 254));
        fields.append(QgsField("说明", QVariant::String, "String", 254));
        fields.append(QgsField("前后值", QVariant::String, "String", 254));

        const QString shpPath = outputDir + "/" + names[t] + ".shp";
        QgsVectorFileWriter::SaveVectorOptions opts;
        opts.driverName = "ESRI Shapefile";
        opts.fileEncoding = "UTF-8";
        std::unique_ptr<QgsVectorFileWriter> writer(
            QgsVectorFileWriter::create(shpPath, fields,
                batch.first()->geometry.wkbType(), crs,
                QgsCoordinateTransformContext(), opts));
        if (!writer || writer->hasError() != QgsVectorFileWriter::NoError) {
            qWarning() << "[FocusShpWrite] 创建失败:" << shpPath
                << (writer ? writer->errorMessage() : "null");
            continue;
        }
        for (const auto* e : batch) {
            QgsFeature f(fields);
            f.setGeometry(e->geometry);
            f.setAttribute(0, e->checkItem);
            f.setAttribute(1, fitDbfUtf8(e->desc));
            f.setAttribute(2, fitDbfUtf8(e->values));
            writer->addFeature(f);
        }
        writer.reset();
        outFiles.append(names[t] + ".shp");
    }
    return outFiles;
}

// ---- 将 M460 定位点写入 综合前后拓扑检查_定位点.shp ----
// ②③断口 / ⑤偏移中心 / ④⑥最大偏离处 各写一个点（甲方双击定位问题位置）；
// 字段与三个主文件一致：检查项 / 说明 / 前后值。
static QStringList writeFocusLocationsToShp(const QString& outputDir,
    const QList<Mission458::FocusError>& errors,
    const QgsCoordinateReferenceSystem& crs)
{
    QStringList outFiles;
    QList<const Mission458::FocusError*> batch;
    for (const auto& e : errors) {
        if (e.hasLoc) batch.append(&e);
    }
    if (batch.isEmpty()) return outFiles;

    QgsFields fields;
    fields.append(QgsField("检查项", QVariant::String, "String", 254));
    fields.append(QgsField("说明", QVariant::String, "String", 254));
    fields.append(QgsField("前后值", QVariant::String, "String", 254));

    const QString shpPath = outputDir + "/" + QStringLiteral("综合前后拓扑检查_定位点.shp");
    QgsVectorFileWriter::SaveVectorOptions opts;
    opts.driverName = "ESRI Shapefile";
    opts.fileEncoding = "UTF-8";
    std::unique_ptr<QgsVectorFileWriter> writer(
        QgsVectorFileWriter::create(shpPath, fields,
            QgsWkbTypes::Point, crs, QgsCoordinateTransformContext(), opts));
    if (!writer || writer->hasError() != QgsVectorFileWriter::NoError) {
        qWarning() << "[FocusShpWrite] 创建失败:" << shpPath
            << (writer ? writer->errorMessage() : "null");
        return outFiles;
    }
    for (const auto* e : batch) {
        QgsFeature f(fields);
        f.setGeometry(QgsGeometry::fromPointXY(e->locPoint));
        f.setAttribute(0, e->checkItem);
        f.setAttribute(1, fitDbfUtf8(e->desc));
        f.setAttribute(2, fitDbfUtf8(e->values));
        writer->addFeature(f);
    }
    writer.reset();
    outFiles.append(QStringLiteral("综合前后拓扑检查_定位点.shp"));
    return outFiles;
}

// ---- 将 M460 选取比例统计写入 CSV（UTF-8 BOM，Excel 直接打开不乱码） ----
// 字段：成果图层 / 原始图层 / 原始要素数 / 成果要素数 / 选取比例(%)
// 返回文件名（失败或空统计返回空串）。
static QString writeSelectionStatsCsv(const QString& outputDir,
    const QList<Mission458::SelectionStat>& stats)
{
    if (stats.isEmpty()) return QString();
    const QString path = outputDir + "/" + QStringLiteral("选取比例统计.csv");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[SelectionStats] 无法写入:" << path;
        return QString();
    }
    file.write("\xEF\xBB\xBF");
    QTextStream ts(&file);
    ts.setCodec("UTF-8");
    ts << QStringLiteral("成果图层,原始图层,原始要素数,成果要素数,选取比例(%)\n");
    ts << QStringLiteral("# 选取比例=成果要素数/原始要素数；同一原始图层被多个成果图层引用时，按该源图层要素总数计算\n");
    for (const auto& s : stats) {
        const QString ratio = s.origCount > 0
            ? QString::number(100.0 * double(s.resultCount) / double(s.origCount), 'f', 2)
            : QStringLiteral("—");
        ts << s.resultLayer << "," << s.origLayer << ","
           << s.origCount << "," << s.resultCount << "," << ratio << "\n";
    }
    // 按源图层汇总：多个成果图层映射到同一源图层（如国道/省道/县道→LRDL）时，
    // 单层比例之和=该源图层总选取比例
    ts << "\n";
    ts << QStringLiteral("# 按源图层汇总（成果合计/源图层要素数）\n");
    ts << QStringLiteral("源图层,原始要素数,成果合计,总选取比例(%)\n");
    QMap<QString, qint64> origCountByLayer, resultSumByLayer;
    for (const auto& s : stats) {
        origCountByLayer[s.origLayer] = s.origCount;
        resultSumByLayer[s.origLayer] += s.resultCount;
    }
    for (auto it = origCountByLayer.constBegin(); it != origCountByLayer.constEnd(); ++it) {
        const qint64 oc = it.value();
        const qint64 rc = resultSumByLayer.value(it.key());
        const QString ratio = oc > 0
            ? QString::number(100.0 * double(rc) / double(oc), 'f', 2)
            : QStringLiteral("—");
        ts << it.key() << "," << oc << "," << rc << "," << ratio << "\n";
    }
    file.close();
    return QStringLiteral("选取比例统计.csv");
}

// ---- 将 M460 ⑤真实移位值逐要素明细写入 CSV（UTF-8 BOM） ----
// 字段：成果图层/FID/匹配依据/移位前参考图层/最大移位/平均移位/顶点数/阈值/是否超阈值/位置
// 返回文件名（失败或空结果返回空串）。
static QString writeTrueShiftCsv(const QString& outputDir,
    const QHash<QString, Mission458::TrueShiftInfo>& shifts, double threshold)
{
    if (shifts.isEmpty()) return QString();
    const QString path = outputDir + "/" + QStringLiteral("真实移位值统计.csv");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[TrueShift] 无法写入:" << path;
        return QString();
    }
    file.write("\xEF\xBB\xBF");
    QTextStream ts(&file);
    ts.setCodec("UTF-8");
    ts << QStringLiteral("成果图层,成果FID,匹配依据,移位前参考图层,最大移位(米),"
                         "平均移位(米),顶点数,同层兜底顶点数,阈值(米),是否超阈值,"
                         "最大移位位置X,最大移位位置Y\n");
    ts << QStringLiteral("# 真实移位值=最终成果线逐顶点到“化简后移位前”同一条路线的最近距离的最大值；"
                         "平均值为全部顶点距离的平均；同层兜底顶点数=标识只覆盖部分线段、"
                         "改用1公里内同层移位前线兜底的顶点数；阈值取 matchParameter.xml MaxDisplacement\n");
    QStringList keys = shifts.keys();
    keys.sort();
    for (const QString& k : keys) {
        const Mission458::TrueShiftInfo& s = shifts[k];
        ts << s.layer << "," << s.fid << ","
           << s.matchKey << "," << s.refLayers << ","
           << QString::number(s.maxShift, 'f', 2) << ","
           << QString::number(s.meanShift, 'f', 2) << ","
           << s.vertexCount << "," << s.fallbackVertices << ","
           << QString::number(threshold, 'f', 2) << ","
           << (s.maxShift > threshold ? QStringLiteral("是") : QStringLiteral("否")) << ","
           << (s.hasPoint ? QString::number(s.point.x(), 'f', 3) : QString()) << ","
           << (s.hasPoint ? QString::number(s.point.y(), 'f', 3) : QString()) << "\n";
    }
    file.close();
    return QStringLiteral("真实移位值统计.csv");
}

// ---- 将输出 SHP 置顶加载到当前工程 ----
// 用于"综合前后拓扑检查_点/线/面.shp 程序置顶加载"要求：入工程后插到图层树最顶层；
// 多次调用时后调用者更靠上（按 面→线→点 顺序调用则点在最上）
static void loadOutputLayerOnTop(const QString& shpPath)
{
    if (!QFileInfo::exists(shpPath)) return;
    auto* lyr = new QgsVectorLayer(shpPath, QFileInfo(shpPath).completeBaseName(), "ogr");
    if (!lyr->isValid()) { delete lyr; return; }
    QgsProject* prj = QgsProject::instance();
    prj->addMapLayer(lyr, false);                 // 入工程但不自动挂树
    prj->layerTreeRoot()->insertLayer(0, lyr);    // 插到图层树最顶层
}

// ================================================================
//  图层映射：获取默认CSV路径
// ================================================================
QString CSE_AutoQualityCheckDialog::getDefaultLayerMappingCsvPath() const
{
    QStringList candidates;
    // 1. 知识库同目录下的图层映射CSV
    QString configDir = QFileInfo(m_qstrMissionXmlPath).absolutePath();
    candidates.append(configDir + "/layer_mapping.csv");

    // 2. 桌面综合前后图层对应关系
    candidates.append(QDir::homePath() + "/Desktop/20260724反馈材料/20260724反馈材料/综合前后图层对应关系/layer_mapping.csv");

    // 3. 插件DLL目录
    candidates.append(QCoreApplication::applicationDirPath() + "/config/layer_mapping.csv");

    for (const auto& p : candidates) {
        if (QFileInfo::exists(p)) return p;
    }
    return candidates.first(); // 返回首选路径（可能不存在，供新建）
}

// ================================================================
//  图层映射：内置默认标准图层列表（93个成果图层）
//  数据来源：综合成果与元数据名称对应表.xlsx
// ================================================================
QList<LayerMappingItem> CSE_AutoQualityCheckDialog::getDefaultStandardLayers() const
{
    QList<LayerMappingItem> items;

    // 格式：{标准图层名, 源图层代码, 源图层描述, 备注}
    struct { const char* stdName; const char* srcCode; const char* srcDesc; const char* note; } defs[] = {
        // ---- 境界 ----
        {"省界", "BOUL", "行政境界", ""},
        {"未定省界", "BOUL", "行政境界", ""},
        {"市界", "BOUL", "行政境界", ""},
        {"县界", "BOUL", "行政境界", ""},
        {"邻区普染色", "BOUL", "行政境界", ""},
        {"省级行政区划面普色", "BOUA2", "省级行政区划面", ""},
        {"市级行政区划面普色", "BOUA4", "市级行政区划面", ""},
        {"县级行政区划面普色", "BOUA5", "县级行政区划面", ""},
        {"晕带", "BOUL", "行政境界", ""},

        // ---- 驻地 ----
        {"省级驻地", "BOUP6", "行政政府驻地", ""},
        {"市级驻地", "BOUP6", "行政政府驻地", ""},
        {"县级驻地", "BOUP6", "行政政府驻地", ""},
        {"乡镇级驻地", "BOUP6", "行政政府驻地", ""},

        // ---- 表面注记 ----
        {"省级表面注记", "BOUA2", "省级行政区划面", ""},
        {"省级表面注记_飞地及小面", "BOUA2", "省级行政区划面", ""},
        {"市级表面注记", "BOUA4", "市级行政区划面", ""},
        {"市级表面注记_飞地及小面", "BOUA4", "市级行政区划面", ""},
        {"县级表面注记", "BOUA5", "县级行政区划面", ""},
        {"县级表面注记_飞地及小面", "BOUA5", "县级行政区划面", ""},
        {"乡镇级表面注记", "BOUA5", "县级行政区划面", ""},
        {"乡镇级表面注记_飞地及小面", "BOUA5", "县级行政区划面", ""},

        // ---- 居民地 ----
        {"行政村", "BOUP7", "行政村", ""},
        {"0行政村_选取", "BOUP7", "行政村", ""},
        {"自然村", "BOUP8", "自然村", ""},
        {"街区面", "RESA", "居民地", ""},

        // ---- 道路 ----
        {"高速公路", "LRDL", "公路", ""},
        {"在建高速公路", "LRDL", "公路", ""},
        {"国道", "LRDL", "公路", ""},
        {"省道", "LRDL", "公路", ""},
        {"县道", "LRDL", "公路", ""},
        {"乡道（专用道）", "LRDL", "公路", ""},
        {"村道", "LRDL", "公路", ""},
        {"高速代码", "LRDL", "公路", ""},
        {"国道代码", "LRDL", "公路", ""},
        {"省道代码", "LRDL", "公路", ""},
        {"县道代码", "LRDL", "公路", ""},
        {"高速_名称", "LRDL", "公路", ""},
        {"高速出入口", "LRDP", "高速出入口", ""},
        {"汽车隧道口", "LRDL", "公路", ""},

        // ---- 铁路 ----
        {"高速铁路", "LRRL", "铁路", ""},
        {"普通铁路", "LRRL", "铁路", ""},
        {"在建普通铁路", "LRRL", "铁路", ""},
        {"铁路休止符", "LRRL", "铁路", ""},
        {"高铁车站", "LRRP", "车站", ""},
        {"普通车站", "LRRP", "车站", ""},
        {"火车隧道口", "LRRL", "铁路", ""},

        // ---- 河流 ----
        {"一级河流", "HYDL_HL", "河流", ""},
        {"二级河流", "HYDL_HL", "河流", ""},
        {"三级河流", "HYDL_HL", "河流", ""},
        {"四级河流", "HYDL_HL", "河流", ""},
        {"五级河流", "HYDL_HL", "河流", ""},
        {"六级河流", "HYDL_HL", "河流", ""},
        {"等外河流", "HYDL_HL", "河流", ""},
        {"一级河流_名称", "HYDL_HL", "河流", ""},
        {"二级河流_名称", "HYDL_HL", "河流", ""},
        {"三级河流_名称", "HYDL_HL", "河流", ""},
        {"四级河流_名称", "HYDL_HL", "河流", ""},
        {"五级河流_名称", "HYDL_HL", "河流", ""},
        {"六级河流_名称", "HYDL_HL", "河流", ""},
        {"等外河流_名称", "HYDL_HL", "河流", ""},

        // ---- 水渠 ----
        {"总干渠", "HYDL_Qu", "水渠", ""},
        {"干渠", "HYDL_Qu", "水渠", ""},
        {"支渠", "HYDL_Qu", "水渠", ""},
        {"总干渠_名称", "HYDL_Qu", "水渠", ""},
        {"干渠_名称", "HYDL_Qu", "水渠", ""},
        {"支渠_名称", "HYDL_Qu", "水渠", ""},
        {"水渠_方向点", "HYDL_Qu", "水渠", ""},

        // ---- 湖泊水库 ----
        {"湖泊水库", "HYDA_HPSK", "湖泊水库", ""},
        {"湖泊水库_名称", "HYDA_HPSK", "湖泊水库", ""},
        {"湖泊水库_点", "HYDA_HPSK", "湖泊水库", ""},

        // ---- 其他水系 ----
        {"干涸河", "HYDA_GSJ", "干涸河", ""},

        // ---- 山脉 ----
        {"二级山脉_名称", "MRL", "山脉", ""},
        {"三级山脉_名称", "MRL", "山脉", ""},

        // ---- 地形 ----
        {"山峰", "MPP", "山峰", ""},
        {"泉", "HYDP", "点泉", ""},
        {"地沟名", "MRL", "山脉", ""},
        {"地理名称", "MRL", "山脉", ""},

        // ---- 景点/公园 ----
        {"A级景区", "A_LvYouJingQu", "旅游景点", ""},
        {"0A级景区_选取", "A_LvYouJingQu", "旅游景点", ""},
        {"公园", "BERP6", "风景名胜", ""},
        {"农林牧", "BERP3", "农林牧", ""},
        {"文物古迹", "WenWuGuJi", "文物古迹", ""},
        {"国家公园界", "GuoJiaGongYuan", "国家公园", ""},
        {"国家公园注记", "GuoJiaGongYuan", "国家公园", ""},

        // ---- 其他 ----
        {"飞机场", "AIRP", "飞机场", ""},
        {"长城", "LVLL", "长城", ""},
        {"隧道", "LRRL", "铁路", ""},
    };

    for (const auto& d : defs) {
        LayerMappingItem item;
        item.stdName    = QString::fromUtf8(d.stdName);
        item.sourceCode = QString::fromUtf8(d.srcCode);
        item.sourceDesc = QString::fromUtf8(d.srcDesc);
        item.note       = QString::fromUtf8(d.note);
        items.append(item);
    }
    return items;
}

// ================================================================
//  从CSV加载图层映射
// ================================================================
void CSE_AutoQualityCheckDialog::loadLayerMappingCsv(const QString& csvPath)
{
    m_layerMappingItems.clear();
    QString path = csvPath;
    // 路径为空、或记录下来的路径已失效（文件被移走）时，回退到默认候选路径，
    // 避免"记住的路径打不开 → 映射静默丢失"
    if (path.isEmpty() || !QFileInfo::exists(path))
        path = getDefaultLayerMappingCsvPath();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // CSV不存在时使用内置默认（仅标准图层名，无实际SHP映射）
        m_layerMappingItems = getDefaultStandardLayers();
        return;
    }

    QTextStream ts(&file);
    ts.setCodec("UTF-8");

    // 逐行解析：不依赖表头。收到的工作表表头写的是“源图层名称,源图层代码”，
    // 但数据行实际是“源图层代码,源图层名称”（代码在前）；本工程旧配置又是名称在前。
    // 统一由 LayerMappingCsv::parseRow 按每行前两列内容识别，旧4列格式继续兼容。
    bool anyRow = false;
    while (!ts.atEnd()) {
        QString line = ts.readLine();
        if (line.startsWith(QChar(0xFEFF)))   // 兜底：个别环境 BOM 未被子流吃掉
            line.remove(0, 1);
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith("#")) continue;

        const QStringList parts = line.split(",");
        LayerMappingItem item;
        bool oldFormat = false;
        LayerMappingCsv::parseRow(parts, item, oldFormat);
        if (item.stdName.isEmpty()) continue;
        m_layerMappingItems.append(item);
        anyRow = true;
    }
    file.close();

    // 文件只有表头/没有有效数据行时，回退到内置标准图层列表
    if (!anyRow)
        m_layerMappingItems = getDefaultStandardLayers();
}

// ================================================================
//  按几何类型获取已映射的SHP文件列表
// ================================================================
QStringList CSE_AutoQualityCheckDialog::getMappedShpByType(const QString& geomType) const
{
    QStringList shps;
    for (const auto& item : m_layerMappingItems) {
        if (item.actualShp.isEmpty()) continue;
        if (item.geomType == geomType || geomType == "all")
            shps.append(item.actualShp);
    }
    shps.removeDuplicates();
    return shps;
}

// ================================================================
//  浏览图层映射CSV
// ================================================================
void CSE_AutoQualityCheckDialog::onBrowseLayerMappingConfig()
{
    QString f = QFileDialog::getOpenFileName(this, "图层映射表CSV",
        m_qstrLayerMappingPath, "CSV文件 (*.csv);;所有文件 (*.*)");
    if (!f.isEmpty()) {
        m_qstrLayerMappingPath = f;
        QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_LayerMapping");
        if (le) le->setText(f);
        loadLayerMappingCsv(f);
    }
}

// ================================================================
//  打开图层映射配置弹窗（新：可视化图层对照表）
// ================================================================
void CSE_AutoQualityCheckDialog::onConfigureLayerMapping()
{
    // 如果还未加载任何标准图层，尝试加载
    if (m_layerMappingItems.isEmpty()) {
        loadLayerMappingCsv(m_qstrLayerMappingPath);
    }

    // 扫描数据目录中的SHP文件
    QStringList allShpFiles;
    if (!m_qstrResultDataPath.isEmpty())
        allShpFiles.append(scanShpFiles(m_qstrResultDataPath));
    if (!m_qstrOrigDataPath.isEmpty())
        allShpFiles.append(scanShpFiles(m_qstrOrigDataPath));
    allShpFiles.removeDuplicates();

    // 打开图层映射弹窗
    SeLayerMappingDialog dlg(this);
    dlg.setStandardLayers(m_layerMappingItems);
    if (!allShpFiles.isEmpty())
        dlg.setDataShpFiles(allShpFiles);

    if (dlg.exec() == QDialog::Accepted) {
        m_layerMappingItems = dlg.getMappingResult();
        // 自动保存到默认CSV路径
        QString savePath = m_qstrLayerMappingPath;
        if (savePath.isEmpty()) savePath = getDefaultLayerMappingCsvPath();
        if (!savePath.isEmpty()) {
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                // 【2026-10-26】先写 UTF-8 BOM。中文 Windows 的 Excel 双击打开 CSV 时
                // 只认 BOM，没有就按 GBK 读 → 整表中文乱码。BOM 不影响本程序读回：
                // 读取侧用 QTextStream + setCodec("UTF-8")，实测会吃掉 BOM（不进首行）。
                file.write("\xEF\xBB\xBF");
                QTextStream ts(&file);
                ts.setCodec("UTF-8");
                // 表头保持与收到的工作表一致（#注释行，程序不读取）。QTextStream
                // 的 operator<<(const char*) 按 Latin-1 解释，中文表头会二次编码，
                // 所以统一用 QString 输出。
                ts << LayerMappingCsv::headerLine() << "\n";
                // 数据行按收到的列序：代码,名称,标准图层,实际SHP,几何类型,备注
                for (const auto& item : m_layerMappingItems) {
                    ts << LayerMappingCsv::codeFirstFields(item).join(",") << "\n";
                }
                file.close();
                m_qstrLayerMappingPath = savePath;
                QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_LayerMapping");
                if (le) le->setText(savePath);
            }
        }
        ui.label_Status->setText(
            QString("状态：图层映射已更新（%1个图层）").arg(m_layerMappingItems.size()));
    }
}

// ================================================================
//  检查项参数设置：界面数值阈值，写回两个 XML
// ================================================================
void CSE_AutoQualityCheckDialog::onOpenCheckParams()
{
    QString cfgPath = ui.lineEdit_MissionXml->text().trimmed();
    if (cfgPath.isEmpty()) cfgPath = m_qstrMissionXmlPath;
    if (cfgPath.isEmpty() || !QFileInfo::exists(cfgPath)) {
        QMessageBox::warning(this, "检查项参数设置",
                             "请先选择有效的 mission_config.xml 配置文件。");
        return;
    }

    // 主界面已改、尚未开始质检的阈值优先带入子界面；
    // 若用户在输入框里换了另一份配置，则以该文件内容为准，不带入旧值。
    QHash<QString, double> overrides;
    if (!m_qstrMissionXmlPath.isEmpty()
        && QFileInfo(cfgPath).absoluteFilePath()
               == QFileInfo(m_qstrMissionXmlPath).absoluteFilePath()) {
        collectThresholdsFromSpinBoxes();
        overrides = m_thresholds;
    }

    CSE_CheckParamsDialog dlg(cfgPath, overrides, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QHash<QString, double> vals = dlg.values();
    for (auto it = vals.constBegin(); it != vals.constEnd(); ++it)
        m_thresholds[it.key()] = it.value();

    // 同步回主界面现有控件（比例尺、结点容差、M459 五个阈值）
    if (vals.contains("Scale"))
        ui.spinBox_Scale->setValue(static_cast<int>(vals.value("Scale")));
    if (vals.contains("FuzzyTolerance"))
        ui.doubleSpinBox_Tolerance->setValue(vals.value("FuzzyTolerance"));
    applyThresholdsToSpinBoxes();

    m_qstrMissionXmlPath = cfgPath;
    ui.lineEdit_MissionXml->setText(cfgPath);
    ui.label_Status->setText("状态：检查项参数已保存并生效");
}

bool CSE_AutoQualityCheckDialog::loadMissionConfig(const QString& xmlPath)
{
    QString path = xmlPath;
    if (path.isEmpty()) path = getDefaultMissionXmlPath();
    if (path.isEmpty() || !QFileInfo::exists(path)) return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QDomDocument doc;
    if (!doc.setContent(&file)) { file.close(); return false; }
    file.close();

    QDomElement root = doc.documentElement();
    if (root.tagName() != "MissionConfig") return false;

    // 解析全局参数
    QDomElement globalParams = root.firstChildElement("GlobalParams");
    if (!globalParams.isNull()) {
        auto readDouble = [&](const QString& tag, double def) -> double {
            QDomElement e = globalParams.firstChildElement(tag);
            return e.isNull() ? def : e.text().toDouble();
        };
        const double scale = readDouble("Scale", 10000);
        const double fuzzy = readDouble("FuzzyTolerance", 0.001);
        ui.spinBox_Scale->setValue(static_cast<int>(scale));
        ui.doubleSpinBox_Tolerance->setValue(fuzzy);
        m_thresholds["Scale"] = scale;
        m_thresholds["FuzzyTolerance"] = fuzzy;
    }

    // 解析阈值默认值
    QDomElement thresholds = root.firstChildElement("Thresholds");
    if (!thresholds.isNull()) {
        auto readThreshold = [&](const QString& tag, double def) -> double {
            QDomElement e = thresholds.firstChildElement(tag);
            return e.isNull() ? def : e.attribute("default", QString::number(def)).toDouble();
        };
        m_thresholds["AcuteAngle"]       = readThreshold("AcuteAngle", 10.0);
        m_thresholds["SliverArea"]       = readThreshold("SliverArea", 1.0);
        m_thresholds["NarrowWidth"]      = readThreshold("NarrowWidth", 0.5);
        m_thresholds["MinArea"]          = readThreshold("MinArea", 1.0);
        m_thresholds["MinNodeDistance"]  = readThreshold("MinNodeDistance", 0.001);
        m_thresholds["ResidentCoverTolerance"] = readThreshold("ResidentCoverTolerance", 10.0);
        m_thresholds["AvgNodeDensityUpper"] = readThreshold("AvgNodeDensityUpper", 0.0);
        m_thresholds["AvgNodeDensityLower"] = readThreshold("AvgNodeDensityLower", 0.0);
        m_thresholds["NodeDensityUpper"] = readThreshold("NodeDensityUpper", 0.0);
        m_thresholds["NodeDensityLower"] = readThreshold("NodeDensityLower", 0.0);
    }
    applyThresholdsToSpinBoxes();

    // 解析 Mission 组
    m_missionGroups.clear();
    QDomNodeList missions = root.elementsByTagName("Mission");
    for (int i = 0; i < missions.count(); i++) {
        QDomElement m = missions.at(i).toElement();
        MissionGroup group;
        group.id           = m.attribute("id").toInt();
        group.name         = m.attribute("name");
        group.category     = m.attribute("category");
        group.note         = m.attribute("note");
        group.implemented  = (m.attribute("status") != "pending");

        QDomNodeList items = m.elementsByTagName("CheckItem");
        for (int j = 0; j < items.count(); j++) {
            QDomElement ci = items.at(j).toElement();
            MissionCheckItem item;
            item.name         = ci.attribute("name");
            item.missionId    = group.id;
            item.mode         = ci.attribute("mode").toInt();
            item.enabled      = (ci.attribute("enabled") == "true");
            item.visible      = (ci.attribute("visible") != "false");
            item.implemented  = (ci.attribute("status") != "pending") && group.implemented;
            item.applyTo      = ci.attribute("applyTo");
            item.thresholdKey = ci.attribute("threshold");
            item.note         = ci.attribute("note");
            group.items.append(item);
        }
        m_missionGroups.append(group);
    }

    // 解析图层映射：XML <LayerMapping> 只补充 CSV 中没有的标准图层，
    // 不覆盖 CSV 已有的 sourceCode —— 以用户当前 layer_mapping.csv 为准
    //（两边对“乡镇级表面注记”等少数图层的代码可能不同，避免XML静默改写）。
    QDomElement layerMapping = root.firstChildElement("LayerMapping");
    if (!layerMapping.isNull()) {
        QDomNodeList maps = layerMapping.elementsByTagName("Map");
        for (int i = 0; i < maps.count(); i++) {
            QDomElement m = maps.at(i).toElement();
            QString stdName = m.attribute("standard").trimmed();
            QString gdbCode = m.attribute("gdb").trimmed();
            if (!stdName.isEmpty() && !gdbCode.isEmpty() && gdbCode != QString::fromUtf8("\xe2\x80\x94")) {
                bool found = false;
                for (const auto& item : m_layerMappingItems) {
                    if (item.stdName == stdName) {
                        found = true;   // CSV已有：保留CSV代码
                        break;
                    }
                }
                if (!found) {
                    LayerMappingItem item;
                    item.stdName = stdName;
                    item.sourceCode = gdbCode;
                    m_layerMappingItems.append(item);
                }
            }
        }
    }

    // 综合前后/线面拓扑参数：从同目录 matchParameter.xml 读入（与 M458 共用）
    loadMatchParams(QFileInfo(xmlPath).absolutePath() + "/matchParameter.xml");

    return true;
}

// ================================================================
//  兜底：XML加载失败时使用内置默认Mission配置
// ================================================================
void CSE_AutoQualityCheckDialog::loadDefaultMissionConfig()
{
    m_missionGroups.clear();

    // 设置默认阈值
    m_thresholds["Scale"]           = 10000;
    m_thresholds["FuzzyTolerance"]  = 0.001;
    m_thresholds["AcuteAngle"]      = 10.0;
    m_thresholds["SliverArea"]      = 1.0;
    m_thresholds["NarrowWidth"]     = 0.5;
    m_thresholds["MinArea"]         = 1.0;
    m_thresholds["MinNodeDistance"] = 0.001;
    m_thresholds["ResidentCoverTolerance"] = 10.0;
    applyThresholdsToSpinBoxes();

    // 按Mission定义结构体数组
    struct { int id; QString name; QString cat; bool impl; QVector<QPair<QString,int>> items; } defs[] = {
        {460, "综合前后拓扑检查", "综合前后", true, {
            {"居民点与道路拓扑",1},{"道路长距离连接",2},{"道路·河流断裂",4},{"道路变形异常",8},
            {"道路过度偏移",16},{"道路拉直异常",32},{"线要素属性一致性",64}
        }},
        {453, "属性检查", "属性", true, {
            {"字段名称检查",1},{"数据类型检查",2},{"字段长度检查",4},{"精度检查",8},
            {"忽略字段检查",16},{"主键检查",32},{"唯一约束检查",128},{"非空约束检查",256},
            {"约束类型检查",512},{"约束集检查",1024}
        }},
        {454, "点拓扑检查", "拓扑", true, {
            {"点必须重合",1},{"点必须分离",2},{"点被线端点覆盖",4},
            {"点必须被线覆盖",8},{"点必须在面内部",16},{"点必须在面边界上",32}
        }},
        {455, "线拓扑检查", "拓扑", true, {
            {"不能有悬挂节点",1},{"不能有伪节点",2},{"线不能自重叠",4},{"线不能自相交",8},
            {"必须是单部件",16},{"线不能相互重叠",32},{"线不能相互相交",64},{"线必须在面内部",4096}
        }},
        {456, "面拓扑检查", "拓扑", true, {
            {"面不能重叠",1},{"面不能有缝隙",2},{"面必须包含点",4},
            {"面边界必须被线覆盖",32},{"面必须大于聚类容差",1024}
        }},
        {457, "关联表检查", "关联", true, {
            {"外键引用检查",1},{"关联记录存在性检查",2},{"关联字段一致性检查",4},
            {"关联表结构检查",8},{"关联数据完整性检查",16}
        }},
        {458, "综合前后匹配", "匹配", true, {
            {"综合前后要素匹配检查",1}
        }},
        {459, "图形规范性检查", "图形", true, {
            {"多部件检查",1},{"空图形检查",2},{"尖锐角检查",4},{"碎面检查",8},
            {"狭长面检查",16},{"小面积检查",32},{"节点平均密度检查",64},{"节点密度检查",128},
            {"线自相交检查",256},{"节点最小距离检查",512}
        }}
    };

    for (const auto& d : defs) {
        MissionGroup group;
        group.id = d.id;
        group.name = d.name;
        group.category = d.cat;
        group.implemented = d.impl;
        for (const auto& it : d.items) {
            MissionCheckItem item;
            item.name = it.first;
            item.missionId = d.id;
            item.mode = it.second;
            // 默认只勾选 M460 综合前后拓扑检查组的 7 项；
            // 其余检查组默认不勾选（需要时在界面上手动勾选）
            item.enabled = (d.id == 460);
            item.implemented = d.impl;
            item.applyTo = "all";
            group.items.append(item);
        }
        m_missionGroups.append(group);
    }
}

// ================================================================
//  构建 Mission UI（竖排 + ▼/▶ 箭头折叠按钮）
// ================================================================
void CSE_AutoQualityCheckDialog::buildMissionUI()
{
    // 清除旧控件
    QLayoutItem* child;
    while ((child = ui.missionsLayout->takeAt(0)) != nullptr) {
        if (child->widget()) delete child->widget();
        delete child;
    }
    m_paramSpins.clear();
    m_paramDecimals.clear();

	// ====== 水平 Tab 控件 ======
	QTabWidget* tabWidget = new QTabWidget(ui.groupBox_Missions);
	tabWidget->setTabPosition(QTabWidget::North);
	// 只留一个"防塌陷"的小下限：检查项区的实际高度不靠写死这个值，
	// 而是由 showEvent 里的整窗定尺寸把富余高度按 verstretch=4 分给本组；
	// 写死太大（如 240）会把整窗最小高度顶到屏幕之外，反而挤掉 tab 底部的按钮。
	tabWidget->setMinimumHeight(150);
	const int cols = 4; // 横向4列排列

	for (auto& group : m_missionGroups) {

	        // ====== Tab 页内容 ======
	        QWidget* page = new QWidget(tabWidget);
	        QVBoxLayout* pageLayout = new QVBoxLayout(page);
	        pageLayout->setContentsMargins(8, 8, 8, 4);

	        // 勾选框网格放入滚动区：可用高度不足时该页检查项纵向滚动，不再被裁切
	        QScrollArea* scrollArea = new QScrollArea(page);
	        scrollArea->setWidgetResizable(true);
	        scrollArea->setFrameShape(QFrame::NoFrame);
	        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

	        QWidget* listHost = new QWidget(scrollArea);
	        QVBoxLayout* listLayout = new QVBoxLayout(listHost);
	        listLayout->setContentsMargins(0, 0, 0, 0);
	        listLayout->setSpacing(0);

	        QGridLayout* gl = new QGridLayout();
	        gl->setSpacing(6);

	        int shown = 0;
	        for (int j = 0; j < group.items.size(); j++) {
	            auto& item = group.items[j];
	            if (!item.visible) continue;   // visible="false"：配置保留，界面不显示
	            QCheckBox* cb = new QCheckBox(listHost);
	            QString cbText = item.name;
	            if (!item.implemented)
	                cbText += " (待开发)";
	            cb->setText(cbText);
	            cb->setChecked(item.implemented && item.enabled); // 默认勾选：已实现且XML中enabled=true
	            cb->setEnabled(item.implemented);
	            if (!item.note.isEmpty())
	                cb->setToolTip(item.note);
	            cb->setStyleSheet("QCheckBox { padding: 2px 6px; }");

	            item.checkbox = cb;
	            gl->addWidget(cb, shown / cols, shown % cols);
	            ++shown;
	        }

	        listLayout->addLayout(gl);

	        // 参数直接放在本组检查项下面，与检查项同页签、按组分类
	        if (QWidget* paramsPanel = buildParamsPanel(group.id, listHost))
	            listLayout->addWidget(paramsPanel);

	        listLayout->addStretch();
	        scrollArea->setWidget(listHost);

	        pageLayout->addWidget(scrollArea, 1); // 检查项占满剩余高度，超高时出现滚动条

	        // 全选 / 取消全选
	        QHBoxLayout* btnLayout = new QHBoxLayout();
	        btnLayout->addStretch();
	        QPushButton* btnAll = new QPushButton("全选", page);
	        QPushButton* btnNone = new QPushButton("取消全选", page);
	        btnAll->setMinimumWidth(70);
	        btnNone->setMinimumWidth(100);
	        btnLayout->addWidget(btnAll);
	        btnLayout->addWidget(btnNone);
	        pageLayout->addLayout(btnLayout);

	        connect(btnAll, &QPushButton::clicked, this, [&group]() {
	            for (auto& item : group.items) {
	                if (item.checkbox && item.checkbox->isEnabled())
	                    item.checkbox->setChecked(true);
	            }
	        });
	        connect(btnNone, &QPushButton::clicked, this, [&group]() {
	            for (auto& item : group.items) {
	                if (item.checkbox)
	                    item.checkbox->setChecked(false);
	            }
	        });

	        // Tab 标题
	        QString tabTitle = group.name;
	        if (!group.implemented)
	            tabTitle += " ⚠";
	        tabWidget->addTab(page, tabTitle);
	        group.groupBox = page;
	    }

	    applyParamsToWidgets();
	    ui.missionsLayout->addWidget(tabWidget);
	}

// ================================================================
//  检查项页签内的参数面板
//  参数不再放独立子窗口：每个检查组页签里，检查项下面跟一个可折叠的
//  「▶ 参数设置」块（默认收起），展开后按 ①②③… 分组；M459/M460 的
//  次要参数再收进「显示高级参数」二级折叠区。
// ================================================================
QWidget* CSE_AutoQualityCheckDialog::buildParamsPanel(int missionId, QWidget* parent)
{
    if (missionId != 455 && missionId != 459 && missionId != 460)
        return nullptr;

    QWidget* panel = new QWidget(parent);
    panel->setObjectName(QString("paramsPanel_%1").arg(missionId));
    QVBoxLayout* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 10, 0, 2);
    lay->setSpacing(4);

    QFrame* line = new QFrame(panel);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    lay->addWidget(line);

    // 整个参数区做成折叠块：默认收起，点标题展开，检查项列表保持清爽
    QPushButton* header = new QPushButton(QStringLiteral("▶ 参数设置"), panel);
    header->setCheckable(true);
    header->setAutoDefault(false);
    header->setCursor(Qt::PointingHandCursor);
    header->setStyleSheet(
        QStringLiteral("QPushButton { text-align:left; border:none; color:#1F4E79; "
                       "font-weight:bold; padding:4px 0; }"));
    lay->addWidget(header);

    QWidget* body = new QWidget(panel);
    body->setVisible(false);
    QVBoxLayout* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(12, 0, 0, 0);
    bodyLayout->setSpacing(4);
    lay->addWidget(body);

    QObject::connect(header, &QPushButton::toggled, body, [header, body](bool on) {
        body->setVisible(on);
        header->setText(on ? QStringLiteral("▼ 参数设置")
                           : QStringLiteral("▶ 参数设置"));
    });

    QFormLayout* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    bodyLayout->addLayout(form);

    auto addSection = [&](QFormLayout* f, const QString& text) {
        QLabel* lbl = new QLabel(text, panel);
        QFont fnt = lbl->font();
        fnt.setBold(true);
        lbl->setFont(fnt);
        lbl->setStyleSheet("color:#1F4E79; margin-top:6px;");
        f->addRow(lbl);
    };
    auto addNote = [&](QFormLayout* f, const QString& text) {
        QLabel* lbl = new QLabel(text, panel);
        lbl->setWordWrap(true);
        lbl->setStyleSheet("color:#666;");
        f->addRow(lbl);
    };
    // 高级参数折叠区（与旧参数窗口一致：默认隐藏）
    auto makeAdvanced = [&](QFormLayout** outForm) -> QWidget* {
        QWidget* host = new QWidget(panel);
        QVBoxLayout* hostLay = new QVBoxLayout(host);
        hostLay->setContentsMargins(0, 6, 0, 0);
        hostLay->setSpacing(4);

        QPushButton* toggle = new QPushButton("显示高级参数", host);
        toggle->setCheckable(true);
        toggle->setAutoDefault(false);
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setStyleSheet(
            QStringLiteral("text-align:left; border:none; color:#1F4E79; padding:4px 0;"));
        hostLay->addWidget(toggle);

        QWidget* body = new QWidget(host);
        body->setVisible(false);
        QFormLayout* advForm = new QFormLayout(body);
        advForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        advForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        advForm->setHorizontalSpacing(12);
        advForm->setVerticalSpacing(6);
        body->setLayout(advForm);
        hostLay->addWidget(body);
        QObject::connect(toggle, &QPushButton::toggled, body, [toggle, body](bool on) {
            body->setVisible(on);
            toggle->setText(on ? QStringLiteral("收起高级参数")
                               : QStringLiteral("显示高级参数"));
        });
        *outForm = advForm;
        return host;
    };

    if (missionId == 460) {
        // 2026-10-30：删除重复的“综合前后要素配对”参数组（BufferDistance /
        // MinOverlapRatio 属于“综合前后匹配”M458，与本组检查项无关，不再显示）。
        // 运行时仍由 loadMatchParams() 从 matchParameter.xml 读取，默认 50 米 / 0.2。
        addSection(form, "① 居民点与道路拓扑");
        addParamField(form, "EndpointBuffer", "端点连接容差 (米)：",
                      0.0, 1000.0, 2, 1.0, 50.0,
                      "村道/乡道折线端点距其他道路或居民点≤该值视为已连接/已压盖；"
                      "0=必须严格相接。");
        addParamField(form, "BoundaryTolerance", "边界端点排除距离 (米)：",
                      0.0, 100000.0, 2, 10.0, 50.0,
                      "端点位于数据外框或县界/市界/省界附近≤该值时，视为数据范围截断，"
                      "不报“居民点与道路拓扑”断点；0=不排除。");
        addParamField(form, "PointSideTolerance", "居民点换边容差 (米)：",
                      0.0, 100000.0, 2, 10.0, 100.0,
                      "只比较距最近道路≤该值的居民点是否左右换边；0=不判定换边。");

        addSection(form, "②③ 道路长距离连接 / 道路·河流断裂");
        addParamField(form, "LongGapThreshold", "长断口阈值 (米)：",
                      0.0, 1000000.0, 2, 10.0, 100.0,
                      "综合后断口大于该值报“道路长距离连接”，否则按短断口处理。");
        addParamField(form, "ConnectTolerance", "连通判定容差 (米)：",
                      0.0, 100000.0, 4, 0.5, 1.0,
                      "综合前后端点间距小于该值视为相接。");
        addParamField(form, "MinBreakGap", "短断口报错下限 (米)：",
                      0.0, 1000000.0, 2, 10.0, 50.0,
                      "综合后断口≤该值视为可接受、不报；>该值且≤长断口阈值报“道路·河流断裂”。");
        addParamField(form, "TunnelPortalTolerance", "隧道口容差 (米)：",
                      0.0, 1000.0, 2, 1.0, 5.0,
                      "成果道路端点距隧道线端点（隧道口）≤该值时，视为经隧道连通，"
                      "不报“道路长距离连接/道路·河流断裂”；0=关闭隧道豁免。");

        addSection(form, "④ 道路变形异常");
        addParamField(form, "MaxChangeRatio", "最大变化率 (0~1)：",
                      0.0, 10.0, 4, 0.05, 0.6,
                      "面积/长度变化率超过该值视为显著变形。");
        addParamField(form, "MaxVertexChange", "最大顶点变化数 (个)：",
                      0.0, 1000000.0, 0, 10.0, 100.0,
                      "顶点数变化超过该值视为显著变形。");

        addSection(form, "⑤ 道路过度偏移");
        addParamField(form, "MaxDisplacement", "偏移缓冲半径 (米)：",
                      0.0, 1000000.0, 2, 10.0, 100.0,
                      "以同路原始线按该半径生成缓冲，成果线落在缓冲外的部分视为偏移；"
                      "成果线合并、截短不会误报。");

        addSection(form, "⑥ 道路拉直异常");
        addParamField(form, "MaxSinuosityDrop", "最大弯曲度下降：",
                      0.0, 10.0, 4, 0.05, 0.5,
                      "弯曲度（线长/首尾直线距离）下降超过该值报“道路拉直异常”（默认0.5）。");

        QFormLayout* advForm = nullptr;
        bodyLayout->addWidget(makeAdvanced(&advForm));
        addSection(advForm, "⑤ 偏移（高级）");
        addParamField(advForm, "MaxOutsideRatio", "缓冲外长度占比 (0~1)：",
                      0.0, 1.0, 3, 0.01, 0.2,
                      "成果线落在同路原始线缓冲外的长度占成果线长的比例超过该值，"
                      "判为“道路过度偏移”；默认0.2（20%），另需缓冲外长度大于50米。");
        addSection(advForm, "④ 变形与化简（高级）");
        addParamField(advForm, "MaxLocalBulge", "局部鼓包弦高 (米)：",
                      0.0, 100000.0, 2, 5.0, 20.0,
                      "局部鼓包弦高超过该值且距原始线超过最大局部偏离时报警；0=关闭该项。");
        addParamField(advForm, "MaxLocalDeviation", "最大局部偏离 (米)：",
                      0.0, 100000.0, 2, 5.0, 20.0,
                      "排除原始道路本身急弯的偏离阈值，与局部鼓包弦高配合使用。");
        addParamField(advForm, "MinSimplifyVertexCount", "化简最少原始顶点数 (个)：",
                      0.0, 1000000.0, 0, 10.0, 20.0,
                      "原始顶点数达到该值才检查是否化简；0=关闭化简检查。"
                      "线要素化简检查含水系（河流/渠），与道路共用本阈值。");
        addParamField(advForm, "MinVertexReduceRatio", "化简最少顶点减少比例 (0~1)：",
                      0.0, 1.0, 4, 0.01, 0.05,
                      "顶点减少比例低于该值视为未化简/化简不足；0=关闭化简检查。"
                      "线要素化简检查含水系（河流/渠），与道路共用本阈值。");
        addSection(advForm, "⑥ 直角拉直重点（高级）");
        addParamField(advForm, "RightAngleMinAngle", "直角拉直最小转角 (°)：",
                      0.0, 180.0, 1, 5.0, 70.0,
                      "成果线拐点转角（0=直行，90=直角）达到该值且不超过最大值时，"
                      "作为“道路直角拉直（重点）”候选。");
        addParamField(advForm, "RightAngleMaxAngle", "直角拉直最大转角 (°)：",
                      0.0, 180.0, 1, 5.0, 110.0,
                      "与最小转角配合使用；默认 70~110°（直角±20°）。");
        addParamField(advForm, "RightAngleOrigMaxTurn", "原始同位置最大转角 (°)：",
                      0.0, 180.0, 1, 5.0, 30.0,
                      "原始同名线在成果拐点附近的转角≤该值时，才判为综合后新增直角。");
        addParamField(advForm, "RightAngleMinSegment", "直角拐点最短邻边 (米)：",
                      0.0, 1000.0, 2, 1.0, 5.0,
                      "拐点两侧线段长度均≥该值才参与直角判定，避免小折点误报。");
        addParamField(advForm, "RightAngleSearchRadius", "原始线搜索半径 (米)：",
                      0.0, 10000.0, 2, 5.0, 60.0,
                      "在原始同名线上搜索对应拐点的半径。");
    } else if (missionId == 459) {
        addSection(form, "当前启用的图形检查");
        addParamField(form, "AcuteAngle", "尖锐角 (°)：",
                      0.1, 180.0, 1, 0.5, 5.0,
                      "面的夹角小于该值视为尖锐角。");
        addParamField(form, "MinArea", "小面积：",
                      0.0, 100000.0, 2, 1.0, 1.0,
                      "面积小于该值的面视为小面积面。");
        addParamField(form, "NodeDensityUpper", "节点密度上限 (个/要素)：",
                      0.0, 1000.0, 4, 1.0, 0.0,
                      "单个要素节点数超过该值报警；0=不启用上限。");
        addParamField(form, "NodeDensityLower", "节点密度下限 (个/要素)：",
                      0.0, 1000.0, 4, 1.0, 0.0,
                      "单个要素节点数低于该值报警；0=不启用下限。");

        QFormLayout* advForm = nullptr;
        bodyLayout->addWidget(makeAdvanced(&advForm));
        addSection(advForm, "已隐藏检查项（重新启用时使用）");
        addParamField(advForm, "SliverArea", "碎面面积：",
                      0.0, 100000.0, 2, 1.0, 1.0,
                      "面积小于该值的面视为碎面（碎面检查当前隐藏）。");
        addParamField(advForm, "NarrowWidth", "狭长面宽度 (米)：",
                      0.0, 1000.0, 2, 0.1, 0.5,
                      "最小外接矩形短边小于该值的面视为狭长面（狭长面检查当前隐藏）。");
        addParamField(advForm, "MinNodeDistance", "节点最小距离 (米)：",
                      0.0, 100.0, 6, 0.001, 0.001,
                      "相邻节点距离小于该值视为节点过密（节点最小距离检查当前隐藏）。");
        addParamField(advForm, "AvgNodeDensityUpper", "节点平均密度上限 (个/米)：",
                      0.0, 1000.0, 4, 0.01, 0.0,
                      "边界节点数/边界长度超过该值报警；0=不启用上限。");
        addParamField(advForm, "AvgNodeDensityLower", "节点平均密度下限 (个/米)：",
                      0.0, 1000.0, 4, 0.01, 0.0,
                      "边界节点数/边界长度低于该值报警；0=不启用下限。");
    } else if (missionId == 455) {
        addSection(form, "线拓扑检查");
        addParamField(form, "ResidentCoverTolerance", "居民点压盖容差 (米)：",
                      0.0, 1000.0, 2, 1.0, 10.0,
                      "悬挂端点距居民点≤该值时视为已连接，不算悬挂。");
        addNote(form, "结点容差见上方「全局参数」；线拓扑缓冲距离与"
                      "「综合前后拓扑检查」页的“缓冲匹配距离”共用。");
    }

    return panel;
}

void CSE_AutoQualityCheckDialog::addParamField(QFormLayout* form, const QString& key,
                                               const QString& label, double minV, double maxV,
                                               int decimals, double step, double defaultV,
                                               const QString& hint)
{
    QDoubleSpinBox* box = new QDoubleSpinBox(this);
    box->setObjectName("param_" + key);
    box->setRange(minV, maxV);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setValue(defaultV);
    box->setKeyboardTracking(false);
    box->setAlignment(Qt::AlignRight);
    box->setMinimumWidth(160);
    if (!hint.isEmpty()) box->setToolTip(hint);

    QLabel* lbl = new QLabel(label, this);
    if (!hint.isEmpty()) lbl->setToolTip(hint);

    form->addRow(lbl, box);
    m_paramSpins.insert(key, box);
    m_paramDecimals.insert(key, decimals);
}

// ================================================================
//  参数值：XML → 控件 / 控件 → m_thresholds
// ================================================================
void CSE_AutoQualityCheckDialog::loadMatchParams(const QString& matchParamPath)
{
    const char* keys[] = {
        "BufferDistance", "MinOverlapRatio", "MaxChangeRatio", "MaxVertexChange",
        "LongGapThreshold", "ConnectTolerance", "MinBreakGap", "MaxDisplacement",
        "MaxOutsideRatio", "MaxSinuosityDrop", "TunnelPortalTolerance",
        "RightAngleMinAngle", "RightAngleMaxAngle", "RightAngleOrigMaxTurn",
        "RightAngleMinSegment", "RightAngleSearchRadius",
        "EndpointBuffer", "BoundaryTolerance", "PointSideTolerance",
        "MinSimplifyVertexCount", "MinVertexReduceRatio",
        "MaxLocalBulge", "MaxLocalDeviation"
    };
    // 换配置时先清掉旧的匹配参数，避免残留上一份 XML 的值
    for (const char* key : keys)
        m_thresholds.remove(QString::fromLatin1(key));

    QFile pf(matchParamPath);
    if (!pf.open(QIODevice::ReadOnly)) return;
    QDomDocument doc;
    if (!doc.setContent(&pf)) { pf.close(); return; }
    pf.close();

    QDomElement param;
    const QDomNodeList missions = doc.documentElement().elementsByTagName("Mission");
    for (int i = 0; i < missions.count(); ++i) {
        const QDomElement m = missions.at(i).toElement();
        if (m.attribute("id").toInt() == 458) {
            param = m.firstChildElement("Parameter");
            break;
        }
    }
    if (param.isNull()) return;

    for (const char* key : keys) {
        const QString tag = QString::fromLatin1(key);
        const QDomElement e = param.firstChildElement(tag);
        if (e.isNull()) continue;
        bool ok = false;
        const double v = e.text().trimmed().toDouble(&ok);
        if (ok) m_thresholds[tag] = v;
    }
}

void CSE_AutoQualityCheckDialog::applyParamsToWidgets()
{
    for (auto it = m_paramSpins.constBegin(); it != m_paramSpins.constEnd(); ++it)
        it.value()->setValue(m_thresholds.value(it.key(), it.value()->value()));
}

void CSE_AutoQualityCheckDialog::collectParamsFromWidgets()
{
    for (auto it = m_paramSpins.constBegin(); it != m_paramSpins.constEnd(); ++it)
        m_thresholds[it.key()] = it.value()->value();
}

bool CSE_AutoQualityCheckDialog::saveParamsToXml(QString* error)
{
    if (m_qstrMissionXmlPath.isEmpty() || !QFileInfo::exists(m_qstrMissionXmlPath)) {
        *error = QStringLiteral("未找到 mission_config.xml，参数未写回配置文件。");
        return false;
    }
    QHash<QString, int> decimals = m_paramDecimals;
    decimals["Scale"] = 0;
    decimals["FuzzyTolerance"] = 6;
    if (!decimals.contains("ResidentCoverTolerance"))
        decimals["ResidentCoverTolerance"] = 2;

    return CSE_CheckParamsDialog::saveValuesToFiles(
        m_qstrMissionXmlPath, m_thresholds, decimals,
        QHash<QString, QString>(), error);
}

// ================================================================
//  阈值 SpinBox  ↔  m_thresholds 同步
// ================================================================
void CSE_AutoQualityCheckDialog::applyThresholdsToSpinBoxes()
{
    ui.doubleSpinBox_AcuteAngle->setValue(m_thresholds.value("AcuteAngle", 10.0));
    ui.doubleSpinBox_SliverArea->setValue(m_thresholds.value("SliverArea", 1.0));
    ui.doubleSpinBox_NarrowWidth->setValue(m_thresholds.value("NarrowWidth", 0.5));
    ui.doubleSpinBox_MinArea->setValue(m_thresholds.value("MinArea", 1.0));
    ui.doubleSpinBox_MinNodeDist->setValue(m_thresholds.value("MinNodeDistance", 0.001));
}

void CSE_AutoQualityCheckDialog::collectThresholdsFromSpinBoxes()
{
    m_thresholds["Scale"]           = static_cast<double>(ui.spinBox_Scale->value());
    m_thresholds["FuzzyTolerance"]  = ui.doubleSpinBox_Tolerance->value();
    m_thresholds["AcuteAngle"]      = ui.doubleSpinBox_AcuteAngle->value();
    m_thresholds["SliverArea"]      = ui.doubleSpinBox_SliverArea->value();
    m_thresholds["NarrowWidth"]     = ui.doubleSpinBox_NarrowWidth->value();
    m_thresholds["MinArea"]         = ui.doubleSpinBox_MinArea->value();
    m_thresholds["MinNodeDistance"] = ui.doubleSpinBox_MinNodeDist->value();
}

//  浏览按钮
// ================================================================
void CSE_AutoQualityCheckDialog::onBrowseOrigData()
{
    // 综合前数据支持 SHP 目录或 FileGDB：FileGDB 本质是文件夹（xxx.gdb），
    // 目录选择框可直接选中；选中后自动按 .gdb 后缀识别
    QString d = QFileDialog::getExistingDirectory(this, "原始数据目录（SHP目录或FileGDB）", m_qstrOrigDataPath);
    if (!d.isEmpty()) { m_qstrOrigDataPath = d; ui.lineEdit_OrigDataPath->setText(d); }
}
void CSE_AutoQualityCheckDialog::onBrowseResultData()
{
    QString d = QFileDialog::getExistingDirectory(this, "成果数据目录", m_qstrResultDataPath);
    if (!d.isEmpty()) { m_qstrResultDataPath = d; ui.lineEdit_ResultDataPath->setText(d); }
}
void CSE_AutoQualityCheckDialog::onBrowseShiftBefore()
{
    QString d = QFileDialog::getExistingDirectory(this, "化简后移位前数据目录", m_qstrShiftBeforePath);
    if (d.isEmpty()) return;
    m_qstrShiftBeforePath = d;
    if (QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_ShiftBeforePath"))
        le->setText(d);
}
void CSE_AutoQualityCheckDialog::onBrowseShiftAfter()
{
    QString d = QFileDialog::getExistingDirectory(this, "最终成果（移位后）数据目录", m_qstrShiftAfterPath);
    if (d.isEmpty()) return;
    m_qstrShiftAfterPath = d;
    if (QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_ShiftAfterPath"))
        le->setText(d);
}
void CSE_AutoQualityCheckDialog::onBrowseMissionXml()
{
    QString f = QFileDialog::getOpenFileName(this, "质检Mission XML", m_qstrMissionXmlPath, "XML (*.xml)");
    if (!f.isEmpty()) {
        m_qstrMissionXmlPath = f;
        ui.lineEdit_MissionXml->setText(f);
        // 重新加载配置
        loadMissionConfig(f);
        buildMissionUI();
    }
}
void CSE_AutoQualityCheckDialog::onBrowseOutputDir()
{
    QString d = QFileDialog::getExistingDirectory(this, "质检输出目录", m_qstrOutputDir);
    if (!d.isEmpty()) { m_qstrOutputDir = d; ui.lineEdit_OutputDir->setText(d); }
}

// ================================================================
//  全选 / 取消全选
// ================================================================
void CSE_AutoQualityCheckDialog::onSelectAll()
{
    for (auto& group : m_missionGroups) {
        for (auto& item : group.items) {
            if (item.checkbox && item.checkbox->isEnabled())
                item.checkbox->setChecked(true);
        }
    }
}
void CSE_AutoQualityCheckDialog::onDeselectAll()
{
    for (auto& group : m_missionGroups) {
        for (auto& item : group.items) {
            if (item.checkbox)
                item.checkbox->setChecked(false);
        }
    }
}

// ================================================================
//  开始质检
// ================================================================
void CSE_AutoQualityCheckDialog::onStartCheck()
{
    // 读回输入框文本（支持手动输入/粘贴路径，不依赖浏览按钮）
    m_qstrOrigDataPath   = ui.lineEdit_OrigDataPath->text().trimmed();
    m_qstrResultDataPath = ui.lineEdit_ResultDataPath->text().trimmed();
    m_qstrOutputDir      = ui.lineEdit_OutputDir->text().trimmed();
    // 分阶段数据（⑤真实移位值）：支持手动输入/粘贴
    if (QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_ShiftBeforePath"))
        m_qstrShiftBeforePath = le->text().trimmed();
    if (QLineEdit* le = ui.groupBox_DataSource->findChild<QLineEdit*>("lineEdit_ShiftAfterPath"))
        m_qstrShiftAfterPath = le->text().trimmed();
    const QString typedMissionXml = ui.lineEdit_MissionXml->text().trimmed();
    if (!typedMissionXml.isEmpty() && typedMissionXml != m_qstrMissionXmlPath) {
        m_qstrMissionXmlPath = typedMissionXml;
        loadMissionConfig(typedMissionXml);
        buildMissionUI();
    }

    // 校验
    if (m_qstrOrigDataPath.isEmpty() && m_qstrResultDataPath.isEmpty()) {
        QMessageBox::warning(this, "警告", "请至少选择一个数据源（原始数据或成果数据）");
        return;
    }
    if (m_qstrOutputDir.isEmpty()) {
        QMessageBox::warning(this, "警告", "请选择质检输出目录");
        return;
    }

    // 收集勾选的检查项摘要
    int totalChecked = 0;
    QStringList enabledChecks;
    for (const auto& group : m_missionGroups) {
        for (const auto& item : group.items) {
            if (item.checkbox && item.checkbox->isChecked() && item.implemented) {
                totalChecked++;
                enabledChecks.append(QString("M%1 %2").arg(group.id).arg(item.name));
            }
        }
    }
    if (totalChecked == 0) {
        QMessageBox::warning(this, "警告", "请至少勾选一项检查规则");
        return;
    }

    // ⑤道路过度偏移：勾选了但没同时填「化简后移位前」「最终成果」两个路径时，
    // 直接提示无法检查并中止，不再静默退回旧的原始数据缓冲法（2026-11-08 甲方口径）。
    bool offsetCheckChecked = false;
    for (const auto& group : m_missionGroups) {
        for (const auto& item : group.items) {
            if (item.checkbox && item.checkbox->isChecked() && item.implemented
                && item.name == QStringLiteral("道路过度偏移")) {
                offsetCheckChecked = true;
                break;
            }
        }
        if (offsetCheckChecked) break;
    }
    if (offsetCheckChecked
        && (m_qstrShiftBeforePath.isEmpty() || m_qstrShiftAfterPath.isEmpty())) {
        QStringList missing;
        if (m_qstrShiftBeforePath.isEmpty())
            missing << QStringLiteral("「移位前数据路径（化简后）」");
        if (m_qstrShiftAfterPath.isEmpty())
            missing << QStringLiteral("「移位后数据路径（最终成果）」");
        QMessageBox::warning(this, QStringLiteral("无法执行道路过度偏移检查"),
            QStringLiteral("已勾选「道路过度偏移」检查，但未填写%1。\n\n"
                           "请同时填写这两个路径，或取消勾选「道路过度偏移」后再开始检查。")
                .arg(missing.join(QStringLiteral("、"))));
        return;
    }

    ui.progressBar->setValue(0);
    ui.label_Status->setText("状态：质检中...");

    // 构建任务列表：原始数据 + 成果数据（各自独立）。
    // FileGDB（综合前）不单独跑成果类检查（453/454/455/456/459），
    // 仅作为 457/458 综合前后对照的参照侧（在 Mission 内部处理）。
    const bool origIsGdb = WujiMissionRunner::isFileGdbSource(m_qstrOrigDataPath);
    const bool resultIsGdb = WujiMissionRunner::isFileGdbSource(m_qstrResultDataPath);
    if (origIsGdb && m_qstrResultDataPath.isEmpty()) {
        QMessageBox::warning(this, "警告",
            "原始数据为FileGDB时，需要同时选择成果数据(SHP)目录才能执行检查");
        return;
    }
    if (resultIsGdb) {
        QMessageBox::warning(this, "警告", "成果数据暂不支持FileGDB，请选择SHP目录");
        return;
    }
    struct { QString path; QString label; } tasks[2];
    int n = 0;
    if (!m_qstrOrigDataPath.isEmpty() && !origIsGdb)
        tasks[n++] = {m_qstrOrigDataPath, "原始数据"};
    if (!m_qstrResultDataPath.isEmpty())
        tasks[n++] = {m_qstrResultDataPath, "成果数据"};

    QStringList allLogPaths;
    // 所有数据源的检查结果汇总，最终合并写一份"综合成果质检"日志
    QStringList combinedErrs, combinedCks, combinedOutputs;
    collectThresholdsFromSpinBoxes();
    collectParamsFromWidgets();

    // 参数改完自动写回 XML（只有内容变化才写，写前自动备份）：
    // M460 运行时直接读取 matchParameter.xml，必须在检查开始前落盘；
    // M459/线面拓扑用内存中的 m_thresholds，落盘同时保证下次启动仍是本次的值。
    if (!m_qstrMissionXmlPath.isEmpty() && QFileInfo::exists(m_qstrMissionXmlPath)) {
        QString paramErr;
        if (!saveParamsToXml(&paramErr))
            QMessageBox::warning(this, "参数保存失败",
                                 paramErr + "\n综合前后拓扑检查(M460)将使用配置文件中的旧参数，"
                                            "图形规范/线面拓扑仍按界面当前参数运行。");
    }

    // 引擎可用性提示（仅记一次）：454/455/456 点线面拓扑检查正常走引擎子进程，
    // 引擎可执行文件不存在时（如麒麟）自动回退本地 QGIS 实现，结果照常输出。
    if (!WujiEngineBridge::engineAvailable()) {
        combinedCks.append(QStringLiteral(
            "提示：未找到无极引擎(MapBatchProcessing.exe)，点/线/面拓扑检查(454/455/456)已回退本地实现"));
    }

    for (int i = 0; i < n; i++) {
        ui.progressBar->setValue(10 + (i * 40) / n);
        ui.label_Status->setText(QString("状态：%1质检中...").arg(tasks[i].label));

        QString dataDir = tasks[i].path;
        QStringList errs, cks, allOutputFiles;
        cks.append("已启用检查项(" + QString::number(totalChecked) + "项)");
        for (const auto& c : enabledChecks) cks.append("  " + c);
        cks.append("数据路径: " + dataDir);

        // ---- 打开各几何类型图层（优先使用图层映射，回退到扫描） ----
        QStringList polyShps, lineShps, ptShps;

        if (!m_layerMappingItems.isEmpty()) {
            // 使用图层映射表中的几何类型分类
            polyShps = getMappedShpByType("面");
            lineShps = getMappedShpByType("线");
            ptShps   = getMappedShpByType("点");

            // 过滤映射中在当前数据目录里不存在的文件
            auto filterExist = [&](QStringList& lst) {
                for (int i = lst.size() - 1; i >= 0; --i)
                    if (!QFileInfo::exists(dataDir + "/" + lst[i]))
                        lst.removeAt(i);
            };
            filterExist(polyShps);
            filterExist(lineShps);
            filterExist(ptShps);
        }

        // 回退：如果映射为空或全部失效，扫描目录中所有SHP
        if (polyShps.isEmpty() && lineShps.isEmpty() && ptShps.isEmpty()) {
            QStringList allShps = scanShpFiles(dataDir);
            for (const auto& s : allShps) {
                QString base = QFileInfo(s).baseName();
                if (base.contains("面") || base.contains("普色") || base.contains("注记"))
                    polyShps.append(s);
                else if (base.contains("界") || base.contains("道") || base.contains("路")
                    || base.contains("河") || base.contains("渠") || base.contains("铁路")
                    || base.contains("高速") || base.contains("线"))
                    lineShps.append(s);
                else if (base.contains("点") || base.contains("站") || base.contains("驻地")
                    || base.contains("山峰") || base.contains("泉"))
                    ptShps.append(s);
                else
                    polyShps.append(s); // 默认当面处理
            }
            if (m_layerMappingItems.isEmpty())
                cks.append("图层归类说明：未配置图层映射，按名称关键词自动分类");
            else
                cks.append(QString("图层归类说明：图层映射表已读取（%1条）但无可用条目"
                                   "（SHP未配置或映射的SHP在本数据目录不存在），已按名称关键词自动分类")
                               .arg(m_layerMappingItems.size()));
        }

        // 打开图层函数
        auto openLayer = [&](const QString& shp, const QString& label) -> QgsVectorLayer* {
            if (shp.isEmpty()) return nullptr;
            QString path = dataDir + "/" + shp;
            auto* lyr = new QgsVectorLayer(path, label, "ogr");
            if (!lyr || !lyr->isValid()) { delete lyr; return nullptr; }
            return lyr;
        };

        // 收集所有成功打开的图层
        QList<QgsVectorLayer*> allPolyLayers, allLineLayers, allPtLayers;
        QgsCoordinateReferenceSystem crs;
        QgsFields polyFields, lineFields, ptFields;

        for (const auto& shp : polyShps) {
            auto* lyr = openLayer(shp, shp);
            if (lyr) {
                allPolyLayers.append(lyr);
                if (!crs.isValid()) crs = lyr->crs();
                if (polyFields.isEmpty()) polyFields = lyr->fields();
            }
        }
        for (const auto& shp : lineShps) {
            auto* lyr = openLayer(shp, shp);
            if (lyr) {
                allLineLayers.append(lyr);
                if (!crs.isValid()) crs = lyr->crs();
                if (lineFields.isEmpty()) lineFields = lyr->fields();
            }
        }
        for (const auto& shp : ptShps) {
            auto* lyr = openLayer(shp, shp);
            if (lyr) {
                allPtLayers.append(lyr);
                if (!crs.isValid()) crs = lyr->crs();
                if (ptFields.isEmpty()) ptFields = lyr->fields();
            }
        }

        // ---- 按实际几何类型重新归类（图层映射只作参考，不一致时以数据为准） ----
        // 甲方成果数据中部分注记图层（如 *_名称、*代码、隧道口）在映射里被标为线，
        // 实际几何是点：喂给 455 线槽会被引擎立即拒绝（线拓扑规则任务执行失败）。
        {
            QStringList movedToPt, movedToLine, movedToPoly;
            auto fixList = [&](QList<QgsVectorLayer*>& list, QgsWkbTypes::GeometryType want) {
                for (int i = list.size() - 1; i >= 0; --i) {
                    QgsVectorLayer* lyr = list.at(i);
                    if (lyr->geometryType() == want) continue;
                    const QString base = QFileInfo(lyr->source()).completeBaseName();
                    if (lyr->geometryType() == QgsWkbTypes::PointGeometry) {
                        allPtLayers.append(lyr);
                        movedToPt.append(base);
                    } else if (lyr->geometryType() == QgsWkbTypes::LineGeometry) {
                        allLineLayers.append(lyr);
                        movedToLine.append(base);
                    } else {
                        allPolyLayers.append(lyr);
                        movedToPoly.append(base);
                    }
                    list.removeAt(i);
                }
            };
            fixList(allPolyLayers, QgsWkbTypes::PolygonGeometry);
            fixList(allLineLayers, QgsWkbTypes::LineGeometry);
            fixList(allPtLayers, QgsWkbTypes::PointGeometry);
            if (!movedToPt.isEmpty())
                cks.append(QStringLiteral("图层归类说明：以下图层按实际几何类型以点参与检查：%1").arg(movedToPt.join(QStringLiteral("、"))));
            if (!movedToLine.isEmpty())
                cks.append(QStringLiteral("图层归类说明：以下图层按实际几何类型以线参与检查：%1").arg(movedToLine.join(QStringLiteral("、"))));
            if (!movedToPoly.isEmpty())
                cks.append(QStringLiteral("图层归类说明：以下图层按实际几何类型以面参与检查：%1").arg(movedToPoly.join(QStringLiteral("、"))));
        }

        // 重新归类后刷新字段回退（首图层可能已被移动）
        polyFields = allPolyLayers.isEmpty() ? QgsFields() : allPolyLayers.first()->fields();
        lineFields = allLineLayers.isEmpty() ? QgsFields() : allLineLayers.first()->fields();
        ptFields   = allPtLayers.isEmpty()   ? QgsFields() : allPtLayers.first()->fields();

        // 为向后兼容保留首图层指针（结果输出字段回退用）
        QgsVectorLayer* polyLayer = allPolyLayers.isEmpty() ? nullptr : allPolyLayers.first();
        QgsVectorLayer* lineLayer = allLineLayers.isEmpty() ? nullptr : allLineLayers.first();

        // 注记类图层（名称注记点、代码点、表面注记面）不参与 454/455/456 拓扑检查：
        // 既不作检查目标，也不作参考图层——它们是制图注记，空间关系规则对它们无意义。
        // 隧道口/方向点/铁路休止符等符号点有真实地物含义，按普通点参与 454 检查。
        auto isAnnotationLayer = [](QgsVectorLayer* lyr) -> bool {
            const QString base = QFileInfo(lyr->source()).completeBaseName();
            return base.contains(QStringLiteral("名称")) || base.contains(QStringLiteral("代码"))
                || base.contains(QStringLiteral("注记"));
        };
        QList<QgsVectorLayer*> topoPtLayers, topoLineLayers, topoPolyLayers;
        QStringList annotationNames;
        for (auto* lyr : allPtLayers) {
            if (isAnnotationLayer(lyr))
                annotationNames.append(QFileInfo(lyr->source()).completeBaseName());
            else
                topoPtLayers.append(lyr);
        }
        for (auto* lyr : allLineLayers) {
            if (isAnnotationLayer(lyr))
                annotationNames.append(QFileInfo(lyr->source()).completeBaseName());
            else
                topoLineLayers.append(lyr);
        }
        for (auto* lyr : allPolyLayers) {
            if (isAnnotationLayer(lyr))
                annotationNames.append(QFileInfo(lyr->source()).completeBaseName());
            else
                topoPolyLayers.append(lyr);
        }
        if (!annotationNames.isEmpty())
            cks.append(QStringLiteral("注记/代码类图层按制图规范不参与拓扑检查：%1").arg(annotationNames.join(QStringLiteral("、"))));

        if (allPolyLayers.isEmpty() && allLineLayers.isEmpty() && allPtLayers.isEmpty()) {
            errs.append("错误：数据目录中未找到有效SHP文件");
            // 无图层数据也进入统一日志，按数据源分节
            combinedErrs.append(QString("======== %1 ========").arg(tasks[i].label));
            combinedErrs.append(errs);
            combinedCks.append(QString("======== %1 ========").arg(tasks[i].label));
            combinedCks.append(cks);
            continue;
        }

        cks.append(QString("已加载图层：面%1个 线%2个 点%3个")
            .arg(allPolyLayers.size()).arg(allLineLayers.size()).arg(allPtLayers.size()));

        // ---- 按 Mission 执行检查 ----
        for (const auto& group : m_missionGroups) {
            // 计算 ProcessMode
            int totalMode = 0;
            for (const auto& item : group.items) {
                if (item.checkbox && item.checkbox->isChecked() && item.implemented)
                    totalMode |= item.mode;
            }
            if (totalMode == 0) continue;

            QList<QPair<QgsFeature, QString>> allErrs;
            QStringList executed;

            switch (group.id) {
            case 460: {
                // ============ 综合前后拓扑检查（七项关注点） ============
                // 仅对成果数据执行一次（原始数据侧任务跳过）；①用引擎(454)单独跑
                // 居民点×道路，②~⑦用 M458 专项模式结构化输出，结果写
                // 综合前后拓扑检查_点/线/面.shp 并置顶加载。
                if (m_qstrResultDataPath.isEmpty()
                    || QDir::cleanPath(dataDir) != QDir::cleanPath(m_qstrResultDataPath)) {
                    executed.append(m_qstrResultDataPath.isEmpty()
                        ? QStringLiteral("综合前后拓扑检查：需同时提供原始数据与成果数据，本次未执行")
                        : QStringLiteral("综合前后拓扑检查：本次仅对成果数据执行"));
                    break;
                }

                // 甲方关注点顺序（与 mission_config.xml 460 组 CheckItem 一致）
                const QStringList focusOrder = {
                    QStringLiteral("居民点与道路拓扑"), QStringLiteral("道路长距离连接"),
                    QStringLiteral("道路·河流断裂"), QStringLiteral("道路变形异常"),
                    QStringLiteral("线要素化简检查"),   // ④附加子项（勾选④即执行，含水系）
                    QStringLiteral("道路过度偏移"), QStringLiteral("道路拉直异常"),
                    QStringLiteral("道路直角拉直（重点）"),   // ⑥附加子项（勾选⑥即执行）
                    QStringLiteral("线要素属性一致性")};
                QStringList checkedFocus;
                for (const auto& item : group.items) {
                    if (item.checkbox && item.checkbox->isChecked() && item.implemented)
                        checkedFocus << item.name;
                }
                // “线要素化简检查”不是独立CheckItem，勾选④变形或⑥拉直任一即执行/报告；
                // 2026-10-30 起检查对象含水系（河流/渠），由 Mission458 内部派生位实现
                if (checkedFocus.contains(QStringLiteral("道路变形异常"))
                    || checkedFocus.contains(QStringLiteral("道路拉直异常")))
                    checkedFocus << QStringLiteral("线要素化简检查");
                // “道路直角拉直（重点）”是⑥的附加子项，勾选⑥即单独统计/输出
                if (checkedFocus.contains(QStringLiteral("道路拉直异常")))
                    checkedFocus << QStringLiteral("道路直角拉直（重点）");

                QList<Mission458::FocusError> allFocus;
                int m458Focus = totalMode & ~1;   // ②~⑦（①由引擎执行）

                // 图层映射（成果图层名→源图层代码）与参数文件：①原始侧配对与②~⑦共用
                QHash<QString, QString> layerHashMap;
                for (const auto& item : m_layerMappingItems) {
                    if (!item.sourceCode.isEmpty())
                        layerHashMap[item.stdName] = item.sourceCode;
                }
                // 1107 对应表中的 4 个道路族在原始数据里不是独立图层，按源图层代码补映射：
                // 城市道路→LCTL，快速路/连接道→LRDL，铁路(合并层)→LRRL。
                if (!layerHashMap.contains(QStringLiteral("城市道路")))
                    layerHashMap.insert(QStringLiteral("城市道路"), QStringLiteral("LCTL"));
                if (!layerHashMap.contains(QStringLiteral("快速路")))
                    layerHashMap.insert(QStringLiteral("快速路"), QStringLiteral("LRDL"));
                if (!layerHashMap.contains(QStringLiteral("连接道")))
                    layerHashMap.insert(QStringLiteral("连接道"), QStringLiteral("LRDL"));
                if (!layerHashMap.contains(QStringLiteral("铁路")))
                    layerHashMap.insert(QStringLiteral("铁路"), QStringLiteral("LRRL"));
                const QString matchParamPath =
                    QFileInfo(m_qstrMissionXmlPath).absolutePath() + QStringLiteral("/matchParameter.xml");

                // ---- ⑤ 真实移位值：加载「化简后移位前」和「最终成果」两套分阶段数据 ----
                // 2026-11-08 甲方口径：⑤只按这两套数据逐顶点计算真实移位值，
                // 不再回退到“原始数据同路缓冲法”。路径未填或匹配不到时直接不执行⑤。
                QHash<QString, Mission458::TrueShiftInfo> trueShifts;
                if (checkedFocus.contains(QStringLiteral("道路过度偏移"))) {
                    if (m_qstrShiftBeforePath.isEmpty() || m_qstrShiftAfterPath.isEmpty()) {
                        executed.append(QStringLiteral(
                            "⑤真实移位值：未同时填写「化简后移位前」和「最终成果」路径，"
                            "⑤道路过度偏移未执行"));
                        checkedFocus.removeAll(QStringLiteral("道路过度偏移"));
                        m458Focus &= ~16;   // FocusDisplace
                    } else {
                        trueShifts = Mission458::computeTrueShifts(
                            m_qstrShiftBeforePath, m_qstrShiftAfterPath,
                            matchParamPath, &executed);
                        if (trueShifts.isEmpty()) {
                            executed.append(QStringLiteral(
                                "⑤真实移位值：未匹配到移位前参照，⑤道路过度偏移未执行"));
                            checkedFocus.removeAll(QStringLiteral("道路过度偏移"));
                            m458Focus &= ~16;   // FocusDisplace
                        } else {
                            const QString shiftCsv = writeTrueShiftCsv(m_qstrOutputDir,
                                trueShifts, m_thresholds.value("MaxDisplacement", 100.0));
                            if (!shiftCsv.isEmpty())
                                allOutputFiles.append(shiftCsv);
                        }
                    }
                }
                const QHash<QString, Mission458::TrueShiftInfo>* trueShiftPtr =
                    trueShifts.isEmpty() ? nullptr : &trueShifts;

                // ---- 选取比例统计：成果要素数 / 原始要素数（只统计，不判错） ----
                QList<Mission458::SelectionStat> selectionStats;
                Mission458::collectSelectionStats(m_qstrOrigDataPath, m_qstrResultDataPath,
                    &layerHashMap, selectionStats, &executed);

                // ---- ① 村道/乡道端点连接：端点必须连接其他道路或被居民点压盖 ----
                // 2026-10-27 按甲方反馈：取消“居民点必须压盖道路”；改为检查成果侧
                // 村道/乡道每个折线端点，距其他道路或居民点 ≤ EndpointBuffer
                // （matchParameter.xml，默认 1mm）即视为连接/压盖，否则报“居民点与道路拓扑”。
                if (totalMode & 1) {
                    QList<QgsVectorLayer*> residentPts, roadLines, villageTownshipRoads;
                    for (auto* lyr : topoPtLayers) {
                        const QString base = QFileInfo(lyr->source()).completeBaseName();
                        if ((base.contains(QStringLiteral("村"))
                             || base.contains(QStringLiteral("驻地")))
                            && !base.contains(QStringLiteral("标注")))
                            residentPts.append(lyr);
                    }
                    for (auto* lyr : topoLineLayers) {
                        const QString base = QFileInfo(lyr->source()).completeBaseName();
                        // 道路类（不含河流/渠/铁路）；高速公路名称无"道"字，需单独纳入
                        if (base.contains(QStringLiteral("道"))
                            || (base.contains(QStringLiteral("高速"))
                                && !base.contains(QStringLiteral("铁路")))) {
                            roadLines.append(lyr);
                            // 检查目标：村道/乡道（成果图层名如“村道”“乡道（专用道）”）
                            if (base.contains(QStringLiteral("村道"))
                                || base.contains(QStringLiteral("乡道")))
                                villageTownshipRoads.append(lyr);
                        }
                    }
                    // 行政界线图层（县界/市界/省界）：边界端点判定参照
                    QList<QgsVectorLayer*> boundaryLines;
                    for (auto* lyr : topoLineLayers) {
                        const QString base = QFileInfo(lyr->source()).completeBaseName();
                        if (base == QStringLiteral("省界") || base == QStringLiteral("未定省界")
                            || base == QStringLiteral("市界") || base == QStringLiteral("县界"))
                            boundaryLines.append(lyr);
                    }
                    if (villageTownshipRoads.isEmpty() || roadLines.isEmpty()) {
                        executed.append(QStringLiteral(
                            "村道/乡道端点连接：未找到村道/乡道图层或道路图层，本次不涉及"));
                    } else {
                        QStringList endpointLog;
                        Mission458::buildRoadEndpointFocusErrors(villageTownshipRoads,
                            roadLines, residentPts, boundaryLines, matchParamPath,
                            allFocus, &endpointLog);
                        executed += endpointLog;
                    }

                    // ---- ①附加：综合前后居民点跨到道路另一侧（左右换边） ----
                    // 直接遍历成果侧全部居民点（不依赖引擎是否报未压盖）；原始同名点
                    // 按 ELEMID/坐标/1米内最近点匹配，同一条道路按 ELEMID 对应，
                    // 道路方向反转时统一方向后再比较左右侧。
                    QStringList sideLog;
                    Mission458::buildResidentSideFlipFocusErrors(residentPts, roadLines,
                        m_qstrOrigDataPath, layerHashMap, matchParamPath, allFocus, &sideLog);
                    executed += sideLog;
                }

                // ---- ②~⑦ 综合前后增强检查（M458 专项模式，一次调用全算） ----
                if (m458Focus != 0) {
                    if (m_qstrOrigDataPath.isEmpty()) {
                        executed.append(QStringLiteral(
                            "综合前后类检查（②~⑦）：需同时提供原始数据，本次未执行"));
                    } else {
                        QList<QPair<QgsFeature, QString>> dummyErrs;
                        if (!layerHashMap.isEmpty()) {
                            Mission458::executeWithMapping(m_qstrOrigDataPath, m_qstrResultDataPath,
                                m458Focus, layerHashMap, dummyErrs, executed, matchParamPath,
                                m458Focus, &allFocus, trueShiftPtr);
                        } else {
                            Mission458::execute(m_qstrOrigDataPath, m_qstrResultDataPath,
                                m458Focus, dummyErrs, executed, nullptr, matchParamPath,
                                m458Focus, &allFocus, trueShiftPtr);
                        }
                    }
                }

                // ---- ①/②③ 同坐标去重（2026-10-30 P0-2） ----
                // 同一个成果侧道路端点，可能同时被①判为“端点未连接”、被②/③判为
                // “综合前相接、综合后断开”。坐标完全重合（按 0.1 米取整）时只保留
                // ②③（带原间距/现间距，信息更具体），去掉①的重复记录；①的
                // “左右换边”记录几何是居民点位置、描述也不含“端点未连接”，不受影响。
                {
                    QSet<QString> breakPointKeys;
                    for (const auto& fe : allFocus) {
                        if (fe.checkItem != QStringLiteral("道路长距离连接")
                            && fe.checkItem != QStringLiteral("道路·河流断裂"))
                            continue;
                        if (fe.geometry.type() != QgsWkbTypes::PointGeometry)
                            continue;
                        const QgsPointXY p = fe.geometry.asPoint();
                        breakPointKeys.insert(QStringLiteral("%1_%2")
                            .arg(qRound64(p.x() * 10.0)).arg(qRound64(p.y() * 10.0)));
                    }
                    if (!breakPointKeys.isEmpty()) {
                        const int beforeDedup = allFocus.size();
                        QList<Mission458::FocusError> dedupFocus;
                        dedupFocus.reserve(allFocus.size());
                        for (const auto& fe : allFocus) {
                            if (fe.checkItem == QStringLiteral("居民点与道路拓扑")
                                && fe.geometry.type() == QgsWkbTypes::PointGeometry
                                && fe.desc.contains(QStringLiteral("端点未连接"))) {
                                const QgsPointXY p = fe.geometry.asPoint();
                                const QString k = QStringLiteral("%1_%2")
                                    .arg(qRound64(p.x() * 10.0)).arg(qRound64(p.y() * 10.0));
                                if (breakPointKeys.contains(k)) continue;
                            }
                            dedupFocus.append(fe);
                        }
                        const int removed = beforeDedup - dedupFocus.size();
                        allFocus = dedupFocus;
                        if (removed > 0)
                            executed.append(QStringLiteral(
                                "①与②③同坐标去重：去掉%1处重复断点（保留②③记录）").arg(removed));
                    }
                }

                // ---- 写 综合前后拓扑检查_点/线/面.shp（带 检查项/说明/前后值 字段）
                //      + 综合前后拓扑检查_定位点.shp（具体问题位置，甲方双击定位） ----
                QStringList focusFiles;
                if (!allFocus.isEmpty()) {
                    focusFiles = writeFocusErrorsToShp(m_qstrOutputDir, allFocus, crs);
                    focusFiles += writeFocusLocationsToShp(m_qstrOutputDir, allFocus, crs);
                }
                allOutputFiles.append(focusFiles);
                // 选取比例统计 CSV（有可配对图层时输出；不是错误图层，不置顶加载）
                const QString selectionCsv = writeSelectionStatsCsv(m_qstrOutputDir, selectionStats);
                if (!selectionCsv.isEmpty())
                    allOutputFiles.append(selectionCsv);
                // 置顶加载到当前工程（面→线→点→定位点依次置顶，定位点在最上）
                const QStringList loadOrder = {QStringLiteral("综合前后拓扑检查_面.shp"),
                                               QStringLiteral("综合前后拓扑检查_线.shp"),
                                               QStringLiteral("综合前后拓扑检查_点.shp"),
                                               QStringLiteral("综合前后拓扑检查_定位点.shp")};
                for (const QString& fn : loadOrder)
                    if (focusFiles.contains(fn))
                        loadOutputLayerOnTop(m_qstrOutputDir + "/" + fn);

                // ---- 逐检查项日志（每条一行）+ 汇总（本组结果先于其他检查） ----
                QMap<QString, int> itemCounts;
                for (const auto& fe : allFocus)
                    itemCounts[fe.checkItem]++;
                auto filesForItem = [&](const QString& name) -> QString {
                    bool hasPt = false, hasLine = false, hasPoly = false;
                    for (const auto& fe : allFocus) {
                        if (fe.checkItem != name) continue;
                        const QgsWkbTypes::GeometryType t = fe.geometry.type();
                        if (t == QgsWkbTypes::PointGeometry) hasPt = true;
                        else if (t == QgsWkbTypes::LineGeometry) hasLine = true;
                        else hasPoly = true;
                    }
                    QStringList names;
                    if (hasPt) names << QStringLiteral("综合前后拓扑检查_点.shp");
                    if (hasLine) names << QStringLiteral("综合前后拓扑检查_线.shp");
                    if (hasPoly) names << QStringLiteral("综合前后拓扑检查_面.shp");
                    return names.join(QStringLiteral("、"));
                };
                errs.append(QStringLiteral("---- 综合前后拓扑检查 ----"));
                for (const QString& name : focusOrder) {
                    if (!checkedFocus.contains(name)) continue;
                    const int c = itemCounts.value(name, 0);
                    errs.append(c == 0
                        ? QStringLiteral("[%1] 未检出异常").arg(name)
                        : QStringLiteral("[%1] 检出 %2 处 → %3")
                              .arg(name).arg(c).arg(filesForItem(name)));
                }
                errs.append(QStringLiteral("---- 综合前后拓扑检查结果汇总 ----"));
                {
                    int nPt = 0, nLine = 0, nPoly = 0;
                    for (const auto& fe : allFocus) {
                        const QgsWkbTypes::GeometryType t = fe.geometry.type();
                        if (t == QgsWkbTypes::PointGeometry) nPt++;
                        else if (t == QgsWkbTypes::LineGeometry) nLine++;
                        else nPoly++;
                    }
                    int nLoc = 0;
                    for (const auto& fe : allFocus) {
                        if (fe.hasLoc) nLoc++;
                    }
                    QStringList parts;
                    if (nPt)   parts << QStringLiteral("综合前后拓扑检查_点.shp（%1处）").arg(nPt);
                    if (nLine) parts << QStringLiteral("综合前后拓扑检查_线.shp（%1处）").arg(nLine);
                    if (nPoly) parts << QStringLiteral("综合前后拓扑检查_面.shp（%1处）").arg(nPoly);
                    if (nLoc)  parts << QStringLiteral("综合前后拓扑检查_定位点.shp（%1处）").arg(nLoc);
                    errs.append(parts.isEmpty()
                        ? QStringLiteral("综合前后拓扑检查：未检出异常")
                        : QStringLiteral("综合前后拓扑检查：共检出 %1 处 → %2")
                              .arg(allFocus.size()).arg(parts.join(QStringLiteral("；"))));
                }
                // ---- 选取比例统计（成果数/原始数） ----
                if (!selectionStats.isEmpty()) {
                    errs.append(QStringLiteral("---- 选取比例统计（成果数/原始数） ----"));
                    for (const auto& s : selectionStats) {
                        const QString ratio = s.origCount > 0
                            ? QStringLiteral("%1%").arg(
                                  100.0 * double(s.resultCount) / double(s.origCount), 0, 'f', 2)
                            : QStringLiteral("—");
                        errs.append(QStringLiteral("[%1] 原始%2 → 成果%3，选取比例%4")
                            .arg(s.resultLayer).arg(s.origCount).arg(s.resultCount).arg(ratio));
                    }
                    // 按源图层汇总：多个成果图层映射到同一源图层时看整体取舍强度
                    QMap<QString, qint64> origCountByLayer, resultSumByLayer;
                    for (const auto& s : selectionStats) {
                        origCountByLayer[s.origLayer] = s.origCount;
                        resultSumByLayer[s.origLayer] += s.resultCount;
                    }
                    errs.append(QStringLiteral("---- 按源图层汇总（成果合计/源图层要素数） ----"));
                    for (auto oit = origCountByLayer.constBegin();
                         oit != origCountByLayer.constEnd(); ++oit) {
                        const qint64 oc = oit.value();
                        const qint64 rc = resultSumByLayer.value(oit.key());
                        const QString ratio = oc > 0
                            ? QStringLiteral("%1%").arg(
                                  100.0 * double(rc) / double(oc), 0, 'f', 2)
                            : QStringLiteral("—");
                        errs.append(QStringLiteral("[%1] 源图层%2 → 成果合计%3，总选取比例%4")
                            .arg(oit.key()).arg(oc).arg(rc).arg(ratio));
                    }
                    errs.append(QStringLiteral("选取比例明细已输出：选取比例统计.csv"));
                } else if (!m_qstrOrigDataPath.isEmpty()) {
                    errs.append(QStringLiteral("选取比例统计：未找到可配对的原始/成果图层，未输出"));
                }
                break;
            }
            case 453: {
                // 属性检查：逐图层对照标准字段结构（随包config/FeatureSchema/<图层名>.xml）
                if (QDir::cleanPath(dataDir) != QDir::cleanPath(m_qstrResultDataPath)) {
                    executed.append("属性检查：本次仅对成果数据执行");
                    break;
                }
                const QString schemaDir = QFileInfo(m_qstrMissionXmlPath).absolutePath() + "/FeatureSchema";
                QList<QgsVectorLayer*> attrLayers;
                attrLayers += allPolyLayers; attrLayers += allLineLayers; attrLayers += allPtLayers;
                if (attrLayers.isEmpty()) {
                    executed.append("属性检查：无图层数据，本次不涉及");
                    break;
                }
                for (QgsVectorLayer* lyr : attrLayers) {
                    QString base = QFileInfo(lyr->source()).completeBaseName();
                    Mission453::execute(lyr, schemaDir + "/" + base + ".xml",
                        totalMode, allErrs, executed);
                }
                break;
            }
            case 454: {
                // 点拓扑检查：每个点图层单独一次引擎调用（全部图层逐层检查，注记类除外）；
                // 参考图层 = 全部线图层合并 + 全部面图层合并（纯几何，合并一次复用）
                if (topoPtLayers.isEmpty()) {
                    executed.append(QStringLiteral("点拓扑检查：无点要素图层，本次不涉及"));
                    break;
                }
                QString refLineShp, refPolyShp;
                QTemporaryDir refWork;
                if (refWork.isValid()) {
                    QString refErr;
                    refLineShp = WujiMissionRunner::mergeLayersShp(
                        topoLineLayers, refWork.path(), QStringLiteral("ref_lines"), &refErr);
                    refPolyShp = WujiMissionRunner::mergeLayersShp(
                        topoPolyLayers, refWork.path(), QStringLiteral("ref_polys"), &refErr);
                }
                // 各点图层的错误统一汇总进 allErrs，最后合并写一份 errors_point.shp
                // （每条错误带"图层名"字段，标明来自哪个图层）
                for (auto* t : topoPtLayers) {
                    Mission454::execute(t, refLineShp, refPolyShp, totalMode,
                                        m_thresholds, allErrs, executed);
                }
                break;
            }
            case 455: {
                // 线拓扑检查：每个线图层单独一次引擎调用（注记类除外）；
                // 参考图层 = 全部点图层合并 + 全部面图层合并
                if (topoLineLayers.isEmpty()) {
                    executed.append(QStringLiteral("线拓扑检查：无线要素图层，本次不涉及"));
                    break;
                }
                QString refPointShp, refPolyShp, refResidentShp;
                QTemporaryDir refWork;
                if (refWork.isValid()) {
                    QString refErr;
                    refPointShp = WujiMissionRunner::mergeLayersShp(
                        topoPtLayers, refWork.path(), QStringLiteral("ref_points"), &refErr);
                    refPolyShp = WujiMissionRunner::mergeLayersShp(
                        topoPolyLayers, refWork.path(), QStringLiteral("ref_polys"), &refErr);
                    // 居民点参考：村/驻地类点图层（排除标注/名称/代码等派生层），
                    // 用于悬挂节点“居民连接点不算悬挂”的豁免。
                    QList<QgsVectorLayer*> residentPts;
                    for (auto* lyr : topoPtLayers) {
                        const QString base = QFileInfo(lyr->source()).completeBaseName();
                        if ((base.contains(QStringLiteral("村"))
                             || base.contains(QStringLiteral("驻地")))
                            && !base.contains(QStringLiteral("标注"))
                            && !base.contains(QStringLiteral("名称"))
                            && !base.contains(QStringLiteral("注记"))
                            && !base.contains(QStringLiteral("代码"))
                            && !base.contains(QStringLiteral("方向点"))
                            && !base.contains(QStringLiteral("选取")))
                            residentPts.append(lyr);
                    }
                    if (!residentPts.isEmpty())
                        refResidentShp = WujiMissionRunner::mergeLayersShp(
                            residentPts, refWork.path(),
                            QStringLiteral("ref_residents"), &refErr);
                }
                // 各线图层的错误统一汇总进 allErrs，最后合并写一份 errors_line.shp
                // （每条错误带"图层名"字段，标明来自哪个图层）
                for (auto* t : topoLineLayers) {
                    Mission455::execute(t, refPointShp, refPolyShp, refResidentShp,
                                        totalMode, m_thresholds, allErrs, executed);
                }
                break;
            }
            case 456: {
                // 面拓扑检查：每个面图层单独一次引擎调用（注记类除外）；
                // 参考图层 = 全部线图层合并 + 全部点图层合并
                if (topoPolyLayers.isEmpty()) {
                    executed.append(QStringLiteral("面拓扑检查：无面要素图层，本次不涉及"));
                    break;
                }
                QString refLineShp, refPointShp;
                QTemporaryDir refWork;
                if (refWork.isValid()) {
                    QString refErr;
                    refLineShp = WujiMissionRunner::mergeLayersShp(
                        topoLineLayers, refWork.path(), QStringLiteral("ref_lines"), &refErr);
                    refPointShp = WujiMissionRunner::mergeLayersShp(
                        topoPtLayers, refWork.path(), QStringLiteral("ref_points"), &refErr);
                }
                // 各面图层的错误统一汇总进 allErrs，最后合并写一份 errors_poly.shp
                // （每条错误带"图层名"字段，标明来自哪个图层）
                for (auto* t : topoPolyLayers) {
                    Mission456::execute(t, refLineShp, refPointShp, totalMode,
                                        m_thresholds, allErrs, executed);
                }
                break;
            }
            case 457: {
                // 关联表检查（本工程本地实现：需要同一数据目录内的多个图层，自动探测关联字段）
                // 注：高版本的 457 是 relation.db 关联表检查，本工程未采用（配置中该组已整组注释），
                //     故此处仍走本工程的 4 参数接口。若将来恢复关联表检查，需同步替换
                //     se_mission457_check.h/.cpp 并改回 5 参数调用。
                Mission457::execute(dataDir, totalMode, allErrs, executed);
                break;
            }
            case 458: {
                // 综合前后匹配：需要原始数据+成果数据两个目录；仅对成果数据执行一次
                // （原始数据侧任务跳过，避免同一次检查被跑两遍、异常重复）
                if (m_qstrResultDataPath.isEmpty()
                    || QDir::cleanPath(dataDir) != QDir::cleanPath(m_qstrResultDataPath)) {
                    executed.append(m_qstrResultDataPath.isEmpty()
                        ? QStringLiteral("综合前后匹配：需同时提供原始数据与成果数据，本次未执行")
                        : QStringLiteral("综合前后匹配：本次仅对成果数据执行"));
                    break;
                }
                if (m_qstrOrigDataPath.isEmpty()) {
                    executed.append("综合前后匹配：需同时提供原始数据与成果数据，本次未执行");
                    break;
                }
                {
                    // 将 QList<LayerMappingItem> 转为 QHash<QString,QString>：stdName → sourceCode
                    QHash<QString, QString> layerHashMap;
                    for (const auto& item : m_layerMappingItems) {
                        if (!item.sourceCode.isEmpty())
                            layerHashMap[item.stdName] = item.sourceCode;
                    }
                    const QString matchParamPath =
                        QFileInfo(m_qstrMissionXmlPath).absolutePath() + QStringLiteral("/matchParameter.xml");
                    if (!layerHashMap.isEmpty()) {
                        Mission458::executeWithMapping(m_qstrOrigDataPath, m_qstrResultDataPath,
                            totalMode, layerHashMap, allErrs, executed, matchParamPath);
                    } else {
                        Mission458::execute(m_qstrOrigDataPath, m_qstrResultDataPath,
                            totalMode, allErrs, executed, nullptr, matchParamPath);
                    }
                }
                break;
            }
            case 459: {
                // 图形规范性检查：纯本地 QGIS 实现，面图层跑面适用项、线图层跑线适用项，
                // 模式按 CheckItem 的 applyTo 过滤，覆盖全部面/线图层
                // （此前只查每类第一个图层，其余图层被漏掉）
                auto modeFor = [&](QgsVectorLayer* lyr) -> int {
                    if (!lyr) return 0;
                    const QString geoType =
                        (lyr->geometryType() == QgsWkbTypes::PolygonGeometry)
                        ? QStringLiteral("polygon") : QStringLiteral("line");
                    int m = 0;
                    for (const auto& item : group.items) {
                        if (!(item.checkbox && item.checkbox->isChecked() && item.implemented))
                            continue;
                        if (item.applyTo.contains(QStringLiteral("all"))
                            || item.applyTo.contains(geoType))
                            m |= item.mode;
                    }
                    return m;
                };
                for (auto* lyr : allPolyLayers) {
                    const int m = modeFor(lyr);
                    if (m) Mission459::execute(lyr, m, m_thresholds, allErrs, executed);
                }
                for (auto* lyr : allLineLayers) {
                    const int m = modeFor(lyr);
                    if (m) Mission459::execute(lyr, m, m_thresholds, allErrs, executed);
                }
                break;
            }
            default:
                break;
            }

            if (group.id == 460)
                cks.append(QString("M460 综合前后拓扑检查：%1").arg(executed.join(" | ")));
            else
                cks.append(QString("M%1: %2").arg(group.id).arg(executed.join(", ")));

            // ---- 写入结果（SHP 或日志） ----
            if (!allErrs.isEmpty()) {
                if (group.id == 453) {
                    // 属性检查：错误是表结构级别，不是空间错误，直接输出详情到日志
                    // 按要素去重，合并相同错误信息
                    QMap<QString, QList<qlonglong>> errGroups;
                    for (const auto& pair : allErrs) {
                        QString msg = pair.second.trimmed();
                        if (msg.isEmpty()) continue;
                        errGroups[msg].append(pair.first.id());
                    }
                    errs.append(QString("M453 检测到 %1 处异常（属性/字段检查，非空间错误）：").arg(allErrs.size()));
                    for (auto it = errGroups.constBegin(); it != errGroups.constEnd(); ++it) {
                        if (it.value().size() > 1)
                            errs.append(QString("  %1（%2个图层）").arg(it.key()).arg(it.value().size()));
                        else
                            errs.append(QString("  %1").arg(it.key()));
                    }
                } else {
                    QString shpBase;
                    QgsFields fieldsForShp;
                    if (group.id == 454)      { shpBase = "errors_point"; fieldsForShp = ptFields; }
                    else if (group.id == 455) { shpBase = "errors_line"; fieldsForShp = lineFields; }
                    else if (group.id == 456) { shpBase = "errors_poly"; fieldsForShp = polyFields; }
                    else if (group.id == 457) { shpBase = "errors_assoc"; fieldsForShp = (polyLayer ? polyFields : (lineLayer ? lineFields : ptFields)); }
                    else if (group.id == 458) { shpBase = "errors_match"; fieldsForShp = (polyLayer ? polyFields : (lineLayer ? lineFields : ptFields)); }
                    else if (group.id == 459) { shpBase = "errors_geom"; fieldsForShp = (polyLayer ? polyFields : lineFields); }

                    if (!shpBase.isEmpty()) {
                        QStringList files = writeErrorsToShp(m_qstrOutputDir, shpBase,
                            allErrs, crs, fieldsForShp);
                        allOutputFiles.append(files);
                    }
                    errs.append(QString("M%1 检测到 %2 处异常").arg(group.id).arg(allErrs.size()));
                }
            }
        }

        if (allOutputFiles.isEmpty()) {
            errs.append("质检完成，未检测到异常");
        } else {
            for (const auto& fo : allOutputFiles)
                errs.append("输出: " + fo);
        }

        // 清理图层
        for (auto* lyr : allPolyLayers) delete lyr;
        for (auto* lyr : allLineLayers) delete lyr;
        for (auto* lyr : allPtLayers)   delete lyr;

        // 汇总到统一日志（所有数据源合并为一份"综合成果质检"日志）
        combinedErrs.append(QString("======== %1 ========").arg(tasks[i].label));
        combinedErrs.append(errs);
        combinedCks.append(QString("======== %1 ========").arg(tasks[i].label));
        combinedCks.append(cks);
        combinedOutputs.append(allOutputFiles);
    }

    // 写统一的 JSON 日志：不再按数据源分别输出，合并为一份"综合成果质检"日志
    if (origIsGdb)
        combinedCks.append(QStringLiteral("原始数据为FileGDB(综合前)格式，作为综合前后匹配的对照数据源参与检查"));
    QString logPath = m_qstrOutputDir + "/综合成果质检_质检日志_"
        + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".json";
    allLogPaths.append(logPath);
    writeJsonLog(logPath, QStringLiteral("综合成果质检"), 0, combinedErrs, combinedCks, combinedOutputs);

    ui.progressBar->setValue(100);
    ui.label_Status->setText("状态：质检完成");

    QString msg = QString("质检完成！共处理 %1 个数据源。\n\n日志输出：\n%2\n\n输出文件：\n%3")
        .arg(n)
        .arg(allLogPaths.join("\n"))
        .arg(m_qstrOutputDir);
    QMessageBox::information(this, "质检完成", msg);
}

// ================================================================
//  JSON 日志
// ================================================================
void CSE_AutoQualityCheckDialog::writeJsonLog(const QString& logPath, const QString& dataType,
    int, const QStringList& errors, const QStringList& checks, const QStringList& outputFiles)
{
    QJsonObject root;
    root["dataType"]    = dataType;
    root["timestamp"]   = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    root["engine"]      = "QGIS 本地检查 + 引擎子进程(MapBatchProcessing.exe)";
    if (dataType.contains("综合")) {
        // 合并日志：原始数据 + 成果数据两个输入都记录
        QStringList paths;
        if (!m_qstrOrigDataPath.isEmpty())   paths << m_qstrOrigDataPath;
        if (!m_qstrResultDataPath.isEmpty()) paths << m_qstrResultDataPath;
        root["inputPaths"] = paths.join(" ; ");
    } else {
        root["inputPaths"] = (dataType.contains("原始") ? m_qstrOrigDataPath : m_qstrResultDataPath);
    }
    root["outputPath"]  = m_qstrOutputDir;

    // 记录引擎输出文件（拷贝到输出目录的SHP）
    QJsonArray outputArr;
    for (const auto& fn : outputFiles)
        outputArr.append(QString(m_qstrOutputDir + "/" + fn));
    root["outputFiles"] = outputArr;
    root["missionXml"]  = m_qstrMissionXmlPath;

    // 记录阈值
    QJsonObject thr;
    for (auto it = m_thresholds.begin(); it != m_thresholds.end(); ++it)
        thr[it.key()] = it.value();
    root["thresholds"] = thr;

    // 记录勾选的检查项
    QJsonObject missionsObj;
    for (const auto& group : m_missionGroups) {
        QJsonArray checkedItems;
        for (const auto& item : group.items) {
            if (item.checkbox && item.checkbox->isChecked() && item.implemented) {
                QJsonObject ci;
                ci["name"] = item.name;
                ci["mode"] = item.mode;
                checkedItems.append(ci);
            }
        }
        if (!checkedItems.isEmpty())
            missionsObj[QString("M%1_%2").arg(group.id).arg(group.name)] = checkedItems;
    }
    root["enabledMissions"] = missionsObj;

    QJsonArray ca; for (const auto& c : checks) ca.append(c); root["checks"] = ca;
    QJsonArray ea; for (const auto& e : errors) ea.append(e); root["errors"] = ea;

    QFile f(logPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }
}

void CSE_AutoQualityCheckDialog::onClose()
{
    reject();
}
