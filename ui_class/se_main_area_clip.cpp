/*--------------SE---------------*/
#include "se_main_area_clip.h"

/*--------------QT---------------*/
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QListWidget>
#include <QListWidgetItem>
#include <cmath>
#include <algorithm>

/*--------------QGIS---------------*/
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeatureid.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgscoordinatetransform.h>
#include <qgscoordinatereferencesystem.h>
#include <qgspointxy.h>
#include <qgsproject.h>
#include <qgsunittypes.h>
#include "qgsgui.h"

// ISO A 系列纸张（宽=长边、高=短边，单位 mm）。
// 注意：数组顺序与"纸张尺寸"下拉框前 5 项一致（A0~A4，索引 0~4），
// 索引 5 为"自定义"
struct SeIsoPaper { const char* name; double w; double h; };
static const SeIsoPaper kSeIsoPapers[] = {
    { "A0", 1189, 841 },
    { "A1", 841, 594 },
    { "A2", 594, 420 },
    { "A3", 420, 297 },
    { "A4", 297, 210 },
};

// mm 数值格式化：整数不带小数，否则保留 1 位小数
static QString seFormatMm(double v)
{
    const double r = std::floor(v + 0.5);
    if (std::fabs(v - r) < 0.05)
        return QString::number(static_cast<long long>(r));
    return QString::number(v, 'f', 1);
}

// 把主区(地面)范围换算为米制宽/高：主区为经纬度时按 Web 墨卡托投影换算，
// 其余按 CRS 长度单位换算。返回 false 表示范围无效
static bool seGroundSizeMeters(const QgsRectangle& rect,
    const QgsCoordinateReferenceSystem& crs, double& outW, double& outH)
{
    if (rect.isEmpty()) return false;

    double w = rect.width();
    double h = rect.height();

    if (crs.isValid())
    {
        const QgsUnitTypes::DistanceUnit u = crs.mapUnits();
        if (u == QgsUnitTypes::DistanceDegrees)
        {
            QgsCoordinateTransform tr(crs,
                QgsCoordinateReferenceSystem::fromEpsgId(3857), QgsProject::instance());
            if (tr.isValid())
            {
                QgsRectangle mRect;
                try { mRect = tr.transformBoundingBox(rect); }
                catch (...) { }
                if (!mRect.isEmpty()) { w = mRect.width(); h = mRect.height(); }
            }
        }
        else if (u == QgsUnitTypes::DistanceFeet)          { w *= 0.3048;   h *= 0.3048;   }
        else if (u == QgsUnitTypes::DistanceMiles)         { w *= 1609.344; h *= 1609.344; }
        else if (u == QgsUnitTypes::DistanceNauticalMiles) { w *= 1852.0;   h *= 1852.0;   }
        else if (u == QgsUnitTypes::DistanceKilometers)    { w *= 1000.0;   h *= 1000.0;   }
    }

    if (w <= 0 || h <= 0) return false;
    outW = w;
    outH = h;
    return true;
}

// 主区 CRS 长度单位 → 米的换算系数（仅用于平面 CRS）
static double seUnitsPerMeter(const QgsCoordinateReferenceSystem& crs)
{
    if (!crs.isValid()) return 1.0;
    switch (crs.mapUnits())
    {
    case QgsUnitTypes::DistanceFeet:          return 1.0 / 0.3048;
    case QgsUnitTypes::DistanceMiles:         return 1.0 / 1609.344;
    case QgsUnitTypes::DistanceNauticalMiles: return 1.0 / 1852.0;
    case QgsUnitTypes::DistanceKilometers:    return 0.001;
    default:                                  return 1.0;   // 米及其他（按米处理）
    }
}


