#ifndef SE_MAIN_AREA_CLIP_H
#define SE_MAIN_AREA_CLIP_H

#include <QDialog>
#include <QStringList>
#include <qgsrectangle.h>

#include "ui_main_area_clip.h"

class QgsVectorLayer;
class QgsCoordinateReferenceSystem;

// ============================================================
// 「按主区裁切导出」对话框（.ui 表单版）
//
// 对应纯 C++ 版 CSE_MainAreaExportDialog（se_data_list_export.h），
// 界面控件与行为完全一致，供外部系统集成调用：
//
//   CSE_MainAreaClipDialogUi dlg(parent);
//   dlg.setSourcePath(srcPath);          // 源数据路径（用于输出路径建议）
//   dlg.setAutoLoadAfterExport(true);
//   if (dlg.exec() == QDialog::Accepted)
//   {
//       QString     path = dlg.mainAreaPath();        // 主区数据文件
//       QStringList ids  = dlg.selectedFeatureIds();  // 选中要素 fid 列表
//       QString     out  = dlg.outputFilePath();      // 输出文件
//       double      scale = dlg.mapScaleDenominator();// 生效比例尺分母
//       ...
//       // 裁切范围：按"推荐比例尺 × 内图廓尺寸"计算，主区范围居中，
//       // 坐标系与主区数据一致（crs 为主区数据坐标系）：
//       QgsCoordinateReferenceSystem crs;
//       QgsRectangle clipRect = dlg.computedClipRect(&crs);
//       // 如需直接取主区所选要素的外包矩形，可用：
//       // QgsRectangle ext = computeMainAreaClipRect(path, ids, &crs);
//       // ... 用 clipRect（crs 为主区数据坐标系）执行裁剪导出
//   }
// ============================================================
class CSE_MainAreaClipDialogUi : public QDialog
{
    Q_OBJECT

public:
    explicit CSE_MainAreaClipDialogUi(QWidget* parent = nullptr);
    ~CSE_MainAreaClipDialogUi() override;

    // 设置或获取源数据路径（可指向单文件或目录，用于输出文件路径建议）
    void setSourcePath(const QString& path) { d_srcPath = path; }
    QString sourcePath() const { return d_srcPath; }

    // 主区数据
    QString mainAreaPath() const;
    QString mainAreaField() const;
    QStringList selectedFeatureIds() const;

    // 输出设置
    // 输出路径：Shapefile 格式时为"保存文件夹"（每个成果一个 shp）；
    // GeoPackage/GDB 格式时为"保存文件"（所有成果写入该单一文件，各占一个图层）
    QString outputFilePath() const;
    QString encoding() const;               // "UTF-8" / "GBK" / "GB2312"
    QString format() const;                 // "ESRI Shapefile (.shp)" / "GeoPackage (.gpkg)" / "GDB (.gdb)"
    bool overwriteExisting() const;

    // 制图参数
    QString paperSize() const;              // 纸张尺寸下拉框文本："A0"~"A4" / "自定义"
    QString paperOrient() const;            // "自动" / "纵向" / "横向"
    QString standardScale() const;          // 生效的推荐比例尺字符串（下拉框可手填），如 "10000"
    double  mapScaleDenominator() const;
    double  paperWidthMm() const;           // 当前生效的纸张宽度（mm），文本框无法解析时返回 0
    double  paperHeightMm() const;          // 当前生效的纸张高度（mm），文本框无法解析时返回 0
    // 内图廓宽/高（mm）：始终可设置，默认与纸张尺寸一致，切换纸张后联动
    double  innerMapFrameWidthMm() const;
    double  innerMapFrameHeightMm() const;

    // 制图比例尺分母：主区范围经投影后的经线方向（南北方向）地面长度 ÷
    // 纸张经线方向图上长度。未选主区要素或纸张无效时返回 0
    double  cartographicScaleDenominator() const;

    // 裁切范围：按"推荐比例尺（或自定义比例尺）× 内图廓宽/高"计算，
    // 与主区空间参考系一致，并尽可能使主区范围居中；
    // 比例尺/内图廓无效时回退为主区所选要素的外包矩形（空矩形表示未选主区）
    QgsRectangle computedClipRect(QgsCoordinateReferenceSystem* outCrs = nullptr) const;

    // 是否在导出后自动加载结果到地图（默认 true）
    void setAutoLoadAfterExport(bool on);
    bool autoLoadAfterExport() const;

private slots:
    void on_Button_BrowseMainArea_clicked();
    void on_comboBox_field_currentIndexChanged(int index);
    void on_Button_BrowseOutput_clicked();
    void on_comboBox_format_currentIndexChanged(int index);
    void on_Button_ComputeOrient_clicked();
    void on_Button_Export_clicked();
    void on_Button_Cancel_clicked();
    // 纸张尺寸下拉框切换：A0~A4 按当前纸张方向填充纸张宽/高文本框；
    // "自定义"不改数值，由用户直接编辑纸张宽/高
    void on_comboBox_paperSize_currentIndexChanged(int index);
    // 纸张宽度/高度文本框可编辑：内容变化时联动内图廓宽/高并刷新制图比例尺
    void on_lineEdit_paperW_textChanged(const QString& text);
    void on_lineEdit_paperH_textChanged(const QString& text);
    // 纸张方向：横向/纵向时校正纸张尺寸文本框的宽/高顺序
    void on_comboBox_orient_currentIndexChanged(int index);
    // 主区要素选择变化 → 刷新制图比例尺显示
    void on_listWidget_features_itemSelectionChanged();

private:
    Ui_SeMainAreaClipDialog ui;

    QString d_srcPath;
    // 缓存主区 layer，以便标识字段切换时无须重新读取整个 shp
    QgsVectorLayer* m_pMainAreaLayer = nullptr;

    void loadMainAreaFields();              // 选择主区文件后加载字段与要素列表
    void reloadFeatures();                  // 标识字段切换时重新填充要素列表
    QgsRectangle selectedMainAreaRect() const;  // 已选要素的合并外包矩形（主区 layer 原始 CRS）
    void syncInnerFrameToPaper();           // 内图廓宽/高与纸张宽/高保持一致
    void refreshCartographicScale();        // 刷新"制图比例尺"文本框显示
    void computePaperOrientation();         // "计算"：依据主区范围自动确定纸张方向
    int  matchingIsoPaperIndex() const;     // 当前宽/高与某 ISO 纸张一致时返回其索引，否则 -1
    void autoSwitchPaperSizeToCustom();     // 手工改宽/高且不符所选纸张时，纸张尺寸切到"自定义"
    void autoRestoreIsoPaperSize();         // 程序性修改宽/高后，若恰为 ISO 纸张则恢复其名称
};

// 从主区矢量读取所选要素的合并外包矩形，作为统一裁剪范围。
// mainAreaPath: 主区 shapefile 路径；featureIds: 选中的要素 ID 字符串列表。
// outCrs（可选）：返回矩形所在的主区 layer 原始 CRS，供裁剪时把范围转换到源数据 CRS。
// 失败（路径无效/图层打不开）返回空矩形（isEmpty()）。
QgsRectangle computeMainAreaClipRect(const QString& mainAreaPath,
    const QStringList& featureIds, QgsCoordinateReferenceSystem* outCrs = nullptr);

#endif // SE_MAIN_AREA_CLIP_H
