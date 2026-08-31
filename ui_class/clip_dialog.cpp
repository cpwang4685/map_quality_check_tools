#include "clip_dialog.h"
#include "map_extent_tool.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QButtonGroup>
#include <QCheckBox>
#include <QLabel>
#include <QRadioButton>
#include <QPushButton>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <qgssettings.h>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsgui.h>
#include <qgisinterface.h>

#include "../ui_task/se_clip_merge_task.h"
#include "ui_fit_helper.h"

ClipDialog::ClipDialog(QgisInterface* iface, QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , mIface(iface)
{
    ui.setupUi(this);
    QgsGui::enableAutoGeometryRestore(this);
    DialogFitHelper::install(this);

    setWindowFlags(Qt::WindowStaysOnTopHint | Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);

    // 地图画布引用
    if (mIface)
        mCanvas = mIface->mapCanvas();

    QButtonGroup* clipModeGroup = new QButtonGroup(this);
    clipModeGroup->addButton(ui.radioButton_featureClip);
    clipModeGroup->addButton(ui.radioButton_coordClip);

    connect(ui.pushButton_browseInput,  &QPushButton::clicked, this, &ClipDialog::browseInput);
    connect(ui.pushButton_browseClip,   &QPushButton::clicked, this, &ClipDialog::browseClip);
    connect(ui.pushButton_browseOutput, &QPushButton::clicked, this, &ClipDialog::browseOutput);
    connect(ui.pushButton_browseLog,    &QPushButton::clicked, this, &ClipDialog::browseLog);
    connect(ui.radioButton_featureClip, &QRadioButton::toggled, this, &ClipDialog::onClipModeChanged);
    connect(ui.checkBox_batch,          &QCheckBox::toggled,    this, &ClipDialog::onBatchToggled);
    connect(ui.radioButton_multiFile,   &QRadioButton::toggled, this, &ClipDialog::onBatchSubChanged);
    connect(ui.radioButton_separate,   &QRadioButton::toggled, this, &ClipDialog::onBatchOutputModeChanged);
    connect(ui.radioButton_merge,      &QRadioButton::toggled, this, &ClipDialog::onBatchOutputModeChanged);
    connect(ui.pushButton_ok,           &QPushButton::clicked,  this, &ClipDialog::onOk);
    connect(ui.pushButton_cancel,       &QPushButton::clicked,  this, &ClipDialog::onCancel);

    // 日志路径跟随输出
    connect(ui.lineEdit_logPath, &QLineEdit::textEdited, this, [this] { m_bLogPathAutoFollow = false; });
    connect(ui.lineEdit_output,  &QLineEdit::textChanged, this, &ClipDialog::onOutputPathEdited);

    // 地图框选按钮（在坐标范围区域内）
    QPushButton* btnMapExtent = new QPushButton(QString::fromUtf8("\xF0\x9F\x9F\x90 从地图框选"));
    btnMapExtent->setObjectName(QStringLiteral("pushButton_mapExtent"));
    btnMapExtent->setEnabled(mCanvas != nullptr);
    btnMapExtent->setMinimumWidth(120);
    connect(btnMapExtent, &QPushButton::clicked, this, &ClipDialog::onSelectExtentFromMap);

    QHBoxLayout* mapBtnRow = new QHBoxLayout();
    mapBtnRow->setContentsMargins(70, 0, 0, 4);
    mapBtnRow->addWidget(btnMapExtent);
    mapBtnRow->addStretch();
    static_cast<QVBoxLayout*>(ui.widget_extent->layout())->addLayout(mapBtnRow);

    ui.comboBox_logLevel->setCurrentIndex(1);
    onClipModeChanged();
    onBatchToggled(false);
    restoreState();
    autoUpdateLogPath();
}

ClipDialog::~ClipDialog()
{
    deactivateMapTool();
    delete mMapExtentTool;
    removeInputPreview();
    for (auto* t : m_tasks) {
        if (t && t->status() == QgsTask::Running)
            t->cancel();
    }
    saveState();
}

void ClipDialog::onBatchToggled(bool checked)
{
    ui.widget_batchRadio->setVisible(checked);
    ui.widget_batchOutputGroup->setVisible(checked);

    onBatchOutputModeChanged();
}

void ClipDialog::onBatchSubChanged()
{
}

void ClipDialog::onBatchOutputModeChanged()
{
    if (!ui.checkBox_batch->isChecked()) return;

    bool merge = ui.radioButton_merge->isChecked();
    ui.label_output->setText(merge ? QStringLiteral("输出文件") : QStringLiteral("输出目录"));

    if (merge)
    {
        QString current = ui.lineEdit_output->text().trimmed();
        if (!current.isEmpty() && QDir(current).exists())
            m_separateDir = current;
        else if (m_separateDir.isEmpty())
            m_separateDir = QDir::homePath();

        if (current.isEmpty() || QDir(current).exists())
            ui.lineEdit_output->setText(m_separateDir + "/result_merged.gpkg");
    }
    else
    {
        if (!m_separateDir.isEmpty())
            ui.lineEdit_output->setText(m_separateDir);
    }
}

void ClipDialog::browseInput()
{
    bool batch = ui.checkBox_batch->isChecked();

    if (!batch)
    {
        QString path = QFileDialog::getOpenFileName(this,
            QStringLiteral("请选择输入矢量文件"), m_inputPath,
            QStringLiteral("矢量文件 (*.shp *.gpkg *.geojson *.json *.gdb)"));
        if (!path.isEmpty()) {
            m_inputPath = path;
            ui.lineEdit_input->setText(path);
            loadInputPreview(path);
        }
    }
    else if (ui.radioButton_multiFile->isChecked())
    {
        QStringList files = QFileDialog::getOpenFileNames(this,
            QStringLiteral("请选择多个矢量文件"), m_inputPath,
            QStringLiteral("矢量文件 (*.shp *.gpkg *.geojson *.json *.gdb)"));
        if (!files.isEmpty()) {
            m_inputPath = files.join(QLatin1String(";"));
            ui.lineEdit_input->setText(m_inputPath);
            loadBatchPreview(files);
        }
    }
    else
    {
        QString dir = QFileDialog::getExistingDirectory(this,
            QStringLiteral("请选择输入文件夹"), m_inputPath);
        if (!dir.isEmpty()) {
            m_inputPath = dir;
            ui.lineEdit_input->setText(dir);
            QStringList previewFiles;
            QDir d(dir);
            QStringList filters;
            filters << "*.shp" << "*.SHP" << "*.gpkg" << "*.GPKG"
                    << "*.geojson" << "*.GEOJSON" << "*.json" << "*.JSON" << "*.gdb" << "*.GDB";
            QFileInfoList entries = d.entryInfoList(filters, QDir::Files | QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot);
            for (const QFileInfo& info : entries) {
                if (info.isFile() || info.suffix().toLower() == "gdb")
                    previewFiles.append(info.absoluteFilePath());
            }
            loadBatchPreview(previewFiles);
        }
    }
}

void ClipDialog::browseClip()
{
    QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("请选择裁剪边界矢量文件"), m_clipPath,
        QStringLiteral("矢量文件 (*.shp *.gpkg *.geojson *.json)"));
    if (!path.isEmpty()) {
        m_clipPath = path;
        ui.lineEdit_clipFeature->setText(path);
    }
}

