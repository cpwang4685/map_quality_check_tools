/*--------------SE---------------*/
#include "se_data_export.h"
#include "ui_data_export.h"

/*--------------QT---------------*/
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QProgressDialog>
#include <QSettings>
#include <QDateTime>
#include <QHeaderView>
#include <QStandardItem>
#include <QRegularExpression>
#include <QTextStream>

/*--------------GDAL/OGR---------------*/
#include <ogrsf_frmts.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogr_geometry.h>
#include <ogr_feature.h>
#include <ogr_p.h>
#include <cpl_conv.h>
#include <cpl_string.h>

/*--------------QGIS---------------*/
#include <qgisinterface.h>
#include <qgsvectorlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgsproject.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsgeometry.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfields.h>
#include <qgsrasterlayer.h>
#include <qgsrasterdataprovider.h>
#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsmapmouseevent.h>
#include <qgsrubberband.h>
#include <qgslayertree.h>
#include <qgslayertreemodel.h>
#include <qgsapplication.h>
#include <qgsproviderregistry.h>
#include <qgsrectangle.h>
#include <qgswkbtypes.h>

#include <QSqlQuery>
#include <QButtonGroup>
#include <algorithm>

// 【2026-08-24】恢复麒麟自适应缩放（更新版移除，本项目双平台保留）
#include "ui_fit_helper.h"

// ==================== 构造 / 析构 ====================

