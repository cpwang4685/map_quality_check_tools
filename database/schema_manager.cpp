#include "schema_manager.h"
#include "postgis_connector.h"
#include <qgis.h>
#include <qgsmessagelog.h>

SchemaManager::SchemaManager(QObject* parent)
	: QObject(parent)
{
}

bool SchemaManager::initializeSchema()
{
	auto* db = PostgisConnector::instance();

	if (!db->isConnected())
	{
		return false;
	}

	// 检查当前 search_path
	auto searchPathResult = db->executeQueryOne("SHOW search_path");
	if (!createExtensions()) {
	}
	if (!createProductMetadataTable()) {
	}
	migrateProductMetadataToV2();  // 按 基础元数据.csv 27项重构字段
	if (!createVersionRecordsTable()) {
	}
	if (!createDirectoryTable()) {
	}
	if (!createTagsTable()) {
	}
	if (!createProductTagsTable()) {
	}
	if (!createPermissionsTable()) {
	}
	migratePermissionsTable();  // 兼容旧表结构迁移（去掉 department，添加 password_hash）
	if (!createAuditLogTable()) {
	}
	if (!createLayerRegistryTable()) {
	}
	// 数据类型专属元数据扩展表
	if (!createVectorMetaTable()) {
	}
	if (!createRasterMetaTable()) {
	}
	if (!createDiagramMetaTable()) {
	}
	if (!createOutputMetaTable()) {
	}
	if (!createDocumentMetaTable()) {
	}
	// 从宽表迁移已有数据到扩展表（仅首次）
	migrateVectorMetaFromWideTable();
	migrateRasterMetaFromWideTable();
	// 按 CSV 规范重建专有元数据表
	migrateVectorMetaToV2();
	migrateRasterMetaToV2();
	migrateDiagramMetaToV2();
	migrateDocumentMetaToV2();
	if (!createServiceUnitsTable()) {
	}
	if (!createServiceRecordsTable()) {
	}
	createServiceDictTable();  // 通用字典表（应用场景/服务等级/服务类别/材质自定义选项）
	migrateServiceRecordsTable();  // 兼容旧表结构迁移（添加UNV2新列）

	// 兼容旧 service_records 表：扩展 service_form 兼容
	executeSchemaSQL(
		"ALTER TABLE service_records ALTER COLUMN service_form TYPE VARCHAR(256)",
		"扩展 service_form 列为 VARCHAR(256)");

	// 兼容旧 service_records 表：移除 unit_id 的外键约束（支持手动输入单位名称）
	// 先查询实际的外键约束名（PostgreSQL 自动生成的约束名可能不同）
	{
		auto fkResult = db->executeQuery(
			"SELECT conname FROM pg_constraint "
			"WHERE conrelid = 'service_records'::regclass AND contype = 'f' AND conkey @> ARRAY["
			"  (SELECT attnum FROM pg_attribute WHERE attrelid = 'service_records'::regclass AND attname = 'unit_id')"
			"]");
		for (const auto& row : fkResult)
		{
			QString conName = row.toMap().value("conname").toString();
			if (!conName.isEmpty())
			{
				executeSchemaSQL(
					QString("ALTER TABLE service_records DROP CONSTRAINT IF EXISTS %1").arg(conName),
					QString("移除 service_records.unit_id 的外键约束: %1").arg(conName));
			}
		}
	}
	executeSchemaSQL(
		"ALTER TABLE service_records ALTER COLUMN unit_id DROP NOT NULL",
		"移除 service_records.unit_id 的 NOT NULL 约束");

	// 确保 product_metadata 包含 layer_table_name 列（兼容旧 Schema）
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS layer_table_name VARCHAR(200)",
		"添加 layer_table_name 列");

	// 确保 product_directory 包含 node_type 列（0=普通目录，1=图层节点；兼容旧 Schema）
	executeSchemaSQL(
		"ALTER TABLE product_directory ADD COLUMN IF NOT EXISTS node_type INTEGER DEFAULT 0",
		"添加 product_directory.node_type 列");

	// 修复旧数据：将「直接挂载矢量产品且无子目录」的叶子节点标记为图层节点，
	// 使旧版本导入的 shp/gdb 图层在目录树中显示矢量图层图标（幂等，仅处理 node_type=0）
	executeSchemaSQL(
		"UPDATE product_directory d SET node_type = 1 "
		"WHERE node_type = 0 "
		"AND EXISTS (SELECT 1 FROM product_metadata p "
		"            WHERE p.parent_dir_id = d.id AND p.product_type = '矢量数据') "
		"AND NOT EXISTS (SELECT 1 FROM product_directory c WHERE c.parent_id = d.id)",
		"修复旧图层节点 node_type 标记");

	// 确保 version_records 包含 file_format 和 layer_table_name 列（兼容旧 Schema）
	executeSchemaSQL(
		"ALTER TABLE version_records ADD COLUMN IF NOT EXISTS file_format VARCHAR(50)",
		"添加 version_records.file_format 列");
	executeSchemaSQL(
		"ALTER TABLE version_records ADD COLUMN IF NOT EXISTS layer_table_name VARCHAR(200)",
		"添加 version_records.layer_table_name 列");

	// 将可能超长的 VARCHAR 列升级为 TEXT（兼容 WKT 长字符串、长比例尺名称等）
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN crs TYPE VARCHAR(200)",
		"升级 crs 列为 VARCHAR(200)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN geometry_type TYPE TEXT",
		"升级 geometry_type 列为 TEXT");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN scale TYPE TEXT",
		"升级 scale 列为 TEXT");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN security_level TYPE VARCHAR(50)",
		"升级 security_level 列为 VARCHAR(50)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN product_type TYPE VARCHAR(100)",
		"升级 product_type 列为 VARCHAR(100)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN file_format TYPE VARCHAR(100)",
		"升级 file_format 列为 VARCHAR(100)");

	// 添加基础元数据扩展列（对齐基础元数据.csv 数据标准）
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS source VARCHAR(200)",
		"添加 source 列（数据来源）");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS city VARCHAR(100)",
		"添加 city 列（所在市州）");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS project_name VARCHAR(200)",
		"添加 project_name 列（项目名称）");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS delivery_status VARCHAR(30)",
		"添加 delivery_status 列（汇交情况）");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS delivery_time TIMESTAMP",
		"添加 delivery_time 列（汇交时间）");

	// 可选扩展（pg_trgm），失败不影响主流程
	createOptionalExtensions();

	if (!createIndexes()) {
	}

	return true;
}

