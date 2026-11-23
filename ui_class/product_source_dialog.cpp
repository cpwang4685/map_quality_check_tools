/*--------------SE---------------*/
#include "product_source_dialog.h"

#include "se_range_export.h"
#include "se_main_area_clip.h"
#include "map_extent_draw_tool.h"

#include "database/product_dao.h"
#include "database/postgis_connector.h"
#include "core/directory_helper.h"
#include "core/layer_icon_helper.h"
#include "core/add_to_map_helper.h"

/*--------------QT---------------*/
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>
#include <QBrush>
#include <QColor>
#include <QRegularExpression>

/*--------------QGIS---------------*/
#include <qgis.h>
#include <qgisinterface.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsmapcanvas.h>
#include <qgsproviderregistry.h>
#include <qgsprovidermetadata.h>
#include <qgsprovidersublayerdetails.h>
#include <gdal_priv.h>
#include <libpq-fe.h>

#include <functional>

namespace
{
// 树节点数据：UserRole = 节点种类，UserRole+1 = 目录 id / 产品 id
const int kRoleKind = Qt::UserRole;
const int kRoleId = Qt::UserRole + 1;

enum NodeKind
{
    KindDir = 0,      // 普通目录（其直属成果作为子节点显示）
    KindProduct = 1,  // 成果数据节点
    KindLayerDir = 2  // 图层节点（nodeType==1，节点本身即数据）
};

// PostgreSQL 字符串字面量转义
QString pgEscape(QString s)
{
    s.replace(QLatin1Char('\''), QLatin1String("''"));
    return s;
}

// 栅格临时文件名：只替换文件系统不允许的字符，保留中文
QString safeFileStem(const QString& name)
{
    QString s = name.trimmed();
    s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\s]")),
        QStringLiteral("_"));
    if (s.isEmpty()) s = QStringLiteral("raster");
    return s;
}

bool isClipCapable(const ProductMetadata& p)
{
    if (p.productType == ProductType::Vector)
        return !p.layerTableName.isEmpty();
    if (p.productType == ProductType::Raster)
        return p.fileOid > 0 || QFileInfo::exists(p.filePath)
            || !p.layerTableName.isEmpty();
    return false;
}
} // namespace

ProductSourceDialog::ProductSourceDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QString::fromUtf8("从数据库获取数据源"));
    resize(780, 600);
    setMinimumSize(620, 420);

    m_dao = new ProductDAO(this);
    buildUi();
    refreshTree();
}

ProductSourceDialog::~ProductSourceDialog() = default;

void ProductSourceDialog::setQgisInterface(QgisInterface* iface)
{
    m_iface = iface;
}

