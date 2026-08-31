/*--------------QT---------------*/
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableView>
#include <QHeaderView>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QSpacerItem>
#include <QMessageBox>
#include <QApplication>
#include <QDesktopWidget>
#include <QSqlDatabase>
#include <QSqlTableModel>
#include <QSqlError>
#include <QSqlRecord>
#include <QSortFilterProxyModel>
#include <QItemSelectionModel>

// ================================================================
//  自定义筛选代理模型
//  解决 QSortFilterProxyModel 默认用原始 QVariant 数据做正则匹配，
//  导致浮点数（如 14.5515 内部存为 14.5514999999）搜不到的问题。
//  改为对表格实际显示文字做 contains 模糊匹配。
// ================================================================
class MetadataFilterProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setSearchFilter(int column, const QString& text)
    {
        m_filterColumn = column;
        m_filterText = text;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        if (m_filterText.isEmpty())
            return true;

        QAbstractItemModel* model = sourceModel();
        int cols = model->columnCount();

        QList<int> checkCols;
        if (m_filterColumn < 0) {
            for (int c = 0; c < cols; c++) checkCols << c;
        } else {
            checkCols << m_filterColumn;
        }

        for (int col : checkCols)
        {
            QModelIndex idx = model->index(sourceRow, col, sourceParent);
            QVariant data = model->data(idx, Qt::DisplayRole);

            // 浮点列 & 数值字符串列：数据值格式化为固定高精度字符串，
            // 直接用原始搜索文本做 contains 匹配。
            // 避免搜索词 toDouble→'g'15 往返转换造成的浮点精度丢失。
            // 'f'12 在 12 位小数处四舍五入，远高于 IEEE 754 误差位（~15-16位），
            // 确保格式化后的字符串包含用户输入的所有有效数字。
            // 同时覆盖 QPSQL 驱动将 DOUBLE PRECISION 当作 String 返回的情况。
            bool handledAsNumber = false;
            {
                double dataVal = 0;
                bool   dataIsNum = false;
                if (data.type() == QVariant::Double)
                {
                    dataVal = data.toDouble();
                    dataIsNum = true;
                }
                else
                {
                    dataVal = data.toString().toDouble(&dataIsNum);
                }
                if (dataIsNum)
                {
                    // 只用原始搜索文本匹配，不把搜索词转 double（避免往返浮点误差）
                    bool searchLooksNumeric = false;
                    m_filterText.toDouble(&searchLooksNumeric);
                    if (searchLooksNumeric)
                    {
                        // 用 'f'12 格式化数据值：12 位小数足够覆盖 DOUBLE PRECISION
                        // 的全部有效位，同时四舍五入消除 IEEE 754 的第 15-16 位误差
                        QString dataStr = QString::number(dataVal, 'f', 12);
                        if (dataStr.contains(m_filterText, Qt::CaseInsensitive))
                            return true;
                        handledAsNumber = true;
                    }
                }
            }
            if (!handledAsNumber && data.toString().contains(m_filterText, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

private:
    int     m_filterColumn = -1;
    QString m_filterText;
};

/*--------------QGIS---------------*/
#include "qgsmessagelog.h"

/*--------------SE---------------*/
#include "se_metadata_viewer.h"

#include "ui_fit_helper.h"

// ====================================================================
//  构造 & 析构
// ====================================================================

CSEMetadataViewerDialog::CSEMetadataViewerDialog(const QString& host,
                                                   int port,
                                                   const QString& database,
                                                   const QString& username,
                                                   const QString& password,
                                                   const QString& schema,
                                                   QWidget* parent)
    : QDialog(parent)
    , m_host(host)
    , m_port(port)
    , m_database(database)
    , m_username(username)
    , m_password(password)
    , m_schema(schema)
    , m_pTableView(nullptr)
    , m_pModel(nullptr)
    , m_pProxyModel(nullptr)
    , m_pLabelFilterCol(nullptr)
    , m_pComboFilterCol(nullptr)
    , m_pLabelFilter(nullptr)
    , m_pEditFilter(nullptr)
    , m_pBtnRefresh(nullptr)
    , m_pBtnSave(nullptr)
    , m_pBtnDelete(nullptr)
    , m_pBtnClose(nullptr)
    , m_pLabelRowCount(nullptr)
{
    // 生成唯一连接名（避免与程序中其他数据库连接冲突）
    m_connName = QString("metadata_viewer_%1")
        .arg(reinterpret_cast<quintptr>(this), 0, 16);

    InitUI();
    DialogFitHelper::install(this);
    OpenDatabase();
    LoadTableData();
}

CSEMetadataViewerDialog::~CSEMetadataViewerDialog()
{
    // 关闭并移除数据库连接
    if (m_pModel)
    {
        m_pModel->submitAll();  // 尝试保存未提交的修改
    }
    {
        QSqlDatabase db = QSqlDatabase::database(m_connName, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connName);
}

// ====================================================================
//  UI 初始化
// ====================================================================

void CSEMetadataViewerDialog::InitUI()
{
    // ---- 窗口属性 ----
    this->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint
                         | Qt::WindowTitleHint | Qt::WindowMinMaxButtonsHint);
    this->setWindowTitle(tr("元数据管理 — gis_metadata"));
    this->setMinimumSize(1000, 600);
    this->resize(1200, 720);

    // 居中
    QRect screenRect = QApplication::desktop()->availableGeometry();
    this->move(screenRect.center().x() - 600, screenRect.center().y() - 360);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // ================================================================
    //  第一行：筛选栏
    // ================================================================
    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->setSpacing(6);

    m_pLabelFilterCol = new QLabel(tr("筛选列:"), this);
    m_pComboFilterCol = new QComboBox(this);
    m_pComboFilterCol->setMinimumWidth(140);
    m_pComboFilterCol->setToolTip(tr("选择要筛选的列，或选 [全部] 不筛选"));

    m_pLabelFilter = new QLabel(tr("关键词:"), this);
    m_pEditFilter = new QLineEdit(this);
    m_pEditFilter->setPlaceholderText(tr("输入筛选关键词..."));
    m_pEditFilter->setMinimumWidth(200);
    m_pEditFilter->setToolTip(tr("对选中列进行模糊匹配（%LIKE%）"));
    m_pEditFilter->setClearButtonEnabled(true);

    filterLayout->addWidget(m_pLabelFilterCol);
    filterLayout->addWidget(m_pComboFilterCol);
    filterLayout->addWidget(m_pLabelFilter);
    filterLayout->addWidget(m_pEditFilter);
    filterLayout->addSpacerItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum));

    m_pLabelRowCount = new QLabel(tr("共 0 条记录"), this);
    filterLayout->addWidget(m_pLabelRowCount);

    mainLayout->addLayout(filterLayout);

    // ================================================================
    //  第二行：数据表格
    // ================================================================
    m_pTableView = new QTableView(this);
    m_pTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pTableView->setEditTriggers(QAbstractItemView::DoubleClicked
                                  | QAbstractItemView::SelectedClicked);
    m_pTableView->setAlternatingRowColors(true);
    m_pTableView->setSortingEnabled(true);
    m_pTableView->horizontalHeader()->setStretchLastSection(true);
    m_pTableView->horizontalHeader()->setSectionsMovable(true);
    m_pTableView->verticalHeader()->setVisible(true);
    mainLayout->addWidget(m_pTableView, 1);  // stretch=1：占据剩余空间

    // ================================================================
    //  第三行：操作按钮
    // ================================================================
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(6);

    m_pBtnRefresh = new QPushButton(tr("刷新"), this);
    m_pBtnRefresh->setMinimumWidth(100);
    m_pBtnRefresh->setToolTip(tr("重新从数据库加载最新数据（放弃本地未保存修改）"));

    m_pBtnSave = new QPushButton(tr("保存修改"), this);
    m_pBtnSave->setMinimumWidth(100);
    m_pBtnSave->setStyleSheet("QPushButton { font-weight: bold; }");
    m_pBtnSave->setToolTip(tr("将表格中所有修改提交到数据库"));

    m_pBtnDelete = new QPushButton(tr("删除选中行"), this);
    m_pBtnDelete->setMinimumWidth(110);
    m_pBtnDelete->setStyleSheet("QPushButton { color: red; }");
    m_pBtnDelete->setToolTip(tr("从数据库中永久删除选中的元数据记录"));

    btnLayout->addWidget(m_pBtnRefresh);
    btnLayout->addWidget(m_pBtnSave);
    btnLayout->addWidget(m_pBtnDelete);
    btnLayout->addSpacerItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum));

    m_pBtnClose = new QPushButton(tr("关闭"), this);
    m_pBtnClose->setMinimumWidth(100);
    btnLayout->addWidget(m_pBtnClose);

    mainLayout->addLayout(btnLayout);

    // ================================================================
    //  信号连接
    // ================================================================
    connect(m_pBtnRefresh,   &QPushButton::clicked, this, &CSEMetadataViewerDialog::slotRefresh);
    connect(m_pBtnSave,      &QPushButton::clicked, this, &CSEMetadataViewerDialog::slotSave);
    connect(m_pBtnDelete,    &QPushButton::clicked, this, &CSEMetadataViewerDialog::slotDeleteRow);
    connect(m_pBtnClose,     &QPushButton::clicked, this, &QDialog::close);
    connect(m_pEditFilter,   &QLineEdit::textChanged, this, &CSEMetadataViewerDialog::slotApplyFilter);
    // 切换筛选列时重新应用筛选
    connect(m_pComboFilterCol, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { slotApplyFilter(m_pEditFilter->text()); });
}

