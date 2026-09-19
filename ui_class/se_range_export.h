#ifndef SE_RANGE_EXPORT_H
#define SE_RANGE_EXPORT_H

#include <QDialog>
#include <QString>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsgeometry.h>

#include "ui_range_export.h"

class QgisInterface;
class QgsMapCanvas;
class QgsVectorLayer;

// ============================================================
// 「按范围导出」对话框（.ui 表单版）
//
// 范围获取方式通过单选按钮 radioButton_inputCoord / radioButton_manualDraw
// / radioButton_selectFeature 选择（默认方式为"选择要素"）：
//   1) 输入坐标：填写外包矩形 X/Y 最小值、最大值；
//   2) 手动绘制：点击"开始绘制"后对话框关闭，由调用方最小化所在窗口，
//                让用户在 QGIS 主界面地图上拖出矩形区域，绘制成功后重新
//                弹出本对话框并回填已绘制范围（手动绘制仅支持矩形），
//                范围以"上/下边界纬度、左/右边界经度"四组文本框显示；
//   3) 选择要素：从地图矢量图层中选择，或点击"打开文件..."加载本地矢量
//                数据；加载后自动读取属性字段，切换属性字段自动列出要素
//                值，选择要素值后自动获取其外包矩形，同样以四组经纬度
//                文本框显示范围信息。
// 输出格式与路径（参考"按主区裁切导出"）：
//   - Shapefile：输出路径为"保存文件夹"，调用方将所选产品按产品名自动命名后
//     全部保存到该目录（每个成果一个 shp）；
//   - GeoPackage / GDB：输出路径为"保存文件"，所有矢量成果作为独立图层写入该
//     单一容器文件；栅格无法写入容器，仍按产品名输出 .tif 到该文件所在目录。
//
// 典型调用（由调用方在非模态环境驱动）：
//   CSE_RangeExportDialogUi dlg(canvas, iface, parent);
//   dlg.resetDrawRequested();
//   if (dlg.exec() == QDialog::Accepted && dlg.drawRequested())
//   {
//       // 最小化父窗口 → 地图上矩形绘制 → 恢复父窗口
//       QgsRectangle r;
//       if (MapExtentDrawTool::drawExtentOnCanvas(canvas, MapExtentDrawTool::DrawRect, r))
//       {
//           dlg.setExtent(r);
//           dlg.setModeIndex(CSE_RangeExportDialogUi::RangeManualDraw);
//           if (dlg.exec() == QDialog::Accepted) { /* 用 dlg.exportExtent() + dlg.outputPath() */ }
//       }
//   }
// ============================================================
class CSE_RangeExportDialogUi : public QDialog
{
    Q_OBJECT

public:
    // 范围获取方式（与 stackedWidget_pages 页下标一致）
    enum RangeMode
    {
        RangeInputCoord = 0,    // 输入坐标
        RangeManualDraw = 1,    // 手动绘制
        RangeSelectFeature = 2  // 选择要素
    };

    // canvas/iface 用于"选择要素"页的矢量图层列表；纯坐标输入场景可传 nullptr
    CSE_RangeExportDialogUi(QgsMapCanvas* canvas, QgisInterface* iface, QWidget* parent = nullptr);
    ~CSE_RangeExportDialogUi() override;

    // 当前生效的导出范围（仅 hasExtent() 为 true 时有效）
    QgsRectangle exportExtent() const;
    bool hasExtent() const;

    // 输出格式（与 comboBox_format 选项一致，参考"按主区裁切导出"）：
    //   "ESRI Shapefile (.shp)" → 输出到文件夹，每个成果导出为独立 shp；
    //   "GeoPackage (.gpkg)" / "GDB (.gdb)" → 输出为单一容器文件，所有矢量成果
    //     作为独立图层写入该文件（栅格无法写入容器，仍按产品名输出 .tif 到同目录）。
    QString format() const;
    // 恢复/读取格式下拉框选择（手动绘制后重新弹出对话框时保持上下文一致）
    void setFormatIndex(int idx);
    int formatIndex() const;
    // 当前格式是否输出为单一文件（GeoPackage / GDB）
    bool outputToSingleFile() const;

    // 输出路径：Shapefile 为保存文件夹（所有图层按产品名自动命名保存到该目录下）；
    // GeoPackage / GDB 为保存文件（所有矢量成果写入该单一文件）
    QString outputPath() const;
    void setOutputPath(const QString& path);