void ClipDialog::browseOutput()
{
    bool batch = ui.checkBox_batch->isChecked();
    bool merge = batch && ui.radioButton_merge->isChecked();

    if (!batch || merge)
    {
        QString path = QFileDialog::getSaveFileName(this,
            QStringLiteral("请选择输出文件"), m_outputPath,
            QStringLiteral("矢量文件 (*.shp *.gpkg)"));
        if (!path.isEmpty()) {
            m_outputPath = path;
            ui.lineEdit_output->setText(path);
        }
    }
    else
    {
        QString dir = QFileDialog::getExistingDirectory(this,
            QStringLiteral("请选择输出目录"), m_outputPath);
        if (!dir.isEmpty()) {
            m_outputPath = dir;
            ui.lineEdit_output->setText(dir);
        }
    }
}

void ClipDialog::browseLog()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("请选择日志保存路径"), m_logPath);
    if (!dir.isEmpty()) {
        m_logPath = dir;
        m_bLogPathAutoFollow = false;
        ui.lineEdit_logPath->setText(dir);
    }
}

void ClipDialog::onClipModeChanged()
{
    bool featureMode = ui.radioButton_featureClip->isChecked();
    ui.label_clipFeature->setVisible(featureMode);
    ui.lineEdit_clipFeature->setVisible(featureMode);
    ui.pushButton_browseClip->setVisible(featureMode);
    ui.widget_extent->setVisible(!featureMode);
}