// ====================================================================
//  数据库连接
// ====================================================================

void CSEMetadataViewerDialog::OpenDatabase()
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", m_connName);
    db.setHostName(m_host.isEmpty() ? "localhost" : m_host);
    db.setPort(m_port);
    db.setDatabaseName(m_database);
    db.setUserName(m_username.isEmpty() ? "postgres" : m_username);
    if (!m_password.isEmpty())
        db.setPassword(m_password);

    // 连接选项：强制 IPv4、设置 client_encoding 为 UTF-8
    db.setConnectOptions("connect_timeout=10");

    if (!db.open())
    {
        QString err = db.lastError().text();
        QgsMessageLog::logMessage(
            tr("元数据查看器：无法连接数据库 [%1:%2/%3]: %4")
                .arg(m_host).arg(m_port).arg(m_database).arg(err),
            tr("元数据管理"), Qgis::Warning);
        QMessageBox::warning(this, tr("数据库连接失败"),
            tr("无法连接到 PostgreSQL 数据库。\n\n"
               "请先在 [导入数据到PostGIS] 中确认数据库连接正常。\n\n"
               "错误信息: %1").arg(err));
    }
}

// ====================================================================
//  加载表数据
// ====================================================================

void CSEMetadataViewerDialog::LoadTableData()
{
    QSqlDatabase db = QSqlDatabase::database(m_connName, false);
    if (!db.isOpen())
    {
        m_pLabelRowCount->setText(tr("数据库未连接"));
        m_pBtnRefresh->setEnabled(false);
        m_pBtnSave->setEnabled(false);
        m_pBtnDelete->setEnabled(false);
        return;
    }

    QString schema = m_schema.isEmpty() ? "public" : m_schema;
    QString tableName = schema + ".gis_metadata";

    // ---- 创建 Model ----
    m_pModel = new QSqlTableModel(this, db);
    m_pModel->setTable(tableName);
    m_pModel->setEditStrategy(QSqlTableModel::OnManualSubmit);

    // 先 select 获取字段信息，再设置表头别名
    if (!m_pModel->select())
    {
        QSqlError err = m_pModel->lastError();
        QgsMessageLog::logMessage(
            tr("元数据查看器：无法加载表 %1: %2").arg(tableName).arg(err.text()),
            tr("元数据管理"), Qgis::Warning);
        QMessageBox::warning(this, tr("加载失败"),
            tr("无法读取元数据表 %1。\n\n"
               "可能原因：表尚未创建（请先导入至少一条数据）\n"
               "错误信息: %2").arg(tableName).arg(err.text()));
        m_pLabelRowCount->setText(tr("加载失败"));
        return;
    }

    // ---- 用XML中的中文列名设置表头（按列名映射）----
    // 使用列名匹配，不依赖列顺序，XML删除/新增字段也不会错乱
    QMap<QString, QString> headerMap;
    // 通用
    headerMap["data_name"]              = tr("数据名称");
    headerMap["data_alias"]             = tr("数据别名");
    headerMap["data_type"]              = tr("数据类型");
    headerMap["data_format"]            = tr("数据格式");
    headerMap["data_version"]           = tr("版本");
    headerMap["west_longitude"]         = tr("西经");
    headerMap["east_longitude"]         = tr("东经");
    headerMap["south_latitude"]         = tr("南纬");
    headerMap["north_latitude"]         = tr("北纬");
    headerMap["min_altitude"]           = tr("最低海拔");
    headerMap["max_altitude"]           = tr("最高海拔");
    headerMap["coordinate_system"]      = tr("坐标系");
    headerMap["production_date"]        = tr("生产日期");
    headerMap["production_organization"]= tr("生产单位");
    headerMap["import_date"]            = tr("导入日期");
    headerMap["update_date"]            = tr("更新日期");
    headerMap["validity_start"]         = tr("有效期起始");
    headerMap["validity_end"]           = tr("有效期截止");
    headerMap["upload_user"]            = tr("上传用户");
    headerMap["upload_user_id"]         = tr("用户ID");
    headerMap["department"]             = tr("部门");
    headerMap["project_name"]           = tr("项目名称");
    headerMap["data_source"]            = tr("数据来源");
    headerMap["data_quality_desc"]      = tr("质量描述");
    headerMap["accuracy_level"]         = tr("精度级别");
    headerMap["confidentiality_level"]  = tr("密级");
    headerMap["tags"]                   = tr("标签");
    headerMap["description"]            = tr("描述");
    headerMap["storage_path"]           = tr("存储路径");
    headerMap["storage_type"]           = tr("存储类型");
    headerMap["file_size_mb"]           = tr("文件大小MB");
    headerMap["table_name"]             = tr("表名");
    headerMap["data_status"]            = tr("数据状态");
    headerMap["is_public"]              = tr("是否公开");
    headerMap["is_latest"]              = tr("是否最新");
    headerMap["parent_data_id"]         = tr("父数据ID");
    headerMap["created_at"]             = tr("创建时间");
    headerMap["updated_at"]             = tr("更新时间");
    headerMap["last_access_time"]       = tr("最后访问");
    headerMap["access_count"]           = tr("访问次数");
    headerMap["remark"]                 = tr("备注");
    // 矢量
    headerMap["geom_type"]              = tr("几何类型");
    headerMap["feature_count"]          = tr("要素数量");
    headerMap["attribute_fields"]       = tr("属性字段");
    headerMap["field_count"]            = tr("字段数");
    headerMap["topology_validity"]      = tr("拓扑有效性");
    headerMap["topology_errors"]        = tr("拓扑错误");
    headerMap["has_m_value"]            = tr("含M值");
    headerMap["has_z_value"]            = tr("含Z值");
    headerMap["encoding"]               = tr("编码");
    headerMap["shp_type"]               = tr("SHP类型");
    // 栅格
    headerMap["resolution_x"]           = tr("X分辨率");
    headerMap["resolution_y"]           = tr("Y分辨率");
    headerMap["band_count"]             = tr("波段数");
    headerMap["band_info"]              = tr("波段信息");
    headerMap["pixel_type"]             = tr("像素类型");
    headerMap["pixel_depth"]            = tr("像素位深");
    headerMap["col_count"]              = tr("列数");
    headerMap["row_count"]              = tr("行数");
    headerMap["has_pyramid"]            = tr("含金字塔");
    headerMap["pyramid_levels"]         = tr("金字塔层级");
    headerMap["compression_type"]       = tr("压缩类型");
    headerMap["has_overviews"]          = tr("含概视图");
    headerMap["color_interpretation"]   = tr("颜色解释");
    headerMap["no_data_value"]          = tr("NoData值");
    headerMap["geotransform"]           = tr("仿射变换");
    headerMap["projection_wkt"]         = tr("投影WKT");
    headerMap["cloud_cover"]            = tr("云覆盖率");
    headerMap["satellite_name"]         = tr("卫星名称");
    headerMap["acquisition_time"]       = tr("采集时间");
    headerMap["tile_format"]            = tr("分块格式");
    // 3D
    headerMap["model_type"]             = tr("模型类型");
    headerMap["model_subtype"]          = tr("模型子类型");
    headerMap["triangle_count"]         = tr("三角面数");
    headerMap["vertex_count"]           = tr("顶点数");
    headerMap["texture_count"]          = tr("纹理数");
    headerMap["texture_format"]         = tr("纹理格式");
    headerMap["texture_resolution"]     = tr("纹理分辨率");
    headerMap["has_color"]              = tr("含颜色");
    headerMap["has_normal"]             = tr("含法线");
    headerMap["has_uv"]                 = tr("含UV");
    headerMap["animation_count"]        = tr("动画数");
    headerMap["file_count"]             = tr("文件数");
    headerMap["lod_levels"]             = tr("LOD级别");
    headerMap["model_center_x"]         = tr("中心X");
    headerMap["model_center_y"]         = tr("中心Y");
    headerMap["model_center_z"]         = tr("中心Z");
    headerMap["bounding_box"]           = tr("包围盒");
    headerMap["bounding_sphere_radius"] = tr("包围球半径");
    headerMap["point_cloud_density"]    = tr("点云密度");
    headerMap["point_cloud_count"]      = tr("点云数");
    headerMap["model_accuracy"]         = tr("模型精度");
    headerMap["building_function"]      = tr("建筑功能");
    headerMap["floor_count"]            = tr("楼层数");
    headerMap["structure_type"]         = tr("结构类型");
    headerMap["export_tool"]            = tr("导出工具");
    headerMap["coordinate_origin"]      = tr("坐标原点");

    // 遍历所有列，设置表头
    QSqlRecord rec = m_pModel->record();
    for (int col = 0; col < rec.count(); col++)
    {
        QString fieldName = rec.fieldName(col);
        if (headerMap.contains(fieldName))
            m_pModel->setHeaderData(col, Qt::Horizontal, headerMap[fieldName]);
    }

    // ---- 创建 ProxyModel 用于筛选 ----
    m_pProxyModel = new MetadataFilterProxy(this);
    m_pProxyModel->setSourceModel(m_pModel);
    m_pProxyModel->setDynamicSortFilter(true);

    m_pTableView->setModel(m_pProxyModel);

    // ---- 填充筛选列下拉框 ----
    m_pComboFilterCol->clear();
    m_pComboFilterCol->addItem(tr("— 全部列 —"), -1);
    for (int col = 0; col < rec.count(); col++)
    {
        QString colName = rec.fieldName(col);
        QString displayName = headerMap.value(colName, colName);
        m_pComboFilterCol->addItem(displayName, col);
    }

    // ---- 隐藏 data_id 主键列（UUID 对用户无意义）----
    int dataIdCol = rec.indexOf("data_id");
    if (dataIdCol >= 0)
    {
        // 通过 proxy model 找到对应的列索引
        QModelIndex proxyIdx = m_pProxyModel->mapFromSource(
            m_pModel->index(0, dataIdCol));
        if (proxyIdx.isValid())
            m_pTableView->setColumnHidden(proxyIdx.column(), true);
    }

    // ---- 设置合理的列宽 ----
    // 对所有列设置默认宽度，避免宽字段撑爆表格
    for (int col = 0; col < m_pProxyModel->columnCount(); col++)
    {
        m_pTableView->setColumnWidth(col, 120);
    }
    // 对名称、描述等宽列特殊处理
    int dataNameCol = rec.indexOf("data_name");
    if (dataNameCol >= 0)
    {
        QModelIndex pi = m_pProxyModel->mapFromSource(m_pModel->index(0, dataNameCol));
        if (pi.isValid()) m_pTableView->setColumnWidth(pi.column(), 180);
    }
    int descCol = rec.indexOf("description");
    if (descCol >= 0)
    {
        QModelIndex pi = m_pProxyModel->mapFromSource(m_pModel->index(0, descCol));
        if (pi.isValid()) m_pTableView->setColumnWidth(pi.column(), 200);
    }
    int pathCol = rec.indexOf("storage_path");
    if (pathCol >= 0)
    {
        QModelIndex pi = m_pProxyModel->mapFromSource(m_pModel->index(0, pathCol));
        if (pi.isValid()) m_pTableView->setColumnWidth(pi.column(), 250);
    }

    // ---- 更新行数标签 ----
    int rowCount = m_pModel->rowCount();
    m_pLabelRowCount->setText(tr("共 %1 条记录").arg(rowCount));
}

