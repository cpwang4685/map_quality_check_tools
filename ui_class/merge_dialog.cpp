#define _HAS_STD_BYTE 0
#include "merge_dialog.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QDateTime>
#include <QTimer>
#include <QSet>
// 麒麟 QGIS SDK 用小写头（qgssettings.h/qgsapplication.h），驼峰仅 Windows SDK 有
#include <qgssettings.h>
#include <qgsapplication.h>

#include <algorithm>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>

#include "../ui_task/se_wuji_xml_generator.h"
#include "../ui_task/se_nmo_sdk_bridge.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

#include "ui_fit_helper.h"

// ===================================================================
// 构造与析构
// ===================================================================

MergeDialog::MergeDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);
    DialogFitHelper::install(this);

    setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);

    // 小符号按钮：平台 QSS 的 QPushButton padding(7px 16px) 会把 32px 宽小按钮的
    // 内容区压缩到近 0，导致 +/×/↑/↓/... 符号被截断或完全看不见。这里在按钮自身
    // 挂局部样式（widget 级样式表不会被平台对顶层窗口的 setStyleSheet 覆盖），
    // 仅清除内边距、设定字号，平台按钮的渐变背景/边框/悬停效果保持不变。
    const QString kSymBtnStyle = QStringLiteral(
        "QPushButton { padding: 0px; font-size: 15px; }");
    ui.pushButton_browseInput->setStyleSheet(kSymBtnStyle);
    ui.pushButton_addDataset->setStyleSheet(kSymBtnStyle);
    ui.pushButton_removeDataset->setStyleSheet(kSymBtnStyle);
    ui.pushButton_moveUp->setStyleSheet(kSymBtnStyle);
    ui.pushButton_moveDown->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseOutput->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseLog->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseExe->setStyleSheet(kSymBtnStyle);

    connect(ui.pushButton_browseInput,   &QPushButton::clicked, this, &MergeDialog::browseInput);
    connect(ui.pushButton_browseOutput,  &QPushButton::clicked, this, &MergeDialog::browseOutput);
    connect(ui.pushButton_browseLog,     &QPushButton::clicked, this, &MergeDialog::browseLog);
    connect(ui.pushButton_addDataset,    &QPushButton::clicked, this, &MergeDialog::addDataset);
    connect(ui.pushButton_removeDataset, &QPushButton::clicked, this, &MergeDialog::removeDataset);
    connect(ui.pushButton_moveUp,        &QPushButton::clicked, this, &MergeDialog::moveDatasetUp);
    connect(ui.pushButton_moveDown,      &QPushButton::clicked, this, &MergeDialog::moveDatasetDown);

    connect(ui.pushButton_ok,            &QPushButton::clicked, this, &MergeDialog::onOk);
    connect(ui.pushButton_cancel,        &QPushButton::clicked, this, &QDialog::close);

    connect(ui.lineEdit_logPath,        &QLineEdit::textEdited,  this, [this] { m_bLogPathAutoFollow = false; });
    connect(ui.lineEdit_outputDataset,  &QLineEdit::textChanged, this, &MergeDialog::onOutputPathEdited);

    ui.comboBox_logLevel->setCurrentIndex(1);
    ui.label_geomWarning->hide();

    // Hide the EXE path row — we call the SDK directly, no EXE needed
    ui.label_wujiExePath->hide();
    ui.lineEdit_wujiExePath->hide();
    ui.pushButton_browseExe->hide();

    // Field combo in the param placeholder (label text switches by mode)
    QVBoxLayout* lay = new QVBoxLayout();
    lay->setContentsMargins(0, 4, 0, 4);
    m_labelField = new QLabel();
    m_comboField = new QComboBox();
    m_comboField->setEditable(true);
    lay->addWidget(m_labelField);
    lay->addWidget(m_comboField);
    ui.layout_paramPlaceholder->addLayout(lay);

    // Polygon (dissolve) advanced parameter group
    m_groupPolygon = new QGroupBox(QStringLiteral("高级参数（面溶解）"));
    QFormLayout* polyForm = new QFormLayout(m_groupPolygon);

    m_spinBufferDistance = new QDoubleSpinBox();
    m_spinBufferDistance->setRange(0.000001, 100.0);
    m_spinBufferDistance->setSingleStep(0.1);
    m_spinBufferDistance->setDecimals(6);
    m_spinBufferDistance->setValue(1.0);
    m_spinBufferDistance->setToolTip(QStringLiteral("识别缓冲误差，单位与数据坐标系一致"));
    polyForm->addRow(QStringLiteral("缓冲距离:"), m_spinBufferDistance);
    m_labelBufferMetric = new QLabel();
    m_labelBufferMetric->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    polyForm->addRow(QString(), m_labelBufferMetric);
    connect(m_spinBufferDistance, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        this, [this](double val) {
            if (m_labelBufferMetric->isVisible())
                m_labelBufferMetric->setText(QStringLiteral("%1° ≈ %2m")
                    .arg(QString::number(val, 'f', 6))
                    .arg(QString::number(val * 111000.0, 'f', 1)));
        });

    m_spinAngleEpsilon = new QDoubleSpinBox();
    m_spinAngleEpsilon->setRange(0.0, 360.0);
    m_spinAngleEpsilon->setDecimals(1);
    m_spinAngleEpsilon->setValue(181.0);
    m_spinAngleEpsilon->setToolTip(QStringLiteral("角度过滤阈值，181=不过滤"));
    polyForm->addRow(QStringLiteral("角度阈值:"), m_spinAngleEpsilon);

    m_comboNeighborStyle = new QComboBox();
    m_comboNeighborStyle->addItem(QStringLiteral("快速直接临近"), 1);
    m_comboNeighborStyle->addItem(QStringLiteral("缓冲"), 2);
    m_comboNeighborStyle->addItem(QStringLiteral("全局拓扑构面"), 4);
    m_comboNeighborStyle->addItem(QStringLiteral("全局拓扑不构面"), 8);
    m_comboNeighborStyle->setCurrentIndex(1); // default: 2-缓冲
    m_comboNeighborStyle->setToolTip(QStringLiteral("临近识别模式"));
    polyForm->addRow(QStringLiteral("临近模式:"), m_comboNeighborStyle);

    ui.layout_paramPlaceholder->addWidget(m_groupPolygon);

    // Line (connect) advanced parameter group
    m_groupLine = new QGroupBox(QStringLiteral("高级参数（线连接）"));
    QFormLayout* lineForm = new QFormLayout(m_groupLine);

    m_spinFuzzyTolerance = new QDoubleSpinBox();
    m_spinFuzzyTolerance->setRange(0.000001, 1.0);
    m_spinFuzzyTolerance->setSingleStep(0.000001);
    m_spinFuzzyTolerance->setDecimals(6);
    m_spinFuzzyTolerance->setValue(0.00005);
    m_spinFuzzyTolerance->setToolTip(QStringLiteral("结点拟合容差，单位与数据坐标系一致"));
    lineForm->addRow(QStringLiteral("结点拟合容差:"), m_spinFuzzyTolerance);
    m_labelFuzzyMetric = new QLabel();
    m_labelFuzzyMetric->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    lineForm->addRow(QString(), m_labelFuzzyMetric);
    connect(m_spinFuzzyTolerance, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        this, [this](double val) {
            if (m_labelFuzzyMetric->isVisible())
                m_labelFuzzyMetric->setText(QStringLiteral("%1° ≈ %2m")
                    .arg(QString::number(val, 'f', 6))
                    .arg(QString::number(val * 111000.0, 'f', 1)));
        });

    m_comboLinkMode = new QComboBox();
    m_comboLinkMode->addItem(QStringLiteral("同一图层内"), 1);
    m_comboLinkMode->addItem(QStringLiteral("跨图层"), 2);
    m_comboLinkMode->addItem(QStringLiteral("全部（同图层+跨图层）"), 3);
    m_comboLinkMode->setCurrentIndex(1); // default: 2-跨图层
    m_comboLinkMode->setToolTip(QStringLiteral("连接模式"));
    lineForm->addRow(QStringLiteral("连接模式:"), m_comboLinkMode);

    ui.layout_paramPlaceholder->addWidget(m_groupLine);

    restoreState();
    applyMode(ModeEmpty);
    autoUpdateLogPath();
    // 初始为空数据状态，窗口收缩到紧凑高度（加数据识别模式后再展开）
    QTimer::singleShot(0, this, [this] { resizeToContent(); });
}