CSE_MainAreaClipDialogUi::CSE_MainAreaClipDialogUi(QWidget* parent)
    : QDialog(parent)
{
    ui.setupUi(this);
    QgsGui::enableAutoGeometryRestore(this);
    // 注意：按钮/下拉框/文本框信号已由 setupUi() 中的 connectSlotsByName 自动连接
    //（on_Button_BrowseMainArea_clicked / on_comboBox_field_currentIndexChanged /
    //  on_comboBox_paperSize_currentIndexChanged / on_lineEdit_paperW_textChanged /
    //  on_listWidget_features_itemSelectionChanged 等），
    // 不需要手动 connect，否则会造成槽函数被调用两次。

    // 初始状态：
    // 1) 默认纸张 A2（横向：宽=长边 594、高=短边 420），内图廓与纸张保持一致
    syncInnerFrameToPaper();
    // 3) 默认格式为 Shapefile → 输出为"文件夹"，标签随之联动
    on_comboBox_format_currentIndexChanged(ui.comboBox_format->currentIndex());
    // 4) 制图比例尺：尚未选择主区要素时为空
    refreshCartographicScale();
}

CSE_MainAreaClipDialogUi::~CSE_MainAreaClipDialogUi()
{
    if (m_pMainAreaLayer) delete m_pMainAreaLayer;
}

void CSE_MainAreaClipDialogUi::on_Button_BrowseMainArea_clicked()
{
    QString f = QFileDialog::getOpenFileName(this, tr("选择主区矢量数据"), QString(),
        tr("矢量数据 (*.shp *.geojson *.json);;所有文件 (*.*)"));
    if (!f.isEmpty())
    {
        ui.lineEdit_mainArea->setText(f);
        loadMainAreaFields();
    }
}

void CSE_MainAreaClipDialogUi::loadMainAreaFields()
{
    ui.comboBox_field->blockSignals(true);
    ui.comboBox_field->clear();
    ui.listWidget_features->clear();
    // 清掉旧缓存
    if (m_pMainAreaLayer)
    {
        delete m_pMainAreaLayer;
        m_pMainAreaLayer = nullptr;
    }
    QString path = ui.lineEdit_mainArea->text();
    if (path.isEmpty() || !QFileInfo::exists(path))
    {
        ui.comboBox_field->blockSignals(false);
        return;
    }
    QgsVectorLayer* pLayer = new QgsVectorLayer(path, QFileInfo(path).completeBaseName(), "ogr");
    if (!pLayer || !pLayer->isValid())
    {
        if (pLayer) delete pLayer;
        ui.comboBox_field->blockSignals(false);
        return;
    }
    const QgsFields flds = pLayer->fields();
    for (int i = 0; i < flds.count(); ++i)
        ui.comboBox_field->addItem(flds[i].name(), flds[i].name());

    // 把 layer 缓存为成员，标识字段切换时重新填充列表，不用每次打开 shp
    m_pMainAreaLayer = pLayer;

    ui.comboBox_field->blockSignals(false);
    reloadFeatures();
}

void CSE_MainAreaClipDialogUi::reloadFeatures()
{
    ui.listWidget_features->clear();
    if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
        return;

    const QString fieldName = ui.comboBox_field->currentText();
    if (fieldName.isEmpty())
        return;

    // 在图层字段中查找当前标识字段对应的索引
    int idxField = m_pMainAreaLayer->fields().lookupField(fieldName);
    if (idxField < 0)
        return;

    // 枚举全部要素，按当前字段构造显示文本：
    //   [序号] <fieldValue>   （首行）
    //   如果 fid 转 QString 后和上面不同，作为副标题（tooltip）
    QgsFeatureIterator it = m_pMainAreaLayer->getFeatures();
    QgsFeature feat;
    int dispIdx = 0;
    while (it.nextFeature(feat))
    {
        QString fid = QString::number(feat.id());
        QString name = feat.attribute(idxField).toString();
        QString label = tr("[%1] ").arg(dispIdx) + (name.isEmpty() ? fid : name);
        QListWidgetItem* item = new QListWidgetItem(label, ui.listWidget_features);
        item->setData(Qt::UserRole, fid);
        item->setToolTip(tr("fid=%1，标识字段=%2").arg(fid).arg(name));
        ++dispIdx;
    }
}

