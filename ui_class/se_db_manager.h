#pragma once
#ifndef SE_DB_MANAGER_H
#define SE_DB_MANAGER_H

#include <QString>
#include <QList>
#include <QHash>
#include <QStringList>

class QgsVectorLayer;
struct FieldDefinition;  // 定义在 se_mission453_check.h 中

// 数据库连接参数
struct DbConnectionParams {
    QString host = "localhost";
    int port = 5432;
    QString dbname = "mpqis";
    QString user = "mpqis_app";
    QString password;
    QString schema = "mpqis";
    bool isValid() const { return !host.isEmpty() && !dbname.isEmpty(); }
};

// 数据库图层元数据（对应 vector_layer_meta 表结构）
struct DbLayerMeta {
    QString tableName;       // 表名（如 a_bld_afc_a）
    QString layerName;       // 图层中文名
    QString geometryType;    // 几何类型（MultiPolygon/MultiLineString/MultiPoint）
    int srid = 4548;
    QStringList fieldNames;
    QStringList fieldTypes;
    QStringList fieldLengths;
};

namespace DbManager {

    // 设置/获取连接参数
    void setConnection(const DbConnectionParams& params);
    DbConnectionParams connection();

    // 测试数据库连接是否可用
    bool testConnection(const DbConnectionParams& params);

    // 从 vector_layer_meta 读取所有图层元数据
    QList<DbLayerMeta> loadLayerMeta();

    // 从 vector_layer_meta / information_schema 读取指定表字段定义
    QList<FieldDefinition> loadFieldDefs(const QString& tableName);

    // 用 QgsDataSourceUri 打开 PostGIS 图层
    QgsVectorLayer* openPostGisLayer(const QString& tableName, const DbConnectionParams& params = DbConnectionParams());

    // 构造 PostGIS 数据源 URI
    QString buildDataSourceUri(const QString& tableName, const DbConnectionParams& params = DbConnectionParams());

} // namespace DbManager

#endif // SE_DB_MANAGER_H
