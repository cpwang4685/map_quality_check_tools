// 数据目录浏览器：由 LTZK 平台左侧数据列表 DataCatalogWidget（D:\thelast\01_Source）移植。
// 保留平台的三段式列表排版、objectName、连接流程与本地文件夹（收藏夹）实现；
// 去掉成果目录（产品目录）那一整套，并把"加载/导出"改为信号交给宿主对话框处理。

#include "data_catalog_browser.h"

#include "DataCatalogConnectionDialog.h"
#include "core/layer_icon_helper.h"      // 地图图层段图标（点/线/面/栅格）

#include "qgsbrowserguimodel.h"
#include "qgsbrowsertreeview.h"
#include "qgsdataitem.h"
#include "qgsdatasourceuri.h"
#include "qgsdirectoryitem.h"
#include "qgsfavoritesitem.h"
#include "qgsmaplayer.h"
#include "qgsproject.h"
#include "qgsrasterlayer.h"
#include "qgsvectorlayer.h"
#include "qgswkbtypes.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSizePolicy>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

namespace
{
    const char* kCatalogDatabaseConnectionPrefix = "CSE_DataCatalogPostgisConnection";

    // QStandardItem 上的自定义数据角色（照搬平台 DataCatalogWidget.h 的 DatabaseItemRole，
    // 去掉成果目录那几个用不到的角色）
    enum DatabaseItemRole
    {
        DatabaseLayerTypeRole = Qt::UserRole + 1,
        DatabaseSchemaRole,
        DatabaseTableRole,
        DatabaseGeometryColumnRole,
        DatabaseNameRole
    };

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

    QString databaseItemSuffix(
        const QString& layerType,
        const QString& geometryType,
        int srid,
        const QString& dataColumn)
    {
        if (layerType == QStringLiteral("raster"))
        {
            if (!dataColumn.isEmpty())
            {
                return QStringLiteral(" [栅格 · %1]").arg(dataColumn);
            }
            return QStringLiteral(" [栅格]");
        }
        if (layerType == QStringLiteral("table"))
        {
            return QStringLiteral(" [数据表]");
        }

        QString detail = geometryType.trimmed();
        if (detail.isEmpty())
        {
            detail = QStringLiteral("空间图层");
        }
        if (!dataColumn.isEmpty())
        {
            detail += QStringLiteral(" · %1").arg(dataColumn);
        }
        if (srid > 0)
        {
            detail += QStringLiteral(" · EPSG:%1").arg(srid);
        }
        return QStringLiteral(" [%1]").arg(detail);
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

    void appendUniqueUri(
        QList<QgsMimeDataUtils::Uri>* destination,
        const QgsMimeDataUtils::Uri& candidate)
    {
        if (!destination || !isSupportedCatalogUri(candidate))
        {
            return;
        }

        for (const QgsMimeDataUtils::Uri& existing : *destination)
        {
            if (existing.layerType == candidate.layerType &&
                existing.providerKey == candidate.providerKey &&
                existing.uri == candidate.uri)
            {
                return;
            }
        }
        destination->append(candidate);
    }

    // 章节标题栏（照平台 createSectionHeader）
    QFrame* createSectionHeader(
        const QString& text,
        QWidget* parent,
        const QList<QToolButton*>& buttons,
        QToolButton** titleButton)
    {
        QFrame* header = new QFrame(parent);
        header->setProperty("catalogSectionHeader", true);
        QHBoxLayout* layout = new QHBoxLayout(header);
        layout->setContentsMargins(8, 4, 4, 4);
        layout->setSpacing(4);

        QToolButton* title = new QToolButton(header);
        title->setProperty("catalogSectionTitle", true);
        title->setText(QString());
        title->setAccessibleName(text);
        title->setToolButtonStyle(Qt::ToolButtonTextOnly);
        title->setAutoRaise(true);
        title->setCursor(Qt::PointingHandCursor);
        title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        QHBoxLayout* titleLayout = new QHBoxLayout(title);
        titleLayout->setContentsMargins(0, 0, 0, 0);
        titleLayout->setSpacing(0);
        QLabel* titleLabel = new QLabel(text, title);
        titleLabel->setProperty("catalogSectionTitle", true);
        titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        titleLayout->addWidget(titleLabel);

        layout->addWidget(title, 1);
        for (QToolButton* button : buttons)
        {
            layout->addWidget(button, 0, Qt::AlignVCenter);
        }

        if (titleButton)
        {
            *titleButton = title;
        }
        return header;
    }
}

CSE_DataCatalogBrowser::CSE_DataCatalogBrowser(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("workspaceDataCatalog"));
    buildUi();
    initializeLocalBrowser();
}

CSE_DataCatalogBrowser::~CSE_DataCatalogBrowser()
{
    closeDatabaseConnection();
}