void CSE_MainAreaClipDialogUi::on_comboBox_field_currentIndexChanged(int /*index*/)
{
    reloadFeatures();
}

// 格式切换时联动输出标签：SHP 输出到文件夹，GPKG/GDB 输出到单一文件
void CSE_MainAreaClipDialogUi::on_comboBox_format_currentIndexChanged(int /*index*/)
{
    const QString fmt = ui.comboBox_format->currentText();
    const bool toFile = fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive)
                        || fmt.contains(QStringLiteral("GDB"), Qt::CaseInsensitive);
    ui.label_output->setText(toFile ? tr("输出文件：") : tr("输出文件夹："));
}

void CSE_MainAreaClipDialogUi::on_Button_BrowseOutput_clicked()
{
    const QString fmt = ui.comboBox_format->currentText();
    const bool toFile = fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive)
                        || fmt.contains(QStringLiteral("GDB"), Qt::CaseInsensitive);

    // 建议路径：<源文件目录>/<源文件名>_mainArea
    const QFileInfo src(d_srcPath);
    const QString baseDir = src.absolutePath();
    QString suggestBase = baseDir;
    if (!suggestBase.isEmpty()) suggestBase += QLatin1String("/");
    suggestBase += src.completeBaseName().isEmpty()
        ? QStringLiteral("clip") : src.completeBaseName();
    suggestBase += QStringLiteral("_mainArea");
    const QString cur = ui.lineEdit_output->text().trimmed();

    if (toFile)
    {
        // GPKG / GDB：选择保存文件，所有裁切成果写入该单一文件
        const bool isGpkg = fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive);
        const QString suffix = isGpkg ? QStringLiteral(".gpkg") : QStringLiteral(".gdb");
        QString suggest = cur;
        if (suggest.isEmpty() || QFileInfo(suggest).isDir())
            suggest = suggestBase + suffix;
        else if (!suggest.endsWith(suffix, Qt::CaseInsensitive))
            suggest = QFileInfo(suggest).absolutePath() + "/"
                      + QFileInfo(suggest).completeBaseName() + suffix;

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
        // SHP：选择保存文件夹，每个成果在该目录下导出为独立 shp
        QString startDir = cur;
        if (startDir.isEmpty()) startDir = baseDir;
        else if (!QFileInfo(startDir).isDir()) startDir = QFileInfo(startDir).absolutePath();
        if (startDir.isEmpty()) startDir = QDir::homePath();

        const QString dir = QFileDialog::getExistingDirectory(this,
            tr("选择保存文件夹（每个成果导出为独立 shp）"), startDir);
        if (!dir.isEmpty())
            ui.lineEdit_output->setText(QDir::toNativeSeparators(dir));
    }
}

// 内图廓宽/高与纸张宽/高保持一致（纸张宽/高文本框变化时联动）
void CSE_MainAreaClipDialogUi::syncInnerFrameToPaper()
{
    const double w = paperWidthMm();
    const double h = paperHeightMm();
    if (w <= 0 || h <= 0) return;
    ui.lineEdit_innerW->setText(seFormatMm(w));
    ui.lineEdit_innerH->setText(seFormatMm(h));
    refreshCartographicScale();
}