bool ClipDialog::isVectorFile(const QString& path)
{
    QString lower = path.toLower();
    return lower.endsWith(".shp") || lower.endsWith(".gpkg")
        || lower.endsWith(".geojson") || lower.endsWith(".json")
        || lower.endsWith(".gdb");
}

QStringList ClipDialog::scanInputFiles()
{
    QStringList result;
    bool batch = ui.checkBox_batch->isChecked();

    if (!batch)
    {
        QString path = ui.lineEdit_input->text().trimmed();
        if (!path.isEmpty() && isVectorFile(path))
            result.append(path);
    }
    else if (ui.radioButton_multiFile->isChecked())
    {
        QString raw = ui.lineEdit_input->text().trimmed();
        QStringList parts = raw.split(QLatin1String(";"), QString::SkipEmptyParts);
        for (const QString& p : parts)
        {
            QString trimmed = p.trimmed();
            if (!trimmed.isEmpty() && isVectorFile(trimmed))
                result.append(trimmed);
        }
    }
    else
    {
        QString dirPath = ui.lineEdit_input->text().trimmed();
        if (dirPath.isEmpty()) return result;

        QDir dir(dirPath);
        QStringList nameFilters;
        nameFilters << "*.shp" << "*.SHP" << "*.gpkg" << "*.GPKG"
                    << "*.geojson" << "*.GEOJSON" << "*.json" << "*.JSON"
                    << "*.gdb" << "*.GDB";
        QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot;

        QList<QDir> dirs;
        dirs.append(dir);
        while (!dirs.isEmpty())
        {
            QDir d = dirs.takeFirst();
            QFileInfoList entries = d.entryInfoList(nameFilters, filters, QDir::Name);
            for (const QFileInfo& info : entries)
            {
                if (info.isDir() && info.suffix().toLower() == "gdb")
                {
                    result.append(info.absoluteFilePath());
                }
                else if (info.isFile())
                {
                    result.append(info.absoluteFilePath());
                }
            }
            QFileInfoList subDirs = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo& sub : subDirs)
            {
                if (sub.suffix().toLower() != "gdb")
                    dirs.append(QDir(sub.absoluteFilePath()));
            }
        }
    }

    if (ui.radioButton_featureClip->isChecked() && !m_clipPath.isEmpty())
    {
        QString clipAbs = QFileInfo(m_clipPath).absoluteFilePath();
        result.removeAll(clipAbs);
    }

    return result;
}

QString ClipDialog::makeOutputPath(const QString& inputFile)
{
    bool batch = ui.checkBox_batch->isChecked();
    if (!batch)
    {
        return ui.lineEdit_output->text().trimmed();
    }

    QFileInfo fi(inputFile);
    QString base = fi.completeBaseName();
    QString outDir = ui.lineEdit_output->text().trimmed();

    if (ui.radioButton_separate->isChecked())
        return outDir + "/" + base + "_clip.shp";
    else
        return outDir + "/" + base + "_clip.gpkg";
}

