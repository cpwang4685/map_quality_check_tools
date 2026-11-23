// 数据目录树：数据库数据 / 本地数据 / 地图图层 三段合成的一棵勾选树。
//
// 【2026-10-25 数据裁剪改造】原来这套是从 LTZK 平台左侧数据列表（DataCatalogWidget）
// 整段移植的三段式布局：垂直 QSplitter 里塞三个带标题栏的子面板，每段一个独立的树
// （数据库走 QStandardItemModel、本地走 QgsBrowserTreeView、地图图层走 QTreeWidget），
// 没有复选框，只能右键单个节点导出一个。
// 现在按「从数据库获取数据源」对话框（ProductSourceDialog）的数据列表重做：
//   - 一棵 QTreeWidget，三个固定顶级节点；
//   - 可裁剪的数据节点带复选框，勾父带子、子全勾父自动勾（含半选态）；
//   - 图标按 点/线/面/栅格 区分；
//   - 目录 / GDB / GPKG 展开时才枚举子节点（懒加载）；导出时若子节点还没读出来，
//     直接扫磁盘，所以"勾了没展开的文件夹"照样能导全。
// "已连接的本地文件夹"仍然存在 QGIS 收藏夹里（与平台左侧数据列表共用一份清单），
// 这里只把收藏夹当清单用，不挂任何视图。

#include "data_catalog_browser.h"

#include "core/layer_icon_helper.h"

#include "qgsbrowserguimodel.h"
#include "qgsbrowsermodel.h"
#include "qgsdataitem.h"
#include "qgsdatasourceuri.h"
#include "qgsdirectoryitem.h"
#include "qgsmaplayer.h"
#include "qgsproject.h"
#include "qgsproviderregistry.h"
#include "qgsprovidermetadata.h"
#include "qgsprovidersublayerdetails.h"
#include "qgsrasterlayer.h"
#include "qgsvectorlayer.h"
#include "qgswkbtypes.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <functional>

namespace
{
const char* kCatalogDatabaseConnectionPrefix = "CSE_DataCatalogPostgisConnection";

// 节点种类
enum NodeKind
{
    KindRootDatabase = 0,  // 顶级：数据库数据
    KindDatabase,          // 数据库本身（库名）
    KindSchema,            // 架构
    KindTable,             // 数据表 / 空间图层
    KindRootLocal,         // 顶级：本地数据
    KindLocalFolder,       // 已连接的文件夹根
    KindLocalDir,          // 子目录
    KindLocalContainer,    // 容器数据（.gdb 目录 / .gpkg 文件）
    KindLocalLayer,        // 容器里的图层
    KindLocalFile,         // 单个数据文件
    KindRootMapLayer,      // 顶级：地图图层
    KindMapLayer,          // 当前工程里的图层
    KindPlaceholder,       // 懒加载占位
    KindMessage            // 灰字提示（未连接 / 空 / 警告）
};

// 节点上的自定义数据角色
const int kRoleKind = Qt::UserRole;
const int kRoleIsData = Qt::UserRole + 1;     // 该节点本身能不能产出裁剪数据
const int kRolePath = Qt::UserRole + 2;       // 本地路径（文件夹/文件/容器）
const int kRoleSchema = Qt::UserRole + 3;
const int kRoleTable = Qt::UserRole + 4;
const int kRoleGeom = Qt::UserRole + 5;
const int kRoleDataType = Qt::UserRole + 6;   // "vector" / "raster" / "table"
const int kRoleLayerName = Qt::UserRole + 7;  // 容器内部图层名
const int kRoleUri = Qt::UserRole + 8;        // 现成 uri（容器子图层）
const int kRoleIsRaster = Qt::UserRole + 9;
const int kRoleMapLayerId = Qt::UserRole + 10;
const int kRoleLazy = Qt::UserRole + 11;      // 子节点还没读（展开时才读）

// ---------- 扩展名判定 ----------

bool isRasterExtension(const QString& lower)
{
    return lower.endsWith(QStringLiteral(".tif")) || lower.endsWith(QStringLiteral(".tiff"))
        || lower.endsWith(QStringLiteral(".img")) || lower.endsWith(QStringLiteral(".png"))
        || lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg"))
        || lower.endsWith(QStringLiteral(".bmp")) || lower.endsWith(QStringLiteral(".ecw"));
}

bool isDataExtension(const QString& lower)
{
    return isRasterExtension(lower)
        || lower.endsWith(QStringLiteral(".shp")) || lower.endsWith(QStringLiteral(".gpkg"))
        || lower.endsWith(QStringLiteral(".json")) || lower.endsWith(QStringLiteral(".geojson"))
        || lower.endsWith(QStringLiteral(".tab")) || lower.endsWith(QStringLiteral(".mif"))
        || lower.endsWith(QStringLiteral(".kml"));
}

// .gdb 目录 / .gpkg 文件：一个路径里装多个图层，要枚举内部图层
bool isContainerPath(const QString& path)
{
    const QString lower = path.toLower();
    const QFileInfo fi(path);
    if (lower.endsWith(QStringLiteral(".gdb")) && fi.isDir())
    {
        return true;
    }
    if (lower.endsWith(QStringLiteral(".gpkg")) && fi.isFile())
    {
        return true;
    }
    return false;
}

// 点/线/面 图标种类（与 LayerIconHelper::drawLayerIcon 的约定一致）；认不出返回 -1
int iconKindFromWkb(QgsWkbTypes::Type type)
{
    switch (QgsWkbTypes::flatType(type))
    {
    case QgsWkbTypes::Point:            return 0;
    case QgsWkbTypes::LineString:
    case QgsWkbTypes::CircularString:
    case QgsWkbTypes::CompoundCurve:    return 1;
    case QgsWkbTypes::Polygon:
    case QgsWkbTypes::CurvePolygon:
    case QgsWkbTypes::Triangle:         return 2;
    default:                            return -1;
    }
}

// Shapefile 的几何类型写在 .shp 文件头偏移 32 处的 int32（小端），读 40 字节就够。
// 打开文件只为画图标，比开一个 OGR 数据源便宜得多。
int shpIconKind(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        return -1;
    }
    const QByteArray head = f.read(40);
    f.close();
    // 文件码固定为 9994（大端），借它确认这不是别的同名文件
    if (head.size() < 36 || static_cast<uchar>(head.at(0)) != 0
        || static_cast<uchar>(head.at(1)) != 0
        || static_cast<uchar>(head.at(2)) != 0x27
        || static_cast<uchar>(head.at(3)) != 0x0a)
    {
        return -1;
    }
    const int shapeType = static_cast<int>(static_cast<uchar>(head.at(32)))
        | (static_cast<int>(static_cast<uchar>(head.at(33))) << 8)
        | (static_cast<int>(static_cast<uchar>(head.at(34))) << 16)
        | (static_cast<int>(static_cast<uchar>(head.at(35))) << 24);
    if (shapeType == 1 || shapeType == 11 || shapeType == 21)
    {
        return 0;
    }
    if (shapeType == 3 || shapeType == 13 || shapeType == 23)
    {
        return 1;
    }
    if (shapeType == 5 || shapeType == 15 || shapeType == 25)
    {
        return 2;
    }
    return -1;
}

// 单个本地数据文件 → QGIS 数据源（容器文件不走这里）
QgsMimeDataUtils::Uri localFileUri(const QString& path)
{
    QgsMimeDataUtils::Uri uri;
    const QFileInfo fi(path);
    if (!fi.isFile())
    {
        return uri;
    }
    const QString lower = path.toLower();
    if (!isDataExtension(lower))
    {
        return uri;
    }
    uri.uri = path;
    uri.providerKey = QStringLiteral("ogr");
    uri.layerType = isRasterExtension(lower)
        ? QStringLiteral("raster")
        : QStringLiteral("vector");
    uri.name = fi.completeBaseName();
    return uri;
}

// ---------- 数据库 ----------

QSqlDatabase catalogDatabase(const QString& connectionName)
{
    if (connectionName.isEmpty() || !QSqlDatabase::contains(connectionName))
    {
        return QSqlDatabase();
    }
    return QSqlDatabase::database(connectionName, false);
}

QString quotedSqlIdentifier(const QString& identifier)
{
    return QStringLiteral("\"%1\"")
        .arg(identifier.trimmed().replace(QStringLiteral("\""), QStringLiteral("\"\"")));
}