bool SchemaManager::isSchemaInitialized()
{
	auto* db = PostgisConnector::instance();
	if (!db->isConnected()) return false;

	// 检查主表是否存在
	auto result = db->executeQueryOne(
		"SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_name = 'product_metadata')"
	);
	return result.value("exists").toString() == "t";
}

bool SchemaManager::executeSchemaSQL(const QString& sql, const QString& stepName)
{
	auto* db = PostgisConnector::instance();
	bool ok = db->executeNonQuery(sql);
	if (!ok) {
		m_lastError = stepName + ": " + db->lastError();
	}
	return ok;
}

bool SchemaManager::createExtensions()
{
	return executeSchemaSQL("CREATE EXTENSION IF NOT EXISTS postgis", "创建postgis扩展") &&
		   executeSchemaSQL("CREATE EXTENSION IF NOT EXISTS \"uuid-ossp\"", "创建uuid-ossp扩展");
}

bool SchemaManager::createOptionalExtensions()
{
	// pg_trgm 是可选的，用于全文检索索引优化，安装失败不影响主流程
	if (!executeSchemaSQL("CREATE EXTENSION IF NOT EXISTS pg_trgm", "创建pg_trgm扩展"))
	{
	}
	return true;
}

bool SchemaManager::createProductMetadataTable()
{
	// 按 基础元数据.csv 27项 重建表结构
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_metadata (
			-- CSV 第 1~27 项：业务元数据
			data_id         VARCHAR(36) NOT NULL DEFAULT uuid_generate_v4()::text UNIQUE,
			product_name    VARCHAR(200) NOT NULL,
			description     VARCHAR(254),
			source          VARCHAR(200),
			version_note    VARCHAR(200),
			file_format     VARCHAR(30),
			file_size       BIGINT DEFAULT 0,
			is_compressed   VARCHAR(4),
			bounds          VARCHAR(40),
			center_lon      VARCHAR(12),
			center_lat      VARCHAR(12),
			start_datetime  TIMESTAMP,
			end_datetime    TIMESTAMP,
			security_level  VARCHAR(50) DEFAULT '非密',
			created_by      VARCHAR(200),
			created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			updated_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			tags            VARCHAR(200),
			city            VARCHAR(100),
			scale           TEXT,
			project_name    VARCHAR(200),
			producer        VARCHAR(500),
			production_date DATE,
			crs             VARCHAR(50),
			compilation_info VARCHAR(500),
			delivery_status VARCHAR(30),
			delivery_time   TIMESTAMP,

			-- 系统内部字段
			id              SERIAL PRIMARY KEY,
			uuid            VARCHAR(36),       -- 兼容旧字段，data_id 别名
			product_type    VARCHAR(100),
			file_path       TEXT,
			file_hash       VARCHAR(128),
			file_oid        OID,
			thumbnail_path  TEXT,
			geom            GEOMETRY(GEOMETRY, 4490),
			min_x           DOUBLE PRECISION,
			min_y           DOUBLE PRECISION,
			max_x           DOUBLE PRECISION,
			max_y           DOUBLE PRECISION,
			spatial_extent_wkt TEXT,
			geometry_type   TEXT,
			band_count      INTEGER,
			pixel_width     INTEGER,
			pixel_height    INTEGER,
			pixel_resolution DOUBLE PRECISION,
			approval_number VARCHAR(200),
			parent_dir_id   INTEGER DEFAULT 0,
			directory_path  TEXT,
			updated_by      VARCHAR(200),
			layer_table_name VARCHAR(200),
			current_version INTEGER DEFAULT 1
		)
	)";
	return executeSchemaSQL(sql, "创建product_metadata表（基础元数据27项）");
}

/**
 * @brief 将旧 product_metadata 迁移到 基础元数据.csv 27项格式（幂等）
 *
 * 处理流程：
 *  1. uuid → data_id（重命名 + 类型转换）
 *  2. 添加 CSV 新增字段
 *  3. 调整已有字段类型/长度使其对齐 CSV 规范
 *  4. 从已有数据填充自动字段（bounds / center_lon / center_lat）
 */
