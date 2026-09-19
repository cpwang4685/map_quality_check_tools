/*--------------SE---------------*/
#include "se_range_export.h"

/*--------------QT---------------*/
#include <QSignalBlocker>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSet>
#include <QDir>
#include <QVariant>
#include <cmath>

/*--------------QGIS---------------*/
#include <qgisinterface.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgsfield.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgscoordinatereferencesystem.h>
#include "qgsgui.h"
namespace {

// 数值是否落在经纬度范围：是则用 6 位小数显示，否则（投影坐标等）用 2 位
int extentPrecision(const QgsRectangle& rect)
{
    const bool looksLikeLatLon =
        rect.xMinimum() >= -180.0 && rect.xMaximum() <= 180.0 &&
        rect.yMinimum() >= -90.0 && rect.yMaximum() <= 90.0 &&
        std::abs(rect.xMaximum() - rect.xMinimum()) <= 360.0 &&
        std::abs(rect.yMaximum() - rect.yMinimum()) <= 180.0;
    return looksLikeLatLon ? 6 : 2;
}

void setBoundEditText(QLineEdit* edit, double value, int precision)
{
    if (edit) edit->setText(QString::number(value, 'f', precision));
}

// 是否输出为单一容器文件（GeoPackage / GDB）；其余（Shapefile）输出到文件夹
bool isSingleFileFormat(const QString& fmt)
{
    return fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive)
        || fmt.contains(QStringLiteral("GDB"), Qt::CaseInsensitive);
}

// 单一容器文件的扩展名（GeoPackage → .gpkg，GDB → .gdb）
QString singleFileSuffix(const QString& fmt)
{
    return fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive)
        ? QStringLiteral(".gpkg") : QStringLiteral(".gdb");
}

} // namespace


CSE_RangeExportDialogUi::CSE_RangeExportDialogUi(QgsMapCanvas* canvas, QgisInterface* iface, QWidget* parent)
    : QDialog(parent)
{
    ui.setupUi(this);
    QgsGui::enableAutoGeometryRestore(this);
    m_pCanvas = canvas;
    m_pIface = iface;

    // 布局拉伸：手动绘制页与要素范围区域两列均分；选择要素页首列标签、次列内容
    ui.gridLayout_draw->setColumnStretch(0, 1);
    ui.gridLayout_draw->setColumnStretch(1, 1);
    ui.gridLayout_feature->setColumnStretch(0, 0);
    ui.gridLayout_feature->setColumnStretch(1, 1);
    ui.gridLayout_featRange->setColumnStretch(0, 1);
    ui.gridLayout_featRange->setColumnStretch(1, 1);

    // "选择要素"页：填充当前地图中的矢量图层列表
    {
        const QSignalBlocker blk(ui.comboBox_layer); // 暂不触发 currentIndexChanged
        if (m_pIface && m_pIface->mapCanvas())
        {
            const QList<QgsMapLayer*>& layers = m_pIface->mapCanvas()->layers();
            for (QgsMapLayer* l : layers)
            {
                if (l->type() == QgsMapLayerType::VectorLayer)
                    ui.comboBox_layer->addItem(l->name(), QVariant::fromValue<void*>((void*)l));
            }
        }
        if (ui.comboBox_layer->count() == 0)
            ui.comboBox_layer->addItem(tr("（无矢量图层）"));
    }
    // 当前图层已就绪：自动读取属性字段与要素值（无需再点击"读取要素"）
    on_comboBox_layer_currentIndexChanged(ui.comboBox_layer->currentIndex());
    clearBoundTextEdits();

    // 默认范围获取方式：选择要素（与 stackedWidget_pages 默认页保持一致）
    setModeIndex(RangeSelectFeature);

    // 默认输出格式为 Shapefile → 输出为"文件夹"，标签随之联动
    on_comboBox_format_currentIndexChanged(ui.comboBox_format->currentIndex());

    // 注意：按钮/单选按钮信号已由 setupUi() 中的 connectSlotsByName 自动连接
    //（on_radioButton_inputCoord_toggled / on_comboBox_field_currentIndexChanged 等），
    // 不需要手动 connect，否则会造成槽函数被调用两次。
}