QString databaseCatalogQuery()
{
    return QStringLiteral(
        "SELECT t.table_schema, t.table_name "
        "FROM information_schema.tables t "
        "WHERE t.table_type IN ('BASE TABLE', 'VIEW', 'FOREIGN') "
        "  AND t.table_schema NOT IN ('pg_catalog', 'information_schema', 'topology') "
        "  AND t.table_schema NOT LIKE 'pg_toast%' "
        "  AND t.table_name NOT IN ("
        "    'user_permissions', 'spatial_ref_sys', 'geometry_columns', "
        "    'geography_columns', 'raster_columns', 'raster_overviews'"
        "  ) "
        "  AND has_table_privilege(format('%I.%I', t.table_schema, t.table_name), 'SELECT') "
        "ORDER BY t.table_schema, t.table_name");
}

QString databaseVectorMetadataQuery()
{
    return QStringLiteral(
        "SELECT f_table_schema, f_table_name, f_geometry_column, type, srid "
        "FROM geometry_columns");
}

QString databaseRasterMetadataQuery()
{
    return QStringLiteral(
        "SELECT r_table_schema, r_table_name, r_raster_column, 'RASTER', srid "
        "FROM raster_columns");
}

bool databaseMetadataRelationExists(
    QSqlDatabase database,
    const QString& relationName,
    QString* errorMessage)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT to_regclass(:relation_name) IS NOT NULL"));
    query.bindValue(QStringLiteral(":relation_name"), relationName);
    if (!query.exec())
    {
        if (errorMessage)
        {
            *errorMessage = query.lastError().text();
        }
        return false;
    }
    if (!query.next())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("未返回元数据关系检查结果。");
        }
        return false;
    }
    return query.value(0).toBool();
}

struct DatabaseCatalogEntry
{
    QString schemaName;
    QString tableName;
    QString dataColumn;
    QString dataType;
    int srid = 0;
    QString layerType = QStringLiteral("table");
};

QString databaseCatalogKey(const QString& schemaName, const QString& tableName)
{
    return schemaName + QChar(0x1f) + tableName;
}

bool isSupportedCatalogUri(const QgsMimeDataUtils::Uri& uri)
{
    return uri.layerType == QStringLiteral("vector") ||
           uri.layerType == QStringLiteral("raster");
}
} // namespace

// ============================================================
// 构造 / 界面
// ============================================================
CSE_DataCatalogBrowser::CSE_DataCatalogBrowser(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("dataCatalogBrowser"));
    buildUi();
    initializeFavoritesModel();
    refreshDatabaseData();
    refreshLocalFolders();
    refreshMapLayers();
}

CSE_DataCatalogBrowser::~CSE_DataCatalogBrowser()
{
    closeDatabaseConnection();
}

void CSE_DataCatalogBrowser::buildUi()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("dataCatalogTree"));
    m_tree->setColumnCount(1);
    // 【2026-10-25】列表格式对齐「从数据库获取数据源」对话框：单列 + 一个表头标题
    m_tree->setHeaderLabels(QStringList() << QStringLiteral("数据名称"));
    m_tree->header()->setStretchLastSection(true);
    m_tree->setHeaderHidden(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // 双击数据节点 = 添加到地图，别顺手把目录展开/收起
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setIndentation(16);
    m_tree->setAnimated(false);
    layout->addWidget(m_tree, 1);

    connect(m_tree, &QTreeWidget::itemChanged,
        this, &CSE_DataCatalogBrowser::onTreeItemChanged);
    connect(m_tree, &QTreeWidget::itemExpanded,
        this, &CSE_DataCatalogBrowser::onTreeItemExpanded);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
        this, &CSE_DataCatalogBrowser::onTreeItemDoubleClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
        this, &CSE_DataCatalogBrowser::onTreeContextMenu);

    buildRoots();
}

void CSE_DataCatalogBrowser::buildRoots()
{
    const bool prev = m_syncing;
    m_syncing = true;

    m_rootDatabase = new QTreeWidgetItem(m_tree);
    m_rootDatabase->setText(0, QStringLiteral("数据库数据"));
    m_rootDatabase->setData(0, kRoleKind, int(KindRootDatabase));
    m_rootDatabase->setIcon(0, style()->standardIcon(QStyle::SP_DriveNetIcon));
    m_rootDatabase->setFlags(m_rootDatabase->flags() | Qt::ItemIsUserCheckable);
    m_rootDatabase->setCheckState(0, Qt::Unchecked);

    m_rootLocal = new QTreeWidgetItem(m_tree);
    m_rootLocal->setText(0, QStringLiteral("本地数据"));
    m_rootLocal->setData(0, kRoleKind, int(KindRootLocal));
    m_rootLocal->setIcon(0, style()->standardIcon(QStyle::SP_DriveHDIcon));
    m_rootLocal->setFlags(m_rootLocal->flags() | Qt::ItemIsUserCheckable);
    m_rootLocal->setCheckState(0, Qt::Unchecked);

    m_rootMapLayer = new QTreeWidgetItem(m_tree);
    m_rootMapLayer->setText(0, QStringLiteral("地图图层"));
    m_rootMapLayer->setData(0, kRoleKind, int(KindRootMapLayer));
    QIcon layersIcon = QgsApplication::getThemeIcon(QStringLiteral("mActionAddGroup.svg"));
    if (layersIcon.isNull())
    {
        layersIcon = style()->standardIcon(QStyle::SP_DirIcon);
    }
    m_rootMapLayer->setIcon(0, layersIcon);
    // 地图里的图层没有能喂给裁剪引擎的 uri，这里只用于"添加到地图 / 定位"，
    // 不参与勾选，免得勾了也导不出来让人误解
    m_rootMapLayer->setFlags(m_rootMapLayer->flags() & ~Qt::ItemIsUserCheckable);
    m_rootMapLayer->setToolTip(0,
        QStringLiteral("这里列出当前工程已加载的图层，双击可定位到该图层"));

    m_syncing = prev;
}

QTreeWidgetItem* CSE_DataCatalogBrowser::addNode(QTreeWidgetItem* parent,
    int kind, const QString& text, bool checkable)
{
    QTreeWidgetItem* item = parent
        ? new QTreeWidgetItem(parent)
        : new QTreeWidgetItem(m_tree);
    item->setText(0, text);
    item->setData(0, kRoleKind, kind);
    item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    if (checkable)
    {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
    }
    else
    {
        // QTreeWidgetItem 默认带 ItemIsUserCheckable，不清掉会画出一个勾选框
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }
    return item;
}

QTreeWidgetItem* CSE_DataCatalogBrowser::addMessageNode(QTreeWidgetItem* parent,
    const QString& text, bool isError)
{
    QTreeWidgetItem* item = addNode(parent, KindMessage, text, false);
    item->setForeground(0, QBrush(QColor(isError ? 192 : 150, isError ? 57 : 150, isError ? 43 : 150)));
    if (isError)
    {
        item->setIcon(0, style()->standardIcon(QStyle::SP_MessageBoxWarning));
    }
    return item;
}

void CSE_DataCatalogBrowser::markLazy(QTreeWidgetItem* item)
{
    if (!item)
    {
        return;
    }
    item->setData(0, kRoleLazy, true);
    QTreeWidgetItem* placeholder = new QTreeWidgetItem(item);
    placeholder->setText(0, QStringLiteral("正在读取…"));
    placeholder->setData(0, kRoleKind, int(KindPlaceholder));
    placeholder->setFlags(Qt::ItemIsEnabled);
    placeholder->setForeground(0, QBrush(QColor(160, 160, 160)));
}

void CSE_DataCatalogBrowser::clearChildren(QTreeWidgetItem* item)
{
    if (!item)
    {
        return;
    }
    while (item->childCount() > 0)
    {
        delete item->takeChild(0);
    }
    item->setData(0, kRoleLazy, false);
}

