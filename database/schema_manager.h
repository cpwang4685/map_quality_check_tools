#ifndef SCHEMA_MANAGER_H
#define SCHEMA_MANAGER_H

#include <QObject>
#include <QString>

/**
 * @brief 数据库Schema管理器
 * 
 * 负责创建和维护PostGIS数据库表结构，
 * 包括成果元数据表、版本记录表、目录结构表、权限表等。
 */
class SchemaManager : public QObject
{
	Q_OBJECT

public:
	explicit SchemaManager(QObject* parent = nullptr);

	// 初始化全部数据库表结构
	bool initializeSchema();

	// 检查Schema是否已初始化
	bool isSchemaInitialized();

	// 获取最后一次错误信息
	QString lastError() const { return m_lastError; }

	// 获取建表SQL
	static QString getCreateSchemaSQL();

	// 为指定产品创建独立的 PostGIS 图层表
	bool createDataLayerTable(const QString& tableName, const QString& geometryType, int srid = 4490);
	// 删除图层表
	bool dropDataLayerTable(const QString& tableName);

private:
	// 创建空间扩展
	bool createExtensions();

	// 创建可选扩展（pg_trgm等，失败不影响主流程）
	bool createOptionalExtensions();

	// 创建各功能表
	bool createProductMetadataTable();
	bool createVersionRecordsTable();
	bool createDirectoryTable();
	bool createTagsTable();
	bool createProductTagsTable();
	bool createPermissionsTable();
	bool createAuditLogTable();

	// 权限表结构迁移（兼容旧版本）
	void migratePermissionsTable();

	// 基础元数据表迁移（uuid→data_id，新增27项字段）
	void migrateProductMetadataToV2();

	// 专有元数据表迁移（对齐 CSV 规范）
	void migrateVectorMetaToV2();
	void migrateRasterMetaToV2();
	void migrateDiagramMetaToV2();
	void migrateDocumentMetaToV2();

	// 图层注册表（记录入库的矢量/栅格图层与 product_metadata 的关联）
	bool createLayerRegistryTable();

	// 数据类型专属元数据扩展表（1:1 关联 product_metadata）
	bool createVectorMetaTable();
	bool createRasterMetaTable();
	bool createDiagramMetaTable();
	bool createOutputMetaTable();
	bool createDocumentMetaTable();

	// 从宽表迁移已有数据到扩展表
	bool migrateVectorMetaFromWideTable();
	bool migrateRasterMetaFromWideTable();

	// 地图服务管理相关表
	bool createServiceUnitsTable();
	bool createServiceRecordsTable();

	// 通用字典表（存储各下拉框自定义选项：应用场景/服务等级/服务类别/材质等）
	bool createServiceDictTable();
	
	// 地图服务表结构迁移（兼容旧版本）
	bool migrateServiceRecordsTable();

	// 创建索引
	bool createIndexes();

	// 执行建表SQL并捕获错误
	bool executeSchemaSQL(const QString& sql, const QString& stepName);

	QString m_lastError;
};

#endif // SCHEMA_MANAGER_H