// ============================================================
// 界面（照平台 buildUi，三段：数据库数据 / 本地数据 / 地图图层）
// ============================================================
void CSE_DataCatalogBrowser::buildUi()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QSplitter* sectionSplitter = new QSplitter(Qt::Vertical, this);
    sectionSplitter->setObjectName(QStringLiteral("dataCatalogSectionSplitter"));
    sectionSplitter->setChildrenCollapsible(false);

    // ---------------- 数据库数据 ----------------
    QWidget* databaseSection = new QWidget(sectionSplitter);
    databaseSection->setObjectName(QStringLiteral("databaseCatalogSection"));
    QVBoxLayout* databaseLayout = new QVBoxLayout(databaseSection);
    databaseLayout->setContentsMargins(0, 0, 0, 0);
    databaseLayout->setSpacing(0);

    QToolButton* databaseTitleButton = nullptr;
    QFrame* databaseHeader = createSectionHeader(
        QStringLiteral("数据库数据"),
        databaseSection,
        QList<QToolButton*>(),
        &databaseTitleButton);
    databaseLayout->addWidget(databaseHeader);
    databaseTitleButton->setObjectName(QStringLiteral("databaseCatalogTitleButton"));

    QWidget* databaseContent = new QWidget(databaseSection);
    databaseContent->setObjectName(QStringLiteral("databaseCatalogContent"));
    QVBoxLayout* databaseContentLayout = new QVBoxLayout(databaseContent);
    databaseContentLayout->setContentsMargins(0, 0, 0, 0);
    databaseContentLayout->setSpacing(0);

    m_databaseModel = new QStandardItemModel(this);
    m_databaseModel->setHorizontalHeaderLabels(QStringList() << QStringLiteral("数据库数据"));
    m_databaseTree = new QTreeView(databaseContent);
    m_databaseTree->setObjectName(QStringLiteral("databaseCatalogTree"));
    m_databaseTree->setModel(m_databaseModel);
    m_databaseTree->setHeaderHidden(true);
    m_databaseTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_databaseTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_databaseTree->setUniformRowHeights(true);
    m_databaseTree->setContextMenuPolicy(Qt::CustomContextMenu);
    databaseContentLayout->addWidget(m_databaseTree, 1);
    databaseLayout->addWidget(databaseContent, 1);

    // ---------------- 本地数据 ----------------
    QWidget* localSection = new QWidget(sectionSplitter);
    localSection->setObjectName(QStringLiteral("localCatalogSection"));
    QVBoxLayout* localLayout = new QVBoxLayout(localSection);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(0);

    QToolButton* localTitleButton = nullptr;
    QFrame* localHeader = createSectionHeader(
        QStringLiteral("本地数据"),
        localSection,
        QList<QToolButton*>(),
        &localTitleButton);
    localLayout->addWidget(localHeader);
    localTitleButton->setObjectName(QStringLiteral("localCatalogTitleButton"));

    QWidget* localContent = new QWidget(localSection);
    localContent->setObjectName(QStringLiteral("localCatalogContent"));
    QVBoxLayout* localContentLayout = new QVBoxLayout(localContent);
    localContentLayout->setContentsMargins(0, 0, 0, 0);
    localContentLayout->setSpacing(0);

    m_localStatusLabel = new QLabel(localContent);
    m_localStatusLabel->setObjectName(QStringLiteral("localCatalogStatusLabel"));
    m_localStatusLabel->setText(QStringLiteral("未连接本地数据文件夹"));
    m_localStatusLabel->setToolTip(QStringLiteral("使用“选择文件夹”选择本地数据目录"));
    m_localStatusLabel->setWordWrap(true);
    localContentLayout->addWidget(m_localStatusLabel);

    m_localTree = new QgsBrowserTreeView(localContent);
    m_localTree->setObjectName(QStringLiteral("localCatalogTree"));
    m_localTree->setHeaderHidden(true);
    m_localTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_localTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 树状态与平台同一 section（平台 DataCatalogWidget.cpp:916 写死该名字）
    m_localTree->setSettingsSection(QStringLiteral("ltzkWorkspaceDataCatalog"));
    m_localTree->setContextMenuPolicy(Qt::CustomContextMenu);
    localContentLayout->addWidget(m_localTree, 1);
    localLayout->addWidget(localContent, 1);

    // ---------------- 地图图层（插件自有段） ----------------
    QWidget* mapLayerSection = new QWidget(sectionSplitter);
    mapLayerSection->setObjectName(QStringLiteral("mapLayerCatalogSection"));
    QVBoxLayout* mapLayerLayout = new QVBoxLayout(mapLayerSection);
    mapLayerLayout->setContentsMargins(0, 0, 0, 0);
    mapLayerLayout->setSpacing(0);

    QToolButton* mapLayerTitleButton = nullptr;
    QFrame* mapLayerHeader = createSectionHeader(
        QStringLiteral("地图图层"),
        mapLayerSection,
        QList<QToolButton*>(),
        &mapLayerTitleButton);
    mapLayerLayout->addWidget(mapLayerHeader);
    mapLayerTitleButton->setObjectName(QStringLiteral("mapLayerCatalogTitleButton"));

    QWidget* mapLayerContent = new QWidget(mapLayerSection);
    mapLayerContent->setObjectName(QStringLiteral("mapLayerCatalogContent"));
    QVBoxLayout* mapLayerContentLayout = new QVBoxLayout(mapLayerContent);
    mapLayerContentLayout->setContentsMargins(0, 0, 0, 0);
    mapLayerContentLayout->setSpacing(0);

    m_mapLayerTree = new QTreeWidget(mapLayerContent);
    m_mapLayerTree->setObjectName(QStringLiteral("mapLayerCatalogTree"));
    m_mapLayerTree->setColumnCount(1);
    m_mapLayerTree->setHeaderHidden(true);
    m_mapLayerTree->setRootIsDecorated(true);
    m_mapLayerTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mapLayerTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mapLayerTree->setContextMenuPolicy(Qt::CustomContextMenu);
    mapLayerContentLayout->addWidget(m_mapLayerTree, 1);
    mapLayerLayout->addWidget(mapLayerContent, 1);

    sectionSplitter->addWidget(databaseSection);
    sectionSplitter->addWidget(localSection);
    sectionSplitter->addWidget(mapLayerSection);
    sectionSplitter->setStretchFactor(0, 1);
    sectionSplitter->setStretchFactor(1, 2);
    sectionSplitter->setStretchFactor(2, 1);
    sectionSplitter->setSizes(QList<int>() << 220 << 420 << 160);

    // 点标题栏：把该段展开、其余段收到最小（与平台 selectCatalogSource 一致）
    const QList<QWidget*> contents =
        { databaseContent, localContent, mapLayerContent };
    const QList<QFrame*> headers =
        { databaseHeader, localHeader, mapLayerHeader };
    auto selectCatalogSection = [sectionSplitter, contents, headers](int section)
    {
        for (int i = 0; i < contents.size(); ++i)
        {
            contents.at(i)->setVisible(i == section);
        }

        QTimer::singleShot(0, sectionSplitter, [sectionSplitter, contents, headers, section]()
        {
            QList<int> currentSizes = sectionSplitter->sizes();
            int totalSize = 0;
            for (int size : currentSizes)
            {
                totalSize += size;
            }
            if (totalSize <= 0)
            {
                return;
            }

            int collapsedSize = 0;
            for (QFrame* header : headers)
            {
                collapsedSize = qMax(collapsedSize, header->sizeHint().height());
            }
            const int expandedSize = qMax(collapsedSize, totalSize - collapsedSize * (contents.size() - 1));
            QList<int> targetSizes;
            for (int i = 0; i < contents.size(); ++i)
            {
                targetSizes << (i == section ? expandedSize : collapsedSize);
            }
            sectionSplitter->setSizes(targetSizes);
        });
    };

    connect(databaseTitleButton, &QToolButton::clicked, this, [selectCatalogSection]()
    {
        selectCatalogSection(0);
    });
    connect(localTitleButton, &QToolButton::clicked, this, [selectCatalogSection]()
    {
        selectCatalogSection(1);
    });
    connect(mapLayerTitleButton, &QToolButton::clicked, this, [selectCatalogSection]()
    {
        selectCatalogSection(2);
    });

    rootLayout->addWidget(sectionSplitter, 1);

    connect(m_databaseTree, &QTreeView::doubleClicked,
        this, &CSE_DataCatalogBrowser::onDatabaseDoubleClicked);
    connect(m_databaseTree, &QTreeView::customContextMenuRequested,
        this, &CSE_DataCatalogBrowser::onDatabaseContextMenu);
    connect(m_localTree, &QTreeView::doubleClicked,
        this, &CSE_DataCatalogBrowser::onLocalDoubleClicked);
    connect(m_localTree, &QTreeView::customContextMenuRequested,
        this, &CSE_DataCatalogBrowser::onLocalContextMenu);
    connect(m_mapLayerTree, &QTreeWidget::itemDoubleClicked,
        this, &CSE_DataCatalogBrowser::onMapLayerDoubleClicked);
    connect(m_mapLayerTree, &QTreeWidget::customContextMenuRequested,
        this, &CSE_DataCatalogBrowser::onMapLayerContextMenu);

    showDatabaseMessage(QStringLiteral("请点击“选择数据库”连接数据库。"), false);
}