CSE_RangeExportDialogUi::~CSE_RangeExportDialogUi()
{
    // 释放通过"打开文件..."加载的本地矢量图层
    delete m_pLocalLayer;
    m_pLocalLayer = nullptr;
}

void CSE_RangeExportDialogUi::on_radioButton_inputCoord_toggled(bool checked)
{
    if (checked) ui.stackedWidget_pages->setCurrentIndex(RangeInputCoord);
}

void CSE_RangeExportDialogUi::on_radioButton_manualDraw_toggled(bool checked)
{
    if (checked) ui.stackedWidget_pages->setCurrentIndex(RangeManualDraw);
}

void CSE_RangeExportDialogUi::on_radioButton_selectFeature_toggled(bool checked)
{
    if (checked) ui.stackedWidget_pages->setCurrentIndex(RangeSelectFeature);
}

void CSE_RangeExportDialogUi::on_Button_StartDraw_clicked()
{
    // 标记用户需要在地图上绘制，然后关闭对话框（close 会结束 modal exec()）。
    // 真正的绘制流程在调用方（非模态环境）中完成，绘制成功后重新弹出本对话框并带上已绘制的范围。
    m_bDrawRequested = true;
    accept();
}

// ---------------------------------------------------------------
// 选择要素页：图层 / 字段 / 要素值 联动自动刷新
// ---------------------------------------------------------------
QgsVectorLayer* CSE_RangeExportDialogUi::currentVectorLayer() const
{
    void* p = ui.comboBox_layer->currentData().value<void*>();
    if (!p) return nullptr;
    return qobject_cast<QgsVectorLayer*>(static_cast<QgsMapLayer*>(p));
}

void CSE_RangeExportDialogUi::refreshFeatureFields()
{
    const QSignalBlocker blk(ui.comboBox_field);
    ui.comboBox_field->clear();
    QgsVectorLayer* vl = currentVectorLayer();
    if (!vl) return;
    const QgsFields& fields = vl->fields();
    for (int i = 0; i < fields.count(); ++i)
        ui.comboBox_field->addItem(fields[i].name());
}

void CSE_RangeExportDialogUi::refreshFeatureValues()
{
    QgsVectorLayer* vl = currentVectorLayer();
    if (!vl)
    {
        ui.comboBox_value->clear();
        return;
    }
    const int fieldIdx = ui.comboBox_field->currentIndex();
    if (fieldIdx < 0 || fieldIdx >= vl->fields().count())
    {
        ui.comboBox_value->clear();
        return;
    }

    // 遍历该字段的全部取值（去重排序）；仅取属性，跳过几何数据以加快速度
    QgsFeatureRequest req;
    req.setFlags(QgsFeatureRequest::NoGeometry);
    QgsFeatureIterator it = vl->getFeatures(req);
    QgsFeature f;
    QSet<QString> values;
    while (it.nextFeature(f))
    {
        QVariant v = f.attribute(fieldIdx);
        if (!v.isNull())
            values.insert(v.toString());
    }
    QStringList sorted = values.values();
    sorted.sort();

    const QSignalBlocker blk(ui.comboBox_value); // 填充过程中不触发获取范围
    ui.comboBox_value->clear();
    ui.comboBox_value->addItems(sorted);
}

bool CSE_RangeExportDialogUi::fetchSelectedFeatureExtent(bool warnOnFail)
{
    QgsVectorLayer* vl = currentVectorLayer();
    if (!vl)
    {
        if (warnOnFail)
            QMessageBox::warning(this, tr("按范围导出"), tr("请选择矢量图层。"));
        return false;
    }
    const int fieldIdx = ui.comboBox_field->currentIndex();
    if (fieldIdx < 0 || ui.comboBox_field->currentText().isEmpty()
        || ui.comboBox_value->currentText().isEmpty())
    {
        if (warnOnFail)
            QMessageBox::warning(this, tr("按范围导出"), tr("请先选择属性字段并输入/选择要素值。"));
        return false;
    }
    const QString val = ui.comboBox_value->currentText();

    QgsFeatureRequest req;
    QgsFeatureIterator it = vl->getFeatures(req);
    QgsFeature f;
    bool found = false;
    while (it.nextFeature(f))
    {
        if (f.attribute(fieldIdx).toString() == val)
        {
            // 保存选中要素的原始几何：若为面要素，导出时按该多边形精确裁剪；
            // 若为线/点等，则按 m_rect（其最小外接矩形）裁剪
            m_featureGeom = f.geometry();
            m_rect = m_featureGeom.boundingBox();
            // 记录范围所在坐标系（该要素所在图层的 CRS），便于裁剪前换算到源数据 CRS
            m_extentCrs = vl->crs();
            m_bHasExtentCrs = m_extentCrs.isValid();
            m_bHasExtent = true;
            fillBoundTextEdits(m_rect);
            found = true;
            break;
        }
    }
    if (!found)
    {
        m_bHasExtent = false;
        clearBoundTextEdits();
        if (warnOnFail)
            QMessageBox::warning(this, tr("按范围导出"),
                tr("未找到与“%1”匹配的要素。").arg(val));
        return false;
    }
    return true;
}