void ProductSourceDialog::buildUi()
{
    auto* main = new QVBoxLayout(this);
    main->setSpacing(8);

    m_lblStatus = new QLabel(this);
    m_lblStatus->setWordWrap(true);
    main->addWidget(m_lblStatus);

    auto* hint = new QLabel(
        QString::fromUtf8("勾选数据后可批量导出；也可右键某个目录或数据单独导出。"), this);
    hint->setStyleSheet(QStringLiteral("QLabel { color: #888; }"));
    main->addWidget(hint);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(1);
    m_tree->setHeaderLabels(QStringList() << QString::fromUtf8("地图成果数据"));
    m_tree->header()->setStretchLastSection(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    main->addWidget(m_tree, 1);

    connect(m_tree, &QTreeWidget::itemChanged,
        this, &ProductSourceDialog::onItemChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
        this, &ProductSourceDialog::onContextMenuRequested);

    auto* btnRow = new QHBoxLayout();
    m_btnRangeChecked = new QPushButton(QString::fromUtf8("按范围导出勾选项"), this);
    m_btnMainAreaChecked = new QPushButton(QString::fromUtf8("按制图导出勾选项"), this);
    m_btnRefresh = new QPushButton(QString::fromUtf8("刷新"), this);
    m_btnClose = new QPushButton(QString::fromUtf8("关闭"), this);

    connect(m_btnRangeChecked, &QPushButton::clicked,
        this, &ProductSourceDialog::onExportCheckedByRange);
    connect(m_btnMainAreaChecked, &QPushButton::clicked,
        this, &ProductSourceDialog::onExportCheckedByMainArea);
    connect(m_btnRefresh, &QPushButton::clicked,
        this, &ProductSourceDialog::refreshTree);
    connect(m_btnClose, &QPushButton::clicked, this, &QDialog::close);

    btnRow->addWidget(m_btnRangeChecked);
    btnRow->addWidget(m_btnMainAreaChecked);
    btnRow->addStretch(1);
    btnRow->addWidget(m_btnRefresh);
    btnRow->addWidget(m_btnClose);
    main->addLayout(btnRow);

    updateButtonStates();
}

void ProductSourceDialog::refreshTree()
{
    m_syncing = true;
    m_tree->clear();

    PostgisConnector* db = PostgisConnector::instance();
    QString connectError;
    // 【2026-10-18】LTZK 平台已连接数据库时直接复用，不必再在插件里单独连一次
    if (!db || (!db->isConnected() && !db->ensureConnected(&connectError)))
    {
        if (connectError.isEmpty())
            connectError = db ? db->lastError() : QString::fromUtf8("数据库模块未初始化");
        m_lblStatus->setText(QString::fromUtf8(
            "<span style='color:#c0392b;'>未连接数据库，且未能复用 LTZK 平台的数据库连接：%1<br/>"
            "请先在 LTZK 平台连接数据库，或在本插件「数据库连接配置」中连接后点「刷新」。</span>")
            .arg(connectError.toHtmlEscaped()));
        m_syncing = false;
        updateButtonStates();
        return;
    }

    QString dbName = db->databaseName();
    if (dbName.isEmpty())
        dbName = QString::fromUtf8("数据库");
    QString schema = db->searchPath();
    if (schema.isEmpty())
        schema = QStringLiteral("public");
    QString statusText = QString::fromUtf8("数据库：%1（架构：%2）").arg(dbName, schema);
    const QString source = db->lastConnectSource();
    if (!source.isEmpty())
        statusText += QString::fromUtf8("　·　%1").arg(source);
    m_lblStatus->setText(statusText);

    QApplication::setOverrideCursor(Qt::WaitCursor);

    QStyle* style = QApplication::style();
    auto* root = new QTreeWidgetItem(m_tree);
    root->setText(0, dbName);
    root->setData(0, kRoleKind, int(KindDir));
    root->setData(0, kRoleId, 0);
    root->setIcon(0, style->standardIcon(QStyle::SP_DirHomeIcon));
    setItemCheckable(root, true);

    // 幂等创建固定目录节点（制图成果 / 制图要素 / 制图资料及其子节点）
    DirectoryHelper::ensureFixedDirectories(*m_dao);
    populateChildren(root, 0);

    // 【2026-10-18】首次打开时统一折叠，展开状态由用户自己点箭头控制
    m_tree->collapseAll();

    QApplication::restoreOverrideCursor();
    m_syncing = false;
    updateButtonStates();
}

void ProductSourceDialog::populateChildren(QTreeWidgetItem* parentItem, int parentDirId)
{
    if (!m_dao) return;

    QStyle* style = QApplication::style();
    const auto children = m_dao->getChildDirectories(parentDirId);
    for (const auto& child : children)
    {
        const auto products = m_dao->getProductsByDirectory(child.id);
        const bool isLayer = (child.nodeType == 1);

        auto* ci = new QTreeWidgetItem(parentItem);
        ci->setText(0, child.name);
        ci->setData(0, kRoleKind, isLayer ? int(KindLayerDir) : int(KindDir));
        ci->setData(0, kRoleId, child.id);

        int iconKind = -1;
        for (const auto& p : products)
        {
            const int k = LayerIconHelper::layerIconKindForProduct(p);
            if (k >= 0) { iconKind = k; break; }
        }
        if (iconKind >= 0)
            ci->setIcon(0, LayerIconHelper::drawLayerIcon(iconKind));
        else if (!isLayer && LayerIconHelper::isGdbDirectory(child.nodeType, child.name, products))
            // 【2026-10-18】GDB 容器目录用 QGIS 主题里的 GDB 图标，
            // 与 LTZK 平台左侧「本地数据」中的 .gdb 项保持一致
            ci->setIcon(0, LayerIconHelper::gdbFolderIcon());
        else
            ci->setIcon(0, style->standardIcon(
                isLayer ? QStyle::SP_FileIcon : QStyle::SP_DirIcon));

        if (isLayer && !products.isEmpty())
            ci->setToolTip(0, QString::fromUtf8("数据名称: %1")
                .arg(products.first().productName));

        setItemCheckable(ci, true);

        // 图层节点本身即数据（节点名就是图层名），不再重复挂同名成果子节点；
        // 普通目录下挂该目录的直属成果。
        if (!isLayer)
        {
            for (const auto& p : products)
            {
                auto* pi = new QTreeWidgetItem(ci);
                pi->setText(0, p.productName);
                pi->setData(0, kRoleKind, int(KindProduct));
                pi->setData(0, kRoleId, p.id);

                const int k = LayerIconHelper::layerIconKindForProduct(p);
                pi->setIcon(0, k >= 0
                    ? LayerIconHelper::drawLayerIcon(k)
                    : style->standardIcon(QStyle::SP_FileIcon));

                QString tip = QString::fromUtf8("数据名称: %1\n类型: %2\n密级: %3")
                    .arg(p.productName,
                        productTypeToString(p.productType),
                        securityLevelToString(p.securityLevel));

                if (isClipCapable(p))
                {
                    pi->setToolTip(0, tip);
                    setItemCheckable(pi, true);
                }
                else
                {
                    // 非空间数据（AI/PDF/CAD/文档/压缩包等）不能裁剪
                    pi->setToolTip(0,
                        tip + QString::fromUtf8("\n该类型不支持空间裁剪"));
                    pi->setFlags(pi->flags() & ~Qt::ItemIsUserCheckable);
                    pi->setForeground(0, QBrush(QColor(150, 150, 150)));
                }
            }
        }

        populateChildren(ci, child.id);
    }
}

void ProductSourceDialog::setItemCheckable(QTreeWidgetItem* item, bool on)
{
    if (!item) return;
    if (on)
    {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
    }
    else
    {
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }
}

void ProductSourceDialog::setSubtreeCheck(QTreeWidgetItem* item, Qt::CheckState state)
{
    if (!item) return;
    if (item->flags() & Qt::ItemIsUserCheckable)
        item->setCheckState(0, state);
    for (int i = 0; i < item->childCount(); ++i)
        setSubtreeCheck(item->child(i), state);
}

void ProductSourceDialog::refreshParentCheckStates(QTreeWidgetItem* item)
{
    if (!item) return;
    QTreeWidgetItem* parent = item->parent();
    while (parent)
    {
        int checkable = 0;
        int checked = 0;
        int partial = 0;
        for (int i = 0; i < parent->childCount(); ++i)
        {
            QTreeWidgetItem* child = parent->child(i);
            if (!(child->flags() & Qt::ItemIsUserCheckable))
                continue;
            ++checkable;
            const Qt::CheckState st = child->checkState(0);
            if (st == Qt::Checked) ++checked;
            else if (st == Qt::PartiallyChecked) ++partial;
        }

        if (checkable > 0)
        {
            Qt::CheckState state = Qt::Unchecked;
            if (checked == checkable) state = Qt::Checked;
            else if (checked > 0 || partial > 0) state = Qt::PartiallyChecked;
            if (parent->checkState(0) != state)
                parent->setCheckState(0, state);
        }
        parent = parent->parent();
    }
}

void ProductSourceDialog::onItemChanged(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    if (m_syncing || !item) return;

    m_syncing = true;
    const Qt::CheckState state = item->checkState(0);
    if (state != Qt::PartiallyChecked)
        setSubtreeCheck(item, state);
    refreshParentCheckStates(item);
    m_syncing = false;

    updateButtonStates();
}

void ProductSourceDialog::collectProductsFromItem(QTreeWidgetItem* item,
    QList<ProductMetadata>& out, QSet<int>& seen) const
{
    if (!item || !m_dao) return;

    const int kind = item->data(0, kRoleKind).toInt();
    const int id = item->data(0, kRoleId).toInt();

    auto addProduct = [&](const ProductMetadata& p)
    {
        if (p.id > 0 && !seen.contains(p.id))
        {
            seen.insert(p.id);
            out << p;
        }
    };

    if (kind == KindProduct)
    {
        if (id > 0)
            addProduct(m_dao->getProduct(id));
        return;
    }

    // 图层节点 / 普通目录：自己的直属成果
    for (const auto& p : m_dao->getProductsByDirectory(id))
        addProduct(p);

    // 普通目录继续递归子目录
    if (kind == KindDir)
    {
        for (int i = 0; i < item->childCount(); ++i)
            collectProductsFromItem(item->child(i), out, seen);
    }
}

QList<ProductMetadata> ProductSourceDialog::productsOfItem(QTreeWidgetItem* item) const
{
    QList<ProductMetadata> out;
    QSet<int> seen;
    collectProductsFromItem(item, out, seen);
    return out;
}

QList<ProductMetadata> ProductSourceDialog::checkedProducts() const
{
    QList<ProductMetadata> out;
    QSet<int> seen;

    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item)
    {
        if (!item) return;
        const int kind = item->data(0, kRoleKind).toInt();
        if ((kind == KindProduct || kind == KindLayerDir)
            && (item->flags() & Qt::ItemIsUserCheckable)
            && item->checkState(0) == Qt::Checked)
        {
            collectProductsFromItem(item, out, seen);
        }
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        walk(m_tree->topLevelItem(i));
    return out;
}

QList<ProductMetadata> ProductSourceDialog::clippableOnly(
    const QList<ProductMetadata>& in)
{
    QList<ProductMetadata> out;
    for (const auto& p : in)
    {
        if (isClipCapable(p))
            out << p;
    }
    return out;
}

void ProductSourceDialog::updateButtonStates()
{
    int nChecked = 0;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item)
    {
        if (!item) return;
        const int kind = item->data(0, kRoleKind).toInt();
        if ((kind == KindProduct || kind == KindLayerDir)
            && (item->flags() & Qt::ItemIsUserCheckable)
            && item->checkState(0) == Qt::Checked)
        {
            ++nChecked;
        }
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        walk(m_tree->topLevelItem(i));

    if (m_btnRangeChecked) m_btnRangeChecked->setEnabled(nChecked > 0);
    if (m_btnMainAreaChecked) m_btnMainAreaChecked->setEnabled(nChecked > 0);
}

void ProductSourceDialog::onContextMenuRequested(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree->itemAt(pos);

    // 右键具体节点 = 导出该节点（目录递归）；右键空白处 = 导出勾选项
    QList<ProductMetadata> products = item
        ? productsOfItem(item)
        : checkedProducts();
    products = clippableOnly(products);

    QMenu menu(this);
    // 【2026-10-18】右键导出一律顺带把导出结果设为地图综合的数据源；
    // 另加一个不裁剪、整份导出的入口。
    QAction* aRange = menu.addAction(QString::fromUtf8("按范围导出并设置为数据源"));
    QAction* aMainArea = menu.addAction(QString::fromUtf8("按制图导出并设置为数据源"));
    QAction* aDirect = menu.addAction(QString::fromUtf8("将该数据设置为数据源"));
    menu.addSeparator();
    QAction* aAddToMap = menu.addAction(QString::fromUtf8("添加到地图"));

    const bool hasData = !products.isEmpty();
    aRange->setEnabled(hasData);
    aMainArea->setEnabled(hasData);
    aDirect->setEnabled(hasData);
    aAddToMap->setEnabled(hasData);

    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == aRange)
    {
        exportProducts(products, true, /*setAsSource*/ true);
    }
    else if (chosen == aMainArea)
    {
        exportProducts(products, false, /*setAsSource*/ true);
    }
    else if (chosen == aDirect)
    {
        exportDirectToSource(products);
    }
    else if (chosen == aAddToMap)
    {
        if (!m_iface)
        {
            QMessageBox::warning(this, QString::fromUtf8("添加到地图"),
                QString::fromUtf8("未获取到 QGIS 地图接口，无法加载数据。"));
            return;
        }
        for (const auto& p : products)
            AddToMapHelper::addToMapWithFeedback(this, m_iface, p);
    }
}