// ============================================================
// 数据库段
// ============================================================
void CSE_DataCatalogBrowser::connectDatabase()
{
    DataCatalogConnectionDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const DataCatalogConnectionParameters parameters = dialog.parameters();
    bool portOk = false;
    const int port = parameters.port.toInt(&portOk);
    if (parameters.host.isEmpty() ||
        !portOk ||
        port < 1 ||
        port > 65535 ||
        parameters.database.isEmpty() ||
        parameters.user.isEmpty())
    {
        QMessageBox::warning(
            this,
            QStringLiteral("数据库连接参数无效"),
            QStringLiteral("请填写主机、有效端口、数据库名称和用户名。"));
        emit message(QStringLiteral("数据库连接参数不完整，请点击「选择数据库」重新填写。"), true);
        return;
    }

    QString errorMessage;
    if (!openDatabaseConnection(parameters, &errorMessage))
    {
        const QString text = errorMessage.isEmpty()
            ? QStringLiteral("无法打开数据库连接。")
            : errorMessage;
        QMessageBox::warning(this, QStringLiteral("数据库连接失败"), text);
        emit message(QStringLiteral("数据库连接失败：%1").arg(text), true);
        return;
    }

    if (dialog.rememberConnection())
    {
        dialog.saveRememberedConnection();
    }
    else
    {
        dialog.clearRememberedConnection();
    }

    emit message(QStringLiteral("已连接数据库：%1").arg(parameters.database), false);
    refreshDatabaseData();
}

bool CSE_DataCatalogBrowser::openDatabaseConnection(
    const DataCatalogConnectionParameters& parameters,
    QString* errorMessage)
{
    const QString connectionName = QStringLiteral("%1_%2_%3")
        .arg(QString::fromLatin1(kCatalogDatabaseConnectionPrefix))
        .arg(QString::number(reinterpret_cast<quintptr>(this), 16))
        .arg(QString::number(++m_databaseConnectionSerial));

    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QPSQL"),
        connectionName);
    database.setHostName(parameters.host);
    database.setPort(parameters.port.toInt());
    database.setDatabaseName(parameters.database);
    database.setUserName(parameters.user);
    database.setPassword(parameters.password);

    const auto discardConnection = [&database, &connectionName]()
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    };

    if (!database.open())
    {
        if (errorMessage)
        {
            *errorMessage = database.lastError().text();
        }
        discardConnection();
        return false;
    }

    const QString schema = parameters.schema.trimmed();
    if (!schema.isEmpty() && schema.compare(QStringLiteral("public"), Qt::CaseInsensitive) != 0)
    {
        QSqlQuery searchPathQuery(database);
        if (!searchPathQuery.exec(QStringLiteral("SET search_path TO %1, public")
                .arg(quotedSqlIdentifier(schema))))
        {
            if (errorMessage)
            {
                *errorMessage = searchPathQuery.lastError().text();
            }
            discardConnection();
            return false;
        }
    }

    closeDatabaseConnection();
    m_databaseConnectionName = connectionName;
    return true;
}