MergeDialog::~MergeDialog()
{
    saveState();
}

// ===================================================================
// 模式检测：按输入几何类型分流 面→溶解(Mission 327) / 线→连接(Mission 175)
// ===================================================================

MergeDialog::GeomCat MergeDialog::geomCategory(int type)
{
    switch (type) {
        case wkbPoint: case wkbMultiPoint: return CatPoint;
        case wkbLineString: case wkbMultiLineString: return CatLine;
        case wkbPolygon: case wkbMultiPolygon: return CatPolygon;
        default: return CatUnknown;
    }
}

MergeDialog::Mode MergeDialog::detectMode()
{
    int count = ui.listWidget_datasets->count();
    if (count == 0) return ModeEmpty;

    bool hasPoint = false, hasLine = false, hasPolygon = false;
    for (int i = 0; i < count; ++i) {
        QString path = ui.listWidget_datasets->item(i)->text();
        GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
            path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poDS) continue;
        OGRLayer* poLayer = poDS->GetLayer(0);
        if (!poLayer) { GDALClose(poDS); continue; }

        int geomType = wkbFlatten(poLayer->GetGeomType());
        if (geomType == wkbUnknown) {
            poLayer->ResetReading();
            OGRFeature* poFeat;
            while ((poFeat = poLayer->GetNextFeature()) != nullptr) {
                OGRGeometry* poGeom = poFeat->GetGeometryRef();
                if (poGeom) {
                    geomType = wkbFlatten(poGeom->getGeometryType());
                    if (geomType != wkbUnknown) {
                        OGRFeature::DestroyFeature(poFeat);
                        break;
                    }
                }
                OGRFeature::DestroyFeature(poFeat);
            }
        }
        GDALClose(poDS);

        GeomCat cat = geomCategory(geomType);
        if (cat == CatPoint) hasPoint = true;
        else if (cat == CatLine) hasLine = true;
        else if (cat == CatPolygon) hasPolygon = true;
    }

    if (hasPoint) return ModePoint;
    if (hasLine && hasPolygon) return ModeMixed;
    if (hasLine) return ModeLine;
    if (hasPolygon) return ModePolygon;
    return ModeEmpty;
}