void ProductSourceDialog::onExportCheckedByRange()
{
    exportProducts(clippableOnly(checkedProducts()), true);
}

void ProductSourceDialog::onExportCheckedByMainArea()
{
    exportProducts(clippableOnly(checkedProducts()), false);
}

bool ProductSourceDialog::productToSource(const ProductMetadata& meta,
    ClipExportEngine::Source& src, QString& err)
{
    PostgisConnector* db = PostgisConnector::instance();
    if (!db || !db->isConnected())
    {
        err = QString::fromUtf8("数据库未连接");
        return false;
    }

    if (meta.productType == ProductType::Vector)
    {
        if (meta.layerTableName.isEmpty())
        {
            err = QString::fromUtf8("缺少图层表名");
            return false;
        }

        PGconn* pg = db->nativeConnection();
        if (!pg || PQstatus(pg) != CONNECTION_OK)
        {
            err = QString::fromUtf8("数据库连接不可用");
            return false;
        }

        const QString host = QString::fromUtf8(PQhost(pg));
        const QString port = QString::fromUtf8(PQport(pg));
        const QString dbName = QString::fromUtf8(PQdb(pg));
        const QString user = QString::fromUtf8(PQuser(pg));
        const QString password = QString::fromUtf8(PQpass(pg));

        QString schema = db->searchPath();
        if (schema.isEmpty())
            schema = QStringLiteral("public");

        QString tableName =
            AddToMapHelper::resolveTableName(schema, meta.layerTableName);
        if (tableName.isEmpty())
            tableName = meta.layerTableName;

        // 优先从 geometry_columns 取真实几何列 / 类型 / SRID；
        // 查不到时回退到导入器固定使用的 geom / 记录里的 geometryType / 4490
        QString geomCol = QStringLiteral("geom");
        QString geomType = meta.geometryType.toUpper();
        int srid = 4490;

        const QVariantMap row = db->executeQueryOne(
            QStringLiteral("SELECT f_geometry_column AS geom_col, "
                           "type AS geom_type, srid "
                           "FROM geometry_columns "
                           "WHERE f_table_schema = $1 AND f_table_name = $2 "
                           "LIMIT 1"),
            QVariantList() << schema << tableName);
        if (!row.isEmpty())
        {
            const QString c = row.value(QStringLiteral("geom_col")).toString();
            const QString t = row.value(QStringLiteral("geom_type")).toString();
            const int s = row.value(QStringLiteral("srid")).toInt();
            if (!c.isEmpty()) geomCol = c;
            if (!t.isEmpty()) geomType = t.toUpper();
            if (s > 0) srid = s;
        }
        if (geomType.isEmpty())
            geomType = QStringLiteral("GEOMETRY");

        src.uri = QStringLiteral(
            "dbname='%1' host='%2' port='%3' user='%4' password='%5' "
            "sslmode=disable key='id' srid=%6 type=%7 "
            "table=\"%8\".\"%9\" (%10)")
            .arg(pgEscape(dbName), pgEscape(host), pgEscape(port),
                pgEscape(user), pgEscape(password))
            .arg(srid)
            .arg(geomType)
            .arg(pgEscape(schema), pgEscape(tableName), geomCol);
        src.name = exportBaseName(meta);
        src.provider = QStringLiteral("postgres");
        src.isRaster = false;
        return true;
    }

    if (meta.productType == ProductType::Raster)
    {
        QString path;
        int oid = meta.fileOid;

        // 老数据 fileOid 可能为 0，从栅格元数据表回退查询
        if (oid <= 0 && !meta.layerTableName.isEmpty())
        {
            const QVariantMap row = db->executeQueryOne(
                QStringLiteral("SELECT file_oid FROM \"%1\" LIMIT 1")
                    .arg(meta.layerTableName));
            if (!row.isEmpty())
                oid = row.value(QStringLiteral("file_oid")).toInt();
        }

        if (oid > 0)
        {
            QString ext = meta.fileFormat.toLower().trimmed();
            if (ext.isEmpty()) ext = QStringLiteral("tif");

            const QString tempDir =
                QDir::tempPath() + QStringLiteral("/ltzk_clip_src");
            QDir().mkpath(tempDir);
            path = tempDir + QLatin1Char('/') + safeFileStem(meta.productName)
                + QStringLiteral("_oid") + QString::number(oid)
                + QLatin1Char('.') + ext;

            if (!QFile::exists(path))
            {
                if (!db->loExport(oid, path))
                {
                    err = QString::fromUtf8("从数据库导出栅格文件失败");
                    return false;
                }
                m_tempFiles << path;
            }
        }
        else if (!meta.filePath.isEmpty() && QFileInfo::exists(meta.filePath))
        {
            path = meta.filePath;
        }
        else
        {
            err = QString::fromUtf8("栅格数据没有可读取的文件来源");
            return false;
        }

        src.uri = path;
        src.name = exportBaseName(meta);
        src.provider = QStringLiteral("gdal");
        src.isRaster = true;
        return true;
    }

    err = QString::fromUtf8("该数据不是矢量或栅格，无法进行空间裁剪");
    return false;
}