// ============================================================
// 数据库段：连接（照平台 connectDatabase / openDatabaseConnection）
//   与平台唯一的差别：不做登录门禁 —— 插件这个数据裁剪对话框没有登录态，
//   连接参数由 DataCatalogConnectionDialog 自己收，连上即可用。
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
        showDatabaseMessage(QStringLiteral("数据库连接参数不完整，请点击“选择数据库”重新填写。"), true);
        return;
    }

    QString errorMessage;
    if (!openDatabaseConnection(parameters, &errorMessage))
    {
        const QString message = errorMessage.isEmpty()
            ? QStringLiteral("无法打开数据库连接。")
            : errorMessage;
        QMessageBox::warning(this, QStringLiteral("数据库连接失败"), message);
        showDatabaseMessage(QStringLiteral("数据库连接失败：%1").arg(message), true);
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

void CSE_DataCatalogBrowser::showDatabaseMessage(const QString& message, bool isError)
{
    if (!m_databaseModel)
    {
        return;
    }

    m_databaseModel->removeRows(0, m_databaseModel->rowCount());
    QStandardItem* item = new QStandardItem(message);
    item->setEditable(false);
    item->setEnabled(false);
    if (isError)
    {
        item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    }
    m_databaseModel->appendRow(item);
    emit this->message(message, isError);
}

QIcon CSE_DataCatalogBrowser::databaseItemIcon(const QString& layerType) const
{
    if (layerType == QStringLiteral("raster"))
    {
        const QIcon icon(QStringLiteral(":/images/themes/default/mActionAddRasterLayer.svg"));
        return icon.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : icon;
    }
    if (layerType == QStringLiteral("table"))
    {
        const QIcon icon(QStringLiteral(":/images/themes/default/mActionOpenTable.svg"));
        return icon.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : icon;
    }
    const QIcon icon(QStringLiteral(":/images/themes/default/mActionAddOgrLayer.svg"));
    return icon.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : icon;
}

QStandardItem* CSE_DataCatalogBrowser::schemaItem(
    const QString& schemaName,
    QStandardItem* databaseItem)
{
    for (int row = 0; row < databaseItem->rowCount(); ++row)
    {
        QStandardItem* item = databaseItem->child(row);
        if (item && item->data(DatabaseSchemaRole).toString() == schemaName)
        {
            return item;
        }
    }

    QStandardItem* item = new QStandardItem(
        style()->standardIcon(QStyle::SP_DirIcon),
        schemaName);
    item->setEditable(false);
    item->setData(schemaName, DatabaseSchemaRole);
    item->setData(databaseItem->data(DatabaseNameRole), DatabaseNameRole);
    databaseItem->appendRow(item);
    return item;
}

// 平台 refreshDatabaseData() 的"普通表浏览"分支（去掉成果目录那一段）
void CSE_DataCatalogBrowser::refreshDatabaseData()
{
    if (!m_databaseModel)
    {
        return;
    }

    m_databaseModel->removeRows(0, m_databaseModel->rowCount());
    if (m_databaseConnectionName.isEmpty())
    {
        showDatabaseMessage(QStringLiteral("请先手动连接数据库。"), false);
        return;
    }

    QSqlDatabase database = catalogDatabase(m_databaseConnectionName);
    if (!database.isValid() || !database.isOpen())
    {
        showDatabaseMessage(QStringLiteral("手动数据库连接尚未打开，请点击“选择数据库”重试。"), true);
        return;
    }

    QStandardItem* databaseItem = new QStandardItem(
        style()->standardIcon(QStyle::SP_DriveNetIcon),
        database.databaseName());
    databaseItem->setEditable(false);
    databaseItem->setData(database.databaseName(), DatabaseNameRole);
    m_databaseModel->appendRow(databaseItem);

    QSqlQuery query(database);
    if (!query.exec(databaseCatalogQuery()))
    {
        showDatabaseMessage(
            QStringLiteral("读取数据库目录失败：%1").arg(query.lastError().text()),
            true);
        return;
    }

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
    if (query.lastError().isValid())
    {
        showDatabaseMessage(
            QStringLiteral("读取数据库目录失败：%1").arg(query.lastError().text()),
            true);
        return;
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
                databaseCatalogKey(schemaName, tableName),
                -1);
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
        if (metadata.lastError().isValid())
        {
            warnings->append(
                QStringLiteral("读取 %1 失败：%2")
                    .arg(relationName, metadata.lastError().text()));
        }
    };

    appendSpatialEntries(
        QStringLiteral("geometry_columns"),
        databaseVectorMetadataQuery(),
        QStringLiteral("vector"),
        &metadataWarnings);
    appendSpatialEntries(
        QStringLiteral("raster_columns"),
        databaseRasterMetadataQuery(),
        QStringLiteral("raster"),
        &metadataWarnings);

    int dataItemCount = 0;
    for (const DatabaseCatalogEntry& entry : entries)
    {
        const QString& schemaName = entry.schemaName;
        const QString& tableName = entry.tableName;
        const QString& geometryColumn = entry.dataColumn;
        const QString& geometryType = entry.dataType;
        const int srid = entry.srid;
        const QString& layerType = entry.layerType;

        QStandardItem* schema = schemaItem(schemaName, databaseItem);
        QStandardItem* table = new QStandardItem(
            databaseItemIcon(layerType),
            tableName + databaseItemSuffix(layerType, geometryType, srid, geometryColumn));
        table->setEditable(false);
        if (layerType == QStringLiteral("table"))
        {
            table->setEnabled(false);
            table->setToolTip(QStringLiteral("普通表仅用于目录浏览，不能作为地图图层加载。"));
        }
        else
        {
            table->setToolTip(QStringLiteral("双击加载 %1.%2").arg(schemaName, tableName));
        }
        table->setData(layerType, DatabaseLayerTypeRole);
        table->setData(schemaName, DatabaseSchemaRole);
        table->setData(tableName, DatabaseTableRole);
        table->setData(geometryColumn, DatabaseGeometryColumnRole);
        table->setData(database.databaseName(), DatabaseNameRole);
        schema->appendRow(table);
        ++dataItemCount;
    }

    for (const QString& warning : metadataWarnings)
    {
        QStandardItem* warningItem = new QStandardItem(
            style()->standardIcon(QStyle::SP_MessageBoxWarning),
            warning);
        warningItem->setEditable(false);
        warningItem->setEnabled(false);
        databaseItem->appendRow(warningItem);
    }

    if (dataItemCount == 0)
    {
        QStandardItem* emptyItem = new QStandardItem(QStringLiteral("当前数据库没有可读取的业务数据。"));
        emptyItem->setEditable(false);
        emptyItem->setEnabled(false);
        databaseItem->appendRow(emptyItem);
    }

    m_databaseTree->expand(databaseItem->index());
}