void MergeDialog::applyMode(Mode mode)
{
    m_mode = mode;

    const bool fieldVisible = (mode == ModePolygon || mode == ModeLine);
    m_labelField->setVisible(fieldVisible);
    m_comboField->setVisible(fieldVisible);
    m_groupPolygon->setVisible(mode == ModePolygon);
    m_groupLine->setVisible(mode == ModeLine);

    if (mode == ModePolygon) {
        m_labelField->setText(QStringLiteral("分组字段:"));
        m_comboField->setToolTip(QStringLiteral("值相同的碎面将合并为同一个面。\n支持多字段组合作为分组条件，用逗号分隔，如: PAC,NAME"));
        m_comboField->lineEdit()->setPlaceholderText(QStringLiteral("如: PAC,NAME"));
    } else if (mode == ModeLine) {
        m_labelField->setText(QStringLiteral("连接字段:"));
        m_comboField->setToolTip(QStringLiteral("值相同的断线将被连接为同一个实体。\n支持多字段组合，用逗号分隔，如: CODE,NAME"));
        m_comboField->lineEdit()->setPlaceholderText(QStringLiteral("如: CODE,NAME"));
    }

    QString warning;
    switch (mode) {
    case ModeEmpty:
        if (ui.listWidget_datasets->count() > 0)
            warning = QStringLiteral("⚠ 无法识别输入数据的几何类型，请检查数据");
        break;
    case ModePoint:
        warning = QStringLiteral("⚠ 暂不支持点要素，请移除点要素数据后再执行");
        break;
    case ModeMixed:
        warning = QStringLiteral("⚠ 几何类型不一致：检测到线与面要素混合，请分开处理（线走连接、面走溶解）");
        break;
    default:
        break;
    }

    if (warning.isEmpty()) {
        ui.label_geomWarning->hide();
    } else {
        ui.label_geomWarning->setText(warning);
        ui.label_geomWarning->show();
    }

    ui.pushButton_ok->setEnabled(fieldVisible);
}

void MergeDialog::refreshMode()
{
    applyMode(detectMode());
    if (m_mode == ModePolygon || m_mode == ModeLine) {
        populateFieldCombo();
        autoAdjustParameters();
    } else {
        m_comboField->clear();
        m_comboField->setCurrentIndex(-1);
    }
    resizeToContent();
}

void MergeDialog::resizeToContent()
{
    // 先把挂起的 LayoutRequest 事件处理掉，清掉布局缓存；再 invalidate+activate 强制重算，
    // 按最新总高度收放（宽度保持不变），防止隐藏控件后仍按旧高度显示。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    layout()->invalidate();
    layout()->activate();
    const int w = width();
    const int h = layout()->totalSizeHint().height();
    if (h > 0)
        resize(w, h);
}

// ===================================================================
// 浏览按钮
// ===================================================================

void MergeDialog::browseInput()
{
    QStringList paths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("请选择输入矢量文件"), m_inputPath,
        QStringLiteral("矢量文件 (*.shp *.gpkg *.gdb *.geojson)"));
    if (!paths.isEmpty()) {
        m_inputPath = QFileInfo(paths[0]).absolutePath();
        ui.lineEdit_inputDataset->setText(paths.join(QStringLiteral("; ")));
        bool fresh = (ui.listWidget_datasets->count() == 0);
        if (fresh) ui.lineEdit_outputDataset->clear();
        for (const QString& path : paths)
            ui.listWidget_datasets->addItem(path);
        refreshMode();
        if (fresh || ui.lineEdit_outputDataset->text().isEmpty())
            setDefaultOutput();
    }
}

void MergeDialog::browseOutput()
{
    QString startPath = ui.lineEdit_outputDataset->text().trimmed();
    if (startPath.isEmpty()) startPath = m_outputPath;

    QString path;
    if (m_mode == ModePolygon && ui.listWidget_datasets->count() > 1) {
        QFileInfo fi(startPath);
        QString startDir = fi.isDir() ? startPath : fi.absolutePath();
        path = QFileDialog::getExistingDirectory(this,
            QStringLiteral("请选择输出目录"), startDir);
    } else {
        path = QFileDialog::getSaveFileName(this,
            QStringLiteral("请选择输出保存位置"), startPath,
            QStringLiteral("矢量文件 (*.shp *.gpkg)"));
    }
    if (!path.isEmpty()) {
        m_outputPath = path;
        ui.lineEdit_outputDataset->setText(path);
    }
}

void MergeDialog::browseLog()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("请选择日志保存路径"), m_logPath);
    if (!dir.isEmpty()) {
        m_logPath = dir;
        m_bLogPathAutoFollow = false;
        ui.lineEdit_logPath->setText(dir);
    }
}

