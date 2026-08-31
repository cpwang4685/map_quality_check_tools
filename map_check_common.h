#pragma once
#ifndef MAP_CHECK_COMMON_H
#define MAP_CHECK_COMMON_H

#include <QString>
#include <QObject>

#include "json.hpp"

#include <QMetaType>

// json.hpp 的"fwd 段"（含 nlohmann::ordered_json 别名）与 QGIS SDK 自带的 json_fwd.hpp
// 共用 INCLUDE_NLOHMANN_JSON_FWD_HPP_ 守卫宏，谁先被包含谁定义它：
//   Windows：json.hpp 先到，fwd 段生效，nlohmann::ordered_json 存在；
//   麒麟：QGIS 头（qgsgeometry.h → json_fwd.hpp）先到，json.hpp 的 fwd 段被整段跳过，
//         nlohmann::ordered_json 缺失。
// 不能 #undef 该宏强开 fwd 段——会与已生效的 SDK json_fwd.hpp 产生模板默认实参重定义
// （redefinition of default argument）错误。
// 两种 include 顺序下，json.hpp 主段都会完整定义 basic_json 与 ordered_map（第16528行），
// 因此直接显式构造别名即可，不依赖 fwd 段是否生效。
using ordered_json = nlohmann::basic_json<nlohmann::ordered_map>;

// ==================== 质检项结果 ====================

// 与全局JSON的check_item节点结构一致
struct CheckItemResult {
    QString id;          // 质检项唯一标识
    QString name;        // 质检项名称（用于匹配）
    QString status;      // 状态："未质检"/"质检成功"/"质检失败"
    bool err_exist;      // 是否存在错误
    ordered_json info;   // 统计信息（如要素数量、错误数）
    ordered_json errors; // 错误详情（如描述、预期值、实际值）
};

Q_DECLARE_METATYPE(CheckItemResult) // 支持Qt信号传递

// ==================== 数据库图层信息 ====================

// 图层指针前置声明（DbLayerInfo 以指针持有图层，跨平台共享头不做完整 include，避免依赖 QGIS 头）
class QgsVectorLayer;
class QgsRasterLayer;

// 数据库图层信息（导出/数据管理/数据列表导出对话框共用；strSourcePath 为 Shape/GDB 文件源路径，
// pLoadedLayer/pLoadedRasterLayer 为从地图直接获取的已加载图层指针，非文件模式使用）
struct DbLayerInfo
{
    QString strSchema;          // Schema
    QString strTableName;       // 表名
    QString strGeomType;        // 几何类型
    QString strSrid;            // SRID
    QString strCrs;             // 坐标系（描述）
    QString strSourcePath;      // 文件源路径（Shape/GDB 模式使用）
    long long iFeatureCount;    // 要素数
    QgsVectorLayer* pLoadedLayer;       // 已加载矢量图层指针（非文件模式使用）
    QgsRasterLayer* pLoadedRasterLayer; // 已加载栅格图层指针（非文件模式使用）

    DbLayerInfo()
        : iFeatureCount(-1), pLoadedLayer(nullptr), pLoadedRasterLayer(nullptr)
    {}
};

// ==================== 备份策略相关类型定义 ====================

// 备份策略类型
enum class BackupType
{
    FullBackup = 0,       // 全量备份
    IncrementalBackup,    // 增量备份
    WALArchive,           // WAL归档
    LogicalExportBackup,  // 逻辑导出备份
    DatabaseBackup        // 数据库备份（新增）
};

// 备份频率类型
enum class BackupFrequency
{
    Weekly = 0,   // 每周
    Daily,        // 每天
    Hourly,       // 每小时
    Monthly,     // 每月
    RealTime     // 实时
};

// 存储位置类型
enum class StorageLocation
{
    Local = 0             // 本地
};

// 备份数据源类型（新增）
enum class BackupDataSource
{
    FileSystem = 0,  // 文件系统
    Database = 1,    // 数据库
    All = 2          // 全部（文件系统 + 数据库）
};

// 数据库类型（新增）
enum class DatabaseType
{
    PostgreSQL = 0, // PostGIS / PostgreSQL
    MySQL = 1,
    Oracle = 2,
    SQLServer = 3
};

// 数据库备份方式（新增，用于多选）
enum class DatabaseBackupMethod
{
    FullBackup = 0,        // 完整备份
    IncrementalBackup = 1, // 增量备份
    DifferentialBackup = 2,// 差异备份
    CustomBinary = 3       // 自定义格式
};

// 数据库备份范围（新增）
enum class DatabaseBackupScope
{
    WholeDatabase = 0,
    Schema = 1,
    SelectedTables = 2
};

// 数据库备份格式（新增）
enum class DatabaseBackupFormat
{
    SQL = 0,        // .sql
    SQL_GZ = 1,     // .sql.gz
    PG_DUMP = 2,    // .dump (pg_dump)
    GPKG = 3        // GeoPackage
};