void SchemaManager::migrateProductMetadataToV2()
{
	auto* db = PostgisConnector::instance();

	// 测试 data_id 列是否已存在 → 已迁移则跳过
	auto checkResult = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_metadata' AND column_name='data_id'");
	if (!checkResult.isEmpty())
	{
		return;  // 已迁移
	}

	// ─── 1. uuid → data_id ───
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS data_id VARCHAR(36)",
		"添加 data_id 列");
	executeSchemaSQL(
		"UPDATE product_metadata SET data_id = uuid::text WHERE data_id IS NULL AND uuid IS NOT NULL",
		"迁移 uuid → data_id");
	// 保留 uuid 字段作为兼容别名

	// ─── 2. 新增 CSV 字段（幂等） ───
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS is_compressed VARCHAR(4)",
		"添加 is_compressed 列");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS bounds VARCHAR(40)",
		"添加 bounds 列");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS center_lon VARCHAR(12)",
		"添加 center_lon 列");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS center_lat VARCHAR(12)",
		"添加 center_lat 列");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS start_datetime TIMESTAMP",
		"添加 start_datetime 列");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS end_datetime TIMESTAMP",
		"添加 end_datetime 列");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ADD COLUMN IF NOT EXISTS tags VARCHAR(200)",
		"添加 tags 列");

	// ─── 3. 调整已有字段类型/长度 ───
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN product_name TYPE VARCHAR(200)",
		"product_name → VARCHAR(200)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN file_format TYPE VARCHAR(30) USING file_format::VARCHAR(30)",
		"file_format → VARCHAR(30)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN description TYPE VARCHAR(254) USING description::VARCHAR(254)",
		"description → VARCHAR(254)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN crs TYPE VARCHAR(50) USING crs::VARCHAR(50)",
		"crs → VARCHAR(50)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN compilation_info TYPE VARCHAR(500) USING compilation_info::VARCHAR(500)",
		"compilation_info → VARCHAR(500)");
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN version_note TYPE VARCHAR(200) USING version_note::VARCHAR(200)",
		"version_note → VARCHAR(200)");
	// production_date: VARCHAR(50) → DATE（安全转换）
	executeSchemaSQL(
		"ALTER TABLE product_metadata ALTER COLUMN production_date TYPE DATE USING "
		"CASE WHEN production_date ~ '^\\d{4}-\\d{2}-\\d{2}' "
		"     THEN production_date::DATE ELSE NULL END",
		"production_date → DATE");

	// ─── 4. 自动填充空间字段 ───
	executeSchemaSQL(
		"UPDATE product_metadata SET bounds = LEFT(spatial_extent_wkt, 40) "
		"WHERE spatial_extent_wkt IS NOT NULL AND bounds IS NULL",
		"自动填充 bounds");
	executeSchemaSQL(
		"UPDATE product_metadata SET "
		"  center_lon = CAST(((min_x + max_x) / 2.0) AS VARCHAR(12)), "
		"  center_lat = CAST(((min_y + max_y) / 2.0) AS VARCHAR(12)) "
		"WHERE min_x IS NOT NULL AND max_x IS NOT NULL AND center_lon IS NULL",
		"自动计算中心点坐标");

	QgsMessageLog::logMessage(
		QStringLiteral("product_metadata → 基础元数据27项 迁移完成"), "SchemaManager", Qgis::Info);
}

bool SchemaManager::createVersionRecordsTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS version_records (
			id              SERIAL PRIMARY KEY,
			product_id      INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE,
			version_number  INTEGER NOT NULL,
			file_path       TEXT,
			file_hash       VARCHAR(128),
			file_size       BIGINT DEFAULT 0,
			file_oid        OID,
			file_format     VARCHAR(50),
			layer_table_name VARCHAR(200),
			change_note     TEXT,
			changed_by      VARCHAR(200),
			changed_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			parent_version  INTEGER DEFAULT 0,
			diff_info       TEXT,
			UNIQUE(product_id, version_number)
		)
	)";
	return executeSchemaSQL(sql, "创建version_records表");
}

bool SchemaManager::createDirectoryTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_directory (
			id              SERIAL PRIMARY KEY,
			parent_id       INTEGER DEFAULT 0,
			name            VARCHAR(300) NOT NULL,
			description     TEXT,
			sort_order      INTEGER DEFAULT 0,
			created_by      VARCHAR(200),
			created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			node_type       INTEGER DEFAULT 0
		)
	)";
	return executeSchemaSQL(sql, "创建product_directory表");
}

bool SchemaManager::createTagsTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_tags (
			id              SERIAL PRIMARY KEY,
			name            VARCHAR(200) NOT NULL UNIQUE,
			color           VARCHAR(20),
			description     TEXT,
			created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)";
	return executeSchemaSQL(sql, "创建product_tags表");
}

bool SchemaManager::createProductTagsTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_tag_mapping (
			product_id      INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE,
			tag_id          INTEGER NOT NULL REFERENCES product_tags(id) ON DELETE CASCADE,
			PRIMARY KEY (product_id, tag_id)
		)
	)";
	return executeSchemaSQL(sql, "创建product_tag_mapping表");
}

bool SchemaManager::createPermissionsTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS user_permissions (
			id              SERIAL PRIMARY KEY,
			user_name       VARCHAR(200) NOT NULL UNIQUE,
			role            VARCHAR(50) NOT NULL DEFAULT '数据操作员',
			password_hash   VARCHAR(128) NOT NULL DEFAULT '',
			permissions     TEXT,
			granted_by      VARCHAR(200),
			granted_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)";
	return executeSchemaSQL(sql, "创建user_permissions表");
}

void SchemaManager::migratePermissionsTable()
{
	// 兼容旧版本表结构：去掉 department 列，添加 password_hash 列
	QString dropDept = R"(ALTER TABLE user_permissions DROP COLUMN IF EXISTS department)";
	QString addPwd = R"(ALTER TABLE user_permissions ADD COLUMN IF NOT EXISTS password_hash VARCHAR(128) NOT NULL DEFAULT '')";

	executeSchemaSQL(dropDept, "迁移: 删除department列");
	executeSchemaSQL(addPwd, "迁移: 添加password_hash列");
}