bool ProductSourceDialog::prepareSources(const QList<ProductMetadata>& products,
    const QString& title, QList<ClipExportEngine::Source>& sources)
{
    sources.clear();
    QStringList errors;
    QSet<QString> usedNames;
    m_tempFiles.clear();
    m_dirCache.clear();   // 【2026-10-18】每轮导出重新读目录节点（可能刚导入过数据）

    for (const auto& p : products)
    {
        ClipExportEngine::Source src;
        QString err;
        if (!productToSource(p, src, err))
        {
            errors << QString::fromUtf8("%1：%2").arg(p.productName, err);
            continue;
        }

        // 同名数据追加序号，避免输出文件互相覆盖
        QString name = src.name;
        if (name.isEmpty()) name = QString::fromUtf8("数据");
        QString unique = name;
        int n = 2;
        while (usedNames.contains(unique))
            unique = QStringLiteral("%1_%2").arg(name).arg(n++);
        usedNames.insert(unique);
        src.name = unique;

        sources << src;
    }

    if (sources.isEmpty())
    {
        QMessageBox::warning(this, title,
            QString::fromUtf8("没有可导出的数据：\n%1").arg(errors.join(QLatin1Char('\n'))));
        return false;
    }
    if (!errors.isEmpty())
    {
        QMessageBox::warning(this, title,
            QString::fromUtf8("以下数据被跳过：\n%1").arg(errors.join(QLatin1Char('\n'))));
    }
    return true;
}

