#define _HAS_STD_BYTE 0
#include "auto_edge_match_dialog.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QApplication>
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

AutoEdgeMatchDialog::AutoEdgeMatchDialog(QWidget* parent, Qt::WindowFlags fl)
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
    ui.pushButton_addDataset->setStyleSheet(kSymBtnStyle);
    ui.pushButton_removeDataset->setStyleSheet(kSymBtnStyle);
    ui.pushButton_moveUp->setStyleSheet(kSymBtnStyle);
    ui.pushButton_moveDown->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseInput->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseOutput->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseLog->setStyleSheet(kSymBtnStyle);
    ui.pushButton_browseExe->setStyleSheet(kSymBtnStyle);

    connect(ui.pushButton_browseInput,   &QPushButton::clicked, this, &AutoEdgeMatchDialog::browseInput);
    connect(ui.pushButton_addDataset,    &QPushButton::clicked, this, &AutoEdgeMatchDialog::addDataset);
    connect(ui.pushButton_removeDataset, &QPushButton::clicked, this, &AutoEdgeMatchDialog::removeDataset);
    connect(ui.pushButton_moveUp,        &QPushButton::clicked, this, &AutoEdgeMatchDialog::moveDatasetUp);
    connect(ui.pushButton_moveDown,      &QPushButton::clicked, this, &AutoEdgeMatchDialog::moveDatasetDown);

    connect(ui.pushButton_browseOutput, &QPushButton::clicked, this, &AutoEdgeMatchDialog::browseOutput);
    connect(ui.pushButton_browseLog,    &QPushButton::clicked, this, &AutoEdgeMatchDialog::browseLog);
    connect(ui.lineEdit_logPath,        &QLineEdit::textEdited,  this, [this] { m_bLogPathAutoFollow = false; });
    connect(ui.lineEdit_outputDataset,  &QLineEdit::textChanged, this, &AutoEdgeMatchDialog::onOutputPathEdited);

    connect(ui.pushButton_ok,     &QPushButton::clicked, this, &AutoEdgeMatchDialog::onOk);
    connect(ui.pushButton_cancel, &QPushButton::clicked, this, &QDialog::close);

    ui.comboBox_logLevel->setCurrentIndex(1);

    // Hide the EXE path row
    ui.label_wujiExePath->hide();
    ui.lineEdit_wujiExePath->hide();
    ui.pushButton_browseExe->hide();

    // Simple Field combo in the param placeholder
    QVBoxLayout* lay = new QVBoxLayout();
    lay->setContentsMargins(0, 4, 0, 4);
    QLabel* lbl = new QLabel(QStringLiteral("连接字段:"));
    m_comboField = new QComboBox();
    m_comboField->setEditable(true);
    m_comboField->setToolTip(QStringLiteral("值相同的断线将被连接为同一个实体。\n支持多字段组合，用逗号分隔，如: CODE,NAME"));
    m_comboField->lineEdit()->setPlaceholderText(QStringLiteral("如: CODE,NAME"));
    lay->addWidget(lbl);
    lay->addWidget(m_comboField);
    ui.layout_paramPlaceholder->addLayout(lay);

    // Advanced parameter panel (auto-adjusted to input CRS)
    m_groupAdvanced = new QGroupBox(QStringLiteral("高级参数"));
    QFormLayout* formLayout = new QFormLayout(m_groupAdvanced);

    m_spinFuzzyTolerance = new QDoubleSpinBox();
    m_spinFuzzyTolerance->setRange(0.000001, 1.0);
    m_spinFuzzyTolerance->setSingleStep(0.000001);
    m_spinFuzzyTolerance->setDecimals(6);
    m_spinFuzzyTolerance->setValue(0.00005);
    m_spinFuzzyTolerance->setToolTip(QStringLiteral("结点拟合容差，单位与数据坐标系一致"));
    formLayout->addRow(QStringLiteral("结点拟合容差:"), m_spinFuzzyTolerance);
    m_labelFuzzyMetric = new QLabel();
    m_labelFuzzyMetric->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    formLayout->addRow(QString(), m_labelFuzzyMetric);
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
    formLayout->addRow(QStringLiteral("连接模式:"), m_comboLinkMode);

    ui.layout_paramPlaceholder->addWidget(m_groupAdvanced);

    restoreState();
    autoUpdateLogPath();
}

AutoEdgeMatchDialog::~AutoEdgeMatchDialog()
{
    saveState();
}

// ===================================================================
// 数据集列表操作
// ===================================================================

void AutoEdgeMatchDialog::browseInput()
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
        populateFieldCombo();
        autoAdjustParameters();
        if (fresh || ui.lineEdit_outputDataset->text().isEmpty()) {
            int n = ui.listWidget_datasets->count();
            QFileInfo fi(ui.listWidget_datasets->item(0)->text());
            if (n == 1) {
                ui.lineEdit_outputDataset->setText(
                    fi.absolutePath() + "/" + fi.completeBaseName() + "_matched.shp");
            } else {
                ui.lineEdit_outputDataset->setText(
                    fi.absolutePath() + "/matched.shp");
            }
            m_outputPath = m_inputPath;
        }
    }
}