// 数据库表 → QGIS 数据源 URI（照平台 databaseUri 的普通表分支）
QgsMimeDataUtils::Uri CSE_DataCatalogBrowser::databaseUri(const QStandardItem* item) const
{
    QgsMimeDataUtils::Uri uri;
    if (!item)
    {
        return uri;
    }

    const QString layerType = item->data(DatabaseLayerTypeRole).toString();
    if (item->data(DatabaseTableRole).toString().isEmpty())
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
    sourceUri.setDataSource(
        item->data(DatabaseSchemaRole).toString(),
        item->data(DatabaseTableRole).toString(),
        item->data(DatabaseGeometryColumnRole).toString());

    uri.layerType = (layerType == QStringLiteral("raster"))
        ? QStringLiteral("raster")
        : QStringLiteral("vector");
    uri.providerKey = (uri.layerType == QStringLiteral("raster"))
        ? QStringLiteral("postgresraster")
        : QStringLiteral("postgres");
    uri.name = item->data(DatabaseTableRole).toString();
    uri.uri = sourceUri.uri(false);
    return uri;
}

// 数据库栅格的 GDAL 连接串候选：不同 GDAL 版本对 schema/table/column 的写法不同，
// 由裁剪引擎在打开时逐个探测（见 ClipExportEngine::firstOpenableRasterUri）。
QStringList CSE_DataCatalogBrowser::databaseRasterUriCandidates(
    const QString& schema,
    const QString& table,
    const QString& geometryColumn) const
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