void ProductSourceDialog::exportProducts(const QList<ProductMetadata>& products,
    bool byRange, bool setAsSource)
{
    const QString title = byRange
        ? QString::fromUtf8("按范围导出")
        : QString::fromUtf8("按制图导出");

    if (products.isEmpty())
    {
        QMessageBox::information(this, title,
            QString::fromUtf8("请先勾选或右键选择矢量 / 栅格数据。"));
        return;
    }

    QList<ClipExportEngine::Source> sources;
    if (!prepareSources(products, title, sources))
        return;

    QStringList written; // 实际写出的文件（设置数据源时取用）
    if (byRange)
    {
        QgsMapCanvas* canvas = m_iface ? m_iface->mapCanvas() : nullptr;
        if (!canvas)
        {
            QMessageBox::warning(this, title,
                QString::fromUtf8("未获取到地图画布，无法进行手动绘制或选择要素。"));
            return;
        }

        // 与数据裁剪对话框同一套流程：点「开始绘制」时先收起本对话框，
        // 在地图上框选后回填范围再重新弹出
        CSE_RangeExportDialogUi dlg(canvas, m_iface, this);
        dlg.setAutoLoadAfterExport(m_autoLoad);
        while (true)
        {
            dlg.resetDrawRequested();
            if (dlg.exec() != QDialog::Accepted) return;
            if (!dlg.drawRequested()) break;

            QgsRectangle drawnRect;
            hide();
            // 【2026-11-07】通知父窗口（地图综合对话框）一起收起，绘制完再一起弹出
            emit drawingStarted();
            const bool drawn = MapExtentDrawTool::drawExtentOnCanvas(
                canvas, MapExtentDrawTool::DrawRect, drawnRect);
            show();
            raise();
            activateWindow();
            emit drawingFinished();

            if (drawn)
            {
                dlg.setExtent(drawnRect);
                dlg.setExtentCrs(canvas->mapSettings().destinationCrs());
                dlg.setModeIndex(CSE_RangeExportDialogUi::RangeManualDraw);
            }
        }

        if (!dlg.hasExtent())
        {
            QMessageBox::warning(this, title,
                QString::fromUtf8("请先定义导出范围。"));
            return;
        }
        const QString out = dlg.outputPath();
        if (out.isEmpty()) return;

        ClipExportEngine::Options opt;
        // 面状要素直接用其几何精确求交；线/点退化为外包矩形
        opt.clipGeom = dlg.hasFeatureGeometry()
            ? dlg.exportFeatureGeometry()
            : ClipExportEngine::polygonFromRect(dlg.exportExtent());
        opt.clipCrs = dlg.exportExtentCrs();
        opt.overwriteExisting = true;

        QString driver;
        bool merge = false;
        if (!driverForFormat(dlg.format(), driver, merge))
        {
            QMessageBox::warning(this, title,
                QString::fromUtf8("当前环境不支持所选输出格式：%1").arg(dlg.format()));
            return;
        }
        opt.vectorDriver = driver;
        opt.mergeLayersIntoOneFile = merge;

        if (merge)
        {
            // GeoPackage / GDB：所有矢量写入同一容器文件；栅格落到同目录 .tif
            opt.explicitOutputFile = out;
            opt.outputDir = QFileInfo(out).absolutePath();
        }
        else
        {
            // Shapefile：输出路径是文件夹，每个数据一个 shp
            opt.outputDir = out;
        }

        written = runClipExport(sources, opt, title, dlg.autoLoadAfterExport());
    }
    else
    {
        CSE_MainAreaClipDialogUi dlg(this);
        const ProductMetadata& first = products.first();
        dlg.setSourcePath(first.filePath.isEmpty()
            ? first.productName
            : first.filePath);
        dlg.setAutoLoadAfterExport(m_autoLoad);
        if (dlg.exec() != QDialog::Accepted) return;

        const QString out = dlg.outputFilePath();
        if (out.isEmpty())
        {
            QMessageBox::warning(this, title,
                QString::fromUtf8("请先指定输出路径。"));
            return;
        }

        QgsCoordinateReferenceSystem clipCrs;
        const QgsRectangle clipRect = dlg.computedClipRect(&clipCrs);
        if (clipRect.isEmpty())
        {
            QMessageBox::warning(this, title,
                QString::fromUtf8("未能从主区数据中得到有效范围，请确认已选择要素。"));
            return;
        }

        ClipExportEngine::Options opt;
        opt.clipGeom = ClipExportEngine::polygonFromRect(clipRect);
        opt.clipCrs = clipCrs;
        opt.overwriteExisting = dlg.overwriteExisting();
        const QString encoding = dlg.encoding().trimmed();
        opt.fileEncoding = encoding.isEmpty()
            ? QStringLiteral("UTF-8")
            : encoding;

        QString driver;
        bool merge = false;
        if (!driverForFormat(dlg.format(), driver, merge))
        {
            QMessageBox::warning(this, title,
                QString::fromUtf8("当前环境不支持所选输出格式：%1").arg(dlg.format()));
            return;
        }
        opt.vectorDriver = driver;
        opt.mergeLayersIntoOneFile = merge;

        if (merge)
        {
            opt.explicitOutputFile = out;
            opt.outputDir = QFileInfo(out).absolutePath();
        }
        else
        {
            opt.outputDir = out;
        }

        written = runClipExport(sources, opt, title, dlg.autoLoadAfterExport());
    }

    // 【2026-10-18】右键「…并设置为数据源」：导出成功后把结果填成矢量数据源
    if (setAsSource && !written.isEmpty())
        emit dataSourceReady(dataSourcePathFor(written.first()));
}

