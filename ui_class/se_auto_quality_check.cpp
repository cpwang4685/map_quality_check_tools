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
#include <QHBoxLayout>
#include <QLabel>
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

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

#include "se_auto_quality_check.h"

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
}

// ================================================================
//  首次显示把窗口收敛到屏幕可用区域（跨分辨率适配）
//  检查项超高部分已由 buildMissionUI 的每页 QScrollArea 滚动兜底，
//  这里再把整窗限制在当前屏幕可用范围内，避免低分辨率/高DPI下
//  窗口越出屏幕导致阈值、输出区与底部按钮不可见。
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
        const int maxW = qMax(200, avail.width());
        const int maxH = qMax(200, avail.height());
        // 仅当屏幕放不下时才收敛（绝不在大屏上缩小用户已调好的窗口）
        if (minimumWidth()  > maxW) setMinimumWidth(maxW);
        if (minimumHeight() > maxH) setMinimumHeight(maxH);
        if (width()  > maxW) resize(maxW, height());
        if (height() > maxH) resize(width(), maxH);
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
    QgsWkbTypes::Type gType = errors.first().first.geometry().wkbType();

    // ===== 1. 错误要素图层 =====
    {
        QString featShp = outputDir + "/" + name + ".shp";
        QgsVectorFileWriter::SaveVectorOptions opts;
        opts.driverName = "ESRI Shapefile";
        opts.fileEncoding = "UTF-8";

        std::unique_ptr<QgsVectorFileWriter> featWriter(
            QgsVectorFileWriter::create(featShp, featFields, gType, crs,
                QgsCoordinateTransformContext(), opts));
        if (!featWriter || featWriter->hasError() != QgsVectorFileWriter::NoError) {
            qWarning() << "[ShpWrite] 创建失败:" << featShp
                << (featWriter ? featWriter->errorMessage() : "null");
        } else {
            for (const auto& pair : errors) {
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
            outFiles.append(name + ".shp");
        }
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
                QgsGeometry ptGeom = pair.first.geometry().centroid();
                if (ptGeom.isNull())
                    ptGeom = QgsGeometry::fromPointXY(QgsPointXY(0, 0));
                QgsFeature ptFeat(ptFields);
                ptFeat.setGeometry(ptGeom);
                ptFeat.setAttribute(0, (qlonglong)pair.first.id());
                ptFeat.setAttribute(1, pair.second);
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
    if (path.isEmpty()) path = getDefaultLayerMappingCsvPath();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // CSV不存在时使用内置默认（仅标准图层名，无实际SHP映射）
        m_layerMappingItems = getDefaultStandardLayers();
        return;
    }

    QTextStream ts(&file);
    ts.setCodec("UTF-8");

    // 读取首行判断格式
    QString firstLine;
    while (!ts.atEnd()) {
        firstLine = ts.readLine().trimmed();
        if (!firstLine.isEmpty() && !firstLine.startsWith("#")) break;
    }
    if (firstLine.isEmpty()) {
        file.close();
        m_layerMappingItems = getDefaultStandardLayers();
        return;
    }

    QStringList firstParts = firstLine.split(",");
    // 判断格式：
    // 新格式：源图层名称,源图层代码,标准成果图层,实际数据SHP,几何类型,备注
    // 旧格式：源图层代码,源图层名称,成果图层名称,说明
    bool isOldFormat = false;
    if (firstParts.size() >= 2) {
        QString col0 = firstParts[0].trimmed();
        // 新格式首列是源图层名称（含中文描述），旧格式首列为源图层代码（短、大写、无中文）
        if (col0.length() <= 12 && !col0.contains(QRegularExpression("[\\x4e00-\\x9fff]"))) {
            isOldFormat = true;
        }
    }

    // 解析首行
    {
        LayerMappingItem item;
        if (isOldFormat) {
            item.sourceCode = firstParts.value(0).trimmed();
            item.sourceDesc = firstParts.value(1).trimmed();
            item.stdName    = firstParts.value(2).trimmed();
            item.note       = firstParts.value(3).trimmed();
        } else {
            item.sourceDesc = firstParts.value(0).trimmed();
            item.sourceCode = firstParts.value(1).trimmed();
            item.stdName    = firstParts.value(2).trimmed();
            item.actualShp  = firstParts.value(3).trimmed();
            item.geomType   = firstParts.value(4).trimmed();
            item.note       = firstParts.value(5).trimmed();
        }
        if (!item.stdName.isEmpty())
            m_layerMappingItems.append(item);
    }

    // 解析剩余行
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith("#")) continue;

        QStringList parts = line.split(",");
        if (parts.size() < 1 || parts[0].trimmed().isEmpty()) continue;

        LayerMappingItem item;
        if (isOldFormat) {
            item.sourceCode = parts.value(0).trimmed();
            item.sourceDesc = parts.value(1).trimmed();
            item.stdName    = parts.value(2).trimmed();
            item.note       = parts.value(3).trimmed();
        } else {
            item.sourceDesc = parts.value(0).trimmed();
            item.sourceCode = parts.value(1).trimmed();
            item.stdName    = parts.value(2).trimmed();
            item.actualShp  = parts.value(3).trimmed();
            item.geomType   = parts.value(4).trimmed();
            item.note       = parts.value(5).trimmed();
        }

        if (!item.stdName.isEmpty())
            m_layerMappingItems.append(item);
    }
    file.close();
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
                QTextStream ts(&file);
                ts.setCodec("UTF-8");
                ts << "# 源图层名称,源图层代码,标准成果图层,实际数据SHP,几何类型,备注\n";
                for (const auto& item : m_layerMappingItems) {
                    ts << item.sourceDesc << ","
                       << item.sourceCode << ","
                       << item.stdName << ","
                       << item.actualShp << ","
                       << item.geomType << ","
                       << item.note << "\n";
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
        ui.spinBox_Scale->setValue(static_cast<int>(readDouble("Scale", 10000)));
        ui.doubleSpinBox_Tolerance->setValue(readDouble("FuzzyTolerance", 0.001));
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
            item.implemented  = (ci.attribute("status") != "pending") && group.implemented;
            item.applyTo      = ci.attribute("applyTo");
            item.thresholdKey = ci.attribute("threshold");
            item.note         = ci.attribute("note");
            group.items.append(item);
        }
        m_missionGroups.append(group);
    }

    // 解析图层映射：如果在XML中有<LayerMapping>，更新m_layerMappingItems
    QDomElement layerMapping = root.firstChildElement("LayerMapping");
    if (!layerMapping.isNull()) {
        QDomNodeList maps = layerMapping.elementsByTagName("Map");
        for (int i = 0; i < maps.count(); i++) {
            QDomElement m = maps.at(i).toElement();
            QString stdName = m.attribute("standard").trimmed();
            QString gdbCode = m.attribute("gdb").trimmed();
            if (!stdName.isEmpty() && !gdbCode.isEmpty() && gdbCode != QString::fromUtf8("\xe2\x80\x94")) {
                // 在列表中查找或追加
                bool found = false;
                for (auto& item : m_layerMappingItems) {
                    if (item.stdName == stdName) {
                        item.sourceCode = gdbCode;
                        found = true;
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
    applyThresholdsToSpinBoxes();

    // 按Mission定义结构体数组
    struct { int id; QString name; QString cat; bool impl; QVector<QPair<QString,int>> items; } defs[] = {
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
            item.enabled = d.impl;  // 已实现的默认启用
            item.implemented = d.impl;
            item.applyTo = "all";
            // 点依附性规则（点被线端点覆盖/点被线覆盖/点在面边界上）仅对
            // 桥梁、界桩等依附性点适用，默认不勾选，避免对普通点图层大面积误报
            if (d.id == 454 && (it.second == 4 || it.second == 8 || it.second == 32))
                item.enabled = false;
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

	// ====== 水平 Tab 控件 ======
	QTabWidget* tabWidget = new QTabWidget(ui.groupBox_Missions);
	tabWidget->setTabPosition(QTabWidget::North);
	tabWidget->setMinimumHeight(180);
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

	        for (int j = 0; j < group.items.size(); j++) {
	            auto& item = group.items[j];
	            QCheckBox* cb = new QCheckBox(listHost);
	            QString cbText = item.name;
	            if (!item.implemented)
	            cbText += " (待开发)";
	            cb->setText(cbText);
	            cb->setChecked(item.enabled && item.implemented);
	            cb->setEnabled(item.implemented);
	            if (!item.note.isEmpty())
	            cb->setToolTip(item.note);
	            cb->setStyleSheet("QCheckBox { padding: 2px 6px; }");
	            
	            item.checkbox = cb;
	            gl->addWidget(cb, j / cols, j % cols);
	        }

	        listLayout->addLayout(gl);
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

	    ui.missionsLayout->addWidget(tabWidget);
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
            cks.append("图层归类说明：未配置图层映射，按名称关键词自动分类");
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
                QString refPointShp, refPolyShp;
                QTemporaryDir refWork;
                if (refWork.isValid()) {
                    QString refErr;
                    refPointShp = WujiMissionRunner::mergeLayersShp(
                        topoPtLayers, refWork.path(), QStringLiteral("ref_points"), &refErr);
                    refPolyShp = WujiMissionRunner::mergeLayersShp(
                        topoPolyLayers, refWork.path(), QStringLiteral("ref_polys"), &refErr);
                }
                // 各线图层的错误统一汇总进 allErrs，最后合并写一份 errors_line.shp
                // （每条错误带"图层名"字段，标明来自哪个图层）
                for (auto* t : topoLineLayers) {
                    Mission455::execute(t, refPointShp, refPolyShp, totalMode,
                                        m_thresholds, allErrs, executed);
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
                // 综合前后匹配：需要原始数据+成果数据两个目录
                if (!m_qstrOrigDataPath.isEmpty() && !m_qstrResultDataPath.isEmpty()) {
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
                } else {
                    executed.append("综合前后匹配：需同时提供原始数据与成果数据，本次未执行");
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