void CSE_DataCatalogBrowser::closeDatabaseConnection()
{
    const QString connectionName = m_databaseConnectionName;
    m_databaseConnectionName.clear();
    if (connectionName.isEmpty() || !QSqlDatabase::contains(connectionName))
    {
        return;
    }

    QSqlDatabase database = QSqlDatabase::database(connectionName, false);
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

QIcon CSE_DataCatalogBrowser::databaseItemIcon(const QString& layerType,
    const QString& geometryType) const
{
    if (layerType == QStringLiteral("raster"))
    {
        return LayerIconHelper::drawLayerIcon(3);
    }
    if (layerType == QStringLiteral("table"))
    {
        const QIcon icon(QStringLiteral(":/images/themes/default/mActionOpenTable.svg"));
        return icon.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : icon;
    }

    // 矢量：按 geometry_columns 里的类型字符串画 点/线/面
    const QString geometry = geometryType.toLower();
    if (geometry.contains(QStringLiteral("point")))
    {
        return LayerIconHelper::drawLayerIcon(0);
    }
    if (geometry.contains(QStringLiteral("line")) || geometry.contains(QStringLiteral("curve")))
    {
        return LayerIconHelper::drawLayerIcon(1);
    }
    return LayerIconHelper::drawLayerIcon(2);
}

// 数据库表 → QGIS 数据源（照平台 databaseUri 的普通表分支）
QgsMimeDataUtils::Uri CSE_DataCatalogBrowser::databaseUriFor(const QString& schema,
    const QString& table, const QString& geometryColumn, const QString& layerType) const
{
    QgsMimeDataUtils::Uri uri;
    if (table.isEmpty())
    {
        return uri;
    }
    if (layerType != QStringLiteral("vector") && layerType != QStringLiteral("raster"))
    {
        return uri;
    }

    QSqlDatabase database = catalogDatabase(m_databaseConnectionName);
    if (!database.isValid() || !database.isOpen())
    {
        return uri;
    }

    QgsDataSourceUri sourceUri;
    sourceUri.setConnection(
        database.hostName(),
        QString::number(database.port()),
        database.databaseName(),
        database.userName(),
        database.password());
    sourceUri.setDataSource(schema, table, geometryColumn);

    uri.layerType = (layerType == QStringLiteral("raster"))
        ? QStringLiteral("raster")
        : QStringLiteral("vector");
    uri.providerKey = (uri.layerType == QStringLiteral("raster"))
        ? QStringLiteral("postgresraster")
        : QStringLiteral("postgres");
    uri.name = table;
    uri.uri = sourceUri.uri(false);
    return uri;
}

// 数据库栅格的 GDAL 连接串候选：不同 GDAL 版本对 schema/table/column 的写法不同，
// 由裁剪引擎在打开时逐个探测（见 ClipExportEngine::firstOpenableRasterUri）。
QStringList CSE_DataCatalogBrowser::databaseRasterUriCandidates(const QString& schema,
    const QString& table, const QString& geometryColumn) const
{
    QStringList candidates;
    QSqlDatabase database = catalogDatabase(m_databaseConnectionName);
    if (!database.isValid() || !database.isOpen())
    {
        return candidates;
    }

    const QString column = geometryColumn.isEmpty()
        ? QStringLiteral("rast") : geometryColumn;
    const QString common = QStringLiteral("PG:host=%1 port=%2 dbname=%3 user=%4 password=%5")
        .arg(database.hostName())
        .arg(QString::number(database.port()))
        .arg(database.databaseName())
        .arg(database.userName())
        .arg(database.password());

    // ① 表名带 schema 引号（GDAL PostGISRaster 文档写法）
    candidates << QStringLiteral("%1 table=%2 column=%3")
        .arg(common,
            quotedSqlIdentifier(schema) + QStringLiteral(".") + quotedSqlIdentifier(table),
            column);
    // ② 独立 schema 关键字
    candidates << QStringLiteral("%1 schema=%2 table=%3 column=%4")
        .arg(common, schema, table, column);
    // ③ 表名直接写 schema.table
    candidates << QStringLiteral("%1 table=%2.%3 column=%4")
        .arg(common, schema, table, column);
    return candidates;
}

void CSE_DataCatalogBrowser::refreshDatabaseData()
{
    const QSet<QString> keep = checkedKeys();
    const bool prevSyncing = m_syncing;
    m_syncing = true;
    clearChildren(m_rootDatabase);

    if (m_databaseConnectionName.isEmpty())
    {
        addMessageNode(m_rootDatabase,
            QStringLiteral("未连接数据库（点「选择数据库」连接）"), false);
    }
    else
    {
        QSqlDatabase database = catalogDatabase(m_databaseConnectionName);
        if (!database.isValid() || !database.isOpen())
        {
            addMessageNode(m_rootDatabase,
                QStringLiteral("数据库连接已断开，请点「选择数据库」重连。"), true);
        }
        else
        {
            QTreeWidgetItem* databaseItem = addNode(m_rootDatabase, KindDatabase,
                database.databaseName(), true);
            // 与「从数据库获取数据源」一致的库名图标
            databaseItem->setIcon(0, style()->standardIcon(QStyle::SP_DirHomeIcon));
            databaseItem->setToolTip(0,
                QStringLiteral("数据库：%1").arg(database.databaseName()));

            QSqlQuery query(database);
            if (!query.exec(databaseCatalogQuery()))
            {
                addMessageNode(databaseItem,
                    QStringLiteral("读取数据库目录失败：%1").arg(query.lastError().text()), true);
            }
            else
            {
                QList<DatabaseCatalogEntry> entries;
                QHash<QString, int> firstEntryByTable;
                while (query.next())
                {
                    DatabaseCatalogEntry entry;
                    entry.schemaName = query.value(0).toString();
                    entry.tableName = query.value(1).toString();
                    firstEntryByTable.insert(
                        databaseCatalogKey(entry.schemaName, entry.tableName),
                        entries.size());
                    entries.append(entry);
                }

                QStringList metadataWarnings;
                const auto appendSpatialEntries = [&database, &entries, &firstEntryByTable](
                    const QString& relationName,
                    const QString& metadataQuery,
                    const QString& layerType,
                    QStringList* warnings)
                {
                    QString probeError;
                    if (!databaseMetadataRelationExists(database, relationName, &probeError))
                    {
                        if (!probeError.isEmpty())
                        {
                            warnings->append(
                                QStringLiteral("检查 %1 失败：%2").arg(relationName, probeError));
                        }
                        return;
                    }

                    QSqlQuery metadata(database);
                    if (!metadata.exec(metadataQuery))
                    {
                        warnings->append(
                            QStringLiteral("读取 %1 失败：%2")
                                .arg(relationName, metadata.lastError().text()));
                        return;
                    }

                    while (metadata.next())
                    {
                        const QString schemaName = metadata.value(0).toString();
                        const QString tableName = metadata.value(1).toString();
                        const int entryIndex = firstEntryByTable.value(
                            databaseCatalogKey(schemaName, tableName), -1);
                        if (entryIndex < 0)
                        {
                            continue;
                        }

                        DatabaseCatalogEntry spatialEntry = entries.at(entryIndex);
                        spatialEntry.dataColumn = metadata.value(2).toString();
                        spatialEntry.dataType = metadata.value(3).toString();
                        spatialEntry.srid = metadata.value(4).toInt();
                        spatialEntry.layerType = layerType;
                        if (entries.at(entryIndex).layerType == QStringLiteral("table"))
                        {
                            entries[entryIndex] = spatialEntry;
                        }
                        else
                        {
                            entries.append(spatialEntry);
                        }
                    }
                };

                appendSpatialEntries(QStringLiteral("geometry_columns"),
                    databaseVectorMetadataQuery(), QStringLiteral("vector"), &metadataWarnings);
                appendSpatialEntries(QStringLiteral("raster_columns"),
                    databaseRasterMetadataQuery(), QStringLiteral("raster"), &metadataWarnings);

                // 【2026-10-25】列表格式对齐「从数据库获取数据源」对话框：
                //   库名下面直接就是数据，节点名就是数据名（几何类型 / 数据列 / EPSG
                //   这些放 tooltip，不再往名字后面拼 [面 · geom · EPSG:4326]）；
                //   只有库里确实有多个架构时才插一层架构节点，免得同名表混在一起。
                QSet<QString> schemaNames;
                for (const DatabaseCatalogEntry& entry : entries)
                {
                    schemaNames.insert(entry.schemaName);
                }
                const bool needSchemaLevel = schemaNames.size() > 1;

                QHash<QString, QTreeWidgetItem*> schemaItems;
                int dataItemCount = 0;
                for (const DatabaseCatalogEntry& entry : entries)
                {
                    QTreeWidgetItem* parentItem = databaseItem;
                    if (needSchemaLevel)
                    {
                        QTreeWidgetItem*& schemaItem = schemaItems[entry.schemaName];
                        if (!schemaItem)
                        {
                            schemaItem = addNode(databaseItem, KindSchema, entry.schemaName, true);
                            schemaItem->setData(0, kRoleSchema, entry.schemaName);
                            schemaItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                            schemaItem->setToolTip(0,
                                QStringLiteral("架构：%1").arg(entry.schemaName));
                        }
                        parentItem = schemaItem;
                    }

                    QTreeWidgetItem* tableItem = addNode(parentItem, KindTable,
                        entry.tableName,
                        entry.layerType != QStringLiteral("table"));
                    tableItem->setData(0, kRoleSchema, entry.schemaName);
                    tableItem->setData(0, kRoleTable, entry.tableName);
                    tableItem->setData(0, kRoleGeom, entry.dataColumn);
                    tableItem->setData(0, kRoleDataType, entry.layerType);
                    tableItem->setData(0, kRoleIsRaster, entry.layerType == QStringLiteral("raster"));
                    tableItem->setData(0, kRoleIsData,
                        entry.layerType == QStringLiteral("vector")
                        || entry.layerType == QStringLiteral("raster"));
                    tableItem->setIcon(0, databaseItemIcon(entry.layerType, entry.dataType));

                    // tooltip 照「从数据库获取数据源」那张列表的多行写法
                    QString tip = QStringLiteral("数据名称: %1").arg(entry.tableName);
                    if (entry.layerType == QStringLiteral("raster"))
                    {
                        tip += QStringLiteral("\n类型: 栅格");
                    }
                    else if (entry.layerType == QStringLiteral("table"))
                    {
                        tip += QStringLiteral("\n类型: 非空间数据表");
                    }
                    else
                    {
                        const QString geometry = entry.dataType.trimmed();
                        tip += QStringLiteral("\n类型: %1").arg(
                            geometry.isEmpty() ? QStringLiteral("空间图层") : geometry);
                    }
                    if (!entry.dataColumn.isEmpty())
                    {
                        tip += QStringLiteral("\n数据列: %1").arg(entry.dataColumn);
                    }
                    if (entry.srid > 0)
                    {
                        tip += QStringLiteral("\nEPSG: %1").arg(entry.srid);
                    }
                    if (!entry.schemaName.isEmpty() && !needSchemaLevel)
                    {
                        // 不插架构层时，架构名放进 tooltip，免得看不出表在哪个 schema
                        tip += QStringLiteral("\n架构: %1").arg(entry.schemaName);
                    }

                    if (entry.layerType == QStringLiteral("table"))
                    {
                        // 普通表（无空间列）只能浏览，不能裁剪：灰显、不给勾选框
                        tableItem->setForeground(0, QBrush(QColor(150, 150, 150)));
                        tableItem->setToolTip(0,
                            tip + QStringLiteral("\n该类型不支持空间裁剪"));
                    }
                    else
                    {
                        tableItem->setToolTip(0, tip);
                    }
                    ++dataItemCount;
                }

                for (const QString& warning : metadataWarnings)
                {
                    addMessageNode(databaseItem, warning, true);
                }
                if (dataItemCount == 0)
                {
                    addMessageNode(databaseItem,
                        QStringLiteral("当前数据库没有可读取的业务数据。"), false);
                }

                databaseItem->setExpanded(true);
                for (auto it = schemaItems.begin(); it != schemaItems.end(); ++it)
                {
                    it.value()->setExpanded(true);
                }
            }
        }
    }

    m_syncing = prevSyncing;
    restoreChecked(keep);
    updateStatusText();
    applyFilter();
}

// ============================================================
// 本地段（"已连接文件夹"复用 QGIS 收藏夹，子节点自己枚举）
// ============================================================
void CSE_DataCatalogBrowser::initializeFavoritesModel()
{
    // 只用来读写收藏夹清单，不挂到任何视图上（原来挂在 QgsBrowserTreeView 上）
    m_favoritesModel = new QgsBrowserGuiModel(this);
    connect(m_favoritesModel, &QgsBrowserModel::stateChanged, this,
        [this](const QModelIndex& index, Qgis::BrowserItemState)
        {
            if (!m_favoritesModel || !index.isValid())
            {
                return;
            }
            QgsDataItem* item = m_favoritesModel->dataItem(index);
            if (!item || item->path() != QStringLiteral("favorites:"))
            {
                return;
            }
            m_favoritesRetry = 0;
            QTimer::singleShot(0, this, &CSE_DataCatalogBrowser::refreshLocalFolders);
        });

    m_favoritesModel->initialize();
}

QList<QPair<QString, QString> > CSE_DataCatalogBrowser::favoriteFolders() const
{
    QList<QPair<QString, QString> > folders;
    if (!m_favoritesModel)
    {
        return folders;
    }

    const QModelIndex favoritesIndex = m_favoritesModel->findPath(QStringLiteral("favorites:"));
    if (!favoritesIndex.isValid())
    {
        return folders;   // 收藏夹还没初始化完
    }

    const int rows = m_favoritesModel->rowCount(favoritesIndex);
    for (int row = 0; row < rows; ++row)
    {
        const QModelIndex index = m_favoritesModel->index(row, 0, favoritesIndex);
        QgsDataItem* item = m_favoritesModel->dataItem(index);
        QgsDirectoryItem* directory = qobject_cast<QgsDirectoryItem*>(item);
        if (!directory)
        {
            continue;
        }
        folders.append(qMakePair(QDir::fromNativeSeparators(directory->dirPath()), item->name()));
    }
    return folders;
}

void CSE_DataCatalogBrowser::refreshLocalFolders()
{
    const QSet<QString> keep = checkedKeys();
    const bool prevSyncing = m_syncing;
    m_syncing = true;
    clearChildren(m_rootLocal);

    const QList<QPair<QString, QString> > folders = favoriteFolders();
    if (folders.isEmpty())
    {
        // QGIS 浏览器模型是异步填充的：刚开对话框时收藏夹可能还没读出来，
        // 排队重试若干次（每次 200ms），别一上来就显示"未连接"误导人
        if (m_favoritesModel && m_favoritesRetry < 20)
        {
            ++m_favoritesRetry;
            addMessageNode(m_rootLocal, QStringLiteral("正在初始化本地数据目录…"), false);
            QTimer::singleShot(200, this, &CSE_DataCatalogBrowser::refreshLocalFolders);
        }
        else
        {
            addMessageNode(m_rootLocal,
                QStringLiteral("未连接本地数据文件夹（点「选择文件夹」添加）"), false);
        }
    }
    else
    {
        m_favoritesRetry = 0;
        for (const QPair<QString, QString>& folder : folders)
        {
            QTreeWidgetItem* node = addNode(m_rootLocal, KindLocalFolder,
                folder.second.isEmpty() ? folder.first : folder.second, true);
            node->setData(0, kRolePath, folder.first);
            node->setIcon(0, style()->standardIcon(QStyle::SP_DirHomeIcon));
            node->setToolTip(0, QDir::toNativeSeparators(folder.first));
            markLazy(node);
        }
    }

    m_syncing = prevSyncing;
    restoreChecked(keep);
    updateStatusText();
    applyFilter();
}

void CSE_DataCatalogBrowser::connectLocalFolder()
{
    const QString path = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择本地数据文件夹"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty() || !m_favoritesModel)
    {
        return;
    }

    QString name = QFileInfo(path).fileName();
    if (name.isEmpty())
    {
        name = path;
    }

    m_favoritesModel->addFavoriteDirectory(path, name);
    emit message(QStringLiteral("已连接本地数据文件夹：%1").arg(QDir::toNativeSeparators(path)), false);

    // addFavoriteDirectory 在收藏夹已填充时立即插入，启动阶段可能异步填充，
    // 因此再排队一次刷新
    QTimer::singleShot(0, this, &CSE_DataCatalogBrowser::refreshLocalFolders);
}

void CSE_DataCatalogBrowser::disconnectSelectedLocalFolder()
{
    if (!m_favoritesModel)
    {
        return;
    }

    // 选中的可能是文件夹下面的子节点，往上找到已连接文件夹的根
    QTreeWidgetItem* node = m_tree ? m_tree->currentItem() : nullptr;
    while (node && node->data(0, kRoleKind).toInt() != KindLocalFolder)
    {
        node = node->parent();
    }
    if (!node)
    {
        QMessageBox::information(this, QStringLiteral("断开连接"),
            QStringLiteral("请先选中一个已连接的文件夹。"));
        return;
    }

    const QString path = node->data(0, kRolePath).toString();
    const QModelIndex favoritesIndex = m_favoritesModel->findPath(QStringLiteral("favorites:"));
    if (favoritesIndex.isValid())
    {
        const int rows = m_favoritesModel->rowCount(favoritesIndex);
        for (int row = 0; row < rows; ++row)
        {
            const QModelIndex index = m_favoritesModel->index(row, 0, favoritesIndex);
            QgsDataItem* item = m_favoritesModel->dataItem(index);
            QgsDirectoryItem* directory = qobject_cast<QgsDirectoryItem*>(item);
            if (directory
                && QDir::fromNativeSeparators(directory->dirPath()) == path)
            {
                m_favoritesModel->removeFavorite(index);
                break;
            }
        }
    }

    emit message(QStringLiteral("已断开本地数据文件夹：%1").arg(QDir::toNativeSeparators(path)), false);
    refreshLocalFolders();
}

void CSE_DataCatalogBrowser::populateLocalChildren(QTreeWidgetItem* item)
{
    if (!item || !item->data(0, kRoleLazy).toBool())
    {
        return;
    }

    const bool prevSyncing = m_syncing;
    m_syncing = true;
    item->setData(0, kRoleLazy, false);
    clearChildren(item);

    const QString path = item->data(0, kRolePath).toString();
    const int kind = item->data(0, kRoleKind).toInt();

    if (kind == KindLocalContainer)
    {
        // .gdb 目录 / .gpkg 文件：用 OGR 枚举内部图层
        QList<QgsProviderSublayerDetails> sublayers;
        if (QgsProviderMetadata* metadata =
            QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("ogr")))
        {
            try
            {
                sublayers = metadata->querySublayers(path, Qgis::SublayerQueryFlags(), nullptr);
            }
            catch (...)
            {
                sublayers.clear();
            }
        }

        int layerCount = 0;
        for (const QgsProviderSublayerDetails& sublayer : sublayers)
        {
            if (sublayer.providerKey() != QLatin1String("ogr"))
            {
                continue;
            }
            const bool isRaster = (sublayer.type() == QgsMapLayerType::RasterLayer);
            QTreeWidgetItem* child = addNode(item, KindLocalLayer,
                sublayer.name(), true);
            child->setData(0, kRolePath, path);
            child->setData(0, kRoleLayerName, sublayer.name());
            child->setData(0, kRoleUri, sublayer.uri());
            child->setData(0, kRoleIsRaster, isRaster);
            child->setData(0, kRoleIsData, true);
            if (isRaster)
            {
                child->setIcon(0, LayerIconHelper::drawLayerIcon(3));
                child->setToolTip(0, QStringLiteral("%1\n栅格图层").arg(sublayer.uri()));
            }
            else
            {
                const int iconKind = iconKindFromWkb(sublayer.wkbType());
                child->setIcon(0, iconKind >= 0
                    ? LayerIconHelper::drawLayerIcon(iconKind)
                    : style()->standardIcon(QStyle::SP_FileIcon));
                child->setToolTip(0,
                    QStringLiteral("%1\n矢量图层，双击添加到地图").arg(sublayer.uri()));
            }
            ++layerCount;
        }
        if (layerCount == 0)
        {
            addMessageNode(item, QStringLiteral("该数据源中没有可读取的图层。"), true);
        }
    }
    else
    {
        const QDir dir(path);
        if (!dir.exists())
        {
            addMessageNode(item, QStringLiteral("目录不存在或无法读取。"), true);
        }
        else
        {
            // 子目录（.gdb 目录 / .gpkg 文件当作容器节点）
            const QFileInfoList dirs =
                dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo& dirInfo : dirs)
            {
                if (dirInfo.fileName().startsWith(QLatin1Char('.')))
                {
                    continue;
                }
                const QString absolutePath = dirInfo.absoluteFilePath();
                if (isContainerPath(absolutePath))
                {
                    QTreeWidgetItem* node = addNode(item, KindLocalContainer,
                        dirInfo.fileName(), true);
                    node->setData(0, kRolePath, absolutePath);
                    node->setIcon(0, LayerIconHelper::gdbFolderIcon());
                    node->setToolTip(0, QDir::toNativeSeparators(absolutePath));
                    markLazy(node);
                }
                else
                {
                    QTreeWidgetItem* node = addNode(item, KindLocalDir,
                        dirInfo.fileName(), true);
                    node->setData(0, kRolePath, absolutePath);
                    node->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                    node->setToolTip(0, QDir::toNativeSeparators(absolutePath));
                    markLazy(node);
                }
            }

            // 数据文件
            const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
            for (const QFileInfo& fileInfo : files)
            {
                appendLocalFileItem(item, fileInfo.absoluteFilePath());
            }
        }
    }

    // 展开前这个节点已经是"全勾"状态：新读出来的子节点跟着勾上
    if (item->checkState(0) == Qt::Checked)
    {
        setSubtreeCheck(item, Qt::Checked);
    }

    m_syncing = prevSyncing;

    // 正在按关键字过滤时新读出来的子节点不会被外层那一趟过滤覆盖到，这里补一遍
    if (m_filterDepth > 0)
    {
        for (int i = 0; i < item->childCount(); ++i)
        {
            bool visible = false;
            applyFilterTo(item->child(i), &visible);
        }
    }
    else
    {
        applyFilter();
    }
    emit checkedCountChanged(checkedCount());
}