// 【2026-10-18】「将该数据设置为数据源」：
// 复用『按范围导出』对话框的格式与输出路径（该对话框以"范围可选"模式打开），
// 不裁剪，直接把所选数据整份导出，然后把结果设为矢量数据源。
void ProductSourceDialog::exportDirectToSource(const QList<ProductMetadata>& products)
{
    const QString title = QString::fromUtf8("将该数据设置为数据源");

    if (products.isEmpty())
    {
        QMessageBox::information(this, title,
            QString::fromUtf8("请先勾选或右键选择矢量 / 栅格数据。"));
        return;
    }

    QList<ClipExportEngine::Source> sources;
    if (!prepareSources(products, title, sources))
        return;

    QgsMapCanvas* canvas = m_iface ? m_iface->mapCanvas() : nullptr;
    if (!canvas)
    {
        QMessageBox::warning(this, title,
            QString::fromUtf8("未获取到地图画布，无法打开导出设置。"));
        return;
    }

    CSE_RangeExportDialogUi dlg(canvas, m_iface, this);
    dlg.setWindowTitle(title);
    dlg.setAutoLoadAfterExport(m_autoLoad);
    dlg.setRangeOptional(true);   // 只取格式/输出路径，不校验范围
    if (dlg.exec() != QDialog::Accepted) return;

    const QString out = dlg.outputPath();
    if (out.isEmpty()) return;

    ClipExportEngine::Options opt;
    opt.noClip = true;            // 不裁剪，按数据自身范围整份导出
    opt.overwriteExisting = true;

    QString driver;
    bool merge = false;
    if (!driverForFormat(dlg.format(), driver, merge))
    {
        QMessageBox::warning(this, title,
            QString::fromUtf8("当前环境不支持所选输出格式：%1").arg(dlg.format()));
        return;
    }
    opt.vectorDriver = driver;
    opt.mergeLayersIntoOneFile = merge;

    if (merge)
    {
        opt.explicitOutputFile = out;
        opt.outputDir = QFileInfo(out).absolutePath();
    }
    else
    {
        opt.outputDir = out;
    }

    const QStringList written =
        runClipExport(sources, opt, title, dlg.autoLoadAfterExport());
    if (!written.isEmpty())
        emit dataSourceReady(dataSourcePathFor(written.first()));
}