CSE_DataExportDialog::CSE_DataExportDialog(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
	, ui(new Ui::SeDataExportDialog)
	, m_currentFormat("SHP")
{
	ui->setupUi(this);

	// 【2026-08-24】恢复麒麟自适应缩放（更新版移除，本项目双平台保留）
	DialogFitHelper::install(this);

	this->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

	// 设置 treeWidget 表头
	ui->treeWidget_dbLayers->header()->setStretchLastSection(false);
	ui->treeWidget_dbLayers->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	ui->treeWidget_dbLayers->setRootIsDecorated(false);

	// 裁剪方式单选组：空间范围过滤 / SHP 裁剪二选一（空间范围过滤内支持坐标填写或地图框选）
	m_pClipModeGroup = new QButtonGroup(this);
	m_pClipModeGroup->addButton(ui->radioButton_useExtent);
	m_pClipModeGroup->addButton(ui->radioButton_useShp);
	m_pClipModeGroup->setExclusive(true);

	// 默认不选中任何裁剪方式，禁用下方编辑控件
	ui->radioButton_useExtent->setChecked(false);
	ui->radioButton_useShp->setChecked(false);
	setSpatialFilterEnabled(false);
	setShpClipEnabled(false);

	// 注意：所有按钮/控件信号已由 setupUi() 中的 connectSlotsByName 自动连接
	// （on_<控件名>_<信号名> 命名约定），不需要手动 connect，否则会造成槽函数被调用两次。

	// 默认输出格式
	on_comboBox_format_currentIndexChanged(0);

	// 默认汇总
	updateSummaryLabel();

	// 让“地图框选范围”按钮在启用时更醒目，提示用户这是核心操作
	ui->Button_PickExtentFromMap->setStyleSheet(
		"QPushButton { background-color: #3498db; color: white; font-weight: bold; border-radius: 4px; padding: 4px 12px; }"
		"QPushButton:hover { background-color: #2980b9; }"
		"QPushButton:pressed { background-color: #1c5a85; }"
		"QPushButton:disabled { background-color: #bdc3c7; color: #7f8c8d; }");
}

CSE_DataExportDialog::~CSE_DataExportDialog()
{
	// 如果框选工具仍处于激活状态，先停用它并恢复之前的地图工具
	if (m_pExtentPicker && m_pExtentPicker->isActive())
	{
		if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
		{
			m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
		}
	}
	delete m_pExtentPicker;
	delete ui;
}

// ==================== 公共接口 ====================

void CSE_DataExportDialog::setDatabaseConnection(const QSqlDatabase& db)
{
	m_dbConnection = db;
}

void CSE_DataExportDialog::setQgisInterface(QgisInterface* iface)
{
	m_pQgisIface = iface;
}

void CSE_DataExportDialog::setAvailableLayers(const QList<DbLayerInfo>& layers)
{
	m_availableLayers = layers;
	refreshLayerTree();
}

void CSE_DataExportDialog::setExportInfo(const DataExportInfo& info)
{
	// 兼容旧接口：把 info 转成 DbLayerInfo 并加入列表
	DbLayerInfo layer;
	layer.strSchema = info.strSchema.isEmpty() ? "public" : info.strSchema;
	layer.strTableName = info.strTableName;
	layer.strGeomType = info.strDataType.contains(tr("栅格")) ? "RASTER" : "TABLE";
	layer.strSrid = info.strCrs;
	layer.strCrs = info.strCrs;
	layer.iFeatureCount = -1;
	m_availableLayers.append(layer);
	refreshLayerTree();

	// 设置输出路径
	ui->lineEdit_outputPath->setText(QDir::homePath() + "/" + info.strTableName + ".shp");
}

void CSE_DataExportDialog::loadProjectLayers()
{
	// 占位：可在外部调用以触发刷新操作
	on_Button_RefreshLayers_clicked();
}

// ==================== UI 刷新 ====================

void CSE_DataExportDialog::refreshLayerTree()
{
	ui->treeWidget_dbLayers->clear();

	for (int i = 0; i < m_availableLayers.size(); ++i)
	{
		const DbLayerInfo& info = m_availableLayers[i];

		QTreeWidgetItem* item = new QTreeWidgetItem(ui->treeWidget_dbLayers);
		QString displayName = info.strSchema.isEmpty() ?
			info.strTableName : QString("%1.%2").arg(info.strSchema).arg(info.strTableName);
		item->setText(0, displayName);
		item->setText(1, info.strGeomType);
		item->setText(2, info.strCrs);

		// 用 UserRole 保存信息（多个角色避免冲突）
		item->setData(0, Qt::UserRole, info.strTableName);
		item->setData(0, Qt::UserRole + 1, info.strSchema);
		item->setData(0, Qt::UserRole + 2, info.strGeomType);
		item->setData(0, Qt::UserRole + 3, info.strSrid);
		item->setData(0, Qt::UserRole + 4, info.strCrs);
		item->setData(0, Qt::UserRole + 5, QVariant::fromValue<long long>(info.iFeatureCount));

		item->setToolTip(0, displayName);
	}

	updateSummaryLabel();
}

void CSE_DataExportDialog::on_treeWidget_dbLayers_itemSelectionChanged()
{
	QList<DbLayerInfo> selected = getSelectedLayers();
	updateSummaryLabel();

	// 显示首个图层的源信息
	if (!selected.isEmpty())
	{
		const DbLayerInfo& first = selected.first();
		QString name = first.strSchema.isEmpty() ?
			first.strTableName : QString("%1.%2").arg(first.strSchema).arg(first.strTableName);
		ui->lineEdit_srcName->setText(name);
		ui->lineEdit_srcType->setText(first.strGeomType == "RASTER" ? tr("栅格数据") : tr("矢量数据"));
		ui->lineEdit_srcCrs->setText(first.strCrs);

		// 异步取要素数
		updateFeatureCountDisplay(first);

		// 根据数据类型动态更新目标格式选项
		updateExportFormatOptions(first);
	}
	else
	{
		ui->lineEdit_srcName->clear();
		ui->lineEdit_srcType->clear();
		ui->lineEdit_srcCrs->clear();
		ui->lineEdit_featureCount->clear();
		ui->lineEdit_srcFormat->clear();
		ui->lineEdit_srcDataType->clear();

		// 清空目标格式，恢复默认禁用状态
		ui->comboBox_format->clear();
		ui->comboBox_format->setEnabled(false);
		ui->label_formatLock->setText(tr("（请先在上方选择图层）"));
	}
}

void CSE_DataExportDialog::updateSummaryLabel()
{
	QList<DbLayerInfo> selected = getSelectedLayers();
	ui->label_summary->setText(tr("已选择 %1 个图层").arg(selected.size()));
}

void CSE_DataExportDialog::updateFeatureCountDisplay(const DbLayerInfo& layer)
{
	if (layer.iFeatureCount < 0)
	{
		// 实时查询
		if (!m_dbConnection.isOpen())
		{
			ui->lineEdit_featureCount->setText(tr("-"));
			return;
		}
		QSqlQuery q(m_dbConnection);
		QString sql = QString("SELECT COUNT(*) FROM \"%1\".\"%2\"")
			.arg(layer.strSchema.isEmpty() ? "public" : layer.strSchema)
			.arg(layer.strTableName);
		if (q.exec(sql) && q.next())
		{
			long long n = q.value(0).toLongLong();
			ui->lineEdit_featureCount->setText(QString::number(n));
		}
		else
		{
			ui->lineEdit_featureCount->setText(tr("-"));
		}
	}
	else
	{
		ui->lineEdit_featureCount->setText(QString::number(layer.iFeatureCount));
	}
}

void CSE_DataExportDialog::updateExportFormatOptions(const DbLayerInfo& layer)
{
	ui->comboBox_format->clear();

	if (layer.strGeomType == "RASTER")
	{
		// 栅格数据：只能导出为 GeoTIFF
		ui->comboBox_format->addItem(tr("GeoTIFF (*.tif)"));
		ui->lineEdit_srcDataType->setText(tr("栅格"));
		ui->lineEdit_srcFormat->setText(tr("PostGIS Raster"));
		ui->label_formatLock->setText(tr("（栅格数据，已锁定为 GeoTIFF）"));
	}
	else
	{
		// 矢量数据：可选 SHP、GDB、GPKG
		ui->comboBox_format->addItem(tr("ESRI Shapefile (*.shp)"));
		ui->comboBox_format->addItem(tr("File Geodatabase (*.gdb)"));
		ui->comboBox_format->addItem(tr("GeoPackage (*.gpkg)"));
		ui->lineEdit_srcDataType->setText(tr("矢量"));
		ui->lineEdit_srcFormat->setText(tr("PostGIS Vector"));
		ui->label_formatLock->setText(tr("（矢量数据，可选格式）"));
	}

	ui->comboBox_format->setEnabled(true);
	ui->comboBox_format->setCurrentIndex(0);

	// 更新输出路径后缀
	on_comboBox_format_currentIndexChanged(0);
	updateOutputPathSuffix();
}

QList<DbLayerInfo> CSE_DataExportDialog::getSelectedLayers() const
{
	QList<DbLayerInfo> result;
	QList<QTreeWidgetItem*> items = ui->treeWidget_dbLayers->selectedItems();
	for (QTreeWidgetItem* item : items)
	{
		DbLayerInfo info;
		info.strTableName = item->data(0, Qt::UserRole).toString();
		info.strSchema = item->data(0, Qt::UserRole + 1).toString();
		info.strGeomType = item->data(0, Qt::UserRole + 2).toString();
		info.strSrid = item->data(0, Qt::UserRole + 3).toString();
		info.strCrs = item->data(0, Qt::UserRole + 4).toString();
		info.iFeatureCount = item->data(0, Qt::UserRole + 5).toLongLong();
		if (!info.strTableName.isEmpty())
		{
			result.append(info);
		}
	}
	return result;
}

// ==================== 过滤与选择 ====================

void CSE_DataExportDialog::on_lineEdit_filter_textChanged(const QString& text)
{
	for (int i = 0; i < ui->treeWidget_dbLayers->topLevelItemCount(); ++i)
	{
		QTreeWidgetItem* item = ui->treeWidget_dbLayers->topLevelItem(i);
		bool visible = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
		item->setHidden(!visible);
	}
}

void CSE_DataExportDialog::on_Button_RefreshLayers_clicked()
{
	if (!m_dbConnection.isOpen())
	{
		appendLog(tr("[提示] 数据库未连接，无法刷新图层。"));
		return;
	}

	appendLog(tr("[刷新] 正在加载数据库矢量表..."));

	QList<DbLayerInfo> layers;

	// 优先查询 geometry_columns
	QSqlQuery q(m_dbConnection);
	QString sql = "SELECT f_table_schema, f_table_name, type, srid "
		"FROM geometry_columns "
		"ORDER BY f_table_schema, f_table_name";

	if (q.exec(sql))
	{
		while (q.next())
		{
			DbLayerInfo info;
			info.strSchema = q.value(0).toString();
			info.strTableName = q.value(1).toString();
			info.strGeomType = q.value(2).toString();
			info.strSrid = q.value(3).toString();
			info.strCrs = info.strSrid.startsWith("EPSG:") ? info.strSrid : QString("EPSG:%1").arg(info.strSrid);
			info.iFeatureCount = -1;
			layers.append(info);
		}
	}
	else
	{
		// 退回 information_schema
		QSqlQuery q2(m_dbConnection);
		if (q2.exec("SELECT table_schema, table_name FROM information_schema.tables "
			"WHERE table_type='BASE TABLE' AND table_schema NOT IN ('pg_catalog','information_schema') "
			"ORDER BY table_schema, table_name"))
		{
			while (q2.next())
			{
				DbLayerInfo info;
				info.strSchema = q2.value(0).toString();
				info.strTableName = q2.value(1).toString();
				info.strGeomType = "TABLE";
				info.strSrid = "";
				info.strCrs = "";
				info.iFeatureCount = -1;
				layers.append(info);
			}
		}
	}

	// 查询栅格表
	QSqlQuery q3(m_dbConnection);
	if (q3.exec("SELECT r_table_schema, r_table_name, srid FROM raster_columns "
		"ORDER BY r_table_schema, r_table_name"))
	{
		while (q3.next())
		{
			DbLayerInfo info;
			info.strSchema = q3.value(0).toString();
			info.strTableName = q3.value(1).toString();
			info.strGeomType = "RASTER";
			info.strSrid = q3.value(2).toString();
			info.strCrs = info.strSrid.startsWith("EPSG:") ? info.strSrid : QString("EPSG:%1").arg(info.strSrid);
			info.iFeatureCount = -1;
			layers.append(info);
		}
	}

	m_availableLayers = layers;
	refreshLayerTree();
	appendLog(tr("[刷新] 已加载 %1 个图层。").arg(layers.size()));
}

void CSE_DataExportDialog::on_Button_SelectAll_clicked()
{
	ui->treeWidget_dbLayers->selectAll();
}

void CSE_DataExportDialog::on_Button_SelectNone_clicked()
{
	ui->treeWidget_dbLayers->clearSelection();
}

// ==================== 空间范围 ====================

void CSE_DataExportDialog::on_Button_GetCanvasExtent_clicked()
{
	// 通过 qgisInterface 获取当前地图范围
	if (QgsProject::instance() && !QgsProject::instance()->mapLayers().isEmpty())
	{
		// 取所有图层合并范围作为参考
		QgsRectangle rect;
		bool first = true;
		const QMap<QString, QgsMapLayer*> layers = QgsProject::instance()->mapLayers();
		for (auto it = layers.begin(); it != layers.end(); ++it)
		{
			if (it.value() && it.value()->isSpatial())
			{
				if (first)
				{
					rect = it.value()->extent();
					first = false;
				}
				else
				{
					rect.combineExtentWith(it.value()->extent());
				}
			}
		}

		if (!first)
		{
			ui->lineEdit_minX->setText(QString::number(rect.xMinimum(), 'f', 6));
			ui->lineEdit_minY->setText(QString::number(rect.yMinimum(), 'f', 6));
			ui->lineEdit_maxX->setText(QString::number(rect.xMaximum(), 'f', 6));
			ui->lineEdit_maxY->setText(QString::number(rect.yMaximum(), 'f', 6));
			ui->radioButton_useExtent->setChecked(true);
			appendLog(tr("[空间范围] 已从项目图层获取合并范围。"));
		}
		else
		{
			appendLog(tr("[提示] 项目中无空间图层，请手动输入范围。"));
		}
	}
}

void CSE_DataExportDialog::on_Button_ClearExtent_clicked()
{
	ui->lineEdit_minX->clear();
	ui->lineEdit_minY->clear();
	ui->lineEdit_maxX->clear();
	ui->lineEdit_maxY->clear();
	ui->label_pickFromMapExtent->setText(tr("已选范围：未选择"));
}

void CSE_DataExportDialog::on_Button_PickExtentFromMap_clicked()
{
	// 地图框选属于空间范围过滤的一种输入方式，自动切换到该模式
	ui->radioButton_useExtent->setChecked(true);

	if (!m_pQgisIface)
	{
		QMessageBox::warning(this, tr("数据出库"),
			tr("未获取到 QGIS 接口，无法使用地图框选功能。请通过插件主菜单打开数据管理。"));
		return;
	}

	QgsMapCanvas* canvas = m_pQgisIface->mapCanvas();
	if (!canvas)
	{
		QMessageBox::warning(this, tr("数据出库"), tr("当前没有可用的地图画布！"));
		return;
	}

	// 保存当前地图工具，框选完成后恢复；避免连续框选时保存到 picker 自身
	QgsMapTool* currentTool = canvas->mapTool();
	if (currentTool != m_pExtentPicker)
	{
		m_pPreviousMapTool = currentTool;
	}
	else
	{
		m_pPreviousMapTool = nullptr;
	}


	if (!m_pExtentPicker)
	{
		m_pExtentPicker = new CMapToolExtentPicker(canvas);
		connect(m_pExtentPicker, &CMapToolExtentPicker::extentSelected,
			this, &CSE_DataExportDialog::onMapExtentSelected);
		connect(m_pExtentPicker, &CMapToolExtentPicker::cancelled,
			this, &CSE_DataExportDialog::onMapExtentPickCancelled);
	}

	// 此时对话框通过 show() + setWindowModality(Qt::WindowModal) 方式打开，
	// QGIS 主窗口不受阻塞，可以直接设置地图工具进行交互。
	canvas->setMapTool(m_pExtentPicker);

	// 将 QGIS 主窗口推到前台，并隐藏本对话框，让用户直接在地图画布上拖拽
	if (QWidget* qgisMain = m_pQgisIface->mainWindow())
	{
		qgisMain->raise();
		qgisMain->activateWindow();
	}
	this->hide();

	appendLog(tr("[地图框选] 请在地图上拖拽绘制导出范围，右键取消。"));
}

void CSE_DataExportDialog::onMapExtentSelected(const QgsRectangle& rect)
{
	if (!m_pExtentPicker)
		return;

	// 将范围回填到空间范围输入框
	ui->lineEdit_minX->setText(QString::number(rect.xMinimum(), 'f', 6));
	ui->lineEdit_minY->setText(QString::number(rect.yMinimum(), 'f', 6));
	ui->lineEdit_maxX->setText(QString::number(rect.xMaximum(), 'f', 6));
	ui->lineEdit_maxY->setText(QString::number(rect.yMaximum(), 'f', 6));

	// 地图框选结果回填后，确保当前处于空间范围过滤模式
	if (!ui->radioButton_useExtent->isChecked())
	{
		ui->radioButton_useExtent->setChecked(true);
	}

	// 同步显示在空间范围过滤组的标签中
	ui->label_pickFromMapExtent->setText(tr("已选范围：X[%1, %2] Y[%3, %4]")
		.arg(rect.xMinimum(), 0, 'f', 6)
		.arg(rect.xMaximum(), 0, 'f', 6)
		.arg(rect.yMinimum(), 0, 'f', 6)
		.arg(rect.yMaximum(), 0, 'f', 6));

	appendLog(tr("[地图框选] 已获取范围：X[%1, %2] Y[%3, %4]")
		.arg(rect.xMinimum(), 0, 'f', 6)
		.arg(rect.xMaximum(), 0, 'f', 6)
		.arg(rect.yMinimum(), 0, 'f', 6)
		.arg(rect.yMaximum(), 0, 'f', 6));

	// 恢复之前的地图工具（setMapTool 会自动触发 picker 的 deactivate）
	if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
	{
		m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
	}

	// 恢复显示本对话框（此时对话框以 WindowModal 非 exec 方式运行，直接 show 即可）
	this->show();
	this->raise();
	this->activateWindow();
}

void CSE_DataExportDialog::onMapExtentPickCancelled()
{
	if (!m_pExtentPicker)
		return;

	appendLog(tr("[地图框选] 已取消。"));

	// 恢复之前的地图工具（setMapTool 会自动触发 picker 的 deactivate）
	if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
	{
		m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
	}

	// 恢复显示本对话框（此时对话框以 WindowModal 非 exec 方式运行，直接 show 即可）
	this->show();
	this->raise();
	this->activateWindow();
}


void CSE_DataExportDialog::setSpatialFilterEnabled(bool enabled)
{
	ui->lineEdit_minX->setEnabled(enabled);
	ui->lineEdit_minY->setEnabled(enabled);
	ui->lineEdit_maxX->setEnabled(enabled);
	ui->lineEdit_maxY->setEnabled(enabled);
	ui->lineEdit_minX->setReadOnly(false);
	ui->lineEdit_minY->setReadOnly(false);
	ui->lineEdit_maxX->setReadOnly(false);
	ui->lineEdit_maxY->setReadOnly(false);
	ui->Button_GetCanvasExtent->setEnabled(enabled);
	ui->Button_ClearExtent->setEnabled(enabled);
	ui->Button_PickExtentFromMap->setEnabled(enabled);
	ui->label_pickFromMapExtent->setEnabled(enabled);
	if (enabled && ui->label_pickFromMapExtent->text() == tr("已选范围：未选择"))
	{
		ui->label_pickFromMapExtent->setText(tr("已选范围：未选择（点击下方按钮到地图拖拽框选）"));
	}
}

void CSE_DataExportDialog::setShpClipEnabled(bool enabled)
{
	ui->lineEdit_clipShpPath->setEnabled(enabled);
	ui->Button_BrowseClipShp->setEnabled(enabled);
	ui->Button_LoadClipShpToMap->setEnabled(enabled);

	// 字段/值控件的可用性还取决于是否已选择 SHP 文件
	bool bHasShp = enabled && !ui->lineEdit_clipShpPath->text().isEmpty() && QFile::exists(ui->lineEdit_clipShpPath->text());
	ui->comboBox_clipAttrField->setEnabled(bHasShp);
	ui->lineEdit_clipAttrValue->setEnabled(bHasShp && ui->comboBox_clipAttrField->currentIndex() > 0);
}

void CSE_DataExportDialog::on_radioButton_useExtent_toggled(bool checked)
{
	setSpatialFilterEnabled(checked);
	if (checked)
	{
		setShpClipEnabled(false);
		appendLog(tr("[裁剪方式] 已选择：空间范围过滤。请手动填写范围、点击“获取当前地图范围”或“地图框选范围”按钮。"));

		// 如果框选工具正在使用中，则停止并恢复地图工具（setMapTool 自动触发 deactivate）
		if (m_pExtentPicker && m_pExtentPicker->isActive())
		{
			if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
			{
				m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
			}
		}
	}
}

void CSE_DataExportDialog::on_radioButton_useShp_toggled(bool checked)
{
	setShpClipEnabled(checked);
	if (checked)
	{
		setSpatialFilterEnabled(false);
		appendLog(tr("[裁剪方式] 已选择：按指定 SHP 文件图层面裁剪。请选择 SHP 文件。"));

		// 如果框选工具正在使用中，则停止并恢复地图工具（setMapTool 自动触发 deactivate）
		if (m_pExtentPicker && m_pExtentPicker->isActive())
		{
			if (m_pQgisIface && m_pQgisIface->mapCanvas() && m_pPreviousMapTool)
			{
				m_pQgisIface->mapCanvas()->setMapTool(m_pPreviousMapTool);
			}
		}
	}
}

bool CSE_DataExportDialog::getSpatialExtent(double& minX, double& minY, double& maxX, double& maxY) const
{
	if (!ui->radioButton_useExtent->isChecked())
	{
		return false;
	}
	bool ok1, ok2, ok3, ok4;
	minX = ui->lineEdit_minX->text().toDouble(&ok1);
	minY = ui->lineEdit_minY->text().toDouble(&ok2);
	maxX = ui->lineEdit_maxX->text().toDouble(&ok3);
	maxY = ui->lineEdit_maxY->text().toDouble(&ok4);

	if (!(ok1 && ok2 && ok3 && ok4))
	{
		return false;
	}
	if (minX >= maxX || minY >= maxY)
	{
		return false;
	}
	return true;
}

// ==================== 裁剪 SHP ====================

void CSE_DataExportDialog::on_Button_BrowseClipShp_clicked()
{
	QString curPath = ui->lineEdit_clipShpPath->text();
	if (curPath.isEmpty())
	{
		QSettings settings;
		curPath = settings.value("DataExport/LastShpDir", QDir::homePath()).toString();
	}
	QString path = QFileDialog::getOpenFileName(this, tr("选择用于裁剪的SHP文件"),
		curPath, tr("ESRI Shapefile (*.shp)"));
	if (!path.isEmpty())
	{
		ui->lineEdit_clipShpPath->setText(path);
		QSettings settings;
		settings.setValue("DataExport/LastShpDir", QFileInfo(path).absolutePath());
	}
}

void CSE_DataExportDialog::on_Button_LoadClipShpToMap_clicked()
{
	QString path = ui->lineEdit_clipShpPath->text();
	if (path.isEmpty() || !QFile::exists(path))
	{
		QMessageBox::warning(this, tr("数据导出"), tr("请先选择有效的 SHP 文件！"));
		return;
	}
	QString base = QFileInfo(path).completeBaseName();
	QgsVectorLayer* layer = new QgsVectorLayer(path, base, "ogr");
	if (!layer->isValid())
	{
		delete layer;
		QMessageBox::warning(this, tr("数据导出"), tr("无法加载该 SHP 文件！"));
		return;
	}
	QgsProject::instance()->addMapLayer(layer);
	appendLog(tr("[裁剪SHP] 已加载到地图：%1").arg(path));
}

void CSE_DataExportDialog::on_lineEdit_clipShpPath_textChanged(const QString& text)
{
	// SHP 裁剪子控件的可用性由单选按钮和文件是否有效共同决定
	setShpClipEnabled(ui->radioButton_useShp->isChecked());

	bool bValid = !text.isEmpty() && QFile::exists(text);
	if (!bValid)
	{
		ui->comboBox_clipAttrField->clear();
		return;
	}

	// 读取字段列表
	ui->comboBox_clipAttrField->clear();
	ui->comboBox_clipAttrField->addItem(tr("(全部)"), QString());
	QgsVectorLayer* layer = new QgsVectorLayer(text, QFileInfo(text).completeBaseName(), "ogr");
	if (layer && layer->isValid())
	{
		const QgsFields fields = layer->fields();
		for (int i = 0; i < fields.count(); ++i)
		{
			ui->comboBox_clipAttrField->addItem(fields.at(i).name(), fields.at(i).name());
		}
	}
	delete layer;
}



void CSE_DataExportDialog::on_comboBox_clipAttrField_currentIndexChanged(int index)
{
	Q_UNUSED(index);
	// 选中有效字段后启用值输入
	ui->lineEdit_clipAttrValue->setEnabled(ui->comboBox_clipAttrField->currentIndex() > 0);
}

bool CSE_DataExportDialog::loadClipGeometry(QgsGeometry& outGeom, QString& errMsg) const
{
	QString shpPath = ui->lineEdit_clipShpPath->text().trimmed();
	if (shpPath.isEmpty() || !QFile::exists(shpPath))
	{
		errMsg = tr("裁剪SHP文件不存在：%1").arg(shpPath);
		return false;
	}

	// 加载并合并几何
	QgsVectorLayer* layer = new QgsVectorLayer(shpPath, QFileInfo(shpPath).completeBaseName(), "ogr");
	if (!layer || !layer->isValid())
	{
		errMsg = tr("无法加载裁剪SHP文件：%1").arg(shpPath);
		delete layer;
		return false;
	}

	// 可选按属性过滤
	QString attrField = ui->comboBox_clipAttrField->currentData().toString();
	QString attrValue = ui->lineEdit_clipAttrValue->text().trimmed();

	QgsFeatureIterator it = layer->getFeatures();
	QgsFeature feat;
	bool first = true;

	QgsCoordinateReferenceSystem clipCrs = layer->crs();
	while (it.nextFeature(feat))
	{
		QgsGeometry g = feat.geometry();
		if (g.isNull() || g.isEmpty())
		{
			continue;
		}
		// 应用属性过滤
		if (!attrField.isEmpty() && !attrValue.isEmpty())
		{
			QVariant v = feat.attribute(attrField);
			if (v.toString().compare(attrValue, Qt::CaseInsensitive) != 0)
			{
				continue;
			}
		}

		if (first)
		{
			outGeom = g;
			first = false;
		}
		else
		{
			QgsGeometry combined = outGeom.combine(g);
			if (!combined.isNull())
			{
				outGeom = combined;
			}
		}
	}
	delete layer;

	if (first)
	{
		errMsg = tr("裁剪SHP中没有可用要素。");
		return false;
	}

	// 若需要转换到目标图层坐标系（在使用时由调用方处理）
	return true;
}

// ==================== 输出路径 ====================

QString CSE_DataExportDialog::currentFormatSuffix() const
{
	QString text = ui->comboBox_format->currentText().toLower();
	if (text.contains("shp")) return ".shp";
	if (text.contains("gdb")) return ".gdb";
	if (text.contains("tif")) return ".tif";
	if (text.contains("gpkg")) return ".gpkg";
	return ".shp";
}

QString CSE_DataExportDialog::currentFormatFilter() const
{
	QString text = ui->comboBox_format->currentText().toLower();
	if (text.contains("shp")) return tr("ESRI Shapefile (*.shp)");
	if (text.contains("gdb")) return tr("File Geodatabase (*.gdb)");
	if (text.contains("tif")) return tr("GeoTIFF (*.tif *.tiff)");
	if (text.contains("gpkg")) return tr("GeoPackage (*.gpkg)");
	return tr("ESRI Shapefile (*.shp)");
}

QString CSE_DataExportDialog::currentDriverName() const
{
	QString text = ui->comboBox_format->currentText().toLower();
	if (text.contains("shp")) return "ESRI Shapefile";
	if (text.contains("gdb")) return "FileGDB";
	if (text.contains("tif")) return "GTiff";
	if (text.contains("gpkg")) return "GPKG";
	return "ESRI Shapefile";
}

void CSE_DataExportDialog::on_Button_BrowseOutput_clicked()
{
	QString curPath = ui->lineEdit_outputPath->text();
	if (curPath.isEmpty())
	{
		QSettings settings;
		curPath = settings.value("DataExport/LastExportDir", QDir::homePath()).toString();
	}

	if (ui->comboBox_format->currentText().toLower().contains("gdb")) // GDB: 选目录
	{
		QString dirPath = QFileDialog::getExistingDirectory(this,
			tr("选择 GDB 输出目录 (将创建 .gdb 目录)"), curPath);
		if (!dirPath.isEmpty())
		{
			QString gdbPath = dirPath;
			if (!gdbPath.endsWith(".gdb", Qt::CaseInsensitive))
			{
				gdbPath += ".gdb";
			}
			ui->lineEdit_outputPath->setText(gdbPath);
			QSettings settings;
			settings.setValue("DataExport/LastExportDir", dirPath);
		}
		return;
	}

	QString dstPath = QFileDialog::getSaveFileName(this,
		tr("选择导出文件路径"), curPath, currentFormatFilter());

	if (!dstPath.isEmpty())
	{
		QString suffix = currentFormatSuffix();
		if (!dstPath.endsWith(suffix, Qt::CaseInsensitive))
		{
			dstPath += suffix;
		}
		ui->lineEdit_outputPath->setText(dstPath);
		QSettings settings;
		settings.setValue("DataExport/LastExportDir", QFileInfo(dstPath).absolutePath());
	}
}

void CSE_DataExportDialog::on_Button_BrowseOutputDir_clicked()
{
	QString curPath = ui->lineEdit_outputDir->text();
	if (curPath.isEmpty())
	{
		QSettings settings;
		curPath = settings.value("DataExport/LastExportDir", QDir::homePath()).toString();
	}
	QString dir = QFileDialog::getExistingDirectory(this, tr("选择批量输出目录"), curPath);
	if (!dir.isEmpty())
	{
		ui->lineEdit_outputDir->setText(dir);
		QSettings settings;
		settings.setValue("DataExport/LastExportDir", dir);
	}
}

void CSE_DataExportDialog::on_comboBox_format_currentIndexChanged(int index)
{
	Q_UNUSED(index);
	// 更新格式字符串（通过 currentText 动态判断，适配动态填充的 comboBox）
	QString text = ui->comboBox_format->currentText().toLower();
	if (text.contains("shp")) m_currentFormat = "SHP";
	else if (text.contains("gdb")) m_currentFormat = "GDB";
	else if (text.contains("tif")) m_currentFormat = "TIF";
	else if (text.contains("gpkg")) m_currentFormat = "GPKG";
	updateOutputPathSuffix();
}

void CSE_DataExportDialog::updateOutputPathSuffix()
{
	QString curPath = ui->lineEdit_outputPath->text();
	if (curPath.isEmpty())
	{
		// 用首个选中图层作为默认名
		QList<DbLayerInfo> sel = getSelectedLayers();
		QString baseName = sel.isEmpty() ? tr("export") : sel.first().strTableName;
		ui->lineEdit_outputPath->setText(QDir::homePath() + "/" + baseName + currentFormatSuffix());
	}
}

// ==================== 导出执行 ====================

QString CSE_DataExportDialog::buildPostGISConnStr() const
{
	if (m_dbConnection.isOpen())
	{
		return QString("PG:host=%1 port=%2 dbname='%3' user='%4' password='%5'")
			.arg(m_dbConnection.hostName())
			.arg(m_dbConnection.port())
			.arg(m_dbConnection.databaseName())
			.arg(m_dbConnection.userName())
			.arg(m_dbConnection.password());
	}
	return QString();
}

QString CSE_DataExportDialog::buildPgUri(const DbLayerInfo& layer) const
{
	if (m_dbConnection.isOpen())
	{
		return QString("host=%1 port=%2 dbname='%3' user='%4' password='%5' "
			"table=\"%6\".\"%7\" (geom) sslmode=disable")
			.arg(m_dbConnection.hostName())
			.arg(m_dbConnection.port())
			.arg(m_dbConnection.databaseName())
			.arg(m_dbConnection.userName())
			.arg(m_dbConnection.password())
			.arg(layer.strSchema.isEmpty() ? "public" : layer.strSchema)
			.arg(layer.strTableName);
	}
	return QString();
}

void CSE_DataExportDialog::appendLog(const QString& msg)
{
	ui->textEdit_log->append(QString("[%1] %2")
		.arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
		.arg(msg));
	QApplication::processEvents();
}

void CSE_DataExportDialog::on_Button_Export_clicked()
{
	QList<DbLayerInfo> selected = getSelectedLayers();
	if (selected.isEmpty())
	{
		QMessageBox::warning(this, tr("数据出库"), tr("请至少选择一个要导出的图层！"));
		return;
	}

	// 校验输出路径
	QString singlePath = ui->lineEdit_outputPath->text().trimmed();
	QString batchDir = ui->lineEdit_outputDir->text().trimmed();
	bool isBatch = (selected.size() > 1) || (!batchDir.isEmpty());

	if (!isBatch && singlePath.isEmpty())
	{
		QMessageBox::warning(this, tr("数据出库"), tr("请指定输出路径或批量输出目录！"));
		return;
	}

	if (isBatch && batchDir.isEmpty())
	{
		// 多个图层但没指定批量目录时，使用 singlePath 所在目录
		QFileInfo fi(singlePath);
		batchDir = fi.absolutePath();
		if (batchDir.isEmpty())
		{
			QMessageBox::warning(this, tr("数据出库"), tr("多图层导出请指定批量输出目录！"));
			return;
		}
		QDir().mkpath(batchDir);
	}

	// 范围与裁剪（空间范围过滤 / SHP 裁剪二选一；空间范围过滤内支持坐标填写或地图框选）
	double minX = 0, minY = 0, maxX = 0, maxY = 0;
	bool bUseExtent = getSpatialExtent(minX, minY, maxX, maxY);
	bool bUseSpatialOrMap = ui->radioButton_useExtent->isChecked();
	if (bUseSpatialOrMap && !bUseExtent)
	{
		QMessageBox::warning(this, tr("数据出库"),
			tr("选择了空间范围过滤但输入值无效（请填写数字且 min<max）！"));
		return;
	}

	// 裁剪几何
	QgsGeometry clipGeom;
	QString clipErr;
	bool bUseClip = ui->radioButton_useShp->isChecked();
	if (bUseClip)
	{
		if (!loadClipGeometry(clipGeom, clipErr))
		{
			QMessageBox::warning(this, tr("数据出库"), tr("加载裁剪SHP失败"));
			return;
		}
		appendLog(tr("[裁剪SHP] 已加载裁剪几何，准备按图层面裁剪。"));
	}

	bool bIntersectOnly = ui->checkBox_intersectOnly->isChecked();
	QString encoding = ui->comboBox_encoding->currentText();

	// 检查是否覆盖
	bool bOverwrite = ui->checkBox_overwrite->isChecked();
	if (!bOverwrite && !isBatch && QFileInfo::exists(singlePath))
	{
		if (QMessageBox::question(this, tr("数据出库"),
			tr("文件已存在，是否覆盖？")) != QMessageBox::Yes)
		{
			return;
		}
	}

	GDALAllRegister();
	OGRRegisterAll();

	QApplication::setOverrideCursor(Qt::WaitCursor);
	int total = selected.size();
	int successCount = 0;
	int failCount = 0;

	for (int i = 0; i < total; ++i)
	{
		const DbLayerInfo& layer = selected[i];
		ui->progressBar_export->setValue((i * 100) / total);

		// 计算目标路径
		QString dstPath;
		if (isBatch)
		{
			QString baseName = layer.strSchema.isEmpty() ?
				layer.strTableName : QString("%1_%2").arg(layer.strSchema).arg(layer.strTableName);
		QString suffix = currentFormatSuffix();
		if (ui->comboBox_format->currentText().toLower().contains("gdb")) // GDB
		{
				dstPath = QDir(batchDir).filePath(baseName + ".gdb");
			}
			else
			{
				dstPath = QDir(batchDir).filePath(baseName + suffix);
			}
		}
		else
		{
			dstPath = singlePath;
		}

		appendLog(tr("[%1/%2] 导出 %3 → %4").arg(i + 1).arg(total).arg(layer.strTableName).arg(dstPath));

		int outCount = 0;
		bool ok = exportLayer(layer, dstPath, bUseExtent, minX, minY, maxX, maxY,
			clipGeom, bIntersectOnly, encoding, outCount);

		if (ok)
		{
			++successCount;
			appendLog(tr("  ✓ 成功，输出要素数：%1").arg(outCount));

			// 添加到地图
			if (ui->checkBox_addToMap->isChecked())
			{
				addOutputToMap(dstPath, m_currentFormat, layer.strTableName);
			}
		}
		else
		{
			++failCount;
			appendLog(tr("  ✗ 失败"));
		}
	}

	ui->progressBar_export->setValue(100);
	QApplication::restoreOverrideCursor();

	QString summary = tr("导出完成！\n成功：%1\n失败：%2\n总计：%3")
		.arg(successCount).arg(failCount).arg(total);
	appendLog(summary);

	if (failCount == 0)
	{
		QMessageBox::information(this, tr("数据出库"), tr("导出完成"));
		accept();
	}
	else
	{
		QMessageBox::warning(this, tr("数据出库"), tr("导出完成，部分图层失败，请查看日志。"));
	}
}

bool CSE_DataExportDialog::exportLayer(const DbLayerInfo& layer,
	const QString& dstPath,
	bool bUseExtent,
	double minX, double minY, double maxX, double maxY,
	const QgsGeometry& clipGeom,
	bool bIntersectOnly,
	const QString& encoding,
	int& outFeatureCount)
{
	outFeatureCount = 0;

	if (layer.strGeomType == "RASTER")
	{
		return exportRasterLayerToTif(layer, dstPath, bUseExtent, minX, minY, maxX, maxY,
			ui->lineEdit_clipShpPath->text(), outFeatureCount);
	}

	// 矢量数据根据当前选中的目标格式（currentText）判断导出方式
	QString fmtText = ui->comboBox_format->currentText().toLower();
	if (fmtText.contains("shp")) // SHP
	{
		return exportVectorLayerToShp(layer, dstPath, bUseExtent, minX, minY, maxX, maxY,
			clipGeom, bIntersectOnly, encoding, outFeatureCount);
	}
	else if (fmtText.contains("gdb")) // GDB
	{
		QString driver = "FileGDB";
		// FileGDB 不可用时回退到 GPKG
		GDALDriver* d = GetGDALDriverManager()->GetDriverByName("FileGDB");
		if (!d)
		{
			driver = "GPKG";
			appendLog(tr("[提示] FileGDB 驱动不可用，回退到 GeoPackage。"));
		}
		return exportVectorLayerToOgr(layer, dstPath, driver, bUseExtent, minX, minY, maxX, maxY,
			clipGeom, bIntersectOnly, encoding, outFeatureCount);
	}
	else if (fmtText.contains("gpkg")) // GPKG
	{
		return exportVectorLayerToOgr(layer, dstPath, "GPKG", bUseExtent, minX, minY, maxX, maxY,
			clipGeom, bIntersectOnly, encoding, outFeatureCount);
	}
	else if (fmtText.contains("tif")) // TIF（矢量图层但选了栅格格式，理论上不会发生，但做兼容）
	{
		return exportRasterLayerToTif(layer, dstPath, bUseExtent, minX, minY, maxX, maxY,
			ui->lineEdit_clipShpPath->text(), outFeatureCount);
	}
	return false;
}

// ==================== 矢量导出到 SHP ====================

bool CSE_DataExportDialog::exportVectorLayerToShp(const DbLayerInfo& layer,
	const QString& dstPath,
	bool bUseExtent,
	double minX, double minY, double maxX, double maxY,
	const QgsGeometry& clipGeom,
	bool bIntersectOnly,
	const QString& encoding,
	int& outFeatureCount)
{
	outFeatureCount = 0;

	QString uri = buildPgUri(layer);
	if (uri.isEmpty())
	{
		appendLog(tr("  ✗ 数据库连接无效"));
		return false;
	}

	QString pgLayerName = layer.strSchema.isEmpty() ?
		layer.strTableName : QString("%1.%2").arg(layer.strSchema).arg(layer.strTableName);
	QgsVectorLayer* pgLayer = new QgsVectorLayer(uri, pgLayerName, "postgres");
	if (!pgLayer || !pgLayer->isValid())
	{
		delete pgLayer;
		appendLog(tr("  ✗ 无法从数据库加载图层"));
		return false;
	}

	// 删除已有目标文件
	QFileInfo fi(dstPath);
	QDir dir = fi.absoluteDir();
	QString baseName = fi.completeBaseName();
	QStringList shpExtensions = { ".shp", ".shx", ".dbf", ".prj", ".cpg", ".qpj" };
	for (const QString& ext : shpExtensions)
	{
		QString f = dir.filePath(baseName + ext);
		if (QFile::exists(f)) QFile::remove(f);
	}

	// 使用 memory provider 构建临时过滤后图层
	// 根据源图层几何类型动态构建 memory layer URI
	QgsWkbTypes::Type wkbType = pgLayer->wkbType();
	QString geomTypeStr;
	switch (QgsWkbTypes::geometryType(wkbType))
	{
	case QgsWkbTypes::PointGeometry:    geomTypeStr = "Point"; break;
	case QgsWkbTypes::LineGeometry:     geomTypeStr = "LineString"; break;
	case QgsWkbTypes::PolygonGeometry:  geomTypeStr = "Polygon"; break;
	default:                            geomTypeStr = "Geometry"; break;
	}
	QString crsAuth = pgLayer->crs().authid();
	if (crsAuth.isEmpty()) crsAuth = "EPSG:4326";
	QString memUri = QString("%1?crs=%2").arg(geomTypeStr).arg(crsAuth);

	QgsVectorLayer* memoryLayer = new QgsVectorLayer(memUri, pgLayerName + "_filtered", "memory");
	if (!memoryLayer || !memoryLayer->isValid())
	{
		delete memoryLayer;
		delete pgLayer;
		appendLog(tr("  ✗ 创建内存图层失败"));
		return false;
	}

	// 复制字段（跳过源字段中的几何字段，保留属性字段）
	memoryLayer->startEditing();
	QgsFields srcFields = pgLayer->fields();
	for (int i = 0; i < srcFields.count(); ++i)
	{
		const QgsField& fld = srcFields.at(i);
		// 跳过几何字段（OGR 的 geom 等）
		if (fld.type() == QVariant::UserType)
			continue;
		memoryLayer->addAttribute(fld);
	}
	memoryLayer->updateFields();

	// 计算并初始化裁剪坐标转换（pg -> clip）
	QgsCoordinateReferenceSystem clipCrs;
	if (!clipGeom.isNull())
	{
		// 假设裁剪 SHP 与数据库图层坐标系一致，否则需要转换
		clipCrs = pgLayer->crs();
	}
	QgsCoordinateTransform pgToClip(pgLayer->crs(), clipCrs, QgsProject::instance());

	// 几何裁剪/过滤
	QgsFeatureIterator it = pgLayer->getFeatures();
	QgsFeature feat;
	int processed = 0;

	while (it.nextFeature(feat))
	{
		QgsGeometry g = feat.geometry();
		if (g.isNull() || g.isEmpty())
		{
			continue;
		}

		// 范围过滤
		if (bUseExtent)
		{
			QgsRectangle ext(minX, minY, maxX, maxY);
			if (!g.intersects(ext))
			{
				continue;
			}
		}

		// 裁剪
		if (!clipGeom.isNull())
		{
			QgsGeometry intersect = g.intersection(clipGeom);
			if (intersect.isNull() || intersect.isEmpty())
			{
				if (bIntersectOnly)
				{
					continue;
				}
			}
			else
			{
				feat.setGeometry(intersect);
			}
		}

		// 写入 memory layer
		QgsFeature newFeat(memoryLayer->fields());
		newFeat.setGeometry(feat.geometry());
		newFeat.setAttributes(feat.attributes());
		memoryLayer->addFeature(newFeat);
		++outFeatureCount;
		++processed;
	}
	memoryLayer->commitChanges();
	delete pgLayer;

	// 写文件
	QgsVectorFileWriter::SaveVectorOptions options;
	options.driverName = "ESRI Shapefile";
	options.fileEncoding = encoding;

	QString errorMsg;
	QgsVectorFileWriter::WriterError error = QgsVectorFileWriter::writeAsVectorFormatV2(
		memoryLayer, dstPath, QgsProject::instance()->transformContext(), options,
		nullptr, &errorMsg);

	delete memoryLayer;

	if (error != QgsVectorFileWriter::NoError)
	{
		appendLog(tr("  ✗ SHP 写出失败：%1").arg(errorMsg.isEmpty() ? tr("未知错误") : errorMsg));
		return false;
	}

	return true;
}

// ==================== 矢量导出到 GDB / GPKG (使用 QGIS API) ====================

bool CSE_DataExportDialog::exportVectorLayerToOgr(const DbLayerInfo& layer,
	const QString& dstPath,
	const QString& driverName,
	bool bUseExtent,
	double minX, double minY, double maxX, double maxY,
	const QgsGeometry& clipGeom,
	bool bIntersectOnly,
	const QString& encoding,
	int& outFeatureCount)
{
	outFeatureCount = 0;

	// 从数据库加载源图层
	QString uri = buildPgUri(layer);
	if (uri.isEmpty())
	{
		appendLog(tr("  ✗ 数据库连接无效"));
		return false;
	}
	QString pgLayerName = layer.strSchema.isEmpty() ?
		layer.strTableName : QString("%1.%2").arg(layer.strSchema).arg(layer.strTableName);
	QgsVectorLayer* pgLayer = new QgsVectorLayer(uri, pgLayerName, "postgres");
	if (!pgLayer || !pgLayer->isValid())
	{
		delete pgLayer;
		appendLog(tr("  ✗ 无法从数据库加载图层"));
		return false;
	}

	// 根据源图层几何类型构建 memory layer
	QgsWkbTypes::Type wkbType = pgLayer->wkbType();
	QString geomTypeStr;
	switch (QgsWkbTypes::geometryType(wkbType))
	{
	case QgsWkbTypes::PointGeometry:    geomTypeStr = "Point"; break;
	case QgsWkbTypes::LineGeometry:     geomTypeStr = "LineString"; break;
	case QgsWkbTypes::PolygonGeometry:  geomTypeStr = "Polygon"; break;
	default:                            geomTypeStr = "Geometry"; break;
	}
	QString crsAuth = pgLayer->crs().authid();
	if (crsAuth.isEmpty()) crsAuth = "EPSG:4326";
	QString memUri = QString("%1?crs=%2").arg(geomTypeStr).arg(crsAuth);

	QgsVectorLayer* memoryLayer = new QgsVectorLayer(memUri, pgLayerName + "_ogr_export", "memory");
	if (!memoryLayer || !memoryLayer->isValid())
	{
		delete memoryLayer;
		delete pgLayer;
		appendLog(tr("  ✗ 创建内存图层失败"));
		return false;
	}

	// 复制属性字段
	memoryLayer->startEditing();
	QgsFields srcFields = pgLayer->fields();
	for (int i = 0; i < srcFields.count(); ++i)
	{
		const QgsField& fld = srcFields.at(i);
		if (fld.type() == QVariant::UserType)
			continue;
		memoryLayer->addAttribute(fld);
	}
	memoryLayer->updateFields();

	// 遍历并过滤源要素
	QgsFeatureIterator it = pgLayer->getFeatures();
	QgsFeature feat;
	while (it.nextFeature(feat))
	{
		QgsGeometry g = feat.geometry();
		if (g.isNull() || g.isEmpty())
			continue;

		// 空间范围过滤
		if (bUseExtent)
		{
			QgsRectangle ext(minX, minY, maxX, maxY);
			if (!g.intersects(ext))
				continue;
		}

		// SHP 裁剪
		if (!clipGeom.isNull())
		{
			QgsGeometry intersect = g.intersection(clipGeom);
			if (intersect.isNull() || intersect.isEmpty())
			{
				if (bIntersectOnly)
					continue;
			}
			else
			{
				feat.setGeometry(intersect);
			}
		}

		QgsFeature newFeat(memoryLayer->fields());
		newFeat.setGeometry(feat.geometry());
		newFeat.setAttributes(feat.attributes());
		memoryLayer->addFeature(newFeat);
		++outFeatureCount;
	}
	memoryLayer->commitChanges();
	delete pgLayer;

	// 删除已有目标文件
	if (QFileInfo(dstPath).exists())
	{
		if (QFileInfo(dstPath).isDir())
			QDir(dstPath).removeRecursively();
		else
			QFile::remove(dstPath);
	}

	// 使用 QgsVectorFileWriter 写出
	QgsVectorFileWriter::SaveVectorOptions options;
	options.driverName = driverName;
	options.fileEncoding = encoding;

	QString errorMsg;
	QgsVectorFileWriter::WriterError error = QgsVectorFileWriter::writeAsVectorFormatV2(
		memoryLayer, dstPath, QgsProject::instance()->transformContext(), options,
		nullptr, &errorMsg);

	delete memoryLayer;

	if (error != QgsVectorFileWriter::NoError)
	{
		appendLog(tr("  ✗ 写出失败：%1").arg(errorMsg.isEmpty() ? tr("未知错误") : errorMsg));
		return false;
	}

	return true;
}

// ==================== 栅格导出到 TIF ====================

bool CSE_DataExportDialog::exportRasterLayerToTif(const DbLayerInfo& layer,
	const QString& dstPath,
	bool bUseExtent,
	double minX, double minY, double maxX, double maxY,
	const QString& clipShpPath,
	int& outFeatureCount)
{
	outFeatureCount = 0;
	Q_UNUSED(outFeatureCount); // 栅格不使用要素计数

	QString pgRasterConn = QString("PG:host=%1 port=%2 dbname='%3' user='%4' password='%5' "
		"schema='%6' table='%7' mode=2")
		.arg(m_dbConnection.hostName())
		.arg(m_dbConnection.port())
		.arg(m_dbConnection.databaseName())
		.arg(m_dbConnection.userName())
		.arg(m_dbConnection.password())
		.arg(layer.strSchema.isEmpty() ? "public" : layer.strSchema)
		.arg(layer.strTableName);

	// 构建 GDAL warp/translate 选项
	char** papszOptions = nullptr;
	if (bUseExtent)
	{
		QString projWin = QString("%1 %2 %3 %4")
			.arg(minX, 0, 'f', 10).arg(maxY, 0, 'f', 10)
			.arg(maxX, 0, 'f', 10).arg(minY, 0, 'f', 10);
		papszOptions = CSLSetNameValue(papszOptions, "projWin", projWin.toUtf8().constData());
	}
	if (!clipShpPath.isEmpty() && QFile::exists(clipShpPath))
	{
		papszOptions = CSLSetNameValue(papszOptions, "cutlineDSName", clipShpPath.toUtf8().constData());
	}

	// 删除已有目标
	if (QFile::exists(dstPath))
		QFile::remove(dstPath);

	GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
	if (!poDriver)
	{
		appendLog(tr("  ✗ GTiff 驱动不可用"));
		return false;
	}

	GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(pgRasterConn.toUtf8().constData(),
		GDAL_OF_RASTER, nullptr, nullptr, nullptr);
	if (!poSrcDS)
	{
		appendLog(tr("  ✗ 无法从数据库加载栅格"));
		CSLDestroy(papszOptions);
		return false;
	}

	// 使用 GDALTranslate 方式创建空间子集副本
	GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(papszOptions, nullptr);
	GDALDatasetH hDstDS = GDALTranslate(dstPath.toUtf8().constData(),
		(GDALDatasetH)poSrcDS, psOptions, nullptr);
	GDALTranslateOptionsFree(psOptions);

	bool bOk = (hDstDS != nullptr);
	if (hDstDS) GDALClose(hDstDS);
	GDALClose(poSrcDS);
	CSLDestroy(papszOptions);

	if (!bOk)
	{
		appendLog(tr("  ✗ 栅格导出失败"));
	}

	return bOk;
}

// ==================== 添加到地图 ====================

void CSE_DataExportDialog::addOutputToMap(const QString& path, const QString& formatKey, const QString& layerName)
{
	if (!QFile::exists(path))
	{
		return;
	}

	if (formatKey == "TIF")
	{
		QgsRasterLayer* rl = new QgsRasterLayer(path, layerName);
		if (rl && rl->isValid())
		{
			QgsProject::instance()->addMapLayer(rl);
		}
		else
		{
			delete rl;
		}
	}
	else
	{
		// GDB 是目录
		QString uri = (formatKey == "GDB") ? path : path;
		QgsVectorLayer* vl = new QgsVectorLayer(uri, layerName, "ogr");
		if (vl && vl->isValid())
		{
			QgsProject::instance()->addMapLayer(vl);
		}
		else
		{
			delete vl;
		}
	}
}

// ==================== 取消 ====================

void CSE_DataExportDialog::on_Button_Cancel_clicked()
{
	reject();
}