bool SchemaManager::createAuditLogTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS audit_log (
			id              SERIAL PRIMARY KEY,
			product_id      INTEGER REFERENCES product_metadata(id) ON DELETE SET NULL,
			action          VARCHAR(100) NOT NULL,
			detail          TEXT,
			operator_name   VARCHAR(200),
			operator_ip     VARCHAR(50),
			created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)";
	return executeSchemaSQL(sql, "创建audit_log表");
}

bool SchemaManager::createLayerRegistryTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS layer_registry (
			id              SERIAL PRIMARY KEY,
			product_id      INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE,
			table_name      VARCHAR(200) NOT NULL UNIQUE,
			geometry_type   VARCHAR(100),
			geometry_srid   INTEGER DEFAULT 4490,
			feature_count   INTEGER DEFAULT 0,
			created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)";
	return executeSchemaSQL(sql, "创建layer_registry表");
}

// ===================== 数据类型专属元数据扩展表 =====================

bool SchemaManager::createVectorMetaTable()
{
	// 按「矢量数据元数据.csv」7项 定义
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_vector_meta (
			-- CSV 第 1~7 项：矢量专属业务元数据
			geom_type         VARCHAR(18),
			inv_scale         INT DEFAULT 0,
			cs_type           VARCHAR(10),
			geodetic_datum    VARCHAR(50),
			epsg_code         VARCHAR(10),
			proj_desc         VARCHAR(60),
			field_desc        VARCHAR(500),
			-- 系统内部字段
			id                SERIAL PRIMARY KEY,
			product_id        INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE UNIQUE,
			feature_count     BIGINT DEFAULT 0,
			field_count       INTEGER DEFAULT 0,
			layer_table_name  VARCHAR(200)
		)
	)";
	return executeSchemaSQL(sql, "创建product_vector_meta表（矢量数据元数据7项）");
}

bool SchemaManager::createRasterMetaTable()
{
	// 按「栅格数据元数据.csv」14项 定义
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_raster_meta (
			-- CSV 第 1~14 项：栅格专属业务元数据
			satellite_name    VARCHAR(50),
			sensor_type       VARCHAR(50),
			acquire_time      TIMESTAMP,
			gsd               FLOAT DEFAULT 0,
			resolution_unit   VARCHAR(10),
			color_type        VARCHAR(20),
			bit_depth         INT DEFAULT 0,
			band_count        INT DEFAULT 0,
			nodata_value      FLOAT DEFAULT 0,
			cs_type           VARCHAR(10),
			geodetic_datum    VARCHAR(50),
			epsg_code         VARCHAR(10),
			proj_desc         VARCHAR(60),
			planar_unit       VARCHAR(10),
			-- 系统内部字段
			id                SERIAL PRIMARY KEY,
			product_id        INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE UNIQUE,
			pixel_width       INT DEFAULT 0,
			pixel_height      INT DEFAULT 0,
			pixel_type        VARCHAR(50),
			layer_table_name  VARCHAR(200)
		)
	)";
	return executeSchemaSQL(sql, "创建product_raster_meta表（栅格数据元数据14项）");
}

bool SchemaManager::createDiagramMetaTable()
{
	// 按「制图成果数据元数据.csv」22项 定义
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_diagram_meta (
			-- CSV 第 1~22 项：制图成果专属业务元数据
			data_id           VARCHAR(32),
			map_series        VARCHAR(255),
			city_prefecture   VARCHAR(100),
			map_scale         INT DEFAULT 0,
			proj_desc         VARCHAR(60),
			production_date   DATE,
			approval_no       VARCHAR(100),
			carto_software    VARCHAR(100),
			format            VARCHAR(30),
			paper_size        VARCHAR(20),
			legend_included   BOOLEAN DEFAULT FALSE,
			modifier          VARCHAR(50),
			last_modified     DATE,
			print_ready       BOOLEAN DEFAULT FALSE,
			color_mode        VARCHAR(20),
			dpi               INT DEFAULT 0,
			proj_name         VARCHAR(200),
			raster_ids        VARCHAR(500),
			vector_ids        VARCHAR(500),
			end_datetime      TIMESTAMP,
			map_productor     VARCHAR(50),
			has_math_base     BOOLEAN DEFAULT FALSE,
			-- 系统内部字段
			id                SERIAL PRIMARY KEY,
			product_id        INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE UNIQUE
		)
	)";
	return executeSchemaSQL(sql, "创建product_diagram_meta表（制图成果数据元数据22项）");
}

bool SchemaManager::createOutputMetaTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_output_meta (
			id                   SERIAL PRIMARY KEY,
			product_id           INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE UNIQUE,
			page_count           INTEGER DEFAULT 0,
			color_mode           VARCHAR(20),
			embedded_fonts       TEXT,
			pdf_version          VARCHAR(20),
			interactive_features TEXT
		)
	)";
	return executeSchemaSQL(sql, "创建product_output_meta表");
}

bool SchemaManager::createDocumentMetaTable()
{
	// 按「文档数据元数据.csv」14项 定义
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS product_document_meta (
			-- CSV 第 1~14 项：文档专属业务元数据
			publisher         VARCHAR(100),
			file_type         VARCHAR(20),
			format            VARCHAR(30),
			file_size         VARCHAR(10),
			language_type     VARCHAR(20),
			key_words         VARCHAR(200),
			summary           VARCHAR(500),
			quality_issues    VARCHAR(500),
			collect_time      TIMESTAMP,
			end_datetime      TIMESTAMP,
			collector         VARCHAR(50),
			collect_purpose   VARCHAR(200),
			project_name      VARCHAR(200),
			is_compressed     BOOLEAN DEFAULT FALSE,
			-- 系统内部字段
			id                SERIAL PRIMARY KEY,
			product_id        INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE UNIQUE
		)
	)";
	return executeSchemaSQL(sql, "创建product_document_meta表（文档数据元数据14项）");
}

