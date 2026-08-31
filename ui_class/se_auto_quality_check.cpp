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
#include <QProcess>
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
#include <QTabWidget>
#include <QMap>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QRegularExpression>
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

    // ---- QProcess ----
    m_chkProcess = new QProcess(this);

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
    HMODULE hm = GetModuleHandleA("map_quality_check_tools.dll");
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
static QString findFirstShpName(const QString& dirPath, const QString& suffix = "")
{
    QDir dir(dirPath);
    QStringList filters;
    if (suffix.isEmpty())
        filters << "*.shp";
    else
        filters << ("*" + suffix + ".shp");
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
    return files.isEmpty() ? QString() : files.first();
}

QStringList CSE_AutoQualityCheckDialog::scanShpFiles(const QString& dirPath)
{
    QDir dir(dirPath);
    return dir.entryList({"*.shp"}, QDir::Files, QDir::Name);
}

// ---- 将错误列表写入 SHP 文件 ----
// 生成两个文件：
//   {outputDir}/{name}.shp    — 错误要素副本 + "error"字段
//   {outputDir}/{name}_pt.shp — 错误位置点标记
static QStringList writeErrorsToShp(const QString& outputDir, const QString& name,
    const QList<QPair<QgsFeature, QString>>& errors,
    const QgsCoordinateReferenceSystem& crs,
    const QgsFields& layerFields)
{
    QStringList outFiles;
    if (errors.isEmpty()) return outFiles;

    // ---- 用图层字段 + 追加 error 字段 ----
    QgsFields featFields(layerFields);
    int errIdx = featFields.indexOf("error");
    if (errIdx < 0) {
        featFields.append(QgsField("error", QVariant::String, "String", 254));
        errIdx = featFields.size() - 1;
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
            {"枚举字段检查",16},{"约束条件检查",32},{"唯一约束检查",128},{"非空约束检查",256},
            {"约束类型检查",512},{"约束组合检查",1024}
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
	        pageLayout->setContentsMargins(8, 8, 8, 8);

	        QGridLayout* gl = new QGridLayout();
	        gl->setSpacing(6);

	        for (int j = 0; j < group.items.size(); j++) {
	            auto& item = group.items[j];
	            QCheckBox* cb = new QCheckBox(page);
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

	        pageLayout->addLayout(gl);
	        pageLayout->addStretch();

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

// ================================================================
//  生成完整质检 Mission XML（453-459 全部生成）
// ================================================================
QString CSE_AutoQualityCheckDialog::generateMissionXml(const QString& dataDir, const QString& dataLabel)
{
    Q_UNUSED(dataLabel)
    collectThresholdsFromSpinBoxes();

    QString polyShp = findFirstShpName(dataDir, "_A");
    if (polyShp.isEmpty()) polyShp = findFirstShpName(dataDir);
    QString lineShp = findFirstShpName(dataDir, "_L");
    QString ptShp  = findFirstShpName(dataDir, "_P");

    double scale       = ui.spinBox_Scale->value();
    double tolerance   = ui.doubleSpinBox_Tolerance->value();
    double acuteAngle  = ui.doubleSpinBox_AcuteAngle->value();
    double sliverArea  = ui.doubleSpinBox_SliverArea->value();
    double narrowWidth = ui.doubleSpinBox_NarrowWidth->value();
    double minNodeDist = ui.doubleSpinBox_MinNodeDist->value();

    QString xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MapGeneBatchProcessing>\n";
    xml += "<RelativePath>" + dataDir + "</RelativePath>\n";
    xml += "<WriteToConsole>true</WriteToConsole>\n";

    for (const auto& group : m_missionGroups) {
        int totalMode = 0;
        for (const auto& item : group.items) {
            if (item.checkbox && item.checkbox->isChecked() && item.implemented)
                totalMode |= item.mode;
        }
        if (totalMode == 0) continue;

        switch (group.id) {
        case 453:
            if (!polyShp.isEmpty()) {
                xml += "<Mission id=\"453\" note=\"属性表检查\">\n<ParaIn>\n";
                xml += "<Layers note=\"SourceDataStore\">\n<FilePath>"+polyShp+"</FilePath>\n</Layers>\n";
                xml += "<Parameter>\n";
                xml += QString("<ProcessMode note=\"1-Name 2-DataType 4-Length 8-Precision 16-Ignore 32-PrimaryKey 128-Unique 256-NotNULL 512-ConstraintType 1024-ConstraintSet\">%1</ProcessMode>\n").arg(totalMode);
                xml += "<Encoding>Encoding_ASCII</Encoding>\n</Parameter>\n</ParaIn>\n";
                xml += "<ParaOut>\n<Layers note=\"DstDataStores\">\n<FilePath editable=\"true\">errors_attr.shp</FilePath>\n</Layers>\n";
                xml += "<Layers note=\"AllErrorDataStore\">\n<FilePath editable=\"true\">errors_attr_pt.shp</FilePath>\n</Layers>\n</ParaOut>\n</Mission>\n";
            }
            break;
        case 454:
            if (!ptShp.isEmpty()) {
                xml += "<Mission id=\"454\" note=\"点拓扑规则\">\n<ParaIn>\n";
                xml += "<Layers note=\"PointDataStore\">\n<FilePath>"+ptShp+"</FilePath>\n</Layers>\n";
                if (!lineShp.isEmpty()) xml += "<Layers note=\"LineDataStore\">\n<FilePath>"+lineShp+"</FilePath>\n</Layers>\n";
                if (!polyShp.isEmpty()) xml += "<Layers note=\"PolygonDataStore\">\n<FilePath>"+polyShp+"</FilePath>\n</Layers>\n";
                xml += "<Parameter>\n";
                xml += QString("<ProcessMode note=\"1-Coincident 2-Disjoint 4-CoveredByEndpoint 8-CoveredByLine 16-Inside 32-OnBoundary\">%1</ProcessMode>\n").arg(totalMode);
                xml += QString("<FuzzyTolerance>%1</FuzzyTolerance>\n").arg(tolerance, 0, 'f', 6);
                xml += "<IsGeographic>false</IsGeographic>\n</Parameter>\n</ParaIn>\n";
                xml += "<ParaOut>\n<Layers note=\"PointDataStore\">\n<FilePath editable=\"true\">errors_point.shp</FilePath>\n</Layers>\n</ParaOut>\n</Mission>\n";
            }
            break;
        case 455:
            if (!lineShp.isEmpty()) {
                xml += "<Mission id=\"455\" note=\"线拓扑规则\">\n<ParaIn>\n";
                xml += "<Layers note=\"LineDataStore\">\n<FilePath>"+lineShp+"</FilePath>\n</Layers>\n";
                if (!ptShp.isEmpty()) xml += "<Layers note=\"PointDataStore\">\n<FilePath>"+ptShp+"</FilePath>\n</Layers>\n";
                if (!polyShp.isEmpty()) xml += "<Layers note=\"PolygonDataStore\">\n<FilePath>"+polyShp+"</FilePath>\n</Layers>\n";
                xml += "<Parameter>\n";
                xml += QString("<ProcessMode note=\"1-Dangles 2-Pseudos 4-SelfOverlap 8-SelfIntersect 16-SinglePart 32-Overlap 64-Intersect 4096-Inside\">%1</ProcessMode>\n").arg(totalMode);
                xml += QString("<FuzzyTolerance>%1</FuzzyTolerance>\n").arg(tolerance, 0, 'f', 6);
                xml += "<IsGeographic>false</IsGeographic>\n</Parameter>\n</ParaIn>\n";
                xml += "<ParaOut>\n<Layers note=\"LineDataStore\">\n<FilePath editable=\"true\">errors_line.shp</FilePath>\n</Layers>\n";
                xml += "<Layers note=\"PointDataStore\">\n<FilePath editable=\"true\">errors_line_pt.shp</FilePath>\n</Layers>\n</ParaOut>\n</Mission>\n";
            }
            break;
        case 456:
            if (!polyShp.isEmpty()) {
                xml += "<Mission id=\"456\" note=\"面拓扑规则\">\n<ParaIn>\n";
                xml += "<Layers note=\"PolygonDataStore\">\n<FilePath>"+polyShp+"</FilePath>\n</Layers>\n";
                if (!lineShp.isEmpty()) xml += "<Layers note=\"LineDataStore\">\n<FilePath>"+lineShp+"</FilePath>\n</Layers>\n";
                if (!ptShp.isEmpty()) xml += "<Layers note=\"PointDataStore\">\n<FilePath>"+ptShp+"</FilePath>\n</Layers>\n";
                xml += "<Parameter>\n";
                xml += QString("<ProcessMode note=\"1-Overlap 2-Gaps 4-ContainsPoint 32-BoundaryCoveredByLine 1024-LargerThanTolerance\">%1</ProcessMode>\n").arg(totalMode);
                xml += QString("<FuzzyTolerance>%1</FuzzyTolerance>\n").arg(tolerance, 0, 'f', 6);
                xml += "<IsGeographic>false</IsGeographic>\n</Parameter>\n</ParaIn>\n";
                xml += "<ParaOut>\n<Layers note=\"PolygonDataStore\">\n<FilePath editable=\"true\">errors_poly.shp</FilePath>\n</Layers>\n";
                xml += "<Layers note=\"LineDataStore\">\n<FilePath editable=\"true\">errors_poly_line.shp</FilePath>\n</Layers>\n";
                xml += "<Layers note=\"PointDataStore\">\n<FilePath editable=\"true\">errors_poly_pt.shp</FilePath>\n</Layers>\n</ParaOut>\n</Mission>\n";
            }
            break;
        case 457: {
            // 关联表检查：需要原始数据+成果数据+图层映射+关联表
            // 从图层映射中获取有GUID字段的图层对
            if (!m_layerMappingItems.isEmpty()) {
                xml += "<Mission id=\"457\" note=\"关联表检查\">\n<ParaIn>\n";

                // SourceDataStore: 原始1w数据（从图层映射的源图层代码推断）
                xml += "<Layers note=\"SourceDataStore\" note2=\"原始1w数据\">\n";
                for (const auto& item : m_layerMappingItems) {
                    if (item.actualShp.isEmpty() || item.sourceCode.isEmpty()) continue;
                    // 查找原始数据目录中的对应SHP
                    QString origShp = item.sourceCode + ".shp";
                    xml += QString("<FilePath GUIDFieldName=\"ELEMID\">%1</FilePath>\n").arg(origShp);
                }
                xml += "</Layers>\n";

                // ResDataStore: 结果5w数据（实际的综合后图层）
                xml += "<Layers note=\"ResDataStore\" note2=\"结果5w数据\">\n";
                for (const auto& item : m_layerMappingItems) {
                    if (item.actualShp.isEmpty()) continue;
                    xml += QString("<FilePath GUIDFieldName=\"ELEMID\">%1</FilePath>\n").arg(item.actualShp);
                }
                xml += "</Layers>\n";

                xml += "<Parameter>\n";
                xml += QString("<RelationTablePath note=\"关联表路径\">relation.db;Relation_1w_5w</RelationTablePath>\n");
                xml += QString("<ProcessMode note=\"1-GUID正确性 2-5wGUID在关联表中 4-1wGUID在数据中 8-5wGUID在数据中 16-注记GUID不变\">%1</ProcessMode>\n").arg(totalMode);
                xml += "<GUIDFieldName note=\"输入数据GUID字段名\">ELEMID</GUIDFieldName>\n";
                xml += "<InfoFieldName note=\"输出信息字段名\">info_NM</InfoFieldName>\n";
                xml += "</Parameter>\n</ParaIn>\n";

                xml += "<ParaOut>\n<Layers note=\"DstDataStores\">\n";
                xml += "<FilePath editable=\"true\">errors_assoc.shp</FilePath>\n</Layers>\n";
                xml += "</ParaOut>\n</Mission>\n";
            }
            break;
        }
        case 458: {
            // 综合前后缓冲匹配检查
            if (!m_layerMappingItems.isEmpty()) {
                xml += "<Mission id=\"458\" note=\"综合前后缓冲匹配检查\">\n<ParaIn>\n";

                // AfterDataStores: 综合后数据
                xml += "<Layers note=\"AfterDataStores\">\n";
                for (const auto& item : m_layerMappingItems) {
                    if (item.actualShp.isEmpty()) continue;
                    xml += QString("<FilePath bufferDis=\"%1\">%2</FilePath>\n")
                        .arg(m_thresholds.value("BufferDis", 0.0), 0, 'f', 1)
                        .arg(item.actualShp);
                }
                xml += "</Layers>\n";

                // BeforeDataStores: 综合前数据（源图层代码）
                xml += "<Layers note=\"BeforeDataStores\">\n";
                QStringList seenCodes;
                for (const auto& item : m_layerMappingItems) {
                    if (item.sourceCode.isEmpty() || seenCodes.contains(item.sourceCode)) continue;
                    seenCodes.append(item.sourceCode);
                    xml += QString("<FilePath>%1.shp</FilePath>\n").arg(item.sourceCode);
                }
                xml += "</Layers>\n";

                double bufferDis = m_thresholds.value("BufferDis", 0.0);
                double areaRatio = m_thresholds.value("AreaRatio", 0.5);
                double lengthRatio = m_thresholds.value("LengthRatio", 0.5);

                xml += "<Parameter>\n";
                xml += "<IsGeographic note=\"是否为经纬度坐标\">false</IsGeographic>\n";
                xml += QString("<Scale note=\"比例尺\">%1</Scale>\n").arg(static_cast<int>(scale));
                xml += QString("<BufferDis note=\"缓冲阈值\">%1</BufferDis>\n").arg(bufferDis, 0, 'f', 1);
                xml += "<IdentityField note=\"匹配数据读取字段名称\">ELEMID</IdentityField>\n";
                xml += QString("<ProcessMode note=\"匹配模式\">%1</ProcessMode>\n").arg(totalMode);
                xml += "<MatchStyle note=\"匹配风格\">0</MatchStyle>\n";
                xml += QString("<AreaRatio note=\"面积比例\">%1</AreaRatio>\n").arg(areaRatio, 0, 'f', 2);
                xml += QString("<LengthRatio note=\"长度比例\">%1</LengthRatio>\n").arg(lengthRatio, 0, 'f', 2);
                xml += "<SelfAreaRatio note=\"自身面积比例\">0</SelfAreaRatio>\n";
                xml += "<SelfLengthRatio note=\"自身长度比例\">0</SelfLengthRatio>\n";
                xml += "<AssociationType note=\"关联规则\">3</AssociationType>\n";
                xml += "<AbsoluteValue note=\"是否是绝对数值\">false</AbsoluteValue>\n";
                xml += "<BufferGeoProcessSelfIntersect note=\"缓冲几何是否处理自相交\">true</BufferGeoProcessSelfIntersect>\n";
                xml += "<MultiGeoToSingle note=\"是否将多几何转换为单几何\">true</MultiGeoToSingle>\n";
                xml += QString("<FuzzyTolerance note=\"结点拟合\">%1</FuzzyTolerance>\n").arg(tolerance, 0, 'f', 6);
                xml += "<IntersectionEpsilon note=\"线段相交容差\">0.00001</IntersectionEpsilon>\n";
                xml += "<RedundancyVertexTolerance note=\"拓扑节点冗余容差\">0.001</RedundancyVertexTolerance>\n";
                xml += "<RelationTablePath note=\"关联表路径\">relation.db;Relation_1w_5w</RelationTablePath>\n";
                xml += "<MatchParameterXMLFile note=\"缓冲匹配阈值配置文件\">matchParameter.xml</MatchParameterXMLFile>\n";
                xml += "</Parameter>\n</ParaIn>\n";

                xml += "<ParaOut>\n<Layers note=\"AfterDataStores\">\n";
                xml += "<FilePath>errors_match.shp</FilePath>\n</Layers>\n";
                xml += "</ParaOut>\n</Mission>\n";
            }
            break;
        }
        case 459:
            if (!polyShp.isEmpty() || !lineShp.isEmpty()) {
                QString shp = !polyShp.isEmpty() ? polyShp : lineShp;
                xml += "<Mission id=\"459\" note=\"图形规范性检查\">\n<ParaIn>\n";
                xml += "<Layers note=\"SourceDataStore\">\n<FilePath>"+shp+"</FilePath>\n</Layers>\n";
                xml += "<Parameter>\n";
                xml += "<IsGeographic>false</IsGeographic>\n";
                xml += QString("<Scale>%1</Scale>\n").arg(static_cast<int>(scale));
                xml += QString("<FuzzyTolerance>%1</FuzzyTolerance>\n").arg(tolerance, 0, 'f', 6);
                xml += QString("<ProcessMode note=\"1-MultiPart 2-Empty 4-AcuteAngle 8-Sliver 16-Narrow 32-SmallArea 64-AvgNodeDensity 128-NodeDensity 256-SelfIntersect 512-MinNodeDistance\">%1</ProcessMode>\n").arg(totalMode);
                xml += QString("<AcuteAngle>%1</AcuteAngle>\n").arg(acuteAngle, 0, 'f', 1);
                xml += QString("<SliverArea>%1</SliverArea>\n").arg(sliverArea, 0, 'f', 2);
                xml += QString("<LongNarrowWidth>%1</LongNarrowWidth>\n").arg(narrowWidth, 0, 'f', 2);
                xml += QString("<MinNodeLength>%1</MinNodeLength>\n").arg(minNodeDist, 0, 'f', 6);
                xml += "<FlagField>error</FlagField>\n";
                xml += "</Parameter>\n</ParaIn>\n";
                xml += "<ParaOut>\n<Layers note=\"DstDataStores\">\n<FilePath editable=\"true\">errors_geom.shp</FilePath>\n</Layers>\n";
                xml += "<Layers note=\"PointDataStore\">\n<FilePath editable=\"true\">errors_geom_pt.shp</FilePath>\n</Layers>\n</ParaOut>\n</Mission>\n";
            }
            break;
        }
    }

    xml += "</MapGeneBatchProcessing>\n";
    return xml;
}

// ================================================================
//  浏览按钮
// ================================================================
void CSE_AutoQualityCheckDialog::onBrowseOrigData()
{
    QString d = QFileDialog::getExistingDirectory(this, "原始数据目录", m_qstrOrigDataPath);
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

    // 构建任务列表：原始数据 + 成果数据（各自独立）
    struct { QString path; QString label; } tasks[2];
    int n = 0;
    if (!m_qstrOrigDataPath.isEmpty())
        tasks[n++] = {m_qstrOrigDataPath, "原始数据"};
    if (!m_qstrResultDataPath.isEmpty())
        tasks[n++] = {m_qstrResultDataPath, "成果数据"};

    QStringList allLogPaths;
    collectThresholdsFromSpinBoxes();

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
        }

        // 回退：如果映射为空，扫描目录中所有SHP
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
            cks.append("注意：未配置图层映射，使用名称关键词自动分类（可能不准确）");
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

        // 为向后兼容保留首图层指针
        QgsVectorLayer* polyLayer = allPolyLayers.isEmpty() ? nullptr : allPolyLayers.first();
        QgsVectorLayer* lineLayer = allLineLayers.isEmpty() ? nullptr : allLineLayers.first();
        QgsVectorLayer* ptLayer   = allPtLayers.isEmpty()   ? nullptr : allPtLayers.first();

        if (allPolyLayers.isEmpty() && allLineLayers.isEmpty() && allPtLayers.isEmpty()) {
            errs.append("错误：数据目录中未找到有效SHP文件");
            QString logPath = m_qstrOutputDir + "/" + tasks[i].label + "_质检日志_"
                + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".json";
            allLogPaths.append(logPath);
            writeJsonLog(logPath, tasks[i].label, 0, errs, cks, allOutputFiles);
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
                // 属性检查：从XML加载字段定义（attribute_check_config.xml）
                if (!polyLayer && !lineLayer) break;
                QgsVectorLayer* lyr = polyLayer ? polyLayer : lineLayer;
                QList<FieldDefinition> fieldDefs;
                // 从mission_config.xml同目录下的attribute_check_config.xml加载字段定义
                QString attrXmlPath = QFileInfo(m_qstrMissionXmlPath).absolutePath() + "/attribute_check_config.xml";
                fieldDefs = Mission453::loadFieldDefs(attrXmlPath);
                if (fieldDefs.isEmpty()) {
                    executed.append("属性检查(跳过：未找到字段定义XML或XML为空)");
                    break;
                }
                Mission453::execute(lyr, fieldDefs, totalMode,
                    allErrs, executed);
                break;
            }
            case 454:
                Mission454::execute(ptLayer, lineLayer, polyLayer, nullptr,
                    totalMode, m_thresholds, allErrs, executed);
                break;
            case 455:
                Mission455::execute(lineLayer, ptLayer, polyLayer, nullptr,
                    totalMode, m_thresholds, allErrs, executed);
                break;
            case 456:
                Mission456::execute(polyLayer, lineLayer, ptLayer, nullptr,
                    totalMode, m_thresholds, allErrs, executed);
                break;
            case 457: {
                // 关联表检查：需要同一数据目录内的多个图层（自动探测关联字段）
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
                    if (!layerHashMap.isEmpty()) {
                        Mission458::executeWithMapping(m_qstrOrigDataPath, m_qstrResultDataPath,
                            totalMode, layerHashMap, allErrs, executed);
                    } else {
                        Mission458::execute(m_qstrOrigDataPath, m_qstrResultDataPath,
                            totalMode, allErrs, executed);
                    }
                } else {
                    executed.append("综合前后匹配(跳过：需要同时选择原始数据和成果数据两个数据源)");
                }
                break;
            }
            case 459: {
                QgsVectorLayer* lyr = polyLayer ? polyLayer : lineLayer;
                if (lyr)
                    Mission459::execute(lyr, totalMode, m_thresholds, allErrs, executed);
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
                        if (it.value().size() <= 5) {
                            QStringList ids;
                            for (auto id : it.value()) ids.append(QString::number(id));
                            errs.append(QString("  %1（涉及要素: %2）").arg(it.key(), ids.join(",")));
                        } else {
                            errs.append(QString("  %1（涉及 %2 个要素）").arg(it.key()).arg(it.value().size()));
                        }
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

        // 写 JSON 日志
        QString logPath = m_qstrOutputDir + "/" + tasks[i].label + "_质检日志_"
            + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".json";
        allLogPaths.append(logPath);
        writeJsonLog(logPath, tasks[i].label, 0, errs, cks, allOutputFiles);
    }

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
    root["engine"]      = "MapBatchProcessing.exe (无极 WJ)";
    root["inputPaths"]  = (dataType.contains("原始") ? m_qstrOrigDataPath : m_qstrResultDataPath);
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

// ================================================================
//  其他槽
// ================================================================
void CSE_AutoQualityCheckDialog::onChkProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(exitCode); Q_UNUSED(status);
    // 同步等待模式，此回调不使用
}

void CSE_AutoQualityCheckDialog::onClose()
{
    reject();
}