void ClipDialog::onOk()
{
    if (ui.lineEdit_input->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请选择输入要素"));
        return;
    }

    bool featureMode = ui.radioButton_featureClip->isChecked();
    if (featureMode && ui.lineEdit_clipFeature->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请选择裁剪要素"));
        return;
    }

    if (ui.lineEdit_output->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请选择输出目录"));
        return;
    }

    if (!featureMode)
    {
        double dMinX = ui.doubleSpinBox_minX->value();
        double dMaxX = ui.doubleSpinBox_maxX->value();
        double dMinY = ui.doubleSpinBox_minY->value();
        double dMaxY = ui.doubleSpinBox_maxY->value();
        if (dMinX >= dMaxX || dMinY >= dMaxY) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("坐标范围无效"));
            return;
        }
    }

    QStringList inputFiles = scanInputFiles();
    if (inputFiles.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("未找到有效的矢量文件"));
        return;
    }

    int filteredCount = 0;
    if (ui.checkBox_batch->isChecked() && ui.radioButton_traverseDir->isChecked())
    {
        QDir rawDir(ui.lineEdit_input->text().trimmed());
        QStringList all = rawDir.entryList({"*.shp","*.SHP","*.gpkg","*.GPKG","*.geojson","*.GEOJSON","*.json","*.JSON","*.gdb","*.GDB"},
            QDir::Files | QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot);
        filteredCount = all.size() - inputFiles.size();
    }

    QString preMsg = QStringLiteral("共检索 %1 个矢量文件").arg(inputFiles.size());
    if (filteredCount > 0)
        preMsg += QStringLiteral("，自动过滤裁剪边界文件 %1 个").arg(filteredCount);
    preMsg += QStringLiteral("，即将处理 %1 个文件。").arg(inputFiles.size());

    if (QMessageBox::question(this, QStringLiteral("裁剪"), preMsg,
                              QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
        return;

    removeInputPreview();

    string strClipMode;
    string strClipFeaturePath;
    double dMinX = 0, dMinY = 0, dMaxX = 0, dMaxY = 0;

    if (featureMode)
    {
        strClipMode = "feature";
        strClipFeaturePath = ui.lineEdit_clipFeature->text().trimmed().toUtf8().toStdString();
    }
    else
    {
        strClipMode = "coordinate";
        dMinX = ui.doubleSpinBox_minX->value();
        dMinY = ui.doubleSpinBox_minY->value();
        dMaxX = ui.doubleSpinBox_maxX->value();
        dMaxY = ui.doubleSpinBox_maxY->value();
    }

    double dTolerance = 0.0;
    QString tolText = ui.lineEdit_tolerance->text().trimmed();
    if (!tolText.isEmpty())
    {
        bool ok;
        dTolerance = tolText.toDouble(&ok);
        if (!ok || dTolerance < 0) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("XY 容差格式不正确"));
            return;
        }
    }

    int logLevel = ui.comboBox_logLevel->currentIndex();
    string logPath = ui.lineEdit_logPath->text().trimmed().toUtf8().toStdString();

    if (ui.checkBox_batch->isChecked() && ui.radioButton_separate->isChecked())
        QDir().mkpath(ui.lineEdit_output->text().trimmed());
    else
        QDir().mkpath(QFileInfo(ui.lineEdit_output->text().trimmed()).absolutePath());
    QDir().mkpath(QString::fromUtf8(logPath.c_str()));

    saveState();
    ui.progressBar->reset();

    bool batch = ui.checkBox_batch->isChecked();
    bool separate = ui.radioButton_separate->isChecked();

    if (batch && separate)
    {
        for (const QString& inputFile : inputFiles)
        {
            QString outPath = makeOutputPath(inputFile);
            vector<string> vecFiles;
            vecFiles.push_back(inputFile.toUtf8().toStdString());

            SeClipMergeTask* task = new SeClipMergeTask(
                tr("裁剪"),
                vecFiles,
                outPath.toUtf8().toStdString(),
                strClipMode, strClipFeaturePath,
                dMinX, dMinY, dMaxX, dMaxY,
                dTolerance, logLevel, logPath);

            connect(task, &SeClipMergeTask::taskFinished, this, &ClipDialog::onTaskFinished);
            connect(task, &QgsTask::progressChanged, this, [this](double) {
                int total = 0;
                for (auto* t : m_tasks)
                    total += t->progress();
                if (!m_tasks.isEmpty())
                    ui.progressBar->setValue(total / m_tasks.size());
            });

            m_tasks.append(task);
        }
    }
    else
    {
        vector<string> vecFiles;
        for (const QString& f : inputFiles)
            vecFiles.push_back(f.toUtf8().toStdString());

        QString outPath;
        if (batch)
        {
            outPath = ui.lineEdit_output->text().trimmed();
        }
        else
        {
            outPath = makeOutputPath(inputFiles.first());
        }

        SeClipMergeTask* task = new SeClipMergeTask(
            tr("裁剪"),
            vecFiles,
            outPath.toUtf8().toStdString(),
            strClipMode, strClipFeaturePath,
            dMinX, dMinY, dMaxX, dMaxY,
            dTolerance, logLevel, logPath,
            batch);

        connect(task, &SeClipMergeTask::taskFinished, this, &ClipDialog::onTaskFinished);
        connect(task, &QgsTask::progressChanged, this, [this](double p) {
            ui.progressBar->setValue(static_cast<int>(p));
        });

        m_tasks.append(task);
    }

    m_tasksTotal = m_tasks.size();
    m_tasksCompleted = 0;
    m_allTasksOk = true;

    for (auto* task : m_tasks)
        QgsApplication::taskManager()->addTask(task);
}