// ============================================================
// 本地段（照平台 initializeLocalBrowser 及配套实现）
// ============================================================
void CSE_DataCatalogBrowser::initializeLocalBrowser()
{
    m_browserModel = new QgsBrowserGuiModel(this);

    // QGIS 内部把"已连接的文件夹"叫做 Favorites。QGIS 懒加载完成后需要重新绑定
    // 树的根节点，这里监听 stateChanged 后排队刷新（与平台一致）。
    connect(
        m_browserModel,
        &QgsBrowserModel::stateChanged,
        this,
        [this](const QModelIndex& index, Qgis::BrowserItemState)
        {
            if (!m_browserModel || !m_localTree || !index.isValid())
            {
                return;
            }

            QgsDataItem* item = m_browserModel->dataItem(index);
            if (!item || item->path() != QStringLiteral("favorites:"))
            {
                return;
            }

            QTimer::singleShot(0, this, &CSE_DataCatalogBrowser::showFavoritesInTree);
        });

    // 先完成模型初始化再挂到视图上，保证视图首次映射收藏夹索引时源树已完整。
    m_browserModel->initialize();

    // 必须显式 setModel：QgsBrowserTreeView::setBrowserModel() 只是把模型记下来
    // （保存/恢复展开状态、rowsInserted 自动展开用），并不会把它设成视图的
    // 数据模型。平台那边没这个问题，是因为它先 setModel(m_localProxyModel)
    // 再 setBrowserModel(m_browserModel)。少了 setModel 视图就没有模型，
    // setRootIndex 会被 Qt 以"index 不属于当前模型"拒掉 → 树一片空白；
    // 而状态标签是直接问模型要行数，所以照样显示"已连接 N 个本地数据文件夹"。
    m_localTree->setModel(m_browserModel);
    m_localTree->setBrowserModel(m_browserModel);
    refreshLocalFolders();
}

