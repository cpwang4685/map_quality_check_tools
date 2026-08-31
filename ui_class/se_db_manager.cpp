#include "se_db_manager.h"
#include "se_mission453_check.h" // FieldDefinition
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <qgsvectorlayer.h>
#include <qgsdatasourceuri.h>
#include <QDebug>

using namespace DbManager;

static DbConnectionParams g_dbParams;

void DbManager::setConnection(const DbConnectionParams& params) {
    g_dbParams = params;
}

DbConnectionParams DbManager::connection() {
    return g_dbParams;
}

bool DbManager::testConnection(const DbConnectionParams& params) {
    const QString connName = "mpqis_test";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
        db.setHostName(params.host);
        db.setPort(params.port);
        db.setDatabaseName(params.dbname);
        db.setUserName(params.user);
        if (!params.password.isEmpty())
            db.setPassword(params.password);
        bool ok = db.open();
        if (ok) db.close();
        QSqlDatabase::removeDatabase(connName);
        return ok;
    }
}

static QSqlDatabase getDb() {
    const QString connName = "mpqis_main";
    if (QSqlDatabase::contains(connName))
        return QSqlDatabase::database(connName);
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
    db.setHostName(g_dbParams.host);
    db.setPort(g_dbParams.port);
    db.setDatabaseName(g_dbParams.dbname);
    db.setUserName(g_dbParams.user);
    if (!g_dbParams.password.isEmpty())
        db.setPassword(g_dbParams.password);
    return db;
}

QList<DbLayerMeta> DbManager::loadLayerMeta() {
    QList<DbLayerMeta> result;
    QSqlDatabase db = getDb();
    if (!db.isOpen() && !db.open()) {
        qWarning() << "[DbManager] 无法连接数据库:" << db.lastError().text();
        return result;
    }
    // 尝试读取 vector_layer_meta 表
    QSqlQuery q(db);
    bool hasTable = q.exec("SELECT EXISTS (SELECT FROM information_schema.tables "
        "WHERE table_schema='" + g_dbParams.schema + "' AND table_name='vector_layer_meta')");
    if (hasTable && q.next() && q.value(0).toBool()) {
        q.exec("SELECT table_name, layer_name, geometry_type, srid FROM "
            + g_dbParams.schema + ".vector_layer_meta ORDER BY table_name");
        while (q.next()) {
            DbLayerMeta m;
            m.tableName = q.value(0).toString();
            m.layerName = q.value(1).toString();
            m.geometryType = q.value(2).toString();
            m.srid = q.value(3).toInt();
            result.append(m);
        }
    }
    // 回退方案：从 information_schema 扫描所有 mpqis schema 的表
    if (result.isEmpty()) {
        q.exec("SELECT table_name FROM information_schema.tables "
            "WHERE table_schema='" + g_dbParams.schema + "' AND table_type='BASE TABLE' "
            "ORDER BY table_name");
        while (q.next()) {
            DbLayerMeta m;
            m.tableName = q.value(0).toString();
            result.append(m);
        }
    }
    return result;
}

QList<FieldDefinition> DbManager::loadFieldDefs(const QString& tableName) {
    QList<FieldDefinition> result;
    QSqlDatabase db = getDb();
    if (!db.isOpen() && !db.open()) {
        qWarning() << "[DbManager] 无法连接数据库:" << db.lastError().text();
        return result;
    }
    // 从 information_schema.columns 读取字段定义
    QSqlQuery q(db);
    q.prepare("SELECT column_name, ordinal_position, data_type, "
        "character_maximum_length, numeric_precision, numeric_scale, "
        "is_nullable, column_default "
        "FROM information_schema.columns "
        "WHERE table_schema=:schema AND table_name=:table "
        "ORDER BY ordinal_position");
    q.bindValue(":schema", g_dbParams.schema);
    q.bindValue(":table", tableName);
    if (!q.exec()) {
        qWarning() << "[DbManager] 读取字段定义失败:" << q.lastError().text();
        return result;
    }
    while (q.next()) {
        FieldDefinition def;
        def.fieldName = q.value(0).toString();
        def.fieldIdx = q.value(1).toInt() - 1; // 序号从0开始
        def.fieldType = q.value(2).toString();
        // 构造 fieldLength: 如 varchar(80) → "80", numeric(10,2) → "10.2"
        int maxLen = q.value(3).toInt();       // character_maximum_length
        int numPrec = q.value(4).toInt();      // numeric_precision
        int numScale = q.value(5).toInt();     // numeric_scale
        if (maxLen > 0)
            def.fieldLength = QString::number(maxLen);
        else if (numPrec > 0 && numScale > 0)
            def.fieldLength = QString("%1.%2").arg(numPrec).arg(numScale);
        else if (numPrec > 0)
            def.fieldLength = QString::number(numPrec);
        def.allowNull = (q.value(6).toString().toUpper() == "YES") ? "yes" : "no";
        def.defaultValue = q.value(7).toString();
        if (!def.fieldName.isEmpty())
            result.append(def);
    }
    return result;
}

QgsVectorLayer* DbManager::openPostGisLayer(const QString& tableName,
    const DbConnectionParams& params) {
    const DbConnectionParams& p = params.isValid() ? params : g_dbParams;
    if (!p.isValid()) return nullptr;
    QString uri = buildDataSourceUri(tableName, p);
    QgsVectorLayer* layer = new QgsVectorLayer(uri, tableName, "postgres");
    if (!layer->isValid()) {
        qWarning() << "[DbManager] 无法打开PostGIS图层:" << tableName << layer->error().message();
        delete layer;
        return nullptr;
    }
    return layer;
}

QString DbManager::buildDataSourceUri(const QString& tableName,
    const DbConnectionParams& params) {
    const DbConnectionParams& p = params.isValid() ? params : g_dbParams;
    QgsDataSourceUri uri;
    uri.setConnection(p.host, QString::number(p.port), p.dbname, p.user, p.password);
    // 用 (schema, table, geometry_column) 三元组
    uri.setDataSource(p.schema, tableName, "geom");
    return uri.uri();
}