// ===================== 从宽表迁移到扩展表 =====================

bool SchemaManager::migrateVectorMetaFromWideTable()
{
	auto* db = PostgisConnector::instance();

	// 直接检查 INSERT 需要的列 geometry_type 是否存在（限定 current_schema 防止跨 schema 误判）
	auto geomTypeCheck = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_vector_meta' AND column_name='geometry_type'");
	if (geomTypeCheck.isEmpty())
	{
		// geometry_type 列不存在（V2 表或尚未创建），跳过宽表迁移
		QgsMessageLog::logMessage("product_vector_meta 无 geometry_type 列，跳过宽表迁移", "SchemaManager", Qgis::Info);
		return true;
	}

	// 旧表结构：从 product_metadata 宽表迁移到 product_vector_meta
	QString sql = R"(
		INSERT INTO product_vector_meta (product_id, geometry_type, layer_table_name)
		SELECT m.id, m.geometry_type, m.layer_table_name
		FROM product_metadata m
		WHERE m.product_type = '矢量数据'
		  AND m.geometry_type IS NOT NULL
		  AND NOT EXISTS (SELECT 1 FROM product_vector_meta v WHERE v.product_id = m.id)
	)";
	executeSchemaSQL(sql, "迁移矢量元数据到product_vector_meta");
	return true;
}

bool SchemaManager::migrateRasterMetaFromWideTable()
{
	auto* db = PostgisConnector::instance();

	// 直接检查 INSERT 需要的列 pixel_resolution 是否存在（限定 current_schema 防止跨 schema 误判）
	auto pixResCheck = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_raster_meta' AND column_name='pixel_resolution'");
	if (pixResCheck.isEmpty())
	{
		// pixel_resolution 列不存在（V2 表或尚未创建），跳过宽表迁移
		QgsMessageLog::logMessage("product_raster_meta 无 pixel_resolution 列，跳过宽表迁移", "SchemaManager", Qgis::Info);
		return true;
	}

	// 旧表结构：从 product_metadata 宽表迁移到 product_raster_meta
	QString sql = R"(
		INSERT INTO product_raster_meta (product_id, band_count, pixel_width, pixel_height, pixel_resolution, layer_table_name)
		SELECT m.id, m.band_count, m.pixel_width, m.pixel_height, m.pixel_resolution, m.layer_table_name
		FROM product_metadata m
		WHERE m.product_type = '栅格数据'
		  AND NOT EXISTS (SELECT 1 FROM product_raster_meta r WHERE r.product_id = m.id)
	)";
	executeSchemaSQL(sql, "迁移栅格元数据到product_raster_meta");
	return true;
}

// ===================== V2 专有元数据迁移（添加 CSV 新列，幂等） =====================

void SchemaManager::migrateVectorMetaToV2()
{
	// 检查新列是否已存在 → 跳过
	auto* db = PostgisConnector::instance();
	auto check = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_vector_meta' AND column_name='geom_type'");
	if (!check.isEmpty()) return;

	// 添加 CSV 新列（保留旧列以兼容）
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS geom_type VARCHAR(18)", "添加 geom_type");
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS inv_scale INT DEFAULT 0", "添加 inv_scale");
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS cs_type VARCHAR(10)", "添加 cs_type");
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS geodetic_datum VARCHAR(50)", "添加 geodetic_datum");
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS epsg_code VARCHAR(10)", "添加 epsg_code");
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS proj_desc VARCHAR(60)", "添加 proj_desc");
	executeSchemaSQL("ALTER TABLE product_vector_meta ADD COLUMN IF NOT EXISTS field_desc VARCHAR(500)", "添加 field_desc");

	// 从旧列迁数据：geometry_type → geom_type, field_details → field_desc
	executeSchemaSQL(
		"UPDATE product_vector_meta SET geom_type = geometry_type "
		"WHERE geometry_type IS NOT NULL AND geom_type IS NULL",
		"迁移 geometry_type → geom_type");
	executeSchemaSQL(
		"UPDATE product_vector_meta SET field_desc = LEFT(field_details::text, 500) "
		"WHERE field_details IS NOT NULL AND field_desc IS NULL",
		"迁移 field_details → field_desc");

	QgsMessageLog::logMessage("迁移 product_vector_meta → 矢量数据元数据7项 完成", "SchemaManager", Qgis::Info);
}

void SchemaManager::migrateRasterMetaToV2()
{
	auto* db = PostgisConnector::instance();
	auto check = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_raster_meta' AND column_name='satellite_name'");
	if (!check.isEmpty()) return;

	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS satellite_name VARCHAR(50)", "添加 satellite_name");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS sensor_type VARCHAR(50)", "添加 sensor_type");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS acquire_time TIMESTAMP", "添加 acquire_time");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS gsd FLOAT DEFAULT 0", "添加 gsd");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS resolution_unit VARCHAR(10)", "添加 resolution_unit");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS color_type VARCHAR(20)", "添加 color_type");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS nodata_value FLOAT DEFAULT 0", "添加 nodata_value");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS cs_type VARCHAR(10)", "添加 cs_type");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS geodetic_datum VARCHAR(50)", "添加 geodetic_datum");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS epsg_code VARCHAR(10)", "添加 epsg_code");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS proj_desc VARCHAR(60)", "添加 proj_desc");
	executeSchemaSQL("ALTER TABLE product_raster_meta ADD COLUMN IF NOT EXISTS planar_unit VARCHAR(10)", "添加 planar_unit");

	// 旧列迁移
	executeSchemaSQL(
		"UPDATE product_raster_meta SET gsd = pixel_resolution "
		"WHERE pixel_resolution IS NOT NULL AND pixel_resolution > 0 AND gsd = 0",
		"迁移 pixel_resolution → gsd");
	executeSchemaSQL(
		"UPDATE product_raster_meta SET color_type = color_space "
		"WHERE color_space IS NOT NULL AND color_type IS NULL",
		"迁移 color_space → color_type");
	executeSchemaSQL(
		"UPDATE product_raster_meta SET acquire_time = image_date::TIMESTAMP "
		"WHERE image_date IS NOT NULL AND acquire_time IS NULL "
		"AND image_date ~ '^\\d{4}-\\d{2}-\\d{2}'",
		"迁移 image_date → acquire_time");
	// nodata_value 类型从 VARCHAR → FLOAT
	executeSchemaSQL(
		"UPDATE product_raster_meta SET nodata_value = NULLIF(no_data_value, '')::FLOAT "
		"WHERE no_data_value IS NOT NULL AND no_data_value != '' AND nodata_value = 0",
		"迁移 no_data_value → nodata_value");

	QgsMessageLog::logMessage("迁移 product_raster_meta → 栅格数据元数据14项 完成", "SchemaManager", Qgis::Info);
}