QTreeWidgetItem* CSE_DataCatalogBrowser::appendLocalFileItem(QTreeWidgetItem* parent,
    const QString& filePath)
{
    const QFileInfo fi(filePath);
    if (!fi.isFile())
    {
        return nullptr;
    }

    if (isContainerPath(filePath))
    {
        // 子目录循环之外再兜一次：容器文件（.gpkg）也当容器节点
        QTreeWidgetItem* node = addNode(parent, KindLocalContainer, fi.fileName(), true);
        node->setData(0, kRolePath, filePath);
        node->setIcon(0, LayerIconHelper::drawLayerIcon(2));
        node->setToolTip(0, QDir::toNativeSeparators(filePath));
        markLazy(node);
        return node;
    }

    const QString lower = filePath.toLower();
    if (!isDataExtension(lower))
    {
        return nullptr;
    }

    const bool isRaster = isRasterExtension(lower);
    QTreeWidgetItem* node = addNode(parent, KindLocalFile, fi.fileName(), true);
    node->setData(0, kRolePath, filePath);
    node->setData(0, kRoleIsData, true);
    node->setData(0, kRoleIsRaster, isRaster);

    if (isRaster)
    {
        node->setIcon(0, LayerIconHelper::drawLayerIcon(3));
        node->setToolTip(0, QStringLiteral("%1\n栅格数据，双击添加到地图")
            .arg(QDir::toNativeSeparators(filePath)));
    }
    else
    {
        int iconKind = -1;
        if (lower.endsWith(QStringLiteral(".shp")))
        {
            iconKind = shpIconKind(filePath);
        }
        if (iconKind >= 0)
        {
            node->setIcon(0, LayerIconHelper::drawLayerIcon(iconKind));
        }
        else
        {
            QIcon icon = QgsApplication::getThemeIcon(QStringLiteral("mActionAddOgrLayer.svg"));
            node->setIcon(0, icon.isNull()
                ? style()->standardIcon(QStyle::SP_FileIcon)
                : icon);
        }
        node->setToolTip(0, QStringLiteral("%1\n矢量数据，双击添加到地图")
            .arg(QDir::toNativeSeparators(filePath)));
    }
    return node;
}