// 备份策略结构
struct TimedBackupStrategy
{
    BackupType      eBackupType;       // 备份类型
    BackupFrequency eFrequency;        // 执行频率
    QString         strExecuteTime;    // 建议执行时间描述
    QString         strRetentionPeriod;// 保留周期描述
    StorageLocation eStorageLocation;  // 存储位置
    bool            bEnabled;          // 是否启用

    // 数据源配置（新增）
    BackupDataSource eDataSource;      // 备份数据源
    DatabaseType    eDbType;           // 数据库类型
    QString         strDbHost;         // 数据库主机
    int             nDbPort;           // 数据库端口
    QString         strDbName;         // 数据库名称
    QString         strDbUser;         // 数据库用户名
    QString         strDbPassword;     // 数据库密码
    QString         strDbSchema;       // 数据库Schema
    DatabaseBackupScope eDbBackupScope;     // 数据库备份范围
    DatabaseBackupMethod eDbBackupMethod;    // 数据库备份方式
    DatabaseBackupFormat eDbBackupFormat;    // 数据库备份格式
    QStringList     lstDbTables;       // 选中的表列表

    TimedBackupStrategy()
        : eBackupType(BackupType::FullBackup)
        , eFrequency(BackupFrequency::Weekly)
        , eStorageLocation(StorageLocation::Local)
        , bEnabled(true)
    , eDataSource(BackupDataSource::FileSystem)
    , eDbType(DatabaseType::PostgreSQL)
    , nDbPort(5432)
    , strDbSchema("public")
    , eDbBackupScope(DatabaseBackupScope::WholeDatabase)
    , eDbBackupMethod(DatabaseBackupMethod::FullBackup)
    , eDbBackupFormat(DatabaseBackupFormat::SQL)
    {}

    QString backupTypeName() const
    {
        switch (eBackupType)
        {
        case BackupType::FullBackup:         return QObject::tr("全量备份");
        case BackupType::IncrementalBackup:  return QObject::tr("增量备份");
        case BackupType::WALArchive:         return QObject::tr("WAL归档");
        case BackupType::LogicalExportBackup:return QObject::tr("逻辑导出备份");
        case BackupType::DatabaseBackup:     return QObject::tr("数据库备份");
        default: return QObject::tr("未知");
        }
    }

    QString frequencyName() const
    {
        switch (eFrequency)
        {
        case BackupFrequency::Weekly:   return QObject::tr("每周1次");
        case BackupFrequency::Daily:    return QObject::tr("每天1次");
        case BackupFrequency::Hourly:   return QObject::tr("实时/每小时");
        case BackupFrequency::Monthly:  return QObject::tr("每月1次");
        case BackupFrequency::RealTime: return QObject::tr("持续进行");
        default: return QObject::tr("未知");
        }
    }

    QString storageLocationName() const
    {
        switch (eStorageLocation)
        {
        case StorageLocation::Local: return QObject::tr("本地存储");
        default: return QObject::tr("未知");
        }
    }

    QString dataSourceName() const
    {
        switch (eDataSource)
        {
        case BackupDataSource::FileSystem: return QObject::tr("文件系统");
        case BackupDataSource::Database:   return QObject::tr("数据库");
        case BackupDataSource::All:        return QObject::tr("全部");
        default: return QObject::tr("未知");
        }
    }

    QString databaseTypeName() const
    {
        switch (eDbType)
        {
        case DatabaseType::PostgreSQL: return QObject::tr("PostGIS / PostgreSQL");
        case DatabaseType::MySQL:      return QObject::tr("MySQL");
        case DatabaseType::Oracle:     return QObject::tr("Oracle");
        case DatabaseType::SQLServer:  return QObject::tr("SQL Server");
        default: return QObject::tr("未知");
        }
    }

    QString databaseBackupScopeName() const
    {
        switch (eDbBackupScope)
        {
        case DatabaseBackupScope::WholeDatabase: return QObject::tr("整个数据库");
        case DatabaseBackupScope::Schema:          return QObject::tr("指定Schema");
        case DatabaseBackupScope::SelectedTables:  return QObject::tr("指定表");
        default: return QObject::tr("未知");
        }
    }

    QString databaseBackupFormatName() const
    {
        switch (eDbBackupFormat)
        {
        case DatabaseBackupFormat::SQL:     return QObject::tr(".sql");
        case DatabaseBackupFormat::SQL_GZ:  return QObject::tr(".sql.gz");
        case DatabaseBackupFormat::PG_DUMP: return QObject::tr(".dump (pg)");
        case DatabaseBackupFormat::GPKG:    return QObject::tr("GeoPackage");
        default: return QObject::tr("未知");
        }
    }
};

#endif // MAP_CHECK_COMMON_H