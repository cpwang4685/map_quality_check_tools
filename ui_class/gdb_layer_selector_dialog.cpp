#include "gdb_layer_selector_dialog.h"
#include "GeneratedFiles/Release/ui_gdb_layer_selector_dialog.h"

#include "gdal_priv.h"
#include "gdal_version.h"   // GDAL_VERSION_NUM / GDAL_COMPUTE_VERSION（版本守卫用）
#include "ogrsf_frmts.h"
#include "cpl_conv.h"

#include <QHash>
#include <QMessageBox>
#include <QListWidgetItem>
#include <QRegularExpression>
#include <QStringList>
#include <qgsmessagelog.h>
#include "ui_fit_helper.h"   // 本工程补（源侧无）：麒麟低分辨率下的对话框自适应
// ==================== 构造 / 析构 ====================

GDBLayerSelectorDialog::GDBLayerSelectorDialog(const QString& gdbPath, QWidget* parent)
	: QDialog(parent)
	, ui(new Ui::GDBLayerSelectorDialog)
{
	ui->setupUi(this);
	DialogFitHelper::install(this);   // 本工程补（源侧无）：麒麟低分辨率下的对话框自适应

	// 连接信号
	connect(ui->mSelectAllBtn, &QPushButton::clicked, this, &GDBLayerSelectorDialog::onSelectAll);
	connect(ui->mDeselectAllBtn, &QPushButton::clicked, this, &GDBLayerSelectorDialog::onDeselectAll);
	connect(ui->mOkBtn, &QPushButton::clicked, this, &GDBLayerSelectorDialog::onOk);
	connect(ui->mCancelBtn, &QPushButton::clicked, this, &QDialog::reject);

	// 确保 GDAL 已注册
	static bool gdalRegistered = false;
	if (!gdalRegistered)
	{
		GDALAllRegister();
		gdalRegistered = true;
	}

	populateLayers(gdbPath);
}

GDBLayerSelectorDialog::~GDBLayerSelectorDialog()
{
	delete ui;
}

// ==================== 公共方法 ====================

QStringList GDBLayerSelectorDialog::selectedLayers() const
{
	QStringList result;
	for (int i = 0; i < ui->mLayerListWidget->count(); ++i)
	{
		QListWidgetItem* item = ui->mLayerListWidget->item(i);
		if (item && item->checkState() == Qt::Checked && i < mAllLayerNames.size())
		{
			result << mAllLayerNames[i];
		}
	}


	return result;
}