// ============================================================
// 地图图层段
// ============================================================
void CSE_DataCatalogBrowser::refreshMapLayers()
{
    rebuildMapLayerSection();
}

void CSE_DataCatalogBrowser::rebuildMapLayerSection()
{
    const bool prevSyncing = m_syncing;
    m_syncing = true;
    clearChildren(m_rootMapLayer);

    const QMap<QString, QgsMapLayer*>& layers = QgsProject::instance()->mapLayers();
    for (auto it = layers.begin(); it != layers.end(); ++it)
    {
        QgsMapLayer* layer = it.value();
        if (!layer)
        {
            continue;
        }

        const bool isRaster = (qobject_cast<QgsRasterLayer*>(layer) != nullptr);
        QTreeWidgetItem* item = addNode(m_rootMapLayer, KindMapLayer, layer->name(), false);
        item->setData(0, kRoleMapLayerId, layer->id());
        item->setData(0, kRoleIsRaster, isRaster);

        int iconKind = 3;
        if (QgsVectorLayer* vectorLayer = qobject_cast<QgsVectorLayer*>(layer))
        {
            const int byGeometry = iconKindFromWkb(vectorLayer->wkbType());
            iconKind = (byGeometry >= 0) ? byGeometry : 0;
        }
        item->setIcon(0, LayerIconHelper::drawLayerIcon(iconKind));
        item->setToolTip(0, QStringLiteral("%1\n双击定位到该图层")
            .arg(isRaster ? QStringLiteral("栅格图层") : QStringLiteral("矢量图层")));
    }

    if (m_rootMapLayer->childCount() == 0)
    {
        addMessageNode(m_rootMapLayer, QStringLiteral("当前工程没有加载图层"), false);
    }

    m_syncing = prevSyncing;
    updateStatusText();
    applyFilter();
}

// ============================================================
// 刷新
// ============================================================
void CSE_DataCatalogBrowser::refreshAll()
{
    refreshDatabaseData();
    refreshLocalFolders();
    refreshMapLayers();
    updateStatusText();
}

// ============================================================
// 勾选
// ============================================================
bool CSE_DataCatalogBrowser::isCheckable(QTreeWidgetItem* item)
{
    return item && (item->flags() & Qt::ItemIsUserCheckable);
}

bool CSE_DataCatalogBrowser::kindIsCheckable(int kind)
{
    return kind != KindRootMapLayer && kind != KindMapLayer
        && kind != KindPlaceholder && kind != KindMessage;
}

void CSE_DataCatalogBrowser::setSubtreeCheck(QTreeWidgetItem* item, Qt::CheckState state)
{
    if (!item || state == Qt::PartiallyChecked)
    {
        return;
    }
    if (isCheckable(item) && item->checkState(0) != state)
    {
        item->setCheckState(0, state);
    }
    for (int i = 0; i < item->childCount(); ++i)
    {
        setSubtreeCheck(item->child(i), state);
    }
}

void CSE_DataCatalogBrowser::refreshParentCheckStates(QTreeWidgetItem* item)
{
    if (!item)
    {
        return;
    }
    QTreeWidgetItem* parent = item->parent();
    while (parent)
    {
        int checkable = 0;
        int checked = 0;
        int partial = 0;
        for (int i = 0; i < parent->childCount(); ++i)
        {
            QTreeWidgetItem* child = parent->child(i);
            if (!isCheckable(child))
            {
                continue;
            }
            ++checkable;
            const Qt::CheckState state = child->checkState(0);
            if (state == Qt::Checked)
            {
                ++checked;
            }
            else if (state == Qt::PartiallyChecked)
            {
                ++partial;
            }
        }

        if (checkable > 0)
        {
            Qt::CheckState state = Qt::Unchecked;
            if (checked == checkable)
            {
                state = Qt::Checked;
            }
            else if (checked > 0 || partial > 0)
            {
                state = Qt::PartiallyChecked;
            }
            if (parent->checkState(0) != state)
            {
                parent->setCheckState(0, state);
            }
        }
        parent = parent->parent();
    }
}

Qt::CheckState CSE_DataCatalogBrowser::recomputeCheckState(QTreeWidgetItem* item)
{
    if (!item)
    {
        return Qt::Unchecked;
    }
    // 懒加载还没展开的节点保持原状，否则会被"没有子节点"算成未勾选
    if (item->data(0, kRoleLazy).toBool())
    {
        return item->checkState(0);
    }

    int checkable = 0;
    int checked = 0;
    int partial = 0;
    for (int i = 0; i < item->childCount(); ++i)
    {
        QTreeWidgetItem* child = item->child(i);
        const Qt::CheckState childState = recomputeCheckState(child);
        if (!isCheckable(child))
        {
            continue;
        }
        ++checkable;
        if (childState == Qt::Checked)
        {
            ++checked;
        }
        else if (childState == Qt::PartiallyChecked)
        {
            ++partial;
        }
    }

    if (!isCheckable(item) || checkable == 0)
    {
        return item->checkState(0);
    }

    Qt::CheckState state = Qt::Unchecked;
    if (checked == checkable)
    {
        state = Qt::Checked;
    }
    else if (checked > 0 || partial > 0)
    {
        state = Qt::PartiallyChecked;
    }
    if (item->checkState(0) != state)
    {
        item->setCheckState(0, state);
    }
    return state;
}

QString CSE_DataCatalogBrowser::nodeKey(QTreeWidgetItem* item)
{
    if (!item)
    {
        return QString();
    }
    switch (item->data(0, kRoleKind).toInt())
    {
    case KindRootDatabase: return QStringLiteral("root:db");
    case KindRootLocal:    return QStringLiteral("root:local");
    case KindRootMapLayer: return QStringLiteral("root:map");
    case KindDatabase:     return QStringLiteral("db:") + item->text(0);
    case KindSchema:
        return QStringLiteral("db:") + item->data(0, kRoleSchema).toString();
    case KindTable:
        return QStringLiteral("db:") + item->data(0, kRoleSchema).toString()
            + QLatin1Char('.') + item->data(0, kRoleTable).toString();
    case KindLocalLayer:
        return QStringLiteral("local:") + item->data(0, kRolePath).toString()
            + QStringLiteral("|") + item->data(0, kRoleLayerName).toString();
    case KindMapLayer:
    case KindPlaceholder:
    case KindMessage:
        return QString();
    default:
        return QStringLiteral("local:") + item->data(0, kRolePath).toString();
    }
}