void ClipDialog::onTaskFinished(bool result)
{
    m_tasksCompleted++;
    if (!result) m_allTasksOk = false;

    if (m_tasksCompleted >= m_tasksTotal)
    {
        if (m_allTasksOk)
            QMessageBox::information(this, QStringLiteral("裁剪"), QStringLiteral("裁剪完成!"));
        else {
            QString logDir = ui.lineEdit_logPath->text().trimmed();
            QString logFile = logDir + "/System_Running_Info_ClipMerge.txt";
            QMessageBox::warning(this, QStringLiteral("裁剪"),
                QStringLiteral("部分任务失败！\n\n日志文件：%1").arg(logFile));
        }

        m_tasks.clear();
        m_tasksCompleted = 0;
        m_tasksTotal = 0;
        m_allTasksOk = true;
    }
}

void ClipDialog::onCancel()
{
    for (auto* t : m_tasks) {
        if (t && t->status() == QgsTask::Running)
            t->cancel();
    }
    reject();
}

void ClipDialog::onOutputPathEdited(const QString& /*text*/)
{
    autoUpdateLogPath();
}

void ClipDialog::autoUpdateLogPath()
{
    if (!m_bLogPathAutoFollow) return;
    QString path = ui.lineEdit_output->text().trimmed();
    if (path.isEmpty()) return;
    QFileInfo fi(path);
    QString logDir = fi.isDir() ? path : fi.absolutePath();
    ui.lineEdit_logPath->setText(logDir);
    m_logPath = logDir;
}

void ClipDialog::restoreState()
{
    const QgsSettings settings;
    m_inputPath  = settings.value(QStringLiteral("Clip/InputPath"),  QDir::homePath()).toString();
    m_clipPath   = settings.value(QStringLiteral("Clip/ClipPath"),   QDir::homePath()).toString();
    m_outputPath = settings.value(QStringLiteral("Clip/OutputPath"), QDir::homePath()).toString();
    m_logPath    = settings.value(QStringLiteral("Clip/LogPath"),    QDir::homePath()).toString();

    ui.lineEdit_input->setText(m_inputPath);
    ui.lineEdit_clipFeature->setText(m_clipPath);
    ui.lineEdit_output->setText(m_outputPath);
    ui.lineEdit_logPath->setText(m_logPath);

    if (!m_outputPath.isEmpty())
        m_separateDir = m_outputPath;
}

void ClipDialog::saveState()
{
    m_inputPath  = ui.lineEdit_input->text();
    m_clipPath   = ui.lineEdit_clipFeature->text();
    m_outputPath = ui.lineEdit_output->text();
    m_logPath    = ui.lineEdit_logPath->text();

    QgsSettings settings;
    settings.setValue(QStringLiteral("Clip/InputPath"),  m_inputPath);
    settings.setValue(QStringLiteral("Clip/ClipPath"),   m_clipPath);
    settings.setValue(QStringLiteral("Clip/OutputPath"), m_outputPath);
    settings.setValue(QStringLiteral("Clip/LogPath"),    m_logPath);
}

void ClipDialog::onSelectExtentFromMap()
{
    if (!mCanvas) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("未检测到地图画布，无法进行地图框选"));
        return;
    }

    // 清除旧的高亮框
    delete mExtentHighlight;
    mExtentHighlight = nullptr;

    if (!mMapExtentTool) {
        mMapExtentTool = new MapExtentTool(mCanvas,
            [this](const QgsRectangle& extent) { onExtentCaptured(extent); });
    }

    mCanvas->setMapTool(mMapExtentTool);
}