QStringList ProductSourceDialog::runClipExport(
    const QList<ClipExportEngine::Source>& sources,
    const ClipExportEngine::Options& opt,
    const QString& title, bool autoLoad)
{
    QProgressDialog progress(QString::fromUtf8("准备导出…"),
        QString::fromUtf8("取消"), 0, sources.size(), this);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    ClipExportEngine::Options options = opt;
    options.progress = [&progress](int done, int total, const QString& message) -> bool
    {
        if (progress.wasCanceled()) return false;
        progress.setMaximum(total > 0 ? total : 1);
        progress.setValue(done);
        progress.setLabelText(message);
        QApplication::processEvents();
        return !progress.wasCanceled();
    };

    const QList<ClipExportEngine::Result> results =
        ClipExportEngine::clipSources(sources, options);
    progress.close();

    int nOk = 0;
    int nCancelled = 0;
    QStringList written;
    QStringList failed;
    for (const ClipExportEngine::Result& r : results)
    {
        if (r.ok)
        {
            ++nOk;
            written << r.outPath;
        }
        else if (r.error == QStringLiteral("已取消"))
        {
            ++nCancelled;
        }
        else
        {
            failed << QString::fromUtf8("%1：%2").arg(r.name, r.error);
        }
    }

    if (autoLoad)
    {
        for (const QString& p : written)
            autoLoadOutputToMap(p);
    }
    else
    {
        // 未自动加载时，本次导出的栅格临时文件可以删掉；
        // 自动加载时文件被地图图层占用，留给系统临时目录清理
        for (const QString& f : m_tempFiles)
            QFile::remove(f);
    }
    m_tempFiles.clear();

    if (nOk > 0 && failed.isEmpty())
    {
        const QString msg = (nOk == 1)
            ? QString::fromUtf8("%1完成：%2").arg(title, written.first())
            : QString::fromUtf8("%1完成，共导出 %2 个数据。").arg(title).arg(nOk);
        QMessageBox::information(this, title, msg);
    }
    else if (nOk > 0)
    {
        QMessageBox::warning(this, title,
            QString::fromUtf8("%1完成 %2 个，失败 %3 个：\n%4")
                .arg(title).arg(nOk).arg(failed.size())
                .arg(failed.join(QLatin1Char('\n'))));
    }
    else if (nCancelled > 0)
    {
        QMessageBox::information(this, title, QString::fromUtf8("已取消。"));
    }
    else
    {
        QMessageBox::warning(this, title,
            failed.isEmpty()
                ? QString::fromUtf8("没有可导出的数据。")
                : failed.join(QLatin1Char('\n')));
    }
    return written;
}