    // 导出范围所在坐标系（裁剪时用于换算到源数据 CRS）。
    // - 手动绘制：调用方绘制成功后由 setExtentCrs() 写入地图画布坐标系；
    // - 选择要素：获取要素范围时自动记录该要素所在图层的坐标系；
    // - 输入坐标：沿用调用方 setExtentCrs() 传入的坐标系；
    // 为无效值时表示范围坐标与源数据坐标系一致，直接按原值裁剪。
    void setExtentCrs(const QgsCoordinateReferenceSystem& crs);
    QgsCoordinateReferenceSystem exportExtentCrs() const;

    // "选择要素"方式选中的要素原始几何（非该方式/尚未选中要素时为空几何）。
    // 导出时的裁剪规则：
    //   - 要素为面/多面（Polygon/MultiPolygon）→ 直接用 exportFeatureGeometry()
    //     作为裁剪多边形，对每个源图层做精确求交裁剪；
    //   - 要素为线/点等 → 以 exportExtent()（该要素的最小外接矩形）作为裁剪范围。
    // 几何坐标系见 exportExtentCrs()（即要素所在图层坐标系）。
    bool hasFeatureGeometry() const;
    QgsGeometry exportFeatureGeometry() const;

    // 预先填充已绘制的范围（重新弹出对话框时用，会同步刷新四组经纬度文本框）
    void setExtent(const QgsRectangle& rect);

    // 用户是否点击了"开始绘制"（若为 true，调用方应在对话框关闭后激活绘制工具再重新打开对话框）
    bool drawRequested() const;
    // 复位"开始绘制"标记（对话框复用场景下，每次 exec() 前必须调用，避免上一轮绘制请求被误判）
    void resetDrawRequested();

    // 记住并恢复"范围获取方式"当前选择（重新弹出对话框时保持上下文一致）
    void setModeIndex(int idx);
    int modeIndex() const;

    // 是否在导出后自动加载结果到地图（默认 true）
    void setAutoLoadAfterExport(bool on);
    bool autoLoadAfterExport() const;

private slots:
    void on_radioButton_inputCoord_toggled(bool checked);
    void on_radioButton_manualDraw_toggled(bool checked);
    void on_radioButton_selectFeature_toggled(bool checked);
    void on_Button_StartDraw_clicked();
    // 选择要素页：图层/字段/要素值联动自动刷新
    void on_comboBox_layer_currentIndexChanged(int index);
    void on_comboBox_field_currentIndexChanged(int index);
    void on_comboBox_value_currentIndexChanged(int index);
    void on_Button_OpenLayer_clicked();   // 打开本地矢量数据
    void on_Button_GetExtent_clicked();   // 按当前要素值获取范围
    void on_comboBox_format_currentIndexChanged(int index); // 格式切换：联动输出标签
    void on_Button_BrowseOutput_clicked();
    void on_Button_OK_clicked();
    void on_Button_Cancel_clicked();

private:
    // 当前选中的矢量图层（地图图层或本地打开的数据；无有效图层返回 nullptr）
    QgsVectorLayer* currentVectorLayer() const;
    // 按当前图层刷新属性字段下拉框
    void refreshFeatureFields();
    // 按当前图层+字段刷新要素值下拉框（去重排序）
    void refreshFeatureValues();
    // 按当前要素值定位要素并记录外包矩形；成功返回 true 并刷新经纬度文本框。
    // warnOnFail 为 true 时在失败情况下弹窗提示。
    bool fetchSelectedFeatureExtent(bool warnOnFail);
    // 以经纬度四文本框（手动绘制页 + 选择要素页）显示范围
    void fillBoundTextEdits(const QgsRectangle& rect);
    void clearBoundTextEdits();

    Ui_SeRangeExportDialog ui;

    QgsMapCanvas* m_pCanvas = nullptr;
    QgisInterface* m_pIface = nullptr;

    // 通过"打开文件..."加载的本地矢量图层（仅本对话框持有，需手动释放）
    QgsVectorLayer* m_pLocalLayer = nullptr;
    QString m_lastOpenDir; // 本地矢量文件浏览的起始目录

    bool m_bHasExtent = false;
    QgsRectangle m_rect;
    QgsGeometry m_featureGeom; // "选择要素"选中的要素几何（坐标系与 m_extentCrs 一致）
    bool m_bDrawRequested = false;

    bool m_bHasExtentCrs = false;
    QgsCoordinateReferenceSystem m_extentCrs;
};

#endif // SE_RANGE_EXPORT_H