void CSE_RangeExportDialogUi::fillBoundTextEdits(const QgsRectangle& rect)
{
    const int prec = extentPrecision(rect);
    // 手动绘制页
    setBoundEditText(ui.lineEdit_drawLatMax, rect.yMaximum(), prec); // 上边界纬度
    setBoundEditText(ui.lineEdit_drawLatMin, rect.yMinimum(), prec); // 下边界纬度
    setBoundEditText(ui.lineEdit_drawLonMin, rect.xMinimum(), prec); // 左边界经度
    setBoundEditText(ui.lineEdit_drawLonMax, rect.xMaximum(), prec); // 右边界经度
    // 选择要素页
    setBoundEditText(ui.lineEdit_featLatMax, rect.yMaximum(), prec);
    setBoundEditText(ui.lineEdit_featLatMin, rect.yMinimum(), prec);
    setBoundEditText(ui.lineEdit_featLonMin, rect.xMinimum(), prec);
    setBoundEditText(ui.lineEdit_featLonMax, rect.xMaximum(), prec);
}

void CSE_RangeExportDialogUi::clearBoundTextEdits()
{
    m_featureGeom = QgsGeometry();
    ui.lineEdit_drawLatMax->clear();
    ui.lineEdit_drawLatMin->clear();
    ui.lineEdit_drawLonMin->clear();
    ui.lineEdit_drawLonMax->clear();
    ui.lineEdit_featLatMax->clear();
    ui.lineEdit_featLatMin->clear();
    ui.lineEdit_featLonMin->clear();
    ui.lineEdit_featLonMax->clear();
}

void CSE_RangeExportDialogUi::on_comboBox_layer_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    refreshFeatureFields();
    refreshFeatureValues();
    clearBoundTextEdits();
}

void CSE_RangeExportDialogUi::on_comboBox_field_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    // 切换属性字段后自动列出该字段的要素值
    refreshFeatureValues();
    clearBoundTextEdits();
}

void CSE_RangeExportDialogUi::on_comboBox_value_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    // 选中要素值后自动获取其外包矩形范围并刷新四组经纬度文本框
    fetchSelectedFeatureExtent(false);
}