void SchemaManager::migrateDiagramMetaToV2()
{
	auto* db = PostgisConnector::instance();
	auto check = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_diagram_meta' AND column_name='data_id'");
	if (!check.isEmpty()) return;

	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS data_id VARCHAR(32)", "添加 data_id");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS map_series VARCHAR(255)", "添加 map_series");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS city_prefecture VARCHAR(100)", "添加 city_prefecture");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS map_scale INT DEFAULT 0", "添加 map_scale");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS proj_desc VARCHAR(60)", "添加 proj_desc");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS production_date DATE", "添加 production_date");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS approval_no VARCHAR(100)", "添加 approval_no");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS carto_software VARCHAR(100)", "添加 carto_software");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS format VARCHAR(30)", "添加 format");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS paper_size VARCHAR(20)", "添加 paper_size");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS legend_included BOOLEAN DEFAULT FALSE", "添加 legend_included");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS modifier VARCHAR(50)", "添加 modifier");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS last_modified DATE", "添加 last_modified");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS print_ready BOOLEAN DEFAULT FALSE", "添加 print_ready");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS color_mode VARCHAR(20)", "添加 color_mode");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS dpi INT DEFAULT 0", "添加 dpi");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS proj_name VARCHAR(200)", "添加 proj_name");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS raster_ids VARCHAR(500)", "添加 raster_ids");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS vector_ids VARCHAR(500)", "添加 vector_ids");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS end_datetime TIMESTAMP", "添加 end_datetime");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS map_productor VARCHAR(50)", "添加 map_productor");
	executeSchemaSQL("ALTER TABLE product_diagram_meta ADD COLUMN IF NOT EXISTS has_math_base BOOLEAN DEFAULT FALSE", "添加 has_math_base");

	// 旧列迁数据
	executeSchemaSQL(
		"UPDATE product_diagram_meta SET paper_size = sheet_size "
		"WHERE sheet_size IS NOT NULL AND paper_size IS NULL",
		"迁移 sheet_size → paper_size");
	executeSchemaSQL(
		"UPDATE product_diagram_meta SET dpi = output_dpi "
		"WHERE output_dpi IS NOT NULL AND output_dpi > 0 AND dpi = 0",
		"迁移 output_dpi → dpi");

	QgsMessageLog::logMessage("迁移 product_diagram_meta → 制图成果数据元数据22项 完成", "SchemaManager", Qgis::Info);
}

void SchemaManager::migrateDocumentMetaToV2()
{
	auto* db = PostgisConnector::instance();
	auto check = db->executeQueryOne(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema=current_schema() AND table_name='product_document_meta' AND column_name='publisher'");
	if (!check.isEmpty()) return;

	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS publisher VARCHAR(100)", "添加 publisher");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS file_type VARCHAR(20)", "添加 file_type");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS format VARCHAR(30)", "添加 format");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS file_size VARCHAR(10)", "添加 file_size");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS language_type VARCHAR(20)", "添加 language_type");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS key_words VARCHAR(200)", "添加 key_words");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS summary VARCHAR(500)", "添加 summary");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS quality_issues VARCHAR(500)", "添加 quality_issues");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS collect_time TIMESTAMP", "添加 collect_time");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS end_datetime TIMESTAMP", "添加 end_datetime");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS collector VARCHAR(50)", "添加 collector");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS collect_purpose VARCHAR(200)", "添加 collect_purpose");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS project_name VARCHAR(200)", "添加 project_name");
	executeSchemaSQL("ALTER TABLE product_document_meta ADD COLUMN IF NOT EXISTS is_compressed BOOLEAN DEFAULT FALSE", "添加 is_compressed");

	// 旧列迁数据
	executeSchemaSQL(
		"UPDATE product_document_meta SET publisher = author "
		"WHERE author IS NOT NULL AND publisher IS NULL",
		"迁移 author → publisher");

	QgsMessageLog::logMessage("迁移 product_document_meta → 文档数据元数据14项 完成", "SchemaManager", Qgis::Info);
}