// 纸张尺寸下拉框切换：A0~A4 按当前纸张方向填充纸张宽/高文本框
//（"自动"方向按横向处理：宽=长边、高=短边）；"自定义"不改数值，
// 由用户直接编辑纸张宽/高。
// 填充期间屏蔽文本框信号——否则"宽已更新、高还是旧值"的中间状态会被
// autoSwitchPaperSizeToCustom 误判为非标准纸张，把下拉框切到"自定义"；
// 填充完成后手动联动内图廓宽/高并刷新制图比例尺
void CSE_MainAreaClipDialogUi::on_comboBox_paperSize_currentIndexChanged(int index)
{
    if (index < 0 || index >= int(sizeof(kSeIsoPapers) / sizeof(kSeIsoPapers[0])))
        return;   // "自定义"：保留当前数值供编辑

    // 当前方向：纵向（索引 1）时宽=短边、高=长边；横向/自动时宽=长边、高=短边
    const bool portrait = (ui.comboBox_orient->currentIndex() == 1);
    const double paperW = portrait ? kSeIsoPapers[index].h : kSeIsoPapers[index].w;
    const double paperH = portrait ? kSeIsoPapers[index].w : kSeIsoPapers[index].h;

    ui.lineEdit_paperW->blockSignals(true);
    ui.lineEdit_paperH->blockSignals(true);
    ui.lineEdit_paperW->setText(seFormatMm(paperW));
    ui.lineEdit_paperH->setText(seFormatMm(paperH));
    ui.lineEdit_paperW->blockSignals(false);
    ui.lineEdit_paperH->blockSignals(false);

    // 内图廓宽/高联动保持一致，制图比例尺同步刷新
    syncInnerFrameToPaper();
}

// 纸张宽度/高度文本框（可编辑）：变化时联动内图廓并刷新制图比例尺；
// 若当前选的是 A0~A4 而手工改动后的宽/高与该纸张标准值不符，
// 纸张尺寸下拉框自动切换为"自定义"
void CSE_MainAreaClipDialogUi::on_lineEdit_paperW_textChanged(const QString& /*text*/)
{
    syncInnerFrameToPaper();
    autoSwitchPaperSizeToCustom();
}

void CSE_MainAreaClipDialogUi::on_lineEdit_paperH_textChanged(const QString& /*text*/)
{
    syncInnerFrameToPaper();
    autoSwitchPaperSizeToCustom();
}

// 纸张方向变化：横向要求宽≥高、纵向要求宽≤高，必要时交换纸张宽/高文本框。
// 交换期间屏蔽文本框信号（避免中间状态被误判为"自定义"），
// 交换后手动联动内图廓并刷新制图比例尺；若交换后恰为标准 ISO 纸张则恢复其名称
void CSE_MainAreaClipDialogUi::on_comboBox_orient_currentIndexChanged(int index)
{
    if (index != 1 && index != 2) return;   // 0=自动：不干预

    const double w = paperWidthMm();
    const double h = paperHeightMm();
    if (w <= 0 || h <= 0 || w == h) return;

    const bool landscape = (w > h);
    if ((index == 2 && !landscape) || (index == 1 && landscape))
    {
        ui.lineEdit_paperW->blockSignals(true);
        ui.lineEdit_paperH->blockSignals(true);
        ui.lineEdit_paperW->setText(seFormatMm(h));
        ui.lineEdit_paperH->setText(seFormatMm(w));
        ui.lineEdit_paperW->blockSignals(false);
        ui.lineEdit_paperH->blockSignals(false);

        syncInnerFrameToPaper();     // 内图廓联动 + 制图比例尺刷新
        autoRestoreIsoPaperSize();   // 恰为 ISO 纸张时恢复对应纸张名
    }
}

// 当前已选要素的合并外包矩形（在主区 layer 的原始 CRS 下）
QgsRectangle CSE_MainAreaClipDialogUi::selectedMainAreaRect() const
{
    QgsRectangle rect;
    if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
        return rect;
    const int n = ui.listWidget_features->count();
    QgsFeatureIds idSet;
    for (int i = 0; i < n; ++i)
    {
        QListWidgetItem* item = ui.listWidget_features->item(i);
        if (item && item->isSelected())
        {
            bool ok = false;
            long long v = item->data(Qt::UserRole).toString().toLongLong(&ok);
            if (ok) idSet.insert(v);
        }
    }
    if (idSet.isEmpty())
        return rect;

    QgsFeatureRequest req;
    req.setFilterFids(idSet);
    QgsFeatureIterator it = m_pMainAreaLayer->getFeatures(req);
    QgsFeature feat;
    bool first = true;
    while (it.nextFeature(feat))
    {
        QgsGeometry g = feat.geometry();
        if (g.isNull()) continue;
        QgsRectangle b = g.boundingBox();
        if (b.isEmpty()) continue;
        if (first) { rect = b; first = false; }
        else rect.combineExtentWith(b);
    }
    return rect;
}