// ===================================================================
// 数据集列表操作
// ===================================================================

void MergeDialog::addDataset()
{
    QStringList paths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("请选择输入矢量文件"), m_inputPath,
        QStringLiteral("矢量文件 (*.shp *.gpkg *.gdb *.geojson)"));
    if (!paths.isEmpty()) {
        for (const QString& path : paths)
            ui.listWidget_datasets->addItem(path);
        refreshMode();
        if (ui.lineEdit_outputDataset->text().isEmpty())
            setDefaultOutput();
    }
}

void MergeDialog::removeDataset()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row >= 0) {
        delete ui.listWidget_datasets->takeItem(row);
        refreshMode();
    }
}

void MergeDialog::moveDatasetUp()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row > 0) {
        QListWidgetItem* item = ui.listWidget_datasets->takeItem(row);
        ui.listWidget_datasets->insertItem(row - 1, item);
        ui.listWidget_datasets->setCurrentRow(row - 1);
    }
}

void MergeDialog::moveDatasetDown()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row >= 0 && row < ui.listWidget_datasets->count() - 1) {
        QListWidgetItem* item = ui.listWidget_datasets->takeItem(row);
        ui.listWidget_datasets->insertItem(row + 1, item);
        ui.listWidget_datasets->setCurrentRow(row + 1);
    }
}

// ===================================================================
// 输出默认值
// ===================================================================

void MergeDialog::setDefaultOutput()
{
    int n = ui.listWidget_datasets->count();
    if (n == 0) return;
    QFileInfo fi(ui.listWidget_datasets->item(0)->text());
    if (n == 1) {
        QString suffix = (m_mode == ModeLine) ? QStringLiteral("_matched.shp") : QStringLiteral("_merged.shp");
        ui.lineEdit_outputDataset->setText(fi.absolutePath() + "/" + fi.completeBaseName() + suffix);
    } else {
        if (m_mode == ModeLine)
            ui.lineEdit_outputDataset->setText(fi.absolutePath() + "/matched.shp");
        else
            ui.lineEdit_outputDataset->setText(fi.absolutePath() + "/merged_output/");
    }
    m_outputPath = m_inputPath;
}

// ===================================================================
// 确定 — 校验 + 用模板 + SDK 执行（面溶解 / 线连接）
// ===================================================================