bool SchemaManager::createDataLayerTable(const QString& tableName, const QString& geometryType, int srid)
{
	// 根据几何类型创建 2D PostGIS 图层表
	// 始终使用 GEOMETRY(GEOMETRY, srid) — 仅 2D。
	// Z/M 维度在 INSERT 时由 ST_Force2D() 统一剥离，避免以下问题：
	//   - GDAL 图层类型声明含 Z/M 但个别要素不含 → "Column has X but geometry does not"
	//   - GDAL 图层类型声明不含 Z/M 但个别要素含 3D → "Geometry has X but column does not"
	// 不限制几何子类型（如 Point/Polygon），使用 GEOMETRY 通用父类型。
	// 原因：Shapefile/GDB 等格式常混合 Polygon/MultiPolygon 等子类型。
	Q_UNUSED(geometryType);
	QString sql = QString(
		"CREATE TABLE IF NOT EXISTS \"%1\" ("
		"  id SERIAL PRIMARY KEY,"
		"  geom GEOMETRY(GEOMETRY, %2)"
		")"
	).arg(tableName, QString::number(srid));

	return executeSchemaSQL(sql, QString("创建数据图层表 %1").arg(tableName));
}

bool SchemaManager::dropDataLayerTable(const QString& tableName)
{
	QString sql = QString("DROP TABLE IF EXISTS \"%1\" CASCADE").arg(tableName);
	return executeSchemaSQL(sql, QString("删除数据图层表 %1").arg(tableName));
}

bool SchemaManager::createIndexes()
{
	bool ok = true;

	ok = ok && executeSchemaSQL(
		"CREATE INDEX IF NOT EXISTS idx_product_metadata_geom ON product_metadata USING GIST(geom)",
		"创建空间索引");

	ok = ok && executeSchemaSQL(
		"CREATE INDEX IF NOT EXISTS idx_product_metadata_type ON product_metadata(product_type)",
		"创建产品类型索引");

	ok = ok && executeSchemaSQL(
		"CREATE INDEX IF NOT EXISTS idx_product_metadata_security ON product_metadata(security_level)",
		"创建密级索引");

	ok = ok && executeSchemaSQL(
		"CREATE INDEX IF NOT EXISTS idx_product_metadata_dir ON product_metadata(parent_dir_id)",
		"创建目录索引");

	ok = ok && executeSchemaSQL(
		"CREATE INDEX IF NOT EXISTS idx_version_records_product ON version_records(product_id)",
		"创建版本记录索引");

	// 全文检索索引（需要 pg_trgm 扩展，如果不存在则跳过不报错）
	if (!executeSchemaSQL(
		"CREATE INDEX IF NOT EXISTS idx_product_metadata_name_trgm ON product_metadata USING GIN(product_name gin_trgm_ops)",
		"创建全文检索索引"))
	{
	}

	return ok;
}

bool SchemaManager::createServiceUnitsTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS service_units (
			id              SERIAL PRIMARY KEY,
			name            VARCHAR(255) NOT NULL UNIQUE,
			created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)";
	return executeSchemaSQL(sql, "创建service_units表");
}

bool SchemaManager::createServiceDictTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS service_dict (
			id          SERIAL PRIMARY KEY,
			dict_type   VARCHAR(32) NOT NULL,   -- scenario/level/category/material
			name        VARCHAR(255) NOT NULL,
			created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			UNIQUE (dict_type, name)
		)
	)";
	return executeSchemaSQL(sql, "创建service_dict表");
}

bool SchemaManager::createServiceRecordsTable()
{
	QString sql = R"(
		CREATE TABLE IF NOT EXISTS service_records (
			id                      SERIAL PRIMARY KEY,
			service_number          VARCHAR(64) NOT NULL,
			record_date             VARCHAR(32),
			application_scenario    VARCHAR(64),
			service_content         TEXT,
			coverage_area           VARCHAR(512),
			service_level           VARCHAR(64),
			arranged_office         VARCHAR(256),
			category                VARCHAR(128),
			service_form            VARCHAR(256),
			other_service_form      VARCHAR(256),
			map_name                VARCHAR(512),
			mapping_region          VARCHAR(512),
			size                    VARCHAR(128),
			material                VARCHAR(128),
			compilation_details     TEXT,
			print_details           TEXT,
			mounting_details        TEXT,
			product_details         TEXT,
			data_details            TEXT,
			other_details           TEXT,
			sheet_count             INTEGER DEFAULT 0,
			unit_id                 INTEGER DEFAULT 0,
			unit_name               VARCHAR(255),
			arranged_leader         VARCHAR(128),
			funding                 NUMERIC(12, 2) DEFAULT 0,
			submit_time             VARCHAR(32),
			receiver                VARCHAR(256),
			status                  INTEGER DEFAULT 0,
			remarks                 TEXT,
			archive_path            VARCHAR(1024),
			archive_format          VARCHAR(64),
			product_ids             TEXT,
			product_names           TEXT,
			approval_file_path      VARCHAR(1024),
			approval_file_oid       INTEGER DEFAULT 0,          -- 审批单 BLOB OID（支持跨机器访问）
			created_by              VARCHAR(128) DEFAULT 'system',
			created_at              TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			updated_by              VARCHAR(128),
			updated_at              TIMESTAMP
		)
	)";
	return executeSchemaSQL(sql, "创建service_records表");
}

bool SchemaManager::migrateServiceRecordsTable()
{
	// 为已有数据库添加新列（如果不存在）
	bool ok = true;

	// 新版 UNV2 列
	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS service_level VARCHAR(64)",
		"迁移-添加service_level列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS arranged_office VARCHAR(256)",
		"迁移-添加arranged_office列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS other_service_form VARCHAR(256)",
		"迁移-添加other_service_form列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS map_name VARCHAR(512)",
		"迁移-添加map_name列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS mapping_region VARCHAR(512)",
		"迁移-添加mapping_region列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS size VARCHAR(128)",
		"迁移-添加size列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS material VARCHAR(128)",
		"迁移-添加material列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS compilation_details TEXT",
		"迁移-添加compilation_details列");

	// UNV3: 各服务形式Tab详情（JSON存储）
	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS print_details TEXT",
		"迁移-添加print_details列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS mounting_details TEXT",
		"迁移-添加mounting_details列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS product_details TEXT",
		"迁移-添加product_details列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS data_details TEXT",
		"迁移-添加data_details列");

	ok = ok && executeSchemaSQL(
		"ALTER TABLE service_records ADD COLUMN IF NOT EXISTS other_details TEXT",
		"迁移-添加other_details列");

	return ok;
}