// 当前生效的纸张宽/高(mm)：读取"纸张宽度/高度"文本框（选择 A0~A4 后自动填充，可编辑）
double CSE_MainAreaClipDialogUi::paperWidthMm() const
{
    return ui.lineEdit_paperW->text().trimmed().toDouble();
}

double CSE_MainAreaClipDialogUi::paperHeightMm() const
{
    return ui.lineEdit_paperH->text().trimmed().toDouble();
}

// 当前宽/高是否与某个 ISO 纸张（按当前方向）的标准值一致；一致时返回索引，否则 -1
int CSE_MainAreaClipDialogUi::matchingIsoPaperIndex() const
{
    const double w = paperWidthMm();
    const double h = paperHeightMm();
    if (w <= 0 || h <= 0) return -1;

    const int paperCount = int(sizeof(kSeIsoPapers) / sizeof(kSeIsoPapers[0]));
    for (int i = 0; i < paperCount; ++i)
    {
        const double longEdge  = std::max(kSeIsoPapers[i].w, kSeIsoPapers[i].h);
        const double shortEdge = std::min(kSeIsoPapers[i].w, kSeIsoPapers[i].h);
        if (std::fabs(std::max(w, h) - longEdge) < 0.05
            && std::fabs(std::min(w, h) - shortEdge) < 0.05)
            return i;
    }
    return -1;
}

// 手工编辑纸张宽/高后，若与当前所选 A0~A4 的标准值不符，纸张尺寸下拉框自动切到"自定义"
void CSE_MainAreaClipDialogUi::autoSwitchPaperSizeToCustom()
{
    const int idx = ui.comboBox_paperSize->currentIndex();
    if (idx < 0 || idx >= int(sizeof(kSeIsoPapers) / sizeof(kSeIsoPapers[0])))
        return;   // 已是"自定义"
    if (matchingIsoPaperIndex() != idx)
    {
        ui.comboBox_paperSize->blockSignals(true);
        ui.comboBox_paperSize->setCurrentIndex(
            int(sizeof(kSeIsoPapers) / sizeof(kSeIsoPapers[0])));   // "自定义"
        ui.comboBox_paperSize->blockSignals(false);
    }
}

// 交换宽/高等程序性修改后：若宽/高恰与某 ISO 纸张一致，恢复对应纸张名（避免误标"自定义"）
void CSE_MainAreaClipDialogUi::autoRestoreIsoPaperSize()
{
    const int matched = matchingIsoPaperIndex();
    if (matched < 0) return;
    if (ui.comboBox_paperSize->currentIndex() == matched) return;
    ui.comboBox_paperSize->blockSignals(true);
    ui.comboBox_paperSize->setCurrentIndex(matched);
    ui.comboBox_paperSize->blockSignals(false);
}

// 制图比例尺分母 = 主区范围经投影后的经线方向（南北/高度方向）地面长度 ÷
// 纸张经线方向图上长度。未选主区要素或纸张无效时返回 0
double CSE_MainAreaClipDialogUi::cartographicScaleDenominator() const
{
    const double paperH_m = paperHeightMm() / 1000.0;
    if (paperH_m <= 0) return 0.0;

    const QgsCoordinateReferenceSystem crs =
        (m_pMainAreaLayer && m_pMainAreaLayer->isValid())
            ? m_pMainAreaLayer->crs() : QgsCoordinateReferenceSystem();

    double groundW = 0, groundH = 0;
    if (!seGroundSizeMeters(selectedMainAreaRect(), crs, groundW, groundH))
        return 0.0;

    return groundH / paperH_m;   // 经线方向（高度方向）
}