void ClipDialog::onExtentCaptured(const QgsRectangle& extent)
{
    deactivateMapTool();

    // 删除旧的高亮框
    delete mExtentHighlight;
    mExtentHighlight = nullptr;

    // 在地图上保留一个持久化矩形标注框选位置
    mExtentHighlight = new QgsRubberBand(mCanvas, QgsWkbTypes::PolygonGeometry);
    mExtentHighlight->setFillColor(QColor(255, 87, 34, 40));
    mExtentHighlight->setStrokeColor(QColor(255, 87, 34));
    mExtentHighlight->setWidth(2);
    mExtentHighlight->setLineStyle(Qt::DashLine);
    mExtentHighlight->addPoint(QgsPointXY(extent.xMinimum(), extent.yMinimum()), false);
    mExtentHighlight->addPoint(QgsPointXY(extent.xMaximum(), extent.yMinimum()), false);
    mExtentHighlight->addPoint(QgsPointXY(extent.xMaximum(), extent.yMaximum()), false);
    mExtentHighlight->addPoint(QgsPointXY(extent.xMinimum(), extent.yMaximum()), true);
    mExtentHighlight->show();

    // 坐标系转换：地图 CRS → WGS84 (EPSG:4326)
    QgsRectangle wgs84Extent = extent;
    QgsCoordinateReferenceSystem mapCrs = mCanvas->mapSettings().destinationCrs();
    QgsCoordinateReferenceSystem wgs84(QStringLiteral("EPSG:4326"));
    if (mapCrs.isValid() && mapCrs != wgs84) {
        QgsCoordinateTransform xform(mapCrs, wgs84, QgsProject::instance());
        try {
            wgs84Extent = xform.transform(extent);
        } catch (...) {
            // 转换失败，使用原始范围
        }
    }

    ui.doubleSpinBox_minX->setValue(wgs84Extent.xMinimum());
    ui.doubleSpinBox_maxX->setValue(wgs84Extent.xMaximum());
    ui.doubleSpinBox_minY->setValue(wgs84Extent.yMinimum());
    ui.doubleSpinBox_maxY->setValue(wgs84Extent.yMaximum());

    raise();
    activateWindow();
}

void ClipDialog::deactivateMapTool()
{
    if (mMapExtentTool && mCanvas && mCanvas->mapTool() == mMapExtentTool)
        mCanvas->unsetMapTool(mMapExtentTool);

    delete mExtentHighlight;
    mExtentHighlight = nullptr;
}

void ClipDialog::loadInputPreview(const QString& path)
{
    if (!mCanvas) return;

    removeInputPreview();

    QFileInfo fi(path);
    QgsVectorLayer* layer = new QgsVectorLayer(path, fi.completeBaseName(), QStringLiteral("ogr"));
    if (!layer->isValid()) {
        delete layer;
        return;
    }

    QgsProject::instance()->addMapLayer(layer);
    mPreviewLayers.append(layer);
    mCanvas->setExtent(layer->extent());
    mCanvas->refresh();
}

void ClipDialog::loadBatchPreview(const QStringList& files)
{
    if (!mCanvas) return;

    removeInputPreview();

    QgsRectangle combinedExtent;
    bool first = true;

    for (const QString& path : files) {
        QFileInfo fi(path);
        QgsVectorLayer* layer = new QgsVectorLayer(path, fi.completeBaseName(), QStringLiteral("ogr"));
        if (!layer->isValid()) {
            delete layer;
            continue;
        }
        QgsProject::instance()->addMapLayer(layer);
        mPreviewLayers.append(layer);

        if (first) {
            combinedExtent = layer->extent();
            first = false;
        } else {
            combinedExtent.combineExtentWith(layer->extent());
        }
    }

    if (!first) {
        mCanvas->setExtent(combinedExtent);
        mCanvas->refresh();
    }
}

void ClipDialog::removeInputPreview()
{
    for (QgsVectorLayer* layer : mPreviewLayers) {
        if (layer)
            QgsProject::instance()->removeMapLayer(layer->id());
    }
    if (!mPreviewLayers.isEmpty()) {
        mPreviewLayers.clear();
        QCoreApplication::processEvents();
    }

    delete mExtentHighlight;
    mExtentHighlight = nullptr;
}