void MergeDialog::onOk()
{
    int count = ui.listWidget_datasets->count();
    if (count < 1) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("请添加至少一个输入数据集"));
        return;
    }
    if (m_mode != ModePolygon && m_mode != ModeLine) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("当前输入数据的几何类型不受支持，请检查输入数据（需全为面或全为线）"));
        return;
    }
    const bool lineMode = (m_mode == ModeLine);

    QString outPath = ui.lineEdit_outputDataset->text().trimmed();
    if (outPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("请选择输出数据集"));
        return;
    }

    QFileInfo outFi(outPath);
    QString outDir = outFi.absolutePath();
    QDir().mkpath(outDir);

    // Generate output files: N inputs → N outputs (SDK requires 1:1 mapping).
    // Polygon multi: subdir named from output basename.
    // Line multi: SDK writes to temp dir; we merge into the single outPath afterwards.
    QStringList outputPaths;
    QString tempDir;
    if (count == 1) {
        outputPaths.append(outPath);
    } else if (lineMode) {
        tempDir = outDir + QStringLiteral("/_edge_match_temp");
        QDir().mkpath(tempDir);
        for (int i = 0; i < count; ++i) {
            QString inBase = QFileInfo(ui.listWidget_datasets->item(i)->text()).completeBaseName();
            outputPaths.append(tempDir + QStringLiteral("/") + inBase + QStringLiteral("_matched.shp"));
        }
    } else {
        // Use user-specified name as subdirectory for multi-file output
        QString subDir = outDir + QStringLiteral("/") + outFi.completeBaseName();
        QDir().mkpath(subDir);
        for (int i = 0; i < count; ++i) {
            QString inBase = QFileInfo(ui.listWidget_datasets->item(i)->text()).completeBaseName();
            outputPaths.append(subDir + QStringLiteral("/") + inBase + QStringLiteral("_merged.shp"));
        }
    }

    for (int i = 0; i < count; ++i) {
        QString inAbs = QFileInfo(ui.listWidget_datasets->item(i)->text()).absoluteFilePath();
        for (const QString& op : outputPaths) {
            if (QFileInfo(op).absoluteFilePath().compare(inAbs, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("提示"),
                    QStringLiteral("输出文件不能与输入文件相同:\n%1").arg(op));
                return;
            }
        }
    }

    saveState();

    // Build template config
    SeWujiXmlGenerator::TemplateConfig cfg;
    // 跨平台模板路径解析：运行时 xml 目录（Windows <LTZK_HOME>/xml，麒麟 /opt/ltzk/xml）→ 部署目录 → 开发源码树
    cfg.templatePath = SeWujiXmlGenerator::resolveTemplatePath(lineMode
        ? QStringLiteral("edge_match_template.xml")
        : QStringLiteral("merge_template.xml"));

    QStringList inputFiles;
    for (int i = 0; i < count; ++i)
        inputFiles.append(ui.listWidget_datasets->item(i)->text());

    // dataPath = directory of first input file
    QString dataDir = QFileInfo(inputFiles[0]).absolutePath();

    // Ensure DBF field names are GBK-encoded (SDK requirement)
    QStringList gbkPaths = SeNmoSdkBridge::ensureGbkShapefiles(inputFiles, dataDir);

    // If inputs use a geographic CRS, reproject to projected CRS for
    // better SDK distance-calculation accuracy (34% → 61% merge rate).
    QString originalSrsWkt;
    QStringList projectedPaths;
    bool didProject = false;
    if (lineMode) {
        projectedPaths = SeNmoSdkBridge::ensureProjectedInputs(gbkPaths, dataDir, originalSrsWkt);
        didProject = !originalSrsWkt.isEmpty();
    }

    for (const QString& f : (lineMode ? projectedPaths : gbkPaths))
        cfg.inputFiles.append(f);
    for (const QString& op : outputPaths)
        cfg.outputFiles.append(op);

    QString field = m_comboField->currentText().trimmed();
    int parenIdx = field.indexOf(QStringLiteral(" ("));
    if (parenIdx > 0) field = field.left(parenIdx);
    cfg.entityField = field;
    cfg.dataPath = dataDir;

    if (lineMode) {
        // 地理坐标会先投影到 EPSG:3857（米），容差必须同步换算成米，
        // 否则 0.00005 会被 SDK 按米理解（≈0.05mm），等于零容差导致接不上
        cfg.fuzzyTolerance = m_spinFuzzyTolerance->value() * (didProject ? 111000.0 : 1.0);
        cfg.linkMode = m_comboLinkMode->currentData().toInt();
    } else {
        cfg.bufferDistance = m_spinBufferDistance->value();
        cfg.angleEpsilon = m_spinAngleEpsilon->value();
        cfg.neighborStyle = m_comboNeighborStyle->currentData().toInt();
    }

    QString xmlPath = SeWujiXmlGenerator::applyTemplate(cfg);
    if (xmlPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("错误"),
            QStringLiteral("无法生成任务XML文件，请检查模板是否存在:\n%1").arg(cfg.templatePath));
        return;
    }

    // Call SDK synchronously (blocks UI, but typical processing is < 30s)
    ui.progressBar->setRange(0, 0);
    ui.pushButton_ok->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();

    bool success = SeNmoSdkBridge::executeMission(xmlPath, cfg.dataPath);

    SeNmoSdkBridge::cleanupGbkTempFiles(gbkPaths);

    // Reproject output back to original CRS before verification
    if (didProject)
        SeNmoSdkBridge::reprojectOutputsToOriginal(outputPaths, originalSrsWkt);

    if (lineMode)
        SeNmoSdkBridge::cleanupProjectedTempFiles(projectedPaths);

    QApplication::restoreOverrideCursor();
    ui.progressBar->setRange(0, 100);
    ui.pushButton_ok->setEnabled(true);

    // 运行日志：写入用户设置的日志路径（GBK 编码，记事本直接可读）
    QString levelTag = QStringLiteral("Info");
    if (ui.comboBox_logLevel->currentIndex() == 0) levelTag = QStringLiteral("Error");
    else if (ui.comboBox_logLevel->currentIndex() == 2) levelTag = QStringLiteral("Debug");
    auto writeLog = [&](const QString& tail, const QStringList& finalFiles, const QStringList& tempFiles) {
        QStringList log;
        log << QStringLiteral("功能: 合并（") + (lineMode ? QStringLiteral("线连接") : QStringLiteral("面溶解")) + QStringLiteral("）");
        log << QStringLiteral("执行时间: ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        log << QStringLiteral("输入文件(%1):").arg(inputFiles.size());
        for (const QString& f : inputFiles) log << QStringLiteral("  ") + f;
        log << QStringLiteral("输出文件(%1):").arg(finalFiles.size());
        for (const QString& f : finalFiles) log << QStringLiteral("  ") + f;
        if (!tempFiles.isEmpty()) {
            log << QStringLiteral("中间临时输出(%1，已合并清理):").arg(tempFiles.size());
            for (const QString& f : tempFiles) log << QStringLiteral("  ") + f;
        }
        if (lineMode) {
            log << QStringLiteral("连接字段: ") + field;
            if (didProject)
                log << QStringLiteral("结点拟合容差: ") + QString::number(m_spinFuzzyTolerance->value())
                    + QStringLiteral("°（投影后 %1 米）").arg(QString::number(cfg.fuzzyTolerance));
            else
                log << QStringLiteral("结点拟合容差: ") + QString::number(cfg.fuzzyTolerance);
            log << QStringLiteral("连接模式: ") + QString::number(cfg.linkMode);
            if (didProject) log << QStringLiteral("投影转换: 已转EPSG:3857并转回原坐标系");
        } else {
            log << QStringLiteral("分组字段: ") + field;
            log << QStringLiteral("缓冲距离: ") + QString::number(cfg.bufferDistance);
            log << QStringLiteral("角度阈值: ") + QString::number(cfg.angleEpsilon);
            log << QStringLiteral("邻接方式: ") + QString::number(cfg.neighborStyle);
        }
        log << QStringLiteral("任务XML: ") + xmlPath;
        log << QStringLiteral("SDK执行结果: ") + (success ? QStringLiteral("成功") : QStringLiteral("失败"));
        log << tail;
        SeNmoSdkBridge::writeRunLog(m_logPath, levelTag,
            lineMode ? QStringLiteral("EdgeMatch") : QStringLiteral("Merge"), log);
    };

    if (success) {
        // Verify all output files exist
        QStringList filesToLoad;
        for (int i = 0; i < outputPaths.count(); ++i) {
            QString outFile = outputPaths[i];
            QDir().mkpath(QFileInfo(outFile).absolutePath());
            if (!QFileInfo::exists(outFile)) {
                writeLog(QStringLiteral("结果: 失败（输出文件未生成: %1）").arg(outFile), QStringList(), QStringList());
                QMessageBox::warning(this, QStringLiteral("合并"),
                    QStringLiteral("输出文件未生成:\n%1").arg(outFile));
                return;
            }
        }

        QString resultTail = QStringLiteral("结果: 成功");
        bool mergedSingle = false;
        if (lineMode && count > 1) {
            // Merge N individual outputs into a single unified file at outPath
            if (SeNmoSdkBridge::mergeShapefiles(outputPaths, outPath)) {
                for (const QString& p : outputPaths)
                    SeNmoSdkBridge::deleteShapefile(p);
                QDir().rmdir(tempDir);
                filesToLoad.append(outPath);
                mergedSingle = true;
            } else {
                resultTail = QStringLiteral("结果: 部分成功（合并输出文件失败，已保留各分幅文件）");
                QMessageBox::warning(this, QStringLiteral("合并"),
                    QStringLiteral("SDK执行成功，但合并输出文件失败。\n将保留各分幅文件。"));
                filesToLoad = outputPaths;
            }
        } else {
            filesToLoad = outputPaths;
        }

        writeLog(resultTail, filesToLoad, mergedSingle ? outputPaths : QStringList());

        // SDK 输出字段名可能是 UTF-8 字节 + LDID=0x57 且无 .cpg 声明，ArcGIS/LTZK
        // 属性表会按本地代码页(GBK)误读而乱码。统一规范化为 GBK(CP936)，并写
        // .cpg=936 + LDID=0x4D，保证 QGIS/LTZK 与 ArcGIS 都能正确显示中文。
        for (const QString& f : filesToLoad)
            SeNmoSdkBridge::normalizeOutputToGbk(f);

        ui.progressBar->setValue(100);
        QString msg = QStringLiteral("合并完成!\n输出文件:\n%1\n\n是否将结果加载到地图？");
        msg = msg.arg(filesToLoad.join(QStringLiteral("\n")));
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("合并"));
        box.setText(msg);
        QPushButton* btnYes = box.addButton(QStringLiteral("是"), QMessageBox::YesRole);
        box.addButton(QStringLiteral("否"), QMessageBox::NoRole);
        box.setDefaultButton(btnYes);
        box.exec();
        if (box.clickedButton() == btnYes)
            for (const QString& f : filesToLoad)
                emit addLayerToMap(f);
    } else {
        writeLog(QStringLiteral("结果: 失败（SDK 执行失败）"), QStringList(), QStringList());
        ui.progressBar->setValue(0);
        QMessageBox::warning(this, QStringLiteral("合并"),
            QStringLiteral("合并失败，请检查模板参数和数据。"));
    }
}