// 刷新"制图比例尺"文本框显示（形如 1:12345）
void CSE_MainAreaClipDialogUi::refreshCartographicScale()
{
    const double s = cartographicScaleDenominator();
    ui.lineEdit_cartScale->setText(s > 0.0
        ? QStringLiteral("1:%1").arg(QString::number(s, 'f', 0))
        : QString());
}

void CSE_MainAreaClipDialogUi::on_listWidget_features_itemSelectionChanged()
{
    refreshCartographicScale();
}

// 依据"推荐比例尺（或自定义比例尺）+ 内图廓宽/高"计算裁切范围，并尽量使主区居中：
//   裁切地面尺寸(米) = 比例尺 × 内图廓尺寸(mm) / 1000
// 裁切范围与主区空间参考系保持一致：主区为经纬度时先在 Web 墨卡托（米）下按
// 主区中心构造范围，再换算回主区 CRS；平面 CRS 直接把米换算为 CRS 长度单位。
// 比例尺或内图廓无效时回退为主区所选要素的外包矩形
QgsRectangle CSE_MainAreaClipDialogUi::computedClipRect(QgsCoordinateReferenceSystem* outCrs) const
{
    const QgsRectangle area = selectedMainAreaRect();
    const QgsCoordinateReferenceSystem crs =
        (m_pMainAreaLayer && m_pMainAreaLayer->isValid())
            ? m_pMainAreaLayer->crs() : QgsCoordinateReferenceSystem();
    if (outCrs && crs.isValid()) *outCrs = crs;
    if (area.isEmpty()) return area;

    const double scale = mapScaleDenominator();
    const double innerW = innerMapFrameWidthMm();
    const double innerH = innerMapFrameHeightMm();
    if (scale <= 0 || innerW <= 0 || innerH <= 0)
        return area;    // 参数不足 → 回退外包矩形

    const double groundW = scale * innerW / 1000.0;  // 裁切地面宽(米)
    const double groundH = scale * innerH / 1000.0;  // 裁切地面高(米)

    // 经纬度 CRS：投影到米制取中心并按米构造范围，再换算回原 CRS
    if (crs.isValid() && crs.mapUnits() == QgsUnitTypes::DistanceDegrees)
    {
        const QgsCoordinateReferenceSystem mercator =
            QgsCoordinateReferenceSystem::fromEpsgId(3857);
        QgsCoordinateTransform toMercator(crs, mercator, QgsProject::instance());
        QgsCoordinateTransform toSource(mercator, crs, QgsProject::instance());
        if (!toMercator.isValid() || !toSource.isValid()) return area;

        QgsRectangle mRect;
        try { mRect = toMercator.transformBoundingBox(area); }
        catch (...) { return area; }
        if (mRect.isEmpty()) return area;

        const QgsPointXY c = mRect.center();
        const QgsRectangle mClip(c.x() - groundW / 2.0, c.y() - groundH / 2.0,
                                 c.x() + groundW / 2.0, c.y() + groundH / 2.0);
        QgsRectangle back;
        try { back = toSource.transformBoundingBox(mClip); }
        catch (...) { return area; }
        return back.isEmpty() ? area : back;
    }

    // 平面 CRS：米 → CRS 长度单位
    const double unitsPerMeter = seUnitsPerMeter(crs);
    const double w = groundW * unitsPerMeter;
    const double h = groundH * unitsPerMeter;
    const QgsPointXY c = area.center();
    return QgsRectangle(c.x() - w / 2.0, c.y() - h / 2.0,
                        c.x() + w / 2.0, c.y() + h / 2.0);
}