void AutoEdgeMatchDialog::addDataset()
{
    QStringList paths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("请选择输入矢量文件"), m_inputPath,
        QStringLiteral("矢量文件 (*.shp *.gpkg *.gdb *.geojson)"));
    if (!paths.isEmpty()) {
        for (const QString& path : paths)
            ui.listWidget_datasets->addItem(path);
        populateFieldCombo();
        autoAdjustParameters();
        if (ui.lineEdit_outputDataset->text().isEmpty()) {
            int n = ui.listWidget_datasets->count();
            QFileInfo fi(ui.listWidget_datasets->item(0)->text());
            if (n == 1) {
                ui.lineEdit_outputDataset->setText(
                    fi.absolutePath() + "/" + fi.completeBaseName() + "_matched.shp");
            } else {
                ui.lineEdit_outputDataset->setText(
                    fi.absolutePath() + "/matched.shp");
            }
            m_outputPath = m_inputPath;
        }
    }
}

void AutoEdgeMatchDialog::removeDataset()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row >= 0) {
        delete ui.listWidget_datasets->takeItem(row);
        populateFieldCombo();
    }
}

void AutoEdgeMatchDialog::moveDatasetUp()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row > 0) {
        QListWidgetItem* item = ui.listWidget_datasets->takeItem(row);
        ui.listWidget_datasets->insertItem(row - 1, item);
        ui.listWidget_datasets->setCurrentRow(row - 1);
    }
}

void AutoEdgeMatchDialog::moveDatasetDown()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row >= 0 && row < ui.listWidget_datasets->count() - 1) {
        QListWidgetItem* item = ui.listWidget_datasets->takeItem(row);
        ui.listWidget_datasets->insertItem(row + 1, item);
        ui.listWidget_datasets->setCurrentRow(row + 1);
    }
}

// ===================================================================
// 输出 / 日志
// ===================================================================

void AutoEdgeMatchDialog::browseOutput()
{
    QString startPath = ui.lineEdit_outputDataset->text().trimmed();
    if (startPath.isEmpty()) startPath = m_outputPath;

    QString path = QFileDialog::getSaveFileName(this,
        QStringLiteral("请选择输出保存位置"), startPath,
        QStringLiteral("矢量文件 (*.shp *.gpkg)"));
    if (!path.isEmpty()) {
        m_outputPath = path;
        ui.lineEdit_outputDataset->setText(path);
    }
}

void AutoEdgeMatchDialog::browseLog()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("请选择日志保存路径"), m_logPath);
    if (!dir.isEmpty()) {
        m_logPath = dir;
        m_bLogPathAutoFollow = false;
        ui.lineEdit_logPath->setText(dir);
    }
}

void AutoEdgeMatchDialog::onOutputPathEdited(const QString& /*text*/)
{
    autoUpdateLogPath();
}

void AutoEdgeMatchDialog::autoUpdateLogPath()
{
    if (!m_bLogPathAutoFollow) return;
    QString path = ui.lineEdit_outputDataset->text().trimmed();
    if (path.isEmpty()) return;
    m_logPath = QFileInfo(path).absolutePath();
    ui.lineEdit_logPath->setText(m_logPath);
}

// ===================================================================
// 确定 — 校验 + 用模板 + SDK 执行拓扑连接
// ===================================================================