// ====================================================================
//  槽函数
// ====================================================================

void CSEMetadataViewerDialog::slotRefresh()
{
    if (!m_pModel)
        return;

    // 检查是否有未保存的修改
    if (m_pModel->isDirty())
    {
        int ret = QMessageBox::question(this, tr("刷新"),
            tr("表格中有未保存的修改。\n\n"
               "刷新数据将丢失所有未保存的修改。\n是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
    }

    // 重新查询
    m_pModel->select();

    // 更新行数
    m_pLabelRowCount->setText(tr("共 %1 条记录").arg(m_pModel->rowCount()));

    QgsMessageLog::logMessage(
        tr("元数据表已刷新，共 %1 条记录").arg(m_pModel->rowCount()),
        tr("元数据管理"), Qgis::Info);
}

void CSEMetadataViewerDialog::slotSave()
{
    if (!m_pModel)
        return;

    if (!m_pModel->isDirty())
    {
        QMessageBox::information(this, tr("保存"), tr("没有需要保存的修改。"));
        return;
    }

    // 将焦点移出表格，结束当前正在编辑的单元格（如有）并提交到 model
    m_pBtnSave->setFocus();

    if (!m_pModel->submitAll())
    {
        QSqlError err = m_pModel->lastError();
        QgsMessageLog::logMessage(
            tr("元数据保存失败: %1").arg(err.text()),
            tr("元数据管理"), Qgis::Warning);
        QMessageBox::critical(this, tr("保存失败"),
            tr("提交修改到数据库时出错。\n\n%1").arg(err.text()));
        return;
    }

    int rowCount = m_pModel->rowCount();
    m_pLabelRowCount->setText(tr("共 %1 条记录").arg(rowCount));
    QgsMessageLog::logMessage(
        tr("元数据修改已保存到数据库（%1 条记录）").arg(rowCount),
        tr("元数据管理"), Qgis::Info);
    QMessageBox::information(this, tr("保存"), tr("修改已成功保存到数据库。"));
}

void CSEMetadataViewerDialog::slotDeleteRow()
{
    if (!m_pModel)
        return;

    QModelIndex proxyIdx = m_pTableView->currentIndex();
    if (!proxyIdx.isValid())
    {
        QMessageBox::information(this, tr("删除"),
            tr("请先在表格中选中要删除的行。"));
        return;
    }

    QModelIndex srcIdx = m_pProxyModel->mapToSource(proxyIdx);
    int row = srcIdx.row();

    // 获取该行部分信息用于确认提示
    QString dataName;
    int nameCol = m_pModel->record().indexOf("data_name");
    if (nameCol >= 0)
        dataName = m_pModel->data(m_pModel->index(row, nameCol)).toString();

    int ret = QMessageBox::question(this, tr("确认删除"),
        tr("确定要永久删除以下元数据记录吗？\n\n"
           "数据名称: %1\n\n"
           "此操作不可撤销！").arg(dataName.isEmpty() ? tr("(空)") : dataName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (ret != QMessageBox::Yes)
        return;

    if (!m_pModel->removeRow(row))
    {
        QgsMessageLog::logMessage(
            tr("删除行失败: %1").arg(m_pModel->lastError().text()),
            tr("元数据管理"), Qgis::Warning);
        return;
    }

    // 立即提交删除
    if (!m_pModel->submitAll())
    {
        QSqlError err = m_pModel->lastError();
        QgsMessageLog::logMessage(
            tr("元数据删除提交失败: %1").arg(err.text()),
            tr("元数据管理"), Qgis::Warning);
        QMessageBox::critical(this, tr("删除失败"),
            tr("无法删除数据库中的记录。\n\n%1").arg(err.text()));
        m_pModel->revertAll();  // 回滚失败的删除
        return;
    }

    // 重新查询以更新表格
    m_pModel->select();
    m_pLabelRowCount->setText(tr("共 %1 条记录").arg(m_pModel->rowCount()));
    QgsMessageLog::logMessage(
        tr("已删除元数据记录: %1").arg(dataName),
        tr("元数据管理"), Qgis::Info);
}

void CSEMetadataViewerDialog::slotApplyFilter(const QString& text)
{
    if (!m_pProxyModel)
        return;

    int filterCol = m_pComboFilterCol->currentData().toInt();
    static_cast<MetadataFilterProxy*>(m_pProxyModel)->setSearchFilter(filterCol, text);

    // 更新行数（过滤后）
    int visibleRows = m_pProxyModel->rowCount();
    int totalRows = m_pModel ? m_pModel->rowCount() : 0;
    if (text.isEmpty())
        m_pLabelRowCount->setText(tr("共 %1 条记录").arg(totalRows));
    else
        m_pLabelRowCount->setText(tr("筛选: %1 / %2 条").arg(visibleRows).arg(totalRows));
}