void CSE_DataCatalogBrowser::expandLocalTreeToPath(const QString& path)
{
    if (!m_localTree || !m_browserModel || path.isEmpty())
    {
        return;
    }

    // 麒麟 SDK 的 QgsBrowserTreeView 没有 expandPath（Windows 侧 QGIS 3.28 才提供该 API），
    // 这里用两个平台都具备的 QgsBrowserModel::findPath 做等价实现：
    // 找到目标节点 → 展开其全部祖先、展开自身、滚动到可见。
    const QModelIndex viewIndex = m_browserModel->findPath(path);
    if (!viewIndex.isValid())
    {
        return;
    }

    // QTreeView 没有对外公开的 expandAncestors（Qt 内部的祖先展开逻辑由 scrollTo 自行调用），
    // 这里显式逐级展开祖先链，确保目标节点可见。
    for (QModelIndex ancestor = viewIndex.parent(); ancestor.isValid(); ancestor = ancestor.parent())
    {
        m_localTree->expand(ancestor);
    }
    m_localTree->expand(viewIndex);
    m_localTree->scrollTo(viewIndex);
}

QModelIndex CSE_DataCatalogBrowser::favoritesRootIndex()
{
    if (!m_browserModel)
    {
        return QModelIndex();
    }

    return m_browserModel->findPath(QStringLiteral("favorites:"));
}

void CSE_DataCatalogBrowser::showFavoritesInTree()
{
    if (!m_browserModel || !m_localTree)
    {
        return;
    }

    const QModelIndex favoritesIndex = favoritesRootIndex();
    if (!favoritesIndex.isValid())
    {
        if (m_localStatusLabel)
        {
            m_localStatusLabel->setText(QStringLiteral("正在初始化本地数据目录…"));
        }
        return;
    }

    m_localTree->setRootIndex(favoritesIndex);
    if (m_browserModel->canFetchMore(favoritesIndex))
    {
        m_browserModel->fetchMore(favoritesIndex);
    }
    m_localTree->expand(favoritesIndex);

    if (m_localStatusLabel)
    {
        const int folderCount = m_browserModel->rowCount(favoritesIndex);
        m_localStatusLabel->setText(
            folderCount > 0
                ? QStringLiteral("已连接 %1 个本地数据文件夹").arg(folderCount)
                : QStringLiteral("未连接本地数据文件夹"));
    }
}

void CSE_DataCatalogBrowser::connectLocalFolder()
{
    const QString path = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择本地数据文件夹"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty() || !m_browserModel)
    {
        return;
    }

    QString name = QFileInfo(path).fileName();
    if (name.isEmpty())
    {
        name = path;
    }

    m_browserModel->addFavoriteDirectory(path, name);
    if (m_localStatusLabel)
    {
        m_localStatusLabel->setText(
            QStringLiteral("正在连接本地数据文件夹：%1")
                .arg(QDir::toNativeSeparators(path)));
    }

    // addFavoriteDirectory 在收藏夹已填充时会立即插入；启动阶段可能异步填充，
    // 因此再排队一次刷新（与平台一致，避免过早清掉刚插入的行）。
    QTimer::singleShot(0, this, [this, path]()
    {
        showFavoritesInTree();
        if (m_localTree)
        {
            expandLocalTreeToPath(path);
        }
    });
}

void CSE_DataCatalogBrowser::disconnectSelectedLocalFolder()
{
    if (!m_browserModel || !m_localTree)
    {
        return;
    }

    const QModelIndex index = m_localTree->currentIndex();
    QgsDataItem* item = m_browserModel->dataItem(index);
    const QModelIndex favoritesIndex = favoritesRootIndex();
    QgsDataItem* favoritesItem = m_browserModel->dataItem(favoritesIndex);
    if (!item || !favoritesItem || item->parent() != favoritesItem ||
        !qobject_cast<QgsDirectoryItem*>(item))
    {
        QMessageBox::information(
            this,
            QStringLiteral("断开连接"),
            QStringLiteral("请选择已连接文件夹的根节点。"));
        return;
    }

    m_browserModel->removeFavorite(index);
    showFavoritesInTree();
}