// 【2026-10-18】输出文件 → 「矢量数据目录」可用的路径：
// FileGDB 直接给 .gdb 目录本身（地图综合按 FileGDB 输入处理），
// 其余格式给所在文件夹（shp 多个文件同目录；gpkg/栅格也放这里）。
QString ProductSourceDialog::dataSourcePathFor(const QString& writtenFile)
{
    const QFileInfo fi(writtenFile);
    if (fi.suffix().compare(QLatin1String("gdb"), Qt::CaseInsensitive) == 0)
        return fi.absoluteFilePath();
    return fi.absolutePath();
}

void ProductSourceDialog::autoLoadOutputToMap(const QString& path)
{
    if (!m_iface || !m_iface->mapCanvas() || path.isEmpty()) return;
    const QFileInfo fi(path);
    if (!fi.exists()) return;

    const QString ext = fi.suffix().toLower();
    const bool isContainer = (ext == QLatin1String("gpkg")
        || ext == QLatin1String("gdb"));

    if (isContainer)
    {
        // 容器文件要枚举内部图层逐个加载
        QList<QgsProviderSublayerDetails> subs;
        if (QgsProviderMetadata* md =
            QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("ogr")))
        {
            try
            {
                subs = md->querySublayers(path, Qgis::SublayerQueryFlags(), nullptr);
            }
            catch (...)
            {
                subs.clear();
            }
        }

        for (const QgsProviderSublayerDetails& sub : subs)
        {
            if (sub.providerKey() != QLatin1String("ogr")) continue;
            auto* layer = new QgsVectorLayer(sub.uri(), sub.name(), QStringLiteral("ogr"));
            if (layer->isValid())
                QgsProject::instance()->addMapLayer(layer);
            else
                delete layer;
        }
        return;
    }

    if (ext == QLatin1String("tif") || ext == QLatin1String("tiff")
        || ext == QLatin1String("img") || ext == QLatin1String("sid"))
    {
        auto* layer = new QgsRasterLayer(path, fi.completeBaseName(),
            QStringLiteral("gdal"));
        if (layer->isValid())
            QgsProject::instance()->addMapLayer(layer);
        else
            delete layer;
        return;
    }

    auto* layer = new QgsVectorLayer(path, fi.completeBaseName(),
        QStringLiteral("ogr"));
    if (layer->isValid())
        QgsProject::instance()->addMapLayer(layer);
    else
        delete layer;
}

QString ProductSourceDialog::exportBaseName(const ProductMetadata& meta)
{
    // 【2026-10-18】产品名是入库时生成的 县名_v版本_图层名（如 永登县_v1_a_lvyoujingqu），
    // 导出成这个名字用户认不出来。图层节点（node_type=1）的节点名才是原始图层名
    // （GDB 里就是 A_LvYouJingQu），所以挂在图层节点下的数据优先用节点名。
    if (m_dao && meta.parentDirId > 0)
    {
        DirectoryNode dir;
        if (m_dirCache.contains(meta.parentDirId))
            dir = m_dirCache.value(meta.parentDirId);
        else
        {
            dir = m_dao->getDirectory(meta.parentDirId);
            m_dirCache.insert(meta.parentDirId, dir);
        }

        if (dir.id > 0 && dir.nodeType == 1 && !dir.name.trimmed().isEmpty())
            return stripDataSuffix(dir.name);
    }

    return stripDataSuffix(meta.productName);
}

QString ProductSourceDialog::stripDataSuffix(const QString& name)
{
    static const char* const kSuffixes[] = {
        ".shp", ".tif", ".tiff", ".img", ".gpkg", ".geojson", ".json",
        ".kml", ".tab", ".mif", ".ecw", ".bmp", ".png", ".jpg", ".jpeg",
        nullptr
    };

    QString s = name.trimmed();
    for (int i = 0; kSuffixes[i]; ++i)
    {
        const QString suffix = QString::fromLatin1(kSuffixes[i]);
        if (s.endsWith(suffix, Qt::CaseInsensitive))
        {
            s.chop(suffix.size());
            break;
        }
    }
    return s.isEmpty() ? name.trimmed() : s;
}

bool ProductSourceDialog::driverForFormat(const QString& format,
    QString& driver, bool& merge)
{
    if (format.contains(QStringLiteral("GeoPackage"), Qt::CaseInsensitive))
    {
        driver = QStringLiteral("GPKG");
        merge = true;
        return true;
    }
    if (format.contains(QStringLiteral("GDB"), Qt::CaseInsensitive))
    {
#if GDAL_VERSION_NUM >= 3060000
        driver = QStringLiteral("OpenFileGDB");
        merge = true;
        return true;
#else
        // 写文件型地理数据库需要 GDAL 3.6+（麒麟为 GDAL 3.0.4）
        driver.clear();
        merge = false;
        return false;
#endif
    }
    if (format.contains(QStringLiteral("Shapefile"), Qt::CaseInsensitive))
    {
        driver = QStringLiteral("ESRI Shapefile");
        merge = false;
        return true;
    }

    driver.clear();
    merge = false;
    return false;
}