void AutoEdgeMatchDialog::onOk()
{
    int count = ui.listWidget_datasets->count();
    if (count < 1) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("请添加至少一个输入数据集"));
        return;
    }
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
    // For N>1, SDK writes to temp dir; we merge into the single outPath afterwards.
    QStringList outputPaths;
    QString tempDir;
    if (count == 1) {
        outputPaths.append(outPath);
    } else {
        tempDir = outDir + QStringLiteral("/_edge_match_temp");
        QDir().mkpath(tempDir);
        for (int i = 0; i < count; ++i) {
            QString inBase = QFileInfo(ui.listWidget_datasets->item(i)->text()).completeBaseName();
            outputPaths.append(tempDir + QStringLiteral("/") + inBase + QStringLiteral("_matched.shp"));
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
    // 定位模板：运行时 xml 目录优先（Windows <运行时>/xml、麒麟 /opt/ltzk/xml），
    // 其次 Windows 部署/开发源码树兜底
    cfg.templatePath = SeWujiXmlGenerator::resolveTemplatePath(QStringLiteral("edge_match_template.xml"));

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
    QStringList projectedPaths = SeNmoSdkBridge::ensureProjectedInputs(gbkPaths, dataDir, originalSrsWkt);
    bool didProject = !originalSrsWkt.isEmpty();

    for (const QString& f : projectedPaths)
        cfg.inputFiles.append(f);
    for (const QString& op : outputPaths)
        cfg.outputFiles.append(op);
    QString field = m_comboField->currentText().trimmed();
    int parenIdx = field.indexOf(QStringLiteral(" ("));
    if (parenIdx > 0) field = field.left(parenIdx);
    cfg.entityField = field;
    cfg.dataPath = dataDir;

    // Read advanced parameter overrides
    cfg.fuzzyTolerance = m_spinFuzzyTolerance->value();
    cfg.linkMode = m_comboLinkMode->currentData().toInt();

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

    SeNmoSdkBridge::cleanupProjectedTempFiles(projectedPaths);

    QApplication::restoreOverrideCursor();
    ui.progressBar->setRange(0, 100);
    ui.pushButton_ok->setEnabled(true);

    if (success) {
        // Verify all output files exist
        for (int i = 0; i < outputPaths.count(); ++i) {
            if (!QFileInfo::exists(outputPaths[i])) {
                QMessageBox::warning(this, QStringLiteral("接边"),
                    QStringLiteral("输出文件未生成:\n%1").arg(outputPaths[i]));
                return;
            }
        }

        QStringList filesToLoad;
        if (count > 1) {
            // Merge N individual outputs into a single unified file at outPath
            if (SeNmoSdkBridge::mergeShapefiles(outputPaths, outPath)) {
                for (const QString& p : outputPaths)
                    SeNmoSdkBridge::deleteShapefile(p);
                QDir().rmdir(tempDir);
                filesToLoad.append(outPath);
            } else {
                QMessageBox::warning(this, QStringLiteral("接边"),
                    QStringLiteral("SDK执行成功，但合并输出文件失败。\n将加载各分幅文件。"));
                filesToLoad = outputPaths;
            }
        } else {
            filesToLoad = outputPaths;
        }

        ui.progressBar->setValue(100);
        QMessageBox::information(this, QStringLiteral("接边"),
            QStringLiteral("执行完成!\n输出文件:\n%1").arg(filesToLoad.join(QStringLiteral("\n"))));

        for (const QString& f : filesToLoad)
            emit addLayerToMap(f);
    } else {
        ui.progressBar->setValue(0);
        QMessageBox::warning(this, QStringLiteral("接边"),
            QStringLiteral("执行失败，请检查模板参数和数据。"));
    }
}

// ===================================================================
// QgsSettings 持久化
// ===================================================================

void AutoEdgeMatchDialog::restoreState()
{
    const QgsSettings settings;
    m_inputPath  = settings.value(QStringLiteral("AutoEdgeMatch/InputPath"),  QDir::homePath()).toString();
    m_outputPath = settings.value(QStringLiteral("AutoEdgeMatch/OutputPath"), QDir::homePath()).toString();
    m_logPath    = settings.value(QStringLiteral("AutoEdgeMatch/LogPath"),    QDir::homePath()).toString();

    ui.lineEdit_inputDataset->setText(m_inputPath);
    ui.lineEdit_outputDataset->setText(m_outputPath);
    ui.lineEdit_logPath->setText(m_logPath);
}

void AutoEdgeMatchDialog::saveState()
{
    m_inputPath  = ui.lineEdit_inputDataset->text();
    m_outputPath = ui.lineEdit_outputDataset->text();
    m_logPath    = ui.lineEdit_logPath->text();

    QgsSettings settings;
    settings.setValue(QStringLiteral("AutoEdgeMatch/InputPath"),   m_inputPath);
    settings.setValue(QStringLiteral("AutoEdgeMatch/OutputPath"),  m_outputPath);
    settings.setValue(QStringLiteral("AutoEdgeMatch/LogPath"),     m_logPath);
}

void AutoEdgeMatchDialog::populateFieldCombo()
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
        QList<QByteArray> rawNames;
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
                fields.insert(name);
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

void AutoEdgeMatchDialog::autoSelectBestField()
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

    auto isPerFeatureField = [](const QString& name) -> bool {
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
        bool perFragment = isPerFeatureField(fieldName);

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

    // Find best from selectable: sort by distinct descending, skip 1-value fields
    const Candidate* best = nullptr;
    if (!selectable.isEmpty()) {
        std::sort(selectable.begin(), selectable.end(),
            [](const Candidate* a, const Candidate* b) { return a->distinct > b->distinct; });

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

void AutoEdgeMatchDialog::autoAdjustParameters()
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

    if (isGeographic) {
        m_spinFuzzyTolerance->setValue(0.00005);
        m_spinFuzzyTolerance->setSingleStep(0.000001);
        m_comboLinkMode->setCurrentIndex(1); // cross-layer only for split maps
        m_labelFuzzyMetric->setText(QStringLiteral("%1° ≈ %2m").arg(QString::number(m_spinFuzzyTolerance->value(), 'f', 6)).arg(QString::number(m_spinFuzzyTolerance->value() * 111000.0, 'f', 1)));
        m_labelFuzzyMetric->show();
    } else {
        m_spinFuzzyTolerance->setValue(5.0);
        m_spinFuzzyTolerance->setSingleStep(0.1);
        m_comboLinkMode->setCurrentIndex(1);
        m_labelFuzzyMetric->hide();
    }
}