QSet<QString> CSE_DataCatalogBrowser::checkedKeys() const
{
    QSet<QString> keys;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item)
    {
        if (!item)
        {
            return;
        }
        if (isCheckable(item) && item->checkState(0) != Qt::Unchecked)
        {
            const QString key = nodeKey(item);
            if (!key.isEmpty())
            {
                keys.insert(key);
            }
        }
        for (int i = 0; i < item->childCount(); ++i)
        {
            walk(item->child(i));
        }
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        walk(m_tree->topLevelItem(i));
    }
    return keys;
}

void CSE_DataCatalogBrowser::restoreChecked(const QSet<QString>& keys)
{
    if (keys.isEmpty() || !m_tree)
    {
        return;
    }

    const bool prevSyncing = m_syncing;
    m_syncing = true;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item)
    {
        if (!item)
        {
            return;
        }
        if (isCheckable(item))
        {
            const QString key = nodeKey(item);
            if (!key.isEmpty() && keys.contains(key) && item->checkState(0) == Qt::Unchecked)
            {
                item->setCheckState(0, Qt::Checked);
            }
        }
        for (int i = 0; i < item->childCount(); ++i)
        {
            walk(item->child(i));
        }
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        walk(m_tree->topLevelItem(i));
    }
    // 子节点都读出来的容器按子节点重算，把"半选"还原回去
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        recomputeCheckState(m_tree->topLevelItem(i));
    }
    m_syncing = prevSyncing;

    // 刷新后勾选状态恢复出来了，批量导出按钮的可用性要跟着回来
    emit checkedCountChanged(checkedCount());
}

void CSE_DataCatalogBrowser::onTreeItemChanged(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    if (m_syncing || !item || !isCheckable(item))
    {
        return;
    }

    m_syncing = true;
    const Qt::CheckState state = item->checkState(0);
    if (state != Qt::PartiallyChecked)
    {
        setSubtreeCheck(item, state);
    }
    refreshParentCheckStates(item);
    m_syncing = false;

    emit checkedCountChanged(checkedCount());
}

int CSE_DataCatalogBrowser::checkedCount() const
{
    int count = 0;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item)
    {
        if (!item)
        {
            return;
        }
        if (!kindIsCheckable(item->data(0, kRoleKind).toInt()))
        {
            return;
        }
        if (isCheckable(item) && item->checkState(0) != Qt::Unchecked)
        {
            // 勾了（或半选）的节点本身就是一个导出单元，不必再往下数
            ++count;
            return;
        }
        for (int i = 0; i < item->childCount(); ++i)
        {
            walk(item->child(i));
        }
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        walk(m_tree->topLevelItem(i));
    }
    return count;
}

// ============================================================
// 关键字过滤
// ============================================================
void CSE_DataCatalogBrowser::setFilterKeyword(const QString& keyword)
{
    // Qt 5.12 兼容：用 QString::SkipEmptyParts（Qt::SkipEmptyParts 是 Qt 5.14 才有的枚举）
    m_filterKeys = keyword.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
        QString::SkipEmptyParts);
    for (int i = 0; i < m_filterKeys.size(); ++i)
    {
        m_filterKeys[i] = m_filterKeys[i].toLower();
    }
    applyFilter();
}

bool CSE_DataCatalogBrowser::textMatchesFilter(const QString& text) const
{
    if (m_filterKeys.isEmpty())
    {
        return true;
    }
    const QString lower = text.toLower();
    for (const QString& key : m_filterKeys)
    {
        if (!lower.contains(key))
        {
            return false;
        }
    }
    return true;
}

void CSE_DataCatalogBrowser::applyFilter()
{
    if (!m_tree || m_filterDepth > 0)
    {
        return;
    }
    ++m_filterDepth;
    const bool prevSyncing = m_syncing;
    m_syncing = true;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        bool visible = false;
        applyFilterTo(m_tree->topLevelItem(i), &visible);
    }
    m_syncing = prevSyncing;
    --m_filterDepth;
}

void CSE_DataCatalogBrowser::applyFilterTo(QTreeWidgetItem* item, bool* anyVisible)
{
    if (!item)
    {
        return;
    }

    bool anyChildVisible = false;
    for (int i = 0; i < item->childCount(); ++i)
    {
        bool childVisible = false;
        applyFilterTo(item->child(i), &childVisible);
        anyChildVisible = anyChildVisible || childVisible;
    }

    const int kind = item->data(0, kRoleKind).toInt();
    const bool selfMatch = textMatchesFilter(item->text(0))
        || kind == KindPlaceholder;
    const bool visible = selfMatch || anyChildVisible;

    item->setHidden(!visible);
    // 有关键字时把命中的分支自动展开，省得用户一层层点
    if (!m_filterKeys.isEmpty() && anyChildVisible)
    {
        item->setExpanded(true);
    }

    if (anyVisible)
    {
        *anyVisible = (*anyVisible) || visible;
    }
}

// ============================================================
// 节点 → 裁剪数据
// ============================================================
CSE_DataCatalogBrowser::CatalogItem CSE_DataCatalogBrowser::leafCatalogItem(
    QTreeWidgetItem* item) const
{
    CatalogItem context;
    if (!item || !item->data(0, kRoleIsData).toBool())
    {
        return context;
    }

    const int kind = item->data(0, kRoleKind).toInt();
    context.valid = true;
    context.isLayer = true;

    if (kind == KindTable)
    {
        const QString schema = item->data(0, kRoleSchema).toString();
        const QString table = item->data(0, kRoleTable).toString();
        const QString geometryColumn = item->data(0, kRoleGeom).toString();
        const QString layerType = item->data(0, kRoleDataType).toString();

        context.kind = CatalogItem::Database;
        context.displayName = table;
        context.detail = schema + QLatin1Char('.') + table;
        context.isRaster = (layerType == QStringLiteral("raster"));

        const QgsMimeDataUtils::Uri uri =
            databaseUriFor(schema, table, geometryColumn, layerType);
        if (isSupportedCatalogUri(uri))
        {
            context.uris.append(uri);
        }
        if (context.isRaster)
        {
            context.rasterUriCandidates =
                databaseRasterUriCandidates(schema, table, geometryColumn);
        }
        return context;
    }

    // 本地数据（数据文件 / 容器里的图层）
    context.kind = CatalogItem::Local;
    context.detail = item->data(0, kRolePath).toString();
    context.isRaster = item->data(0, kRoleIsRaster).toBool();

    if (kind == KindLocalLayer)
    {
        QgsMimeDataUtils::Uri uri;
        uri.uri = item->data(0, kRoleUri).toString();
        uri.name = item->data(0, kRoleLayerName).toString();
        uri.providerKey = QStringLiteral("ogr");
        uri.layerType = context.isRaster
            ? QStringLiteral("raster")
            : QStringLiteral("vector");
        if (!uri.uri.isEmpty())
        {
            context.uris.append(uri);
        }
        context.displayName = uri.name;
    }
    else
    {
        const QgsMimeDataUtils::Uri uri = localFileUri(context.detail);
        if (!uri.uri.isEmpty())
        {
            context.uris.append(uri);
            context.displayName = uri.name;
        }
    }
    if (context.displayName.isEmpty())
    {
        context.displayName = item->text(0);
    }
    return context;
}

void CSE_DataCatalogBrowser::collectLocalSources(const QString& path,
    QList<QgsMimeDataUtils::Uri>* uris) const
{
    if (!uris || path.isEmpty())
    {
        return;
    }

    if (isContainerPath(path))
    {
        if (QgsProviderMetadata* metadata =
            QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("ogr")))
        {
            QList<QgsProviderSublayerDetails> sublayers;
            try
            {
                sublayers = metadata->querySublayers(path, Qgis::SublayerQueryFlags(), nullptr);
            }
            catch (...)
            {
                sublayers.clear();
            }
            for (const QgsProviderSublayerDetails& sublayer : sublayers)
            {
                if (sublayer.providerKey() != QLatin1String("ogr"))
                {
                    continue;
                }
                QgsMimeDataUtils::Uri uri;
                uri.uri = sublayer.uri();
                uri.name = sublayer.name();
                uri.providerKey = QStringLiteral("ogr");
                uri.layerType = (sublayer.type() == QgsMapLayerType::RasterLayer)
                    ? QStringLiteral("raster")
                    : QStringLiteral("vector");
                uris->append(uri);
            }
        }
        return;
    }

    const QFileInfo info(path);
    if (info.isFile())
    {
        const QgsMimeDataUtils::Uri uri = localFileUri(path);
        if (!uri.uri.isEmpty())
        {
            uris->append(uri);
        }
        return;
    }

    QDir dir(path);
    if (!dir.exists())
    {
        return;
    }

    // 先目录后文件，顺序与树里一致
    const QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& dirInfo : dirs)
    {
        if (dirInfo.fileName().startsWith(QLatin1Char('.')))
        {
            continue;
        }
        collectLocalSources(dirInfo.absoluteFilePath(), uris);
    }

    const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& fileInfo : files)
    {
        const QString absolutePath = fileInfo.absoluteFilePath();
        if (isContainerPath(absolutePath))
        {
            collectLocalSources(absolutePath, uris);
            continue;
        }
        const QgsMimeDataUtils::Uri uri = localFileUri(absolutePath);
        if (!uri.uri.isEmpty())
        {
            uris->append(uri);
        }
    }
}