// ===================================================================
// 日志跟随 / 持久化
// ===================================================================

void MergeDialog::onOutputPathEdited(const QString& /*text*/)
{
    autoUpdateLogPath();
}

void MergeDialog::autoUpdateLogPath()
{
    if (!m_bLogPathAutoFollow) return;
    QString path = ui.lineEdit_outputDataset->text().trimmed();
    if (path.isEmpty()) return;
    QFileInfo fi(path);
    QString logDir = fi.absolutePath();
    ui.lineEdit_logPath->setText(logDir);
    m_logPath = logDir;
}

void MergeDialog::restoreState()
{
    const QgsSettings settings;
    m_inputPath  = settings.value(QStringLiteral("Merge/InputPath"),  QDir::homePath()).toString();
    m_outputPath = settings.value(QStringLiteral("Merge/OutputPath"), QDir::homePath()).toString();
    m_logPath    = settings.value(QStringLiteral("Merge/LogPath"),    QDir::homePath()).toString();

    ui.lineEdit_inputDataset->setText(m_inputPath);
    ui.lineEdit_outputDataset->setText(m_outputPath);
    ui.lineEdit_logPath->setText(m_logPath);
}

void MergeDialog::saveState()
{
    m_inputPath  = ui.lineEdit_inputDataset->text();
    m_outputPath = ui.lineEdit_outputDataset->text();
    m_logPath    = ui.lineEdit_logPath->text();

    QgsSettings settings;
    settings.setValue(QStringLiteral("Merge/InputPath"),  m_inputPath);
    settings.setValue(QStringLiteral("Merge/OutputPath"), m_outputPath);
    settings.setValue(QStringLiteral("Merge/LogPath"),    m_logPath);
}