// "计算"按钮：依据主区范围自动确定纸张方向（不改纸张尺寸）
//   方向：地面宽 ≥ 高 → 横向；接近正方形（宽/高 ≥ 0.95）时同样优先横向
// 切换方向后由 on_comboBox_orient_currentIndexChanged 负责校正
// 纸张宽/高顺序（必要时交换），并联动内图廓宽/高与制图比例尺
void CSE_MainAreaClipDialogUi::computePaperOrientation()
{
    const QgsRectangle rect = selectedMainAreaRect();
    if (rect.isEmpty())
    {
        QMessageBox::information(this, tr("计算纸张方向"),
            tr("请先在要素列表中选中至少一个要素作为主区，再点击计算。"));
        return;
    }

    const QgsCoordinateReferenceSystem crs =
        (m_pMainAreaLayer && m_pMainAreaLayer->isValid())
            ? m_pMainAreaLayer->crs() : QgsCoordinateReferenceSystem();

    double groundW = 0, groundH = 0;
    if (!seGroundSizeMeters(rect, crs, groundW, groundH))
    {
        QMessageBox::warning(this, tr("计算纸张方向"), tr("主区范围无效，无法计算。"));
        return;
    }

    // 方向：宽 ≥ 高 → 横向；接近正方形时优先横向
    const double kAlmostSquare = 0.95;  // 地面宽/高比 ≥ 0.95 视为接近正方形
    const bool landscape = (groundW >= groundH * kAlmostSquare);

    // 索引：0=自动 1=纵向 2=横向
    ui.comboBox_orient->setCurrentIndex(landscape ? 2 : 1);

    QMessageBox::information(this, tr("计算纸张方向"),
        tr("主区地面范围：%1 m × %2 m\n纸张方向：%3")
        .arg(QString::number(groundW, 'f', 1))
        .arg(QString::number(groundH, 'f', 1))
        .arg(landscape ? tr("横向") : tr("纵向")));
}

void CSE_MainAreaClipDialogUi::on_Button_ComputeOrient_clicked()
{
    computePaperOrientation();
}

void CSE_MainAreaClipDialogUi::on_Button_Export_clicked()
{
    if (ui.lineEdit_mainArea->text().isEmpty())
    {
        QMessageBox::warning(this, tr("按制图导出"), tr("请先选择主区数据。"));
        return;
    }
    if (ui.listWidget_features->selectedItems().isEmpty())
    {
        QMessageBox::warning(this, tr("按制图导出"), tr("请至少选择一个要素作为主区。"));
        return;
    }
    if (mapScaleDenominator() <= 0)
    {
        QMessageBox::warning(this, tr("按制图导出"), tr("比例尺无效，请设置推荐比例尺或自定义比例尺。"));
        return;
    }
    if (innerMapFrameWidthMm() <= 0 || innerMapFrameHeightMm() <= 0)
    {
        QMessageBox::warning(this, tr("按制图导出"), tr("内图廓宽/高无效，请设置内图廓尺寸(mm)。"));
        return;
    }
    if (ui.lineEdit_output->text().isEmpty())
    {
        const QString fmt = ui.comboBox_format->currentText();
        const bool toFile = fmt.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive)
                            || fmt.contains(QStringLiteral("GDB"), Qt::CaseInsensitive);
        QMessageBox::warning(this, tr("按制图导出"),
            toFile ? tr("请设置输出文件路径。") : tr("请设置输出文件夹路径。"));
        return;
    }
    accept();
}

void CSE_MainAreaClipDialogUi::on_Button_Cancel_clicked()
{
    reject();
}

