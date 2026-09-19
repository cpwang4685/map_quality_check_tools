#pragma once

#include <QDialog>
#include <QStringList>
#include <QList>

namespace Ui {
class GDBLayerSelectorDialog;
}

/// GDB/MDB 单个矢量图层的元信息
/// （用于入库时按要素数据集组织目录树）
struct GDBLayerInfo
{
	/// 所属要素数据集名称；为空表示图层直接挂在 gdb 根下（无要素数据集归属）
	QString datasetName;
	/// 图层名称
	QString layerName;
	/// 几何类型名（如 Polygon / Point / LineString）
	QString geomTypeName;
	/// 要素数量（驱动无法给出时为 -1）
	int featureCount = -1;
};

class GDBLayerSelectorDialog : public QDialog
{
	Q_OBJECT

public:
	explicit GDBLayerSelectorDialog(const QString& gdbPath, QWidget* parent = nullptr);
	~GDBLayerSelectorDialog();

	/// 返回用户勾选的图层名称列表
	QStringList selectedLayers() const;

	/// 枚举 GDB/MDB 中所有矢量图层名称（用于批量导入自动全选，不弹出对话框）
	static QStringList allLayerNames(const QString& gdbPath);

	/// 枚举 GDB/MDB 中所有矢量图层（包含所属要素数据集信息），
	/// 按 gdb 要素数据集→图层 的层次产出，便于入库时按要素数据集创建目录节点。
	/// 旧版 OpenFileGDB 驱动无 Group API 时，所有图层的 datasetName 为空。
	static QList<GDBLayerInfo> allLayerInfos(const QString& gdbPath);

private slots:
	void onSelectAll();
	void onDeselectAll();
	void onOk();

private:
	void populateLayers(const QString& gdbPath);

	Ui::GDBLayerSelectorDialog* ui;
	QStringList mAllLayerNames;
};