// ===================================================================
// 字段下拉（面=分组字段 / 线=连接字段）
// ===================================================================

void MergeDialog::populateFieldCombo()
{
    QString current = m_comboField->currentText();
    // Strip " (N)" suffix if auto-select had annotated it
    int parenIdx = current.indexOf(QStringLiteral(" ("));
    if (parenIdx > 0)
        current = current.left(parenIdx);
    m_comboField->clear();

    int count = ui.listWidget_datasets->count();
    if (count == 0) return;

    QSet<QString> fields;
    // Per-file: detect encoding collectively (one false UTF-8 positive → use GBK for all)
    auto isValidUtf8 = [](const QByteArray& raw, bool* hasMb) -> bool {
        *hasMb = false;
        int j = 0;
        while (j < raw.size()) {
            unsigned char c = raw[j];
            int len; unsigned int minCp;
            if (c < 0x80)           { len = 1; minCp = 0; }
            else if ((c & 0xE0) == 0xC0) { len = 2; minCp = 0x80; *hasMb = true; }
            else if ((c & 0xF0) == 0xE0) { len = 3; minCp = 0x800; *hasMb = true; }
            else if ((c & 0xF8) == 0xF0) { len = 4; minCp = 0x10000; *hasMb = true; }
            else return false;
            if (j + len > raw.size()) return false;
            for (int k = 1; k < len; ++k)
                if ((raw[j+k] & 0xC0) != 0x80) return false;
            unsigned int cp;
            if (len == 2) cp = ((c & 0x1F) << 6) | (raw[j+1] & 0x3F);
            else if (len == 3) cp = ((c & 0x0F) << 12) | ((raw[j+1] & 0x3F) << 6) | (raw[j+2] & 0x3F);
            else cp = ((c & 0x07) << 18) | ((raw[j+1] & 0x3F) << 12) | ((raw[j+2] & 0x3F) << 6) | (raw[j+3] & 0x3F);
            if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
            j += len;
        }
        return true;
    };

    bool firstFile = true;
    for (int di = 0; di < count; ++di) {
        QString path = ui.listWidget_datasets->item(di)->text();
        GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
            path.toUtf8().toStdString().c_str(),
            GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poDS) continue;
        OGRLayer* poLayer = poDS->GetLayer(0);
        if (!poLayer) { GDALClose(poDS); continue; }
        OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
        int nFields = poDefn->GetFieldCount();
        // Collect raw bytes and determine file-level encoding
        QList<QByteArray> rawNames;
        QSet<QString> fileFields;
        bool fileHasMb = false;
        bool fileIsUtf8 = true;
        for (int f = 0; f < nFields; ++f) {
            QByteArray raw(poDefn->GetFieldDefn(f)->GetNameRef());
            rawNames.append(raw);
            bool hasMb;
            bool fieldUtf8 = isValidUtf8(raw, &hasMb);
            if (hasMb && !fieldUtf8) fileIsUtf8 = false;
            if (hasMb) fileHasMb = true;
        }
        for (const QByteArray& raw : rawNames) {
            bool hasMb;
            isValidUtf8(raw, &hasMb);
            QString name = (fileIsUtf8 && hasMb) ? QString::fromUtf8(raw)
                         : hasMb ? QString::fromLocal8Bit(raw)
                         : QString::fromUtf8(raw);
            if (!name.isEmpty())
                fileFields.insert(name);
        }
        if (firstFile) {
            fields = fileFields;
            firstFile = false;
        } else {
            fields.intersect(fileFields);
        }
        GDALClose(poDS);
    }

    m_comboField->addItems(fields.values());
    if (!current.isEmpty() && fields.contains(current))
        m_comboField->setCurrentText(current);
    else
        m_comboField->setCurrentIndex(-1);
    autoSelectBestField();
}