QString SchemaManager::getCreateSchemaSQL()
{
	return R"(
-- ============================================
-- 地图成果管理系统 - 数据库Schema
-- 需要PostGIS扩展
-- ============================================

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- 成果元数据主表
CREATE TABLE product_metadata (
    id              SERIAL PRIMARY KEY,
    uuid            UUID NOT NULL DEFAULT uuid_generate_v4() UNIQUE,
    product_name    VARCHAR(500) NOT NULL,
    product_type    VARCHAR(50) NOT NULL,
    file_format     VARCHAR(50),
    file_path       TEXT,
    file_size       BIGINT DEFAULT 0,
    file_hash       VARCHAR(128),
    file_oid        OID,
    thumbnail_path  TEXT,
    geom            GEOMETRY(GEOMETRY, 4490),
    min_x           DOUBLE PRECISION,
    min_y           DOUBLE PRECISION,
    max_x           DOUBLE PRECISION,
    max_y           DOUBLE PRECISION,
    crs             VARCHAR(200),
    spatial_extent_wkt TEXT,
    geometry_type   TEXT,
    band_count      INTEGER,
    pixel_width     INTEGER,
    pixel_height    INTEGER,
    pixel_resolution DOUBLE PRECISION,
    scale           TEXT,
    compilation_info TEXT,
    approval_number VARCHAR(200),
    security_level  VARCHAR(50) DEFAULT '非密',
    producer        VARCHAR(500),
    production_date VARCHAR(50),
    description     TEXT,
    source          VARCHAR(200),
    city            VARCHAR(100),
    project_name    VARCHAR(200),
    delivery_status VARCHAR(30),
    delivery_time   TIMESTAMP,
    parent_dir_id   INTEGER DEFAULT 0,
    directory_path  TEXT,
    current_version INTEGER DEFAULT 1,
    version_note    TEXT,
    created_by      VARCHAR(200),
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_by      VARCHAR(200),
    updated_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 版本记录表
CREATE TABLE version_records (
    id              SERIAL PRIMARY KEY,
    product_id      INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE,
    version_number  INTEGER NOT NULL,
    file_path       TEXT,
    file_hash       VARCHAR(128),
    file_size       BIGINT DEFAULT 0,
    file_oid        OID,
    file_format     VARCHAR(50),
    layer_table_name VARCHAR(200),
    change_note     TEXT,
    changed_by      VARCHAR(200),
    changed_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    parent_version  INTEGER DEFAULT 0,
    diff_info       TEXT,
    UNIQUE(product_id, version_number)
);

-- 多级目录表
CREATE TABLE product_directory (
    id              SERIAL PRIMARY KEY,
    parent_id       INTEGER DEFAULT 0,
    name            VARCHAR(300) NOT NULL,
    description     TEXT,
    sort_order      INTEGER DEFAULT 0,
    created_by      VARCHAR(200),
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    node_type       INTEGER DEFAULT 0
);

-- 标签表
CREATE TABLE product_tags (
    id              SERIAL PRIMARY KEY,
    name            VARCHAR(200) NOT NULL UNIQUE,
    color           VARCHAR(20),
    description     TEXT,
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 产品-标签关联表
CREATE TABLE product_tag_mapping (
    product_id      INTEGER NOT NULL REFERENCES product_metadata(id) ON DELETE CASCADE,
    tag_id          INTEGER NOT NULL REFERENCES product_tags(id) ON DELETE CASCADE,
    PRIMARY KEY (product_id, tag_id)
);

-- 用户权限表
CREATE TABLE user_permissions (
    id              SERIAL PRIMARY KEY,
    user_name       VARCHAR(200) NOT NULL UNIQUE,
    role            VARCHAR(50) NOT NULL DEFAULT '数据入库员',
    password_hash   VARCHAR(128) NOT NULL DEFAULT '',
    permissions     TEXT,
    granted_by      VARCHAR(200),
    granted_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 审计日志表
CREATE TABLE audit_log (
    id              SERIAL PRIMARY KEY,
    product_id      INTEGER REFERENCES product_metadata(id) ON DELETE SET NULL,
    action          VARCHAR(100) NOT NULL,
    detail          TEXT,
    operator_name   VARCHAR(200),
    operator_ip     VARCHAR(50),
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 索引
CREATE INDEX idx_product_metadata_geom ON product_metadata USING GIST(geom);
CREATE INDEX idx_product_metadata_type ON product_metadata(product_type);
CREATE INDEX idx_product_metadata_security ON product_metadata(security_level);
CREATE INDEX idx_product_metadata_dir ON product_metadata(parent_dir_id);
CREATE INDEX idx_version_records_product ON version_records(product_id);

COMMENT ON TABLE product_metadata IS '地图成果元数据主表';
COMMENT ON TABLE version_records IS '版本记录表';
COMMENT ON TABLE product_directory IS '多级目录表';
COMMENT ON TABLE product_tags IS '标签表';
COMMENT ON TABLE product_tag_mapping IS '产品-标签关联表';
COMMENT ON TABLE user_permissions IS '用户权限表';
COMMENT ON TABLE audit_log IS '审计日志表';
)";
}