void CSE_DataCatalogBrowser::refreshLocalFolders()
{
    if (!m_browserModel || !m_localTree)
    {
        return;
    }

    QModelIndex favoritesIndex = favoritesRootIndex();
    if (!favoritesIndex.isValid())
    {
        m_browserModel->reload();
        showFavoritesInTree();
        return;
    }

    showFavoritesInTree();
    QgsDataItem* favoritesItem = m_browserModel->dataItem(favoritesIndex);
    if (favoritesItem && favoritesItem->state() == Qgis::BrowserItemState::Populated)
    {
        m_browserModel->refresh(favoritesIndex);
    }
    QTimer::singleShot(0, this, &CSE_DataCatalogBrowser::showFavoritesInTree);
}

// ============================================================
// 地图图层段（插件自有）
// ============================================================
void CSE_DataCatalogBrowser::refreshMapLayers()
{
    if (!m_mapLayerTree)
    {
        return;
    }

    const QString currentId = m_mapLayerTree->currentItem()
        ? m_mapLayerTree->currentItem()->data(0, Qt::UserRole).toString()
        : QString();
    m_mapLayerTree->clear();

    const QMap<QString, QgsMapLayer*>& layers = QgsProject::instance()->mapLayers();
    for (auto it = layers.begin(); it != layers.end(); ++it)
    {
        QgsMapLayer* layer = it.value();
        if (!layer)
        {
            continue;
        }

        const bool isRaster = (qobject_cast<QgsRasterLayer*>(layer) != nullptr);
        QTreeWidgetItem* item = new QTreeWidgetItem(m_mapLayerTree);
        item->setText(0, layer->name());
        item->setToolTip(0, QStringLiteral("%1（双击在地图上定位）")
            .arg(isRaster ? QStringLiteral("栅格图层") : QStringLiteral("矢量图层")));
        // 图标与"成果存储/元数据管理"等对话框共用同一套（点/线/面/栅格）
        int iconKind = 3;
        if (QgsVectorLayer* vectorLayer = qobject_cast<QgsVectorLayer*>(layer))
        {
            switch (vectorLayer->geometryType())
            {
            case QgsWkbTypes::LineGeometry:    iconKind = 1; break;
            case QgsWkbTypes::PolygonGeometry: iconKind = 2; break;
            default:                           iconKind = 0; break;
            }
        }
        item->setIcon(0, LayerIconHelper::drawLayerIcon(iconKind));
        item->setData(0, Qt::UserRole, layer->id());
        item->setData(0, Qt::UserRole + 1, isRaster);
        if (!currentId.isEmpty() && layer->id() == currentId)
        {
            m_mapLayerTree->setCurrentItem(item);
        }
    }
}

// ============================================================
// 节点 → 数据源
// ============================================================
void CSE_DataCatalogBrowser::collectLocalUris(
    QgsDataItem* item,
    QList<QgsMimeDataUtils::Uri>* uris,
    int* unloadedCount) const
{
    if (!item || !uris)
    {
        return;
    }

    const QgsMimeDataUtils::UriList ownUris = item->mimeUris();
    for (const QgsMimeDataUtils::Uri& uri : ownUris)
    {
        appendUniqueUri(uris, uri);
    }

    if (m_browserModel && item->state() != Qgis::BrowserItemState::Populated)
    {
        const QModelIndex itemIndex = m_browserModel->findPath(item->path());
        if (itemIndex.isValid() && m_browserModel->canFetchMore(itemIndex))
        {
            m_browserModel->fetchMore(itemIndex);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
        }
    }

    const QVector<QgsDataItem*> children = item->children();
    if (ownUris.isEmpty() && children.isEmpty() &&
        item->state() != Qgis::BrowserItemState::Populated &&
        unloadedCount)
    {
        ++(*unloadedCount);
    }
    for (QgsDataItem* child : children)
    {
        collectLocalUris(child, uris, unloadedCount);
    }
}

CSE_DataCatalogBrowser::CatalogItem CSE_DataCatalogBrowser::databaseItemAt(const QPoint& pos) const
{
    CatalogItem context;
    context.kind = CatalogItem::Database;
    if (!m_databaseTree || !m_databaseModel)
    {
        return context;
    }

    const QModelIndex index = m_databaseTree->indexAt(pos);
    if (!index.isValid())
    {
        return context;
    }

    m_databaseTree->setCurrentIndex(index);
    const QStandardItem* item = m_databaseModel->itemFromIndex(index);
    if (!item)
    {
        return context;
    }

    context.valid = true;
    context.displayName = item->text();
    const QString schema = item->data(DatabaseSchemaRole).toString();
    const QString table = item->data(DatabaseTableRole).toString();
    if (!schema.isEmpty() && !table.isEmpty())
    {
        context.detail = schema + QStringLiteral(".") + table;
    }

    const QString layerType = item->data(DatabaseLayerTypeRole).toString();
    if (layerType == QStringLiteral("vector") || layerType == QStringLiteral("raster"))
    {
        const QgsMimeDataUtils::Uri uri = databaseUri(item);
        if (isSupportedCatalogUri(uri))
        {
            context.uris.append(uri);
            context.isLayer = true;
            context.isRaster = (uri.layerType == QStringLiteral("raster"));
            if (context.isRaster)
            {
                context.rasterUriCandidates = databaseRasterUriCandidates(
                    schema, table, item->data(DatabaseGeometryColumnRole).toString());
            }
        }
    }
    return context;
}