QStringList GDBLayerSelectorDialog::allLayerNames(const QString& gdbPath)
{
	QStringList names;
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(gdbPath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
	if (!poDS) return names;

	int nLayers = poDS->GetLayerCount();
	for (int i = 0; i < nLayers; ++i)
	{
		OGRLayer* poLayer = poDS->GetLayer(i);
		if (poLayer)
		{
			const char* pszName = poLayer->GetName();
			if (pszName)
				names << QString::fromUtf8(pszName);
		}
	}
	GDALClose(poDS);
	return names;
}

// 枚举 GDB/MDB 中所有矢量图层（含要素数据集归属信息）
// 主路径：通过 GDB_Items 系统目录表解析每个要素类的 Path，
//        Path 形如 \道路\道路线，第一段即为该图层所属的要素数据集名称。
// 备路径：GDAL 3.4+ OpenFileGDB 通过 GDALGroup API 暴露要素数据集。
// 上述两条路径都不可用时，全部图层视为无要素数据集归属（兜底）。
QList<GDBLayerInfo> GDBLayerSelectorDialog::allLayerInfos(const QString& gdbPath)
{
	QList<GDBLayerInfo> result;
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(gdbPath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
	if (!poDS) return result;

	// 填充单条图层的几何类型与要素数量
	auto fillLayer = [](GDBLayerInfo& info, OGRLayer* poLayer)
	{
		if (!poLayer) return;
		info.geomTypeName = QString::fromUtf8(OGRGeometryTypeToName(poLayer->GetGeomType()));
		GIntBig rawCount = poLayer->GetFeatureCount(false);
		if (rawCount >= 0) info.featureCount = static_cast<int>(rawCount);
	};

	// ===== 主路径：解析 GDB_Items 的 Path 列 =====
	// 要素类（Feature Class）UUID：
	//   - FileGDB（Esri 驱动）  : {7072F1E0-ECAF-11D2-A4D9-00C04F6BC8FE}
	//   - OpenFileGDB（GDAL 驱动）: {70737809-852C-4A03-9E22-2CECEA5B9BFA}
	// 凡 Type UUID 包含上述任一前缀均视为要素类。
	QHash<QString, QString> layerToDataset; // 图层名 → 要素数据集名

	OGRLayer* poItems = poDS->GetLayerByName("GDB_Items");
	if (poItems)
	{
		poItems->ResetReading();
		OGRFeature* poFeat = nullptr;
		while ((poFeat = poItems->GetNextFeature()) != nullptr)
		{
			const QString typeStr = QString::fromUtf8(poFeat->GetFieldAsString("Type")).toLower();
			const bool isFc = typeStr.contains("7072f1e0") || typeStr.contains("70737809");
			if (isFc)
			{
				const QString layerName = QString::fromUtf8(poFeat->GetFieldAsString("Name"));
				QString path = QString::fromUtf8(poFeat->GetFieldAsString("Path"));
				path = path.trimmed();
				while (path.startsWith('\\') || path.startsWith('/'))
					path.remove(0, 1);
				while (path.endsWith('\\') || path.endsWith('/'))
					path.chop(1);

				// Qt 5.12 兼容：Qt::SkipEmptyParts 是 Qt 5.14 才有的枚举，
				// 麒麟 Qt 5.12.12 只有 QString::SkipEmptyParts（语义相同）。
				const QStringList parts = path.split(QRegularExpression("[\\\\/]"), QString::SkipEmptyParts);
				if (parts.size() >= 2)
				{
					// 父级目录链（去掉末段图层名）→ 用首段作为要素数据集名
					layerToDataset.insert(layerName, parts.first());
				}
			}
			OGRFeature::DestroyFeature(poFeat);
		}
	}
	else
	{
		QgsMessageLog::logMessage(
			QStringLiteral("[allLayerInfos] %1: GetLayerByName(\"GDB_Items\") 返回 null，请检查驱动版本")
				.arg(gdbPath),
			"MapProductTools", Qgis::Warning);
	}

	// 详细日志：打印解析得到的每个图层→数据集映射
	{
		QStringList pairs;
		for (auto it = layerToDataset.begin(); it != layerToDataset.end(); ++it)
			pairs << QStringLiteral("%1→%2").arg(it.key(), it.value());
		QgsMessageLog::logMessage(
			QStringLiteral("[allLayerInfos] %1: GDB_Items 解析 %2 条映射: %3")
				.arg(gdbPath).arg(layerToDataset.size()).arg(pairs.join(QStringLiteral("; "))),
			"MapProductTools", Qgis::Info);
	}

	// ===== 备路径：GDALGroup API（GDAL 3.4+ OpenFileGDB） =====
	// 版本守卫：GetRootGroup()/GDALGroup 是 GDAL 3.4 新增（官方文档 "New in version 3.4"）。
	// 麒麟是系统 GDAL 3.0.4（CMakeLists 用 find_package(GDAL) 取系统库），根本没这几个
	// 方法——这是编译期错误，下面的运行期 if (rootGroup) 兜不住，必须条件编译。
	// 3.0.4 上本段整体编掉，只剩主路径（GDB_Items）；若主路径也取不到，
	// 则所有图层 datasetName 为空，退化为平铺（不崩溃），见下方两 map 合并逻辑。
	QHash<QString, QString> groupToDataset; // 图层名 → 要素数据集名（来自 GDALGroup）
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 4, 0)
	auto rootGroup = poDS->GetRootGroup();
	if (rootGroup)
	{
		const auto fdNames = rootGroup->GetGroupNames();
		for (const std::string& fdName : fdNames)
		{
			auto fdGroup = rootGroup->OpenGroup(fdName);
			if (!fdGroup) continue;
			const QString datasetName = QString::fromStdString(fdName);
			const auto fdLayers = fdGroup->GetVectorLayerNames();
			for (const std::string& name : fdLayers)
				groupToDataset.insert(QString::fromStdString(name), datasetName);
		}
	}
#endif // GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 4, 0)

	QgsMessageLog::logMessage(
		QStringLiteral("[allLayerInfos] %1: GDALGroup 解析得到 %2 个图层归属条目")
			.arg(gdbPath).arg(groupToDataset.size()),
		"MapProductTools", Qgis::Info);

	// ===== 按数据源默认顺序遍历所有图层，按上述两个 map 合并 datasetName =====
	int nLayers = poDS->GetLayerCount();
	for (int i = 0; i < nLayers; ++i)
	{
		OGRLayer* poLayer = poDS->GetLayer(i);
		if (!poLayer) continue;
		// 跳过 GDB 系统目录表（仅用于解析，不应作为成果图层入库）
		const QString lname = QString::fromUtf8(poLayer->GetName());
		if (lname.startsWith("GDB_", Qt::CaseInsensitive))
			continue;

		GDBLayerInfo info;
		info.layerName = lname;
		fillLayer(info, poLayer);

		// GDB_Items 优先（最权威，含完整路径）；GDALGroup 作为补充
		if (layerToDataset.contains(lname))
			info.datasetName = layerToDataset.value(lname);
		else if (groupToDataset.contains(lname))
			info.datasetName = groupToDataset.value(lname);

		result << info;
	}

	// 最终诊断：每条图层的 datasetName 值
	QStringList diag;
	for (const auto& info : result)
		diag << QStringLiteral("%1[dataset=%2]").arg(info.layerName,
			info.datasetName.isEmpty() ? QStringLiteral("(无)") : info.datasetName);
	QgsMessageLog::logMessage(
		QStringLiteral("[allLayerInfos] %1: 最终图层清单=%2 条: %3")
			.arg(gdbPath).arg(result.size()).arg(diag.join(QStringLiteral("; "))),
		"MapProductTools", Qgis::Info);

	GDALClose(poDS);
	return result;
}

// ==================== 私有槽 ====================

void GDBLayerSelectorDialog::onSelectAll()
{
	for (int i = 0; i < ui->mLayerListWidget->count(); ++i)
	{
		QListWidgetItem* item = ui->mLayerListWidget->item(i);
		if (item)
			item->setCheckState(Qt::Checked);
	}
}

void GDBLayerSelectorDialog::onDeselectAll()
{
	for (int i = 0; i < ui->mLayerListWidget->count(); ++i)
	{
		QListWidgetItem* item = ui->mLayerListWidget->item(i);
		if (item)
			item->setCheckState(Qt::Unchecked);
	}
}

void GDBLayerSelectorDialog::onOk()
{
	if (selectedLayers().isEmpty())
	{
		QMessageBox::warning(this, "提示", "请至少选择一个图层进行入库。");
		return;
	}
	accept();
}

// ==================== 私有方法 ====================

void GDBLayerSelectorDialog::populateLayers(const QString& gdbPath)
{

	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(gdbPath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));

	if (!poDS)
	{
		QString gdalError = QString::fromUtf8(CPLGetLastErrorMsg());

		QMessageBox::critical(this, "错误",
			QString("无法打开 GDB 文件:\n%1\n\nGDAL 错误: %2")
				.arg(gdbPath, gdalError));
		return;
	}

	int nLayers = poDS->GetLayerCount();

	// 获取 GDB 驱动名称确认格式
	QString driverName = QString::fromUtf8(poDS->GetDriverName());

	if (nLayers == 0)
	{

		QMessageBox::information(this, "提示", "该 GDB 文件中没有找到矢量图层。");
		GDALClose(poDS);
		return;
	}

	// 遍历所有图层，添加到列表中
	for (int i = 0; i < nLayers; ++i)
	{
		OGRLayer* poLayer = poDS->GetLayer(i);
		if (!poLayer)
		{
			continue;
		}

		QString layerName = QString::fromUtf8(poLayer->GetName());
		OGRwkbGeometryType eGeomType = poLayer->GetGeomType();
		QString geomTypeName = QString::fromUtf8(OGRGeometryTypeToName(eGeomType));

		// 尝试获取要素数量（GDB可能返回 -1）
		GIntBig rawCount = poLayer->GetFeatureCount(false);
		int featureCount = (rawCount >= 0) ? static_cast<int>(rawCount) : -1;


		// 构建显示文本
		QString displayText;
		if (featureCount >= 0)
			displayText = QString("%1  [%2, %3 要素]")
				.arg(layerName, geomTypeName)
				.arg(featureCount);
		else
			displayText = QString("%1  [%2]")
				.arg(layerName, geomTypeName);

		QListWidgetItem* item = new QListWidgetItem(displayText);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Checked); // 默认全选
		ui->mLayerListWidget->addItem(item);

		mAllLayerNames << layerName;
	}

	GDALClose(poDS);


	// 设置对话框标题显示图层数
	setWindowTitle(QString("选择要入库的GDB图层 (%1 个图层)").arg(nLayers));
}