void CSE_RangeExportDialogUi::on_Button_OpenLayer_clicked()
{
    QString startDir = m_lastOpenDir;
    if (startDir.isEmpty()) startDir = QDir::homePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("打开本地矢量数据"), startDir,
        tr("矢量数据 (*.shp *.geojson *.json *.kml *.gpkg *.tab *.sqlite *.dxf *.csv);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    m_lastOpenDir = QFileInfo(path).absolutePath();

    QFileInfo fi(path);
    QgsVectorLayer* vl = new QgsVectorLayer(path, fi.completeBaseName(), QStringLiteral("ogr"));
    if (!vl->isValid())
    {
        delete vl;
        QMessageBox::warning(this, tr("打开本地矢量数据"), tr("无法打开矢量数据：\n%1").arg(path));
        return;
    }

    // 释放上一个本地打开的图层
    delete m_pLocalLayer;
    m_pLocalLayer = vl;

    // 移除"（无矢量图层）"占位项
    for (int i = 0; i < ui.comboBox_layer->count(); ++i)
    {
        if (!ui.comboBox_layer->itemData(i).value<void*>())
        {
            ui.comboBox_layer->removeItem(i);
            break;
        }
    }
    // 将本地图层插入列表并选中（blockSignals，稍后统一刷新）
    {
        const QSignalBlocker blk(ui.comboBox_layer);
        ui.comboBox_layer->insertItem(0, vl->name(), QVariant::fromValue<void*>((void*)vl));
        ui.comboBox_layer->setCurrentIndex(0);
    }
    // 加载后自动读取属性字段与要素值
    on_comboBox_layer_currentIndexChanged(ui.comboBox_layer->currentIndex());
}

void CSE_RangeExportDialogUi::on_Button_GetExtent_clicked()
{
    fetchSelectedFeatureExtent(true);
}

// 格式切换时联动输出标签：SHP 输出到文件夹，GPKG/GDB 输出到单一文件
void CSE_RangeExportDialogUi::on_comboBox_format_currentIndexChanged(int /*index*/)
{
    const bool toFile = isSingleFileFormat(ui.comboBox_format->currentText());
    ui.label_output->setText(toFile ? tr("输出文件:") : tr("输出文件夹:"));
}

void CSE_RangeExportDialogUi::on_Button_BrowseOutput_clicked()
{
    const QString fmt = ui.comboBox_format->currentText();
    const bool toFile = isSingleFileFormat(fmt);
    const QString cur = ui.lineEdit_output->text().trimmed();

    if (toFile)
    {
        // GeoPackage / GDB：选择保存文件，所有矢量成果写入该单一文件
        const bool isGpkg = fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive);
        const QString suffix = singleFileSuffix(fmt);
        QString suggest = cur;
        if (suggest.isEmpty())
        {
            suggest = QDir::homePath() + QStringLiteral("/clip_export") + suffix;
        }
        else if (QFileInfo(suggest).isDir())
        {
            suggest = QDir(suggest).filePath(QStringLiteral("clip_export") + suffix);
        }
        else if (!suggest.endsWith(suffix, Qt::CaseInsensitive))
        {
            suggest = QFileInfo(suggest).absolutePath() + "/"
                      + QFileInfo(suggest).completeBaseName() + suffix;
        }

        const QString f = QFileDialog::getSaveFileName(this,
            isGpkg ? tr("选择输出 GeoPackage 文件") : tr("选择输出 GDB 文件"),
            suggest,
            isGpkg ? tr("GeoPackage (*.gpkg)") : tr("文件地理数据库 (*.gdb)"));
        if (f.isEmpty()) return;
        QString finalFile = f;
        if (!finalFile.endsWith(suffix, Qt::CaseInsensitive)) finalFile += suffix;
        ui.lineEdit_output->setText(QDir::toNativeSeparators(finalFile));
    }
    else
    {
        // Shapefile：选择保存文件夹，每个成果在该目录下导出为独立 shp
        QString startDir = cur;
        if (startDir.isEmpty()) startDir = QDir::homePath();
        else if (!QFileInfo(startDir).isDir()) startDir = QFileInfo(startDir).absolutePath();
        if (startDir.isEmpty()) startDir = QDir::homePath();

        const QString dir = QFileDialog::getExistingDirectory(this,
            tr("选择保存文件夹（每个成果导出为独立 shp）"), startDir);
        if (!dir.isEmpty()) ui.lineEdit_output->setText(QDir::toNativeSeparators(dir));
    }
}

void CSE_RangeExportDialogUi::on_Button_OK_clicked()
{
    if (ui.lineEdit_output->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, tr("按范围导出"),
            outputToSingleFile() ? tr("请选择输出文件。") : tr("请选择输出文件夹。"));
        return;
    }

    switch (ui.stackedWidget_pages->currentIndex())
    {
    case RangeInputCoord:
    {
        double minX = ui.doubleSpinBox_minX->value(), minY = ui.doubleSpinBox_minY->value();
        double maxX = ui.doubleSpinBox_maxX->value(), maxY = ui.doubleSpinBox_maxY->value();
        if (minX >= maxX || minY >= maxY)
        {
            QMessageBox::warning(this, tr("按范围导出"), tr("坐标范围不合法，请检查最小/最大值。"));
            return;
        }
        m_bHasExtent = true;
        m_rect = QgsRectangle(minX, minY, maxX, maxY);
        break;
    }
    case RangeManualDraw:
    {
        if (!m_bHasExtent)
        {
            QMessageBox::warning(this, tr("按范围导出"), tr("请先在地图上绘制导出范围。"));
            return;
        }
        break;
    }
    case RangeSelectFeature:
    {
        // 以当前图层/字段/要素值定位要素并确定范围（失败时已弹窗提示）
        if (!fetchSelectedFeatureExtent(true))
            return;
        break;
    }
    }
    accept();
}