CSE_DataCatalogBrowser::CatalogItem CSE_DataCatalogBrowser::localItemAt(const QPoint& pos) const
{
    CatalogItem context;
    context.kind = CatalogItem::Local;
    if (!m_localTree || !m_browserModel)
    {
        return context;
    }

    const QModelIndex index = m_localTree->indexAt(pos);
    if (!index.isValid())
    {
        return context;
    }

    m_localTree->setCurrentIndex(index);
    QgsDataItem* item = m_browserModel->dataItem(index);
    if (!item)
    {
        return context;
    }

    context.valid = true;
    context.displayName = item->name();
    context.detail = item->path();

    int unloadedCount = 0;
    collectLocalUris(item, &context.uris, &unloadedCount);
    if (!context.uris.isEmpty())
    {
        // 单项数据（文件/GDB 子图层）才算"具体数据"；目录节点也可导出其下所有数据
        const QgsMimeDataUtils::UriList ownUris = item->mimeUris();
        context.isLayer = !ownUris.isEmpty() || !item->children().isEmpty();
        if (ownUris.size() == 1)
        {
            context.isRaster = (ownUris.first().layerType == QStringLiteral("raster"));
        }
        if (unloadedCount > 0)
        {
            context.detail += QStringLiteral("（%1 个子节点尚未展开）").arg(unloadedCount);
        }
    }
    return context;
}

CSE_DataCatalogBrowser::CatalogItem CSE_DataCatalogBrowser::mapLayerItemAt(const QPoint& pos) const
{
    CatalogItem context;
    context.kind = CatalogItem::MapLayer;
    if (!m_mapLayerTree)
    {
        return context;
    }

    QTreeWidgetItem* item = m_mapLayerTree->itemAt(pos);
    if (!item)
    {
        return context;
    }

    m_mapLayerTree->setCurrentItem(item);
    context.valid = true;
    context.mapLayerId = item->data(0, Qt::UserRole).toString();
    context.displayName = item->text(0);
    context.isRaster = item->data(0, Qt::UserRole + 1).toBool();
    return context;
}

// ============================================================
// 右键 / 双击
// ============================================================
void CSE_DataCatalogBrowser::onDatabaseContextMenu(const QPoint& pos)
{
    const CatalogItem item = databaseItemAt(pos);
    if (!item.valid)
    {
        return;
    }
    emit contextMenuRequested(m_databaseTree->viewport()->mapToGlobal(pos), item);
}

void CSE_DataCatalogBrowser::onLocalContextMenu(const QPoint& pos)
{
    const CatalogItem item = localItemAt(pos);
    if (!item.valid)
    {
        return;
    }
    emit contextMenuRequested(m_localTree->viewport()->mapToGlobal(pos), item);
}

void CSE_DataCatalogBrowser::onMapLayerContextMenu(const QPoint& pos)
{
    const CatalogItem item = mapLayerItemAt(pos);
    if (!item.valid)
    {
        return;
    }
    emit contextMenuRequested(m_mapLayerTree->viewport()->mapToGlobal(pos), item);
}

void CSE_DataCatalogBrowser::onDatabaseDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid() || !m_databaseModel)
    {
        return;
    }
    const CatalogItem item = databaseItemAt(m_databaseTree->visualRect(index).center());
    if (item.valid && !item.uris.isEmpty())
    {
        emit itemActivated(item);
    }
}

void CSE_DataCatalogBrowser::onLocalDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid())
    {
        return;
    }
    const CatalogItem item = localItemAt(m_localTree->visualRect(index).center());
    if (item.valid && !item.uris.isEmpty())
    {
        emit itemActivated(item);
    }
}

void CSE_DataCatalogBrowser::onMapLayerDoubleClicked(QTreeWidgetItem* item, int)
{
    if (!item)
    {
        return;
    }
    const CatalogItem context = mapLayerItemAt(m_mapLayerTree->visualItemRect(item).center());
    if (context.valid)
    {
        emit itemActivated(context);
    }
}