QString    CSE_MainAreaClipDialogUi::mainAreaPath() const { return ui.lineEdit_mainArea->text(); }
QString    CSE_MainAreaClipDialogUi::mainAreaField() const { return ui.comboBox_field->currentText(); }
QStringList CSE_MainAreaClipDialogUi::selectedFeatureIds() const
{
    QStringList ids;
    for (QListWidgetItem* it : ui.listWidget_features->selectedItems())
        ids << it->data(Qt::UserRole).toString();
    return ids;
}
QString CSE_MainAreaClipDialogUi::outputFilePath() const { return ui.lineEdit_output->text(); }
QString CSE_MainAreaClipDialogUi::encoding() const { return ui.comboBox_encoding->currentText(); }
QString CSE_MainAreaClipDialogUi::format() const { return ui.comboBox_format->currentText(); }
bool    CSE_MainAreaClipDialogUi::overwriteExisting() const { return ui.checkBox_overwrite->isChecked(); }
QString CSE_MainAreaClipDialogUi::paperSize() const { return ui.comboBox_paperSize->currentText(); }
QString CSE_MainAreaClipDialogUi::paperOrient() const { return ui.comboBox_orient->currentText(); }

// 生效比例尺字符串：读推荐比例尺下拉框（可编辑，可直接手填自定义比例尺分母）
QString CSE_MainAreaClipDialogUi::standardScale() const
{
    return ui.comboBox_standardScale->currentText().trimmed();
}

double CSE_MainAreaClipDialogUi::mapScaleDenominator() const
{
    return standardScale().toDouble();
}

// 内图廓尺寸(mm)：不再有"启用内图廓"开关，始终取输入值
double CSE_MainAreaClipDialogUi::innerMapFrameWidthMm() const
{
    return ui.lineEdit_innerW->text().toDouble();
}
double CSE_MainAreaClipDialogUi::innerMapFrameHeightMm() const
{
    return ui.lineEdit_innerH->text().toDouble();
}

void CSE_MainAreaClipDialogUi::setAutoLoadAfterExport(bool on)
{
    ui.checkBox_autoLoad->setChecked(on);
}

// 是否在导出后自动加载结果到地图（默认 true）
bool CSE_MainAreaClipDialogUi::autoLoadAfterExport() const
{
    return ui.checkBox_autoLoad->isChecked();
}


// ============================================================
// 从主区矢量读取所选要素的合并外包矩形，作为统一裁剪范围
// ============================================================
QgsRectangle computeMainAreaClipRect(const QString& mainAreaPath,
    const QStringList& featureIds, QgsCoordinateReferenceSystem* outCrs)
{
    QgsRectangle rect;
    if (mainAreaPath.isEmpty() || !QFileInfo::exists(mainAreaPath)) return rect;
    QgsVectorLayer* pLayer = new QgsVectorLayer(mainAreaPath,
        QFileInfo(mainAreaPath).completeBaseName(), "ogr");
    if (!pLayer || !pLayer->isValid())
    {
        if (pLayer) delete pLayer;
        return rect;
    }

    // 将字符串 ID 集合转换为 long long 集合用于 feature 请求
    QgsFeatureIds idSet;
    for (const QString& s : featureIds)
    {
        bool ok = false;
        long long v = s.toLongLong(&ok);
        if (ok) idSet.insert(v);
    }

    QgsFeatureRequest req;
    if (!idSet.isEmpty())
        req.setFilterFids(idSet);

    QgsFeatureIterator it = pLayer->getFeatures(req);
    QgsFeature feat;
    bool first = true;
    while (it.nextFeature(feat))
    {
        QgsGeometry g = feat.geometry();
        if (g.isNull()) continue;
        QgsRectangle b = g.boundingBox();
        if (b.isEmpty()) continue;
        if (first) { rect = b; first = false; }
        else rect.combineExtentWith(b);
    }
    // 输出矩形所在的主区 layer 原始 CRS，供裁剪时把范围正确转换到源数据 CRS
    if (outCrs && pLayer->crs().isValid())
        *outCrs = pLayer->crs();
    pLayer->deleteLater();
    return rect;
}