void CSE_RangeExportDialogUi::on_Button_Cancel_clicked()
{
    reject();
}

void CSE_RangeExportDialogUi::setExtent(const QgsRectangle& rect)
{
    if (!rect.isNull())
    {
        m_rect = rect;
        m_bHasExtent = true;
        // 同步到坐标输入框
        ui.doubleSpinBox_minX->setValue(rect.xMinimum());
        ui.doubleSpinBox_minY->setValue(rect.yMinimum());
        ui.doubleSpinBox_maxX->setValue(rect.xMaximum());
        ui.doubleSpinBox_maxY->setValue(rect.yMaximum());
        // 用"上/下边界纬度、左/右边界经度"四组文本框显示范围
        fillBoundTextEdits(rect);
    }
}

bool CSE_RangeExportDialogUi::drawRequested() const
{
    return m_bDrawRequested;
}

void CSE_RangeExportDialogUi::resetDrawRequested()
{
    m_bDrawRequested = false;
}

void CSE_RangeExportDialogUi::setModeIndex(int idx)
{
    ui.radioButton_inputCoord->setChecked(idx == RangeInputCoord);
    ui.radioButton_manualDraw->setChecked(idx == RangeManualDraw);
    ui.radioButton_selectFeature->setChecked(idx == RangeSelectFeature);
    // 单选按钮 toggled 槽会同步 stack 页；此处再显式设置保证一致
    ui.stackedWidget_pages->setCurrentIndex(idx);
}

int CSE_RangeExportDialogUi::modeIndex() const
{
    if (ui.radioButton_manualDraw->isChecked())
        return RangeManualDraw;
    if (ui.radioButton_selectFeature->isChecked())
        return RangeSelectFeature;
    return RangeInputCoord;
}

void CSE_RangeExportDialogUi::setAutoLoadAfterExport(bool on)
{
    ui.checkBox_autoLoad->setChecked(on);
}

// 是否在导出后自动加载结果到地图（默认 true）
bool CSE_RangeExportDialogUi::autoLoadAfterExport() const
{
    return ui.checkBox_autoLoad->isChecked();
}

QgsRectangle CSE_RangeExportDialogUi::exportExtent() const { return m_rect; }
bool CSE_RangeExportDialogUi::hasExtent() const { return m_bHasExtent; }
QString CSE_RangeExportDialogUi::outputPath() const { return ui.lineEdit_output->text(); }

void CSE_RangeExportDialogUi::setOutputPath(const QString& path)
{
    ui.lineEdit_output->setText(path);
}

QString CSE_RangeExportDialogUi::format() const
{
    return ui.comboBox_format->currentText();
}

int CSE_RangeExportDialogUi::formatIndex() const
{
    return ui.comboBox_format->currentIndex();
}

void CSE_RangeExportDialogUi::setFormatIndex(int idx)
{
    if (idx < 0 || idx >= ui.comboBox_format->count()) return;
    ui.comboBox_format->setCurrentIndex(idx);
    // 保证输出标签与格式一致（blockSignals 等场景下不依赖信号）
    on_comboBox_format_currentIndexChanged(idx);
}

bool CSE_RangeExportDialogUi::outputToSingleFile() const
{
    return isSingleFileFormat(ui.comboBox_format->currentText());
}

void CSE_RangeExportDialogUi::setExtentCrs(const QgsCoordinateReferenceSystem& crs)
{
    m_extentCrs = crs;
    m_bHasExtentCrs = crs.isValid();
}

QgsCoordinateReferenceSystem CSE_RangeExportDialogUi::exportExtentCrs() const
{
    return m_bHasExtentCrs ? m_extentCrs : QgsCoordinateReferenceSystem();
}

bool CSE_RangeExportDialogUi::hasFeatureGeometry() const
{
    return !m_featureGeom.isNull();
}

QgsGeometry CSE_RangeExportDialogUi::exportFeatureGeometry() const
{
    return m_featureGeom;
}