void CSE_DataCatalogBrowser::collectLeafItems(QTreeWidgetItem* item,
    QList<CatalogItem>* out, QSet<QString>* seen) const
{
    if (!item || !out)
    {
        return;
    }

    const int kind = item->data(0, kRoleKind).toInt();
    if (item->data(0, kRoleIsData).toBool())
    {
        const CatalogItem leaf = leafCatalogItem(item);
        if (leaf.uris.isEmpty())
        {
            return;
        }
        const QString key = leaf.uris.first().uri;
        if (seen)
        {
            if (seen->contains(key))
            {
                return;
            }
            seen->insert(key);
        }
        out->append(leaf);
        return;
    }

    // 本地容器/目录：子节点可能是懒加载还没读出来的，直接扫磁盘最稳
    if (kind == KindLocalFolder || kind == KindLocalDir || kind == KindLocalContainer)
    {
        QList<QgsMimeDataUtils::Uri> uris;
        const QString path = item->data(0, kRolePath).toString();
        collectLocalSources(path, &uris);

        for (const QgsMimeDataUtils::Uri& uri : uris)
        {
            if (uri.uri.isEmpty())
            {
                continue;
            }
            if (seen)
            {
                if (seen->contains(uri.uri))
                {
                    continue;
                }
                seen->insert(uri.uri);
            }

            CatalogItem leaf;
            leaf.kind = CatalogItem::Local;
            leaf.valid = true;
            leaf.isLayer = true;
            leaf.isRaster = (uri.layerType == QStringLiteral("raster"));
            leaf.displayName = uri.name;
            leaf.detail = path;
            leaf.uris.append(uri);
            out->append(leaf);
        }
        return;
    }

    for (int i = 0; i < item->childCount(); ++i)
    {
        collectLeafItems(item->child(i), out, seen);
    }
}

void CSE_DataCatalogBrowser::collectCheckedItems(QTreeWidgetItem* item,
    QList<CatalogItem>* out, QSet<QString>* seen) const
{
    if (!item || !out)
    {
        return;
    }

    const int kind = item->data(0, kRoleKind).toInt();
    if (!kindIsCheckable(kind) || !isCheckable(item))
    {
        return;
    }

    const Qt::CheckState state = item->checkState(0);
    if (state == Qt::Checked)
    {
        // 全勾：整棵子树都导（没展开的目录直接扫盘）
        collectLeafItems(item, out, seen);
        return;
    }
    if (state == Qt::PartiallyChecked)
    {
        // 半选：逐个看子节点，被取消的子节点整棵跳过
        for (int i = 0; i < item->childCount(); ++i)
        {
            collectCheckedItems(item->child(i), out, seen);
        }
    }
    // 未勾选：整棵子树都不导
}

QList<CSE_DataCatalogBrowser::CatalogItem> CSE_DataCatalogBrowser::checkedItems() const
{
    QList<CatalogItem> items;
    QSet<QString> seen;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        collectCheckedItems(m_tree->topLevelItem(i), &items, &seen);
    }
    return items;
}

CSE_DataCatalogBrowser::CatalogItem CSE_DataCatalogBrowser::catalogItemFor(
    QTreeWidgetItem* item) const
{
    CatalogItem context;
    if (!item)
    {
        return context;
    }

    const int kind = item->data(0, kRoleKind).toInt();
    if (kind == KindMessage || kind == KindPlaceholder)
    {
        return context;
    }

    context.valid = true;
    context.displayName = item->text(0);

    if (kind == KindMapLayer)
    {
        context.kind = CatalogItem::MapLayer;
        context.mapLayerId = item->data(0, kRoleMapLayerId).toString();
        context.isRaster = item->data(0, kRoleIsRaster).toBool();
        return context;
    }

    if (item->data(0, kRoleIsData).toBool())
    {
        const CatalogItem leaf = leafCatalogItem(item);
        context.kind = leaf.kind;
        context.isLayer = leaf.isLayer;
        context.isRaster = leaf.isRaster;
        context.detail = leaf.detail;
        context.uris = leaf.uris;
        context.rasterUriCandidates = leaf.rasterUriCandidates;
        if (!leaf.displayName.isEmpty())
        {
            context.displayName = leaf.displayName;
        }
    }
    else
    {
        // 目录 / 架构 / 库根：汇总其下所有数据（与改造前"右键目录=导出整棵"一致）
        QList<CatalogItem> leaves;
        QSet<QString> seen;
        collectLeafItems(item, &leaves, &seen);

        const bool isLocalKind = (kind == KindRootLocal || kind == KindLocalFolder
            || kind == KindLocalDir || kind == KindLocalContainer);
        context.kind = isLocalKind ? CatalogItem::Local : CatalogItem::Database;
        context.detail = isLocalKind
            ? item->data(0, kRolePath).toString()
            : item->data(0, kRoleSchema).toString();
        for (const CatalogItem& leaf : leaves)
        {
            context.uris.append(leaf.uris);
            if (leaf.isRaster)
            {
                context.isRaster = true;
            }
        }
        context.isLayer = !context.uris.isEmpty();
    }

    context.isLocalFolderRoot = (kind == KindLocalFolder);
    return context;
}

// ============================================================
// 展开 / 双击 / 右键
// ============================================================
void CSE_DataCatalogBrowser::onTreeItemExpanded(QTreeWidgetItem* item)
{
    if (!item)
    {
        return;
    }
    if (item->data(0, kRoleLazy).toBool())
    {
        populateLocalChildren(item);
    }
}

void CSE_DataCatalogBrowser::onTreeItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    if (!item)
    {
        return;
    }

    const int kind = item->data(0, kRoleKind).toInt();
    if (kind == KindLocalFolder || kind == KindLocalDir || kind == KindLocalContainer)
    {
        // 目录类双击 = 展开/收起（视图的 doubleClick 展开已关掉，避免和"添加到地图"打架）
        item->setExpanded(!item->isExpanded());
        return;
    }

    const CatalogItem context = catalogItemFor(item);
    if (context.valid && !context.uris.isEmpty())
    {
        emit itemActivated(context);
    }
}

void CSE_DataCatalogBrowser::onTreeContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item)
    {
        return;
    }
    m_tree->setCurrentItem(item);

    const CatalogItem context = catalogItemFor(item);
    if (!context.valid)
    {
        return;
    }
    emit contextMenuRequested(m_tree->viewport()->mapToGlobal(pos), context);
}

// ============================================================
// 状态行
// ============================================================
void CSE_DataCatalogBrowser::updateStatusText()
{
    QStringList parts;

    QString databaseText = QStringLiteral("未连接数据库");
    if (!m_databaseConnectionName.isEmpty())
    {
        const QSqlDatabase database = catalogDatabase(m_databaseConnectionName);
        if (database.isValid() && database.isOpen())
        {
            databaseText = QStringLiteral("数据库：%1").arg(database.databaseName());
        }
        else
        {
            databaseText = QStringLiteral("数据库连接已断开");
        }
    }
    parts << databaseText;

    parts << QStringLiteral("本地文件夹 %1 个").arg(favoriteFolders().size());

    int layerCount = 0;
    if (m_rootMapLayer)
    {
        for (int i = 0; i < m_rootMapLayer->childCount(); ++i)
        {
            if (m_rootMapLayer->child(i)->data(0, kRoleKind).toInt() == KindMapLayer)
            {
                ++layerCount;
            }
        }
    }
    parts << QStringLiteral("地图图层 %1 个").arg(layerCount);

    emit statusTextChanged(parts.join(QStringLiteral("　·　")));
}