void MergeDialog::autoSelectBestField()
{
    int datasetCount = ui.listWidget_datasets->count();
    int comboCount = m_comboField->count();
    if (datasetCount == 0 || comboCount == 0) return;

    // Save current plain field name (strip annotation if present)
    int curIdx = m_comboField->currentIndex();
    QString curField;
    if (curIdx >= 0) {
        curField = m_comboField->itemText(curIdx);
        int p = curField.indexOf(QStringLiteral(" ("));
        if (p > 0) curField = curField.left(p);
    }
    bool hadSelection = !curField.isEmpty();

    // Fields that are almost certainly per-fragment unique → skip
    auto isPerFragmentField = [](const QString& name) -> bool {
        QString u = name.toUpper();
        return u == QStringLiteral("SHAPE_AREA")
            || u == QStringLiteral("SHAPE_LENG")
            || u == QStringLiteral("SHAPE_LE_1")
            || u.startsWith(QStringLiteral("FID"))
            || u == QStringLiteral("OGC_FID");
    };

    struct Candidate { QString name; int distinct; bool perFragment; };
    QList<Candidate> candidates;

    for (int ci = 0; ci < comboCount; ++ci) {
        QString fieldName = m_comboField->itemText(ci);
        bool perFragment = isPerFragmentField(fieldName);

        QSet<QString> values;
        for (int di = 0; di < datasetCount; ++di) {
            QString path = ui.listWidget_datasets->item(di)->text();
            GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
                path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
            if (!poDS) continue;
            OGRLayer* poLayer = poDS->GetLayer(0);
            if (!poLayer) { GDALClose(poDS); continue; }

            int idx = poLayer->GetLayerDefn()->GetFieldIndex(fieldName.toUtf8().constData());
            if (idx < 0)
                idx = poLayer->GetLayerDefn()->GetFieldIndex(fieldName.toLocal8Bit().constData());
            if (idx < 0) { GDALClose(poDS); continue; }

            poLayer->ResetReading();
            OGRFeature* feat;
            int sampled = 0;
            const int kMaxSample = 5000;
            while ((feat = poLayer->GetNextFeature()) != nullptr && sampled < kMaxSample) {
                if (feat->IsFieldSetAndNotNull(idx))
                    values.insert(QString::fromUtf8(feat->GetFieldAsString(idx)));
                OGRFeature::DestroyFeature(feat);
                ++sampled;
            }
            while ((feat = poLayer->GetNextFeature()) != nullptr)
                OGRFeature::DestroyFeature(feat);
            GDALClose(poDS);
        }

        int distinct = values.count();
        candidates.append({fieldName, distinct, perFragment});
    }

    if (candidates.isEmpty()) return;

    // Build list of selectable (non-per-fragment) candidates
    QList<const Candidate*> selectable;
    for (const auto& c : candidates) {
        if (!c.perFragment && c.distinct >= 1)
            selectable.append(&c);
    }

    // Find best from selectable: polygon ascending (grouping), line descending (connect)
    const bool desc = (m_mode == ModeLine);
    const Candidate* best = nullptr;
    if (!selectable.isEmpty()) {
        std::sort(selectable.begin(), selectable.end(),
            [desc](const Candidate* a, const Candidate* b) {
                return desc ? (a->distinct > b->distinct) : (a->distinct < b->distinct);
            });

        best = selectable.first();
        for (const auto* c : selectable) {
            if (c->distinct > 1) { best = c; break; }
        }
    }

    // Find distinct count of currently selected field
    int curDistinct = 0;
    if (hadSelection) {
        for (const auto& c : candidates) {
            if (c.name == curField) { curDistinct = c.distinct; break; }
        }
    }

    // Auto-select if nothing was previously selected, or if current field has no grouping power
    if (best && (!hadSelection || (curDistinct <= 1 && best->distinct > 1))) {
        int idx = m_comboField->findText(best->name);
        if (idx >= 0)
            m_comboField->setCurrentIndex(idx);
    }

    // Annotate all items with distinct counts
    for (int ci = 0; ci < comboCount; ++ci) {
        QString name = m_comboField->itemText(ci);
        for (const auto& c : candidates) {
            if (c.name == name) {
                m_comboField->setItemText(ci, QStringLiteral("%1 (%2种)").arg(name).arg(c.distinct));
                break;
            }
        }
    }
}

void MergeDialog::autoAdjustParameters()
{
    int count = ui.listWidget_datasets->count();
    if (count == 0) return;

    QString firstPath = ui.listWidget_datasets->item(0)->text();
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
        firstPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poDS) return;

    OGRLayer* poLayer = poDS->GetLayer(0);
    if (!poLayer) { GDALClose(poDS); return; }

    OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
    bool isGeographic = poSRS && poSRS->IsGeographic();
    GDALClose(poDS);

    if (m_mode == ModePolygon) {
        if (isGeographic) {
            m_spinBufferDistance->setValue(0.00001);
            m_spinBufferDistance->setSingleStep(0.000001);
            m_labelBufferMetric->setText(QStringLiteral("%1 ≈ %2m").arg(QString::number(m_spinBufferDistance->value(), 'f', 6)).arg(QString::number(m_spinBufferDistance->value() * 111000.0, 'f', 1)));
            m_labelBufferMetric->show();
        } else {
            m_spinBufferDistance->setValue(1.0);
            m_spinBufferDistance->setSingleStep(0.1);
            m_labelBufferMetric->hide();
        }
    } else if (m_mode == ModeLine) {
        // Auto-select LinkMode: single file uses same-layer, multi-file uses cross-layer
        m_comboLinkMode->setCurrentIndex(count == 1 ? 0 : 1);

        if (isGeographic) {
            m_spinFuzzyTolerance->setValue(0.00005);
            m_spinFuzzyTolerance->setSingleStep(0.000001);
            m_labelFuzzyMetric->setText(QStringLiteral("%1 ≈ %2m").arg(QString::number(m_spinFuzzyTolerance->value(), 'f', 6)).arg(QString::number(m_spinFuzzyTolerance->value() * 111000.0, 'f', 1)));
            m_labelFuzzyMetric->show();
        } else {
            m_spinFuzzyTolerance->setValue(1.0);
            m_spinFuzzyTolerance->setSingleStep(0.1);
            m_labelFuzzyMetric->hide();
        }
    }
}
