#include "product_dao.h"
#include "postgis_connector.h"
#include "schema_manager.h"
#include "core/data_importer.h"
#include "core/metadata_extractor.h"
#include <qgis.h>
#include <QUuid>
#include <QDir>
#include <QRegularExpression>
#include <qgsmessagelog.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

ProductDAO::ProductDAO(QObject* parent)
	: QObject(parent)
{
}

// ===================== 产品CRUD =====================
int ProductDAO::insertProduct(const ProductMetadata& meta)
{
	auto* db = PostgisConnector::instance();

	QString sql = R"(
		INSERT INTO product_metadata (
			data_id, product_name, description, source, version_note,
			file_format, file_size, is_compressed,
			bounds, center_lon, center_lat,
			start_datetime, end_datetime, security_level,
			created_by, tags,
			city, scale, project_name, producer, production_date,
			crs, compilation_info, delivery_status, delivery_time,
			product_type, file_path, file_hash, file_oid, thumbnail_path,
			min_x, min_y, max_x, max_y, spatial_extent_wkt, geometry_type,
			band_count, pixel_width, pixel_height, pixel_resolution,
			approval_number, parent_dir_id, directory_path, updated_by,
			layer_table_name, current_version,
			created_at, updated_at
		) VALUES (
			$1, $2, $3, $4, $5,
			$6, $7, $8,
			$9, $10, $11,
			$12, $13, $14,
			$15, $16,
			$17, $18, $19, $20, $21,
			$22, $23, $24, $25,
			$26, $27, $28, $29, $30,
			$31, $32, $33, $34, $35, $36,
			$37, $38, $39, $40,
			$41, $42, $43, $44,
			$45, $46,
			CURRENT_TIMESTAMP, CURRENT_TIMESTAMP
		) RETURNING id
	)";

	QString dataId = meta.dataId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : meta.dataId;

	QVariantList params;
	params << dataId
		<< meta.productName << meta.description << meta.source << meta.versionNote
		<< meta.fileFormat << meta.fileSize << meta.isCompressed
		<< meta.bounds << meta.centerLon << meta.centerLat
		<< (meta.startDatetime.isValid() ? meta.startDatetime : QVariant())
		<< (meta.endDatetime.isValid() ? meta.endDatetime : QVariant())
		<< securityLevelToString(meta.securityLevel)
		<< meta.createdBy << meta.tags
		<< meta.city << meta.scale << meta.projectName << meta.producer
		<< (meta.productionDate.isValid() ? meta.productionDate : QVariant())
		<< meta.crs << meta.compilationInfo << meta.deliveryStatus
		<< (meta.deliveryTime.isValid() ? meta.deliveryTime : QVariant())
		<< productTypeToString(meta.productType) << meta.filePath << meta.fileHash
		<< meta.fileOid << meta.thumbnailPath
		<< meta.minX << meta.minY << meta.maxX << meta.maxY
		<< meta.spatialExtentWKT << meta.geometryType
		<< meta.bandCount << meta.pixelWidth << meta.pixelHeight << meta.pixelResolution
		<< meta.approvalNumber << meta.parentDirId << meta.directoryPath << meta.updatedBy
		<< meta.layerTableName << meta.currentVersion;

	auto result = db->executeQueryOne(sql, params);
	int newId = result.value("id", "-1").toString().toInt();

	if (newId > 0)
	{
		// 同时更新空间几何字段
		if (!meta.spatialExtentWKT.isEmpty())
		{
			QString geomSql = QString(
				"UPDATE product_metadata SET geom = ST_GeomFromText('%1', 4490) WHERE id = %2"
			).arg(meta.spatialExtentWKT).arg(newId);
			db->executeNonQuery(geomSql);
		}

		// 同步标签（product_tags 表 + 内联 tags 列）
		if (!meta.tags.isEmpty())
		{
			setProductTags(newId, meta.tags.split(QLatin1Char(';'), QString::SkipEmptyParts));
		}

		// 创建初始版本记录（版本1），确保版本历史至少有一条记录
		VersionRecord initialVer;
		initialVer.productId = newId;
		initialVer.versionNumber = meta.currentVersion;  // 默认为 1
		initialVer.filePath = meta.filePath;
		initialVer.fileHash = meta.fileHash;
		initialVer.fileSize = meta.fileSize;
		initialVer.fileOid = meta.fileOid;
		initialVer.fileFormat = meta.fileFormat;
		initialVer.layerTableName = meta.layerTableName;
		initialVer.changeNote = QString::fromUtf8("初始版本");
		initialVer.changedBy = meta.createdBy;
		initialVer.parentVersion = 0;  // 根版本，无父版本
		insertVersionRecord(initialVer);

		// 写审计日志
		writeAuditLog(newId, "CREATE", "创建新产品成果", meta.createdBy);

		emit operationCompleted("产品创建成功");
	}
	else
	{
		emit operationFailed("产品创建", db->lastError());
	}

	return newId;
}

bool ProductDAO::updateProduct(const ProductMetadata& meta)
{
	auto* db = PostgisConnector::instance();

	// 更新前查询旧的文件哈希，用于判断是否需要创建新版本
	QString oldHash;
	auto oldRow = db->executeQueryOne("SELECT file_hash, current_version FROM product_metadata WHERE id = $1", {meta.id});
	if (!oldRow.isEmpty())
	{
		oldHash = oldRow.value("file_hash").toString();
	}

	// 文件哈希发生变化时，计算新版本号
	int newVersion = oldRow.value("current_version", "1").toString().toInt();
	bool hashChanged = (!meta.fileHash.isEmpty() && meta.fileHash != oldHash);
	if (hashChanged)
	{
		newVersion = newVersion + 1;
	}

	QString sql = R"(
		UPDATE product_metadata SET
			product_name = $1,
			description = $2,
			source = $3,
			version_note = $4,
			file_format = $5,
			file_size = $6,
			file_hash = $7,
			thumbnail_path = $8,
			is_compressed = $9,
			bounds = $10,
			center_lon = $11,
			center_lat = $12,
			start_datetime = $13,
			end_datetime = $14,
			security_level = $15,
			tags = $16,
			city = $17,
			scale = $18,
			project_name = $19,
			producer = $20,
			production_date = $21,
			crs = $22,
			compilation_info = $23,
			delivery_status = $24,
			delivery_time = $25,
			product_type = $26,
			file_path = $27,
			approval_number = $28,
			parent_dir_id = $29,
			directory_path = $30,
			updated_by = $31,
			current_version = $32,
			updated_at = CURRENT_TIMESTAMP
		WHERE id = $33
	)";

	QVariantList params;
	params << meta.productName
		<< meta.description << meta.source << meta.versionNote
		<< meta.fileFormat << meta.fileSize << meta.fileHash << meta.thumbnailPath
		<< meta.isCompressed << meta.bounds << meta.centerLon << meta.centerLat
		<< (meta.startDatetime.isValid() ? meta.startDatetime : QVariant())
		<< (meta.endDatetime.isValid() ? meta.endDatetime : QVariant())
		<< securityLevelToString(meta.securityLevel)
		<< meta.tags
		<< meta.city << meta.scale << meta.projectName << meta.producer
		<< (meta.productionDate.isValid() ? meta.productionDate : QVariant())
		<< meta.crs << meta.compilationInfo << meta.deliveryStatus
		<< (meta.deliveryTime.isValid() ? meta.deliveryTime : QVariant())
		<< productTypeToString(meta.productType)
		<< meta.filePath << meta.approvalNumber << meta.parentDirId << meta.directoryPath
		<< meta.updatedBy << newVersion
		<< meta.id;

	bool ok = db->executeNonQuery(sql, params);

	if (ok)
	{
		// 补写 file_oid（仅对老产品 file_oid 为空/0 时写入，不影响已有值）
		if (meta.fileOid > 0)
		{
			db->executeNonQuery(
				"UPDATE product_metadata SET file_oid = $1 WHERE id = $2 AND (file_oid IS NULL OR file_oid <= 0)",
				{meta.fileOid, meta.id}
			);
		}

		// 更新空间几何
		if (!meta.spatialExtentWKT.isEmpty())
		{
			QString geomSql = QString(
				"UPDATE product_metadata SET geom = ST_GeomFromText('%1', 4490) WHERE id = %2"
			).arg(meta.spatialExtentWKT).arg(meta.id);
			db->executeNonQuery(geomSql);
		}

		// 更新标签
		setProductTags(meta.id, meta.tags.split(QLatin1Char(';'), QString::SkipEmptyParts));

		// 文件哈希发生变化时，自动创建新版本记录
		if (hashChanged)
		{
			// newVersion 已在上面计算好（oldVersion + 1）
			VersionRecord verRec;
			verRec.productId = meta.id;
			verRec.versionNumber = newVersion;
			verRec.filePath = meta.filePath;
			verRec.fileHash = meta.fileHash;
			verRec.fileSize = meta.fileSize;
			verRec.fileOid = meta.fileOid;
			verRec.fileFormat = meta.fileFormat;
			verRec.layerTableName = meta.layerTableName;
			verRec.changeNote = meta.versionNote.isEmpty()
				? QString("更新至版本 %1").arg(newVersion) : meta.versionNote;
			verRec.changedBy = meta.updatedBy;
			verRec.parentVersion = newVersion - 1;
			insertVersionRecord(verRec);

			writeAuditLog(meta.id, "UPDATE",
				QString("更新产品元数据（文件哈希变更，自动创建版本 %1）").arg(newVersion), meta.updatedBy);
		}
		else
		{
			writeAuditLog(meta.id, "UPDATE", "更新产品元数据", meta.updatedBy);
		}

		emit operationCompleted("产品更新成功");
	}
	else
	{
		emit operationFailed("产品更新", db->lastError());
	}

	return ok;
}

bool ProductDAO::deleteProduct(int productId)
{
	auto* db = PostgisConnector::instance();
	writeAuditLog(productId, "DELETE", "删除产品", "");

	// 先删除关联的图层表
	unregisterLayer(productId);

	bool ok = db->executeNonQuery(
		"DELETE FROM product_metadata WHERE id = $1",
		{productId}
	);

	if (ok)
		emit operationCompleted("产品删除成功");
	else
		emit operationFailed("产品删除", db->lastError());

	return ok;
}

ProductMetadata ProductDAO::getProduct(int productId)
{
	auto* db = PostgisConnector::instance();
	QString sql = R"(
		SELECT *, ST_AsText(geom) AS geom_wkt
		FROM product_metadata WHERE id = $1
	)";
	auto row = db->executeQueryOne(sql, {productId});
	if (row.isEmpty()) return ProductMetadata();

	auto meta = ProductMetadata::fromVariantMap(row);
	meta.spatialExtentWKT = row.value("geom_wkt").toString();
	meta.tags = getProductTags(productId).join(QLatin1Char(';'));
	return meta;
}

ProductMetadata ProductDAO::getProductByUUID(const QString& uuid)
{
	auto* db = PostgisConnector::instance();
	auto row = db->executeQueryOne(
		"SELECT * FROM product_metadata WHERE uuid = $1",
		{uuid}
	);
	if (row.isEmpty()) return ProductMetadata();

	auto meta = ProductMetadata::fromVariantMap(row);
	meta.tags = getProductTags(meta.id).join(QLatin1Char(';'));
	return meta;
}

QList<ProductMetadata> ProductDAO::getAllProducts()
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery("SELECT * FROM product_metadata ORDER BY created_at DESC");

	QList<ProductMetadata> results;
	for (const auto& row : rows)
	{
		auto meta = ProductMetadata::fromVariantMap(row.toMap());
		meta.tags = getProductTags(meta.id).join(QLatin1Char(';'));
		results.append(meta);
	}
	return results;
}

QList<ProductMetadata> ProductDAO::getProductsByType(ProductType type)
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery(
		"SELECT * FROM product_metadata WHERE product_type = $1 ORDER BY created_at DESC",
		{productTypeToString(type)}
	);

	QList<ProductMetadata> results;
	for (const auto& row : rows)
	{
		auto meta = ProductMetadata::fromVariantMap(row.toMap());
		meta.tags = getProductTags(meta.id).join(QLatin1Char(';'));
		results.append(meta);
	}
	return results;
}

QList<ProductMetadata> ProductDAO::getProductsByDirectory(int dirId)
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery(
		"SELECT * FROM product_metadata WHERE parent_dir_id = $1 ORDER BY created_at DESC",
		{dirId}
	);

	QList<ProductMetadata> results;
	for (const auto& row : rows)
	{
		auto meta = ProductMetadata::fromVariantMap(row.toMap());
		meta.tags = getProductTags(meta.id).join(QLatin1Char(';'));
		results.append(meta);
	}
	return results;
}

QList<ProductMetadata> ProductDAO::getProductsBySecurityLevel(SecurityLevel level)
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery(
		"SELECT * FROM product_metadata WHERE security_level = $1 ORDER BY created_at DESC",
		{securityLevelToString(level)}
	);

	QList<ProductMetadata> results;
	for (const auto& row : rows)
	{
		auto meta = ProductMetadata::fromVariantMap(row.toMap());
		meta.tags = getProductTags(meta.id).join(QLatin1Char(';'));
		results.append(meta);
	}
	return results;
}

// ===================== 多条件检索 =====================
QString ProductDAO::buildSearchSQL(const SearchCondition& cond, bool countOnly)
{
	QString select = countOnly ? "SELECT COUNT(*) AS total" : "SELECT *";
	QString sql = select + " FROM product_metadata WHERE 1=1";

	if (!cond.keyword.isEmpty())
	{
		sql += " AND (product_name ILIKE $kw OR description ILIKE $kw)";
	}
	if (cond.productType != ProductType::Other)
	{
		sql += " AND product_type = $ptype";
	}
	if (!cond.scale.isEmpty())
	{
		sql += " AND scale = $scale";
	}
	// securityLevel: 用特殊值标记"不限"（在参数构建中处理）
	if (!cond.producer.isEmpty())
	{
		sql += " AND producer ILIKE $producer";
	}
	if (!cond.approvalNumber.isEmpty())
	{
		sql += " AND approval_number ILIKE $approval";
	}
	if (!cond.dateFrom.isEmpty())
	{
		sql += " AND created_at >= $datefrom";
	}
	if (!cond.dateTo.isEmpty())
	{
		sql += " AND created_at <= $dateto";
	}
	if (!cond.directoryIds.isEmpty())
	{
		QStringList idStrs;
		for (int id : cond.directoryIds) idStrs << QString::number(id);
		sql += QString(" AND parent_dir_id IN (%1)").arg(idStrs.join(", "));
	}
	else if (cond.directoryId >= 0)
	{
		sql += " AND parent_dir_id = $dirid";
	}
	if (cond.hasSpatialFilter)
	{
		sql += QString(" AND geom && ST_MakeEnvelope(%1, %2, %3, %4, 4490)")
			.arg(cond.minX).arg(cond.minY).arg(cond.maxX).arg(cond.maxY);
	}
	if (!countOnly)
	{
		sql += " ORDER BY created_at DESC LIMIT $limit OFFSET $offset";
	}

	return sql;
}

QVariantList ProductDAO::buildSearchParams(const SearchCondition& cond)
{
	QVariantList params;
	// 注意：参数顺序必须与buildSearchSQL中的占位符顺序一致
	// 这里使用$1, $2...的参数编号方式，按条件逐一添加
	if (!cond.keyword.isEmpty())
		params << QVariant(QStringLiteral("%%1%").arg(cond.keyword));
	if (cond.productType != ProductType::Other)
		params << productTypeToString(cond.productType);
	if (!cond.scale.isEmpty())
		params << cond.scale;
	// securityLevel handled separately
	if (!cond.producer.isEmpty())
		params << QVariant(QStringLiteral("%%1%").arg(cond.producer));
	if (!cond.approvalNumber.isEmpty())
		params << QVariant(QStringLiteral("%%1%").arg(cond.approvalNumber));
	if (!cond.dateFrom.isEmpty())
		params << cond.dateFrom;
	if (!cond.dateTo.isEmpty())
		params << cond.dateTo;
	if (cond.directoryIds.isEmpty() && cond.directoryId >= 0)
		params << cond.directoryId;
	if (!cond.hasSpatialFilter)
	{
		params << cond.limit << cond.offset;
	}
	return params;
}

QList<ProductMetadata> ProductDAO::searchProducts(const SearchCondition& cond)
{
	auto* db = PostgisConnector::instance();

	// 构建带编号参数的SQL
	QString sql = "SELECT * FROM product_metadata WHERE 1=1";
	QVariantList params;
	int paramIdx = 1;

	if (!cond.keyword.isEmpty())
	{
		sql += QString(" AND (product_name ILIKE $%1 OR description ILIKE $%1"
					   " OR producer ILIKE $%1 OR approval_number ILIKE $%1"
					   " OR compilation_info ILIKE $%1 OR scale ILIKE $%1"
					   " OR file_format ILIKE $%1)").arg(paramIdx++);
		// 使用 QString::fromLatin1 确保百分号正确转义
		QString likePattern = QString::fromLatin1("%%1%").arg(cond.keyword);
		params << QVariant(likePattern);
	}
	if (!cond.productTypes.isEmpty())
	{
		QStringList placeholders;
		for (const auto& pt : cond.productTypes)
		{
			placeholders << QString("$%1").arg(paramIdx++);
			params << productTypeToString(pt);
		}
		sql += QString(" AND product_type IN (%1)").arg(placeholders.join(","));
	}
	else if (cond.productType != ProductType::Other)
	{
		sql += QString(" AND product_type = $%1").arg(paramIdx++);
		params << productTypeToString(cond.productType);
	}
	if (!cond.scale.isEmpty())
	{
		sql += QString(" AND scale = $%1").arg(paramIdx++);
		params << cond.scale;
	}
	// securityLevel as integer enum, skip "不限"
	if (!cond.producer.isEmpty())
	{
		sql += QString(" AND producer ILIKE $%1").arg(paramIdx++);
		params << QVariant(QStringLiteral("%%1%").arg(cond.producer));
	}
	if (!cond.approvalNumber.isEmpty())
	{
		sql += QString(" AND approval_number ILIKE $%1").arg(paramIdx++);
		params << QVariant(QStringLiteral("%%1%").arg(cond.approvalNumber));
	}
	if (!cond.dateFrom.isEmpty())
	{
		sql += QString(" AND created_at >= $%1").arg(paramIdx++);
		params << cond.dateFrom;
	}
	if (!cond.dateTo.isEmpty())
	{
		sql += QString(" AND created_at <= $%1").arg(paramIdx++);
		params << cond.dateTo;
	}
	if (!cond.directoryIds.isEmpty())
	{
		QStringList idStrs;
		for (int id : cond.directoryIds) idStrs << QString::number(id);
		sql += QString(" AND parent_dir_id IN (%1)").arg(idStrs.join(", "));
	}
	else if (cond.directoryId >= 0)
	{
		sql += QString(" AND parent_dir_id = $%1").arg(paramIdx++);
		params << cond.directoryId;
	}
	if (cond.hasSpatialFilter)
	{
		sql += QString(" AND geom && ST_MakeEnvelope($%1, $%2, $%3, $%4, 4490)")
			.arg(paramIdx).arg(paramIdx+1).arg(paramIdx+2).arg(paramIdx+3);
		paramIdx += 4;
		params << cond.minX << cond.minY << cond.maxX << cond.maxY;
	}

	// 标签筛选
	if (!cond.tags.isEmpty())
	{
		QStringList tagPlaceholders;
		for (const auto& tag : cond.tags)
		{
			tagPlaceholders << QString("$%1").arg(paramIdx++);
			params << tag;
		}
		sql += QString(" AND id IN (SELECT product_id FROM product_tag_mapping WHERE tag_id IN "
					   "(SELECT id FROM product_tags WHERE name IN (%1)))")
			.arg(tagPlaceholders.join(","));
	}

	sql += QString(" ORDER BY created_at DESC LIMIT $%1 OFFSET $%2").arg(paramIdx).arg(paramIdx+1);
	params << cond.limit << cond.offset;

	auto rows = db->executeQuery(sql, params);

	QList<ProductMetadata> results;
	for (const auto& row : rows)
	{
		auto meta = ProductMetadata::fromVariantMap(row.toMap());
		meta.tags = getProductTags(meta.id).join(QLatin1Char(';'));
		results.append(meta);
	}
	return results;
}

int ProductDAO::countSearchResults(const SearchCondition& cond)
{
	auto* db = PostgisConnector::instance();

	QString sql = "SELECT COUNT(*) AS total FROM product_metadata WHERE 1=1";
	QVariantList params;
	int paramIdx = 1;

	if (!cond.keyword.isEmpty())
	{
		sql += QString(" AND (product_name ILIKE $%1 OR description ILIKE $%1"
					   " OR producer ILIKE $%1 OR approval_number ILIKE $%1"
					   " OR compilation_info ILIKE $%1 OR scale ILIKE $%1"
					   " OR file_format ILIKE $%1)").arg(paramIdx++);
		// 使用 QString::fromLatin1 确保百分号正确转义
		QString likePattern = QString::fromLatin1("%%1%").arg(cond.keyword);
		params << QVariant(likePattern);
	}
	if (!cond.productTypes.isEmpty())
	{
		QStringList placeholders;
		for (const auto& pt : cond.productTypes)
		{
			placeholders << QString("$%1").arg(paramIdx++);
			params << productTypeToString(pt);
		}
		sql += QString(" AND product_type IN (%1)").arg(placeholders.join(","));
	}
	else if (cond.productType != ProductType::Other)
	{
		sql += QString(" AND product_type = $%1").arg(paramIdx++);
		params << productTypeToString(cond.productType);
	}
	if (!cond.scale.isEmpty())
	{
		sql += QString(" AND scale = $%1").arg(paramIdx++);
		params << cond.scale;
	}
	if (!cond.producer.isEmpty())
	{
		sql += QString(" AND producer ILIKE $%1").arg(paramIdx++);
		params << QVariant(QStringLiteral("%%1%").arg(cond.producer));
	}
	if (!cond.approvalNumber.isEmpty())
	{
		sql += QString(" AND approval_number ILIKE $%1").arg(paramIdx++);
		params << QVariant(QStringLiteral("%%1%").arg(cond.approvalNumber));
	}
	if (!cond.dateFrom.isEmpty())
	{
		sql += QString(" AND created_at >= $%1").arg(paramIdx++);
		params << cond.dateFrom;
	}
	if (!cond.dateTo.isEmpty())
	{
		sql += QString(" AND created_at <= $%1").arg(paramIdx++);
		params << cond.dateTo;
	}
	if (!cond.directoryIds.isEmpty())
	{
		QStringList idStrs;
		for (int id : cond.directoryIds) idStrs << QString::number(id);
		sql += QString(" AND parent_dir_id IN (%1)").arg(idStrs.join(", "));
	}
	else if (cond.directoryId >= 0)
	{
		sql += QString(" AND parent_dir_id = $%1").arg(paramIdx++);
		params << cond.directoryId;
	}
	if (cond.hasSpatialFilter)
	{
		sql += QString(" AND geom && ST_MakeEnvelope($%1, $%2, $%3, $%4, 4490)")
			.arg(paramIdx).arg(paramIdx+1).arg(paramIdx+2).arg(paramIdx+3);
		paramIdx += 4;
		params << cond.minX << cond.minY << cond.maxX << cond.maxY;
	}

	auto result = db->executeQueryOne(sql, params);
	return result.value("total", "0").toString().toInt();
}

ProductMetadata ProductDAO::findByHash(const QString& hash)
{
	if (hash.isEmpty())
		return ProductMetadata();

	auto* db = PostgisConnector::instance();
	QString sql = "SELECT * FROM product_metadata WHERE file_hash = $1 LIMIT 1";
	QVariantList params;
	params << hash;

	auto row = db->executeQueryOne(sql, params);
	if (row.isEmpty())
		return ProductMetadata();

	return ProductMetadata::fromVariantMap(row);
}

// ===================== 版本管理 =====================
int ProductDAO::insertVersionRecord(const VersionRecord& record)
{
	auto* db = PostgisConnector::instance();

	QString sql = R"(
		INSERT INTO version_records (
			product_id, version_number, file_path, file_hash,
			file_size, file_oid, file_format, layer_table_name,
			change_note, changed_by, changed_at,
			parent_version, diff_info
		) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, CURRENT_TIMESTAMP, $11, $12)
		ON CONFLICT (product_id, version_number) DO UPDATE SET
			file_path = $3, file_hash = $4, file_size = $5, file_oid = $6,
			file_format = $7, layer_table_name = $8,
			change_note = $9, changed_by = $10, changed_at = CURRENT_TIMESTAMP,
			parent_version = $11, diff_info = $12
		RETURNING id
	)";

	QVariantList params;
	params << record.productId
		   << record.versionNumber
		   << record.filePath
		   << record.fileHash
		   << record.fileSize
		   << record.fileOid
		   << record.fileFormat
		   << record.layerTableName
		   << record.changeNote
		   << record.changedBy
		   << record.parentVersion
		   << record.diffInfo;

	auto result = db->executeQueryOne(sql, params);
	int newId = result.value("id", "-1").toString().toInt();

	if (newId > 0)
	{
		// 更新产品主表的当前版本号，同时同步 file_format 和 layer_table_name
		db->executeNonQuery(
			"UPDATE product_metadata SET current_version = $1, file_format = $2, "
			"layer_table_name = $3, updated_at = CURRENT_TIMESTAMP WHERE id = $4",
			{record.versionNumber, record.fileFormat, record.layerTableName, record.productId}
		);

		writeAuditLog(record.productId, "VERSION",
			QString("新增版本 %1").arg(record.versionNumber), record.changedBy);

		emit operationCompleted(QString("版本 %1 创建成功").arg(record.versionNumber));
	}

	return newId;
}

QList<VersionRecord> ProductDAO::getVersionHistory(int productId)
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery(
		"SELECT * FROM version_records WHERE product_id = $1 ORDER BY version_number DESC",
		{productId}
	);

	QList<VersionRecord> results;
	for (const auto& row : rows)
	{
		results.append(VersionRecord::fromVariantMap(row.toMap()));
	}
	return results;
}

VersionRecord ProductDAO::getVersionRecord(int productId, int versionNumber)
{
	auto* db = PostgisConnector::instance();
	auto row = db->executeQueryOne(
		"SELECT * FROM version_records WHERE product_id = $1 AND version_number = $2",
		{productId, versionNumber}
	);
	return VersionRecord::fromVariantMap(row);
}

bool ProductDAO::rollbackToVersion(int productId, int versionNumber, const QString& operatorName)
{
	auto* db = PostgisConnector::instance();
	auto targetVersion = getVersionRecord(productId, versionNumber);

	if (targetVersion.id < 0) return false;

	// 获取当前产品元数据（需要 uuid 等信息）
	auto productMeta = getProduct(productId);
	if (productMeta.id < 0)
	{
		writeAuditLog(productId, "ROLLBACK_FAIL",
			QString("回退失败：无法获取产品元数据"), operatorName);
		return false;
	}

	// 获取当前最大版本号
	auto maxVer = db->executeQueryOne(
		"SELECT MAX(version_number) AS max_ver FROM version_records WHERE product_id = $1",
		{productId}
	);
	int maxVersion = maxVer.value("max_ver", "0").toString().toInt();

	// 获取当前最新版本的哈希，与目标版本比较
	auto latestVersion = getVersionRecord(productId, maxVersion);
	if (latestVersion.id > 0 && latestVersion.fileHash == targetVersion.fileHash)
	{
		// 哈希相同，文件内容未变，无需回退
		writeAuditLog(productId, "ROLLBACK_SKIP",
			QString("跳过回退：当前版本 %1 与目标版本 %2 文件哈希相同").arg(maxVersion).arg(versionNumber),
			operatorName);
		return false;
	}

	int newVersionNumber = maxVersion + 1;
	QString newLayerTableName = targetVersion.layerTableName;
	QString restoredFilePath = targetVersion.filePath;

	// ========== 根据数据类型恢复实际文件内容 ==========
	switch (productMeta.productType)
	{
	case ProductType::Raster:
	{
		// 栅格数据：BLOB (OID) 在 pg_largeobject 中不变，直接复用
		// 从 BLOB 导出旧版本文件供后续使用
		restoredFilePath = restoreLocalFile(productMeta.dataId, versionNumber,
			newVersionNumber, targetVersion.filePath);
		if (restoredFilePath.isEmpty() && !targetVersion.filePath.isEmpty())
		{
			restoredFilePath = targetVersion.filePath;
		}
		break;
	}

	case ProductType::Vector:
	{
		// 矢量数据：PostGIS 表需要复制一份，避免回退后新版本与目标版本共享同一张表
		// 如果后续修改回退后的版本，会影响目标版本的数据

		// 先恢复本地文件副本（Shapefile 等源文件）
		restoredFilePath = restoreLocalFile(productMeta.dataId, versionNumber,
			newVersionNumber, targetVersion.filePath);
		if (restoredFilePath.isEmpty() && !targetVersion.filePath.isEmpty())
		{
			restoredFilePath = targetVersion.filePath;
		}

		if (!targetVersion.layerTableName.isEmpty())
		{
			// 为新版本生成独立的图层表名：去掉已有的 _vN 后缀，追加新版本号
			QString baseTableName = targetVersion.layerTableName;
			baseTableName.replace(QRegularExpression("_v\\d+$"), "");
			QString clonedTableName = QString("%1_v%2")
				.arg(baseTableName)
				.arg(newVersionNumber);

			// 优先从本地恢复的 SHP 文件重新导入，确保数据是目标版本的原始数据
			// 直接 clone PostGIS 表可能拿到被后续版本覆盖后的数据
			bool importOk = false;
			int featureCount = 0;

			if (!restoredFilePath.isEmpty() && restoredFilePath.endsWith(".shp", Qt::CaseInsensitive))
			{
				DataImporter importer;
				featureCount = importer.importVectorToPostGIS(
					restoredFilePath, clonedTableName, 0, 4490, "UTF-8");

				if (featureCount >= 0)
				{
					importOk = true;
					newLayerTableName = clonedTableName;

					registerLayer(productId, clonedTableName,
						"GEOMETRY", 4490, featureCount);
				}
				else
				{}
			}

			if (!importOk)
			{
				// 本地文件不可用，尝试 clone PostGIS 表
				if (cloneVectorTable(targetVersion.layerTableName, clonedTableName))
				{
					newLayerTableName = clonedTableName;

					// 注册新图层表
					auto layerInfo = db->executeQueryOne(
						"SELECT geometry_type, geometry_srid, feature_count FROM layer_registry WHERE table_name = $1",
						{targetVersion.layerTableName});
					if (!layerInfo.isEmpty())
					{
						registerLayer(productId, clonedTableName,
							layerInfo.value("geometry_type").toString(),
							layerInfo.value("geometry_srid", "4490").toString().toInt(),
							layerInfo.value("feature_count", "0").toString().toInt());
					}
				}
			}
		}
		break;
	}

	case ProductType::PDF:
	case ProductType::CAD:
	case ProductType::AI:
	case ProductType::CDR:
	{
		// 仅本地存储的类型：必须从旧版本目录恢复文件
		restoredFilePath = restoreLocalFile(productMeta.dataId, versionNumber,
			newVersionNumber, targetVersion.filePath);
		if (restoredFilePath.isEmpty())
		{
			writeAuditLog(productId, "ROLLBACK_FAIL",
				QString("回退失败：无法恢复本地文件 (targetVer=%1, path=%2)")
					.arg(versionNumber).arg(targetVersion.filePath),
				operatorName);
			return false;
		}
		break;
	}

	case ProductType::Other:
	{
		// 其他类型也尝试恢复本地文件
		restoredFilePath = restoreLocalFile(productMeta.dataId, versionNumber,
			newVersionNumber, targetVersion.filePath);
		if (restoredFilePath.isEmpty() && !targetVersion.filePath.isEmpty())
		{
			restoredFilePath = targetVersion.filePath;
		}
		break;
	}
	}

	// ========== 创建新版本记录 ==========
	VersionRecord newRecord;
	newRecord.productId = productId;
	newRecord.versionNumber = newVersionNumber;
	newRecord.filePath = restoredFilePath;            // 恢复后的本地文件路径
	newRecord.fileHash = targetVersion.fileHash;
	newRecord.fileSize = targetVersion.fileSize;
	newRecord.fileOid = targetVersion.fileOid;        // 复用目标版本 OID（BLOB 不变）
	newRecord.fileFormat = targetVersion.fileFormat;
	newRecord.layerTableName = newLayerTableName;     // 矢量：克隆后的新表名；其他：同目标版本
	newRecord.changeNote = QString("回退至版本 %1").arg(versionNumber);
	newRecord.changedBy = operatorName;
	newRecord.parentVersion = versionNumber;

	int newId = insertVersionRecord(newRecord);

	if (newId > 0)
	{
		// 同步更新 product_metadata 表
		bool updateOk = db->executeNonQuery(
			"UPDATE product_metadata SET file_path = $1, file_hash = $2, file_size = $3, "
			"file_oid = $4, file_format = $5, layer_table_name = $6, "
			"current_version = $9, updated_by = $7, updated_at = CURRENT_TIMESTAMP WHERE id = $8",
			{restoredFilePath, targetVersion.fileHash, targetVersion.fileSize,
			 targetVersion.fileOid, targetVersion.fileFormat, newLayerTableName,
			 operatorName, productId, newVersionNumber});

		if (!updateOk)
		{
			// 回滚：删除刚创建的版本记录和克隆表
			db->executeNonQuery("DELETE FROM version_records WHERE id = $1", {newId});
			if (newLayerTableName != targetVersion.layerTableName && !newLayerTableName.isEmpty())
			{
				SchemaManager schemaMgr;
				schemaMgr.dropDataLayerTable(newLayerTableName);
			}

			writeAuditLog(productId, "ROLLBACK_FAIL",
				QString("回退失败：更新产品元数据失败 - %1").arg(db->lastError()),
				operatorName);
			return false;
		}

		writeAuditLog(productId, "ROLLBACK",
			QString("回退至版本 %1，生成新版本 %2 (filePath=%3, tableName=%4)")
				.arg(versionNumber).arg(newVersionNumber)
				.arg(restoredFilePath, newLayerTableName),
			operatorName);

		emit operationCompleted(QString("回退成功：版本 %1 → %2").arg(versionNumber).arg(newVersionNumber));
	}
	else
	{
		writeAuditLog(productId, "ROLLBACK_FAIL",
			QString("回退失败：创建版本记录失败"), operatorName);
	}

	return newId > 0;
}

QString ProductDAO::restoreLocalFile(const QString& productUUID, int targetVersion,
									  int newVersion, const QString& targetFilePath)
{
	// 优先从 BLOB 导出旧版本文件（替代 product_storage 本地拷贝）
	auto* db = PostgisConnector::instance();

	// 获取目标版本的 file_oid
	auto verInfo = db->executeQueryOne(
		"SELECT file_oid, file_format FROM version_records "
		"WHERE product_id = $1 AND version_number = $2",
		{productUUID, targetVersion});

	int fileOid = verInfo.value("file_oid", "0").toString().toInt();
	if (fileOid > 0)
	{
		// 从 BLOB 导出到临时文件，供后续重新导入使用
		QFileInfo targetFi(targetFilePath);
		QString fileName = targetFi.fileName();
		if (fileName.isEmpty())
			fileName = QString("v%1_backup").arg(targetVersion);

		QString tempDir = QDir::tempPath() + "/pmt_rollback";
		QDir().mkpath(tempDir);
		QString destFile = tempDir + "/" + fileName;

		PGconn* conn = db->nativeConnection();
		if (PostgisConnector::loExport(conn, fileOid, destFile))
		{
			QgsMessageLog::logMessage(
				QString::fromUtf8("版本回退 BLOB 导出成功: OID=%1 → %2").arg(fileOid).arg(destFile),
				QString::fromUtf8("成果仓"), Qgis::Info);
			return destFile;
		}

		QgsMessageLog::logMessage(
			QString::fromUtf8("版本回退 BLOB 导出失败: OID=%1, %2").arg(fileOid)
				.arg(PostgisConnector::instance()->lastError()),
			QString::fromUtf8("成果仓"), Qgis::Warning);
		return QString();
	}

	// BLOB 不可用时的回退：直接使用 targetFilePath（可能是原始文件路径，仅本机有效）
	QFileInfo targetFi(targetFilePath);
	if (targetFi.fileName().isEmpty())
		return QString();

	if (QFile::exists(targetFilePath))
		return targetFilePath;

	QgsMessageLog::logMessage(
		QString::fromUtf8("版本回退失败: BLOB 和本地文件均不可用 (%1)").arg(targetFilePath),
		QString::fromUtf8("成果仓"), Qgis::Warning);
	return QString();
}

bool ProductDAO::cloneVectorTable(const QString& sourceTableName, const QString& targetTableName)
{
	auto* db = PostgisConnector::instance();
	if (!db->isConnected()) return false;

	// 检查源表是否存在（PostgreSQL 表名默认转为小写，需要 LOWER 比较）
	auto checkResult = db->executeQueryOne(
		QString("SELECT EXISTS (SELECT FROM information_schema.tables WHERE LOWER(table_name) = LOWER('%1'))")
			.arg(sourceTableName));

	if (checkResult.value("exists").toString() != "t")
	{
		return false;
	}

	// 删除可能存在的目标表
	SchemaManager schemaMgr;
	schemaMgr.dropDataLayerTable(targetTableName);

	// 使用 CREATE TABLE ... AS SELECT * 克隆表结构和数据
	QString cloneSql = QString(
		"CREATE TABLE %1 AS SELECT * FROM %2")
		.arg(targetTableName, sourceTableName);

	if (!db->executeNonQuery(cloneSql))
	{
		return false;
	}

	// 为克隆表添加 SERIAL 主键约束（CREATE TABLE AS 不会复制约束）
	// 先检查是否已有 id 列
	auto colCheck = db->executeQueryOne(
		QString("SELECT EXISTS (SELECT FROM information_schema.columns "
			"WHERE LOWER(table_name) = LOWER('%1') AND column_name = 'id')")
			.arg(targetTableName));

	if (colCheck.value("exists").toString() == "t")
	{
		// 添加主键约束
		QString pkSql = QString("ALTER TABLE \"%1\" ADD PRIMARY KEY (id)").arg(targetTableName);
		db->executeNonQuery(pkSql);

		// 设置 id 列为自增序列（以当前最大 id + 1 为起点）
		auto maxId = db->executeQueryOne(
			QString("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM %1").arg(targetTableName));
		int nextId = maxId.value("next_id", "1").toString().toInt();

		QString seqSql = QString(
			"CREATE SEQUENCE IF NOT EXISTS %1_id_seq OWNED BY \"%1\".id;"
			"SELECT setval('%1_id_seq', %2);"
			"ALTER TABLE \"%1\" ALTER COLUMN id SET DEFAULT nextval('%1_id_seq')")
			.arg(targetTableName).arg(nextId);
		db->executeNonQuery(seqSql);
	}

	// 创建 GIST 空间索引
	QString indexSql = QString("CREATE INDEX IF NOT EXISTS idx_%1_geom ON \"%1\" USING GIST(geom)")
		.arg(targetTableName);
	db->executeNonQuery(indexSql);

	return true;
}

QList<VersionRecord> ProductDAO::compareVersions(int productId, int versionA, int versionB)
{
	QList<VersionRecord> results;
	results.append(getVersionRecord(productId, versionA));
	results.append(getVersionRecord(productId, versionB));
	return results;
}

// ===================== 目录管理 =====================
int ProductDAO::insertDirectory(const DirectoryNode& node)
{
	auto* db = PostgisConnector::instance();
	QString sql = R"(
		INSERT INTO product_directory (parent_id, name, description, sort_order, created_by, created_at, node_type)
		VALUES ($1, $2, $3, $4, $5, CURRENT_TIMESTAMP, $6) RETURNING id
	)";
	auto result = db->executeQueryOne(sql, {
		node.parentId, node.name, node.description, node.sortOrder, node.createdBy, node.nodeType
	});
	return result.value("id", "-1").toString().toInt();
}

bool ProductDAO::updateDirectory(const DirectoryNode& node)
{
	auto* db = PostgisConnector::instance();
	return db->executeNonQuery(
		"UPDATE product_directory SET name = $1, description = $2, sort_order = $3 WHERE id = $4",
		{node.name, node.description, node.sortOrder, node.id}
	);
}

bool ProductDAO::deleteDirectory(int dirId)
{
	auto* db = PostgisConnector::instance();
	// 先删除子目录
	db->executeNonQuery("DELETE FROM product_directory WHERE parent_id = $1", {dirId});
	return db->executeNonQuery("DELETE FROM product_directory WHERE id = $1", {dirId});
}

QList<DirectoryNode> ProductDAO::getChildDirectories(int parentId)
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery(
		"SELECT * FROM product_directory WHERE parent_id = $1 ORDER BY sort_order, name",
		{parentId}
	);
	QList<DirectoryNode> results;
	for (const auto& row : rows)
		results.append(DirectoryNode::fromVariantMap(row.toMap()));
	return results;
}

QList<int> ProductDAO::collectSubtreeDirectoryIds(int rootDirId)
{
	QList<int> result;
	if (rootDirId <= 0) return result;
	result.append(rootDirId);
	// 用队列广度优先收集所有子孙目录 id
	QList<int> pending;
	pending.append(rootDirId);
	while (!pending.isEmpty())
	{
		int cur = pending.takeFirst();
		const auto children = getChildDirectories(cur);
		for (const auto& c : children)
		{
			result.append(c.id);
			pending.append(c.id);
		}
	}
	return result;
}

DirectoryNode ProductDAO::getDirectory(int dirId)
{
	auto* db = PostgisConnector::instance();
	auto row = db->executeQueryOne("SELECT * FROM product_directory WHERE id = $1", {dirId});
	return DirectoryNode::fromVariantMap(row);
}

QList<DirectoryNode> ProductDAO::getAllDirectories()
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery("SELECT * FROM product_directory ORDER BY parent_id, sort_order, name");
	QList<DirectoryNode> results;
	for (const auto& row : rows)
		results.append(DirectoryNode::fromVariantMap(row.toMap()));
	return results;
}

int ProductDAO::findOrCreateDirectory(const QString& name, int parentId, int nodeType)
{
	auto* db = PostgisConnector::instance();

	// 先查找同名目录
	auto row = db->executeQueryOne(
		"SELECT * FROM product_directory WHERE name = $1 AND parent_id = $2",
		{name, parentId}
	);
	if (!row.isEmpty())
	{
		int existingId = row.value("id", "-1").toString().toInt();
		int existingType = row.value("node_type", 0).toInt();
		// 已存在节点：若期望的节点类型不同，则更新（用于导入时把旧/普通目录标记为图层节点）
		if (existingType != nodeType)
		{
			db->executeNonQuery(
				"UPDATE product_directory SET node_type = $1 WHERE id = $2",
				{nodeType, existingId});
		}
		return existingId;
	}

	// 不存在则创建
	DirectoryNode node;
	node.name = name;
	node.parentId = parentId;
	node.createdBy = "system";
	node.nodeType = nodeType;
	return insertDirectory(node);
}

// ===================== 标签管理 =====================
bool ProductDAO::addTag(const QString& name, const QString& color, const QString& desc)
{
	auto* db = PostgisConnector::instance();
	return db->executeNonQuery(
		"INSERT INTO product_tags (name, color, description) VALUES ($1, $2, $3) ON CONFLICT (name) DO NOTHING",
		{name, color, desc}
	);
}

bool ProductDAO::removeTag(int tagId)
{
	auto* db = PostgisConnector::instance();
	db->executeNonQuery("DELETE FROM product_tag_mapping WHERE tag_id = $1", {tagId});
	return db->executeNonQuery("DELETE FROM product_tags WHERE id = $1", {tagId});
}

QVariantList ProductDAO::getAllTags()
{
	auto* db = PostgisConnector::instance();
	return db->executeQuery("SELECT * FROM product_tags ORDER BY name");
}

bool ProductDAO::setProductTags(int productId, const QStringList& tags)
{
	auto* db = PostgisConnector::instance();

	// 清除原有标签
	db->executeNonQuery("DELETE FROM product_tag_mapping WHERE product_id = $1", {productId});

	// 添加新标签
	for (const auto& tag : tags)
	{
		QString trimmed = tag.trimmed();
		if (trimmed.isEmpty()) continue;

		// 确保标签存在
		db->executeNonQuery(
			"INSERT INTO product_tags (name) VALUES ($1) ON CONFLICT (name) DO NOTHING",
			{trimmed}
		);

		// 建立关联
		db->executeNonQuery(
			"INSERT INTO product_tag_mapping (product_id, tag_id) "
			"SELECT $1, id FROM product_tags WHERE name = $2 "
			"ON CONFLICT DO NOTHING",
			{productId, trimmed}
		);
	}
	return true;
}

QStringList ProductDAO::getProductTags(int productId)
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery(
		"SELECT t.name FROM product_tags t "
		"INNER JOIN product_tag_mapping m ON t.id = m.tag_id "
		"WHERE m.product_id = $1 ORDER BY t.name",
		{productId}
	);

	QStringList tags;
	for (const auto& row : rows)
		tags << row.toMap().value("name").toString();
	return tags;
}

// ===================== 权限管理 =====================
bool ProductDAO::setUserPermission(const UserPermission& perm)
{
	auto* db = PostgisConnector::instance();
	QString sql = R"(
		INSERT INTO user_permissions (user_name, role, password_hash, permissions, granted_by, granted_at)
		VALUES ($1, $2, $3, $4, $5, CURRENT_TIMESTAMP)
		ON CONFLICT (user_name) DO UPDATE SET
			role = $2, password_hash = $3, permissions = $4, granted_by = $5, granted_at = CURRENT_TIMESTAMP
	)";
	return db->executeNonQuery(sql, {
		perm.userName, accessRoleToString(perm.role), perm.passwordHash,
		perm.permissions, perm.grantedBy
	});
}

bool ProductDAO::removeUserPermission(const QString& userName)
{
	auto* db = PostgisConnector::instance();
	return db->executeNonQuery("DELETE FROM user_permissions WHERE user_name = $1", {userName});
}

UserPermission ProductDAO::getUserPermission(const QString& userName)
{
	auto* db = PostgisConnector::instance();
	auto row = db->executeQueryOne(
		"SELECT * FROM user_permissions WHERE user_name = $1",
		{userName}
	);
	return UserPermission::fromVariantMap(row);
}

QList<UserPermission> ProductDAO::getAllPermissions()
{
	auto* db = PostgisConnector::instance();
	auto rows = db->executeQuery("SELECT * FROM user_permissions ORDER BY user_name");
	QList<UserPermission> results;
	for (const auto& row : rows)
		results.append(UserPermission::fromVariantMap(row.toMap()));
	return results;
}

AccessRole ProductDAO::getUserAccessRole(const QString& userName)
{
	auto perm = getUserPermission(userName);
	return perm.id < 0 ? AccessRole::DataOperator : perm.role;
}

// ===================== 用户认证 =====================
UserPermission ProductDAO::authenticateUser(const QString& userName, const QString& password)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return UserPermission();

	QString passwordHash = hashPassword(password);
	auto row = db->executeQueryOne(
		"SELECT * FROM user_permissions WHERE user_name = $1 AND password_hash = $2",
		{userName, passwordHash}
	);

	UserPermission perm = UserPermission::fromVariantMap(row);

	// 不返回密码哈希给调用方
	if (perm.id > 0)
		perm.passwordHash.clear();

	return perm;
}

bool ProductDAO::addUserWithPassword(const QString& userName, const QString& password,
									  AccessRole role, const QString& grantedBy)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return false;

	QString passwordHash = hashPassword(password);
	QString roleName = accessRoleToString(role);

	return db->executeNonQuery(
		"INSERT INTO user_permissions (user_name, role, password_hash, granted_by, granted_at) "
		"VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP) "
		"ON CONFLICT (user_name) DO UPDATE SET "
		"role = $2, password_hash = $3, granted_by = $4, granted_at = CURRENT_TIMESTAMP",
		{userName, roleName, passwordHash, grantedBy}
	);
}

bool ProductDAO::changePassword(const QString& userName, const QString& newPassword)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return false;

	QString passwordHash = hashPassword(newPassword);
	return db->executeNonQuery(
		"UPDATE user_permissions SET password_hash = $1 WHERE user_name = $2",
		{passwordHash, userName}
	);
}

bool ProductDAO::changeUserRole(const QString& userName, AccessRole newRole, const QString& grantedBy)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return false;

	QString roleName = accessRoleToString(newRole);
	return db->executeNonQuery(
		"UPDATE user_permissions SET role = $1, granted_by = $2, granted_at = CURRENT_TIMESTAMP "
		"WHERE user_name = $3",
		{roleName, grantedBy, userName}
	);
}

// ===================== 审计日志 =====================
bool ProductDAO::writeAuditLog(int productId, const QString& action,
								const QString& detail, const QString& operatorName)
{
	auto* db = PostgisConnector::instance();
	return db->executeNonQuery(
		"INSERT INTO audit_log (product_id, action, detail, operator_name, created_at) "
		"VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP)",
		{productId, action, detail, operatorName}
	);
}

QVariantList ProductDAO::getAuditLogs(int productId, int limit)
{
	auto* db = PostgisConnector::instance();
	return db->executeQuery(
		"SELECT * FROM audit_log WHERE product_id = $1 ORDER BY created_at DESC LIMIT $2",
		{productId, limit}
	);
}

// ===================== 图层注册（完整入库） =====================
bool ProductDAO::registerLayer(int productId, const QString& tableName,
								const QString& geometryType, int srid, int featureCount)
{
	auto* db = PostgisConnector::instance();

	// 写入 layer_registry 表
	QString sql = R"(
		INSERT INTO layer_registry (product_id, table_name, geometry_type, geometry_srid, feature_count)
		VALUES ($1, $2, $3, $4, $5)
		ON CONFLICT (table_name) DO UPDATE SET
			product_id = $1, geometry_type = $3, geometry_srid = $4, feature_count = $5
	)";
	bool ok = db->executeNonQuery(sql, {productId, tableName, geometryType, srid, featureCount});

	// 同步更新 product_metadata 中的 layer_table_name
	if (ok)
	{
		db->executeNonQuery(
			"UPDATE product_metadata SET layer_table_name = $1 WHERE id = $2",
			{tableName, productId}
		);
	}

	return ok;
}

bool ProductDAO::unregisterLayer(int productId)
{
	auto* db = PostgisConnector::instance();

	// 查询关联的图层表名
	auto row = db->executeQueryOne(
		"SELECT table_name FROM layer_registry WHERE product_id = $1",
		{productId}
	);
	QString tableName = row.value("table_name").toString();

	if (!tableName.isEmpty())
	{
		// 删除图层表
		db->executeNonQuery(QString("DROP TABLE IF EXISTS \"%1\" CASCADE").arg(tableName));
	}

	// 删除注册记录
	db->executeNonQuery("DELETE FROM layer_registry WHERE product_id = $1", {productId});

	// 清除 product_metadata 中的引用
	db->executeNonQuery(
		"UPDATE product_metadata SET layer_table_name = NULL WHERE id = $1",
		{productId}
	);

	return true;
}

QString ProductDAO::getLayerTableName(int productId)
{
	auto* db = PostgisConnector::instance();
	auto row = db->executeQueryOne(
		"SELECT layer_table_name FROM product_metadata WHERE id = $1",
		{productId}
	);
	return row.value("layer_table_name").toString();
}

// ===================== 数据类型专属元数据 DAO =====================

bool ProductDAO::saveVectorMeta(const ProductVectorMeta& meta)
{
	auto* db = PostgisConnector::instance();

	auto existing = db->executeQueryOne(
		"SELECT id FROM product_vector_meta WHERE product_id = $1",
		{meta.productId}
	);

	if (!existing.isEmpty()) {
		return db->executeNonQuery(R"(
			UPDATE product_vector_meta SET
				geom_type        = $1,
				inv_scale        = $2,
				cs_type          = $3,
				geodetic_datum   = $4,
				epsg_code        = $5,
				proj_desc        = $6,
				field_desc       = $7,
				feature_count    = $8,
				field_count      = $9,
				layer_table_name = $10
			WHERE product_id = $11
		)", {
			meta.geomType, meta.invScale, meta.csType, meta.geodeticDatum,
			meta.epsgCode, meta.projDesc, meta.fieldDesc,
			meta.featureCount, meta.fieldCount, meta.layerTableName,
			meta.productId
		});
	} else {
		return db->executeNonQuery(R"(
			INSERT INTO product_vector_meta
				(product_id, geom_type, inv_scale, cs_type, geodetic_datum,
				 epsg_code, proj_desc, field_desc,
				 feature_count, field_count, layer_table_name)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)
		)", {
			meta.productId, meta.geomType, meta.invScale, meta.csType,
			meta.geodeticDatum, meta.epsgCode, meta.projDesc, meta.fieldDesc,
			meta.featureCount, meta.fieldCount, meta.layerTableName
		});
	}
}

ProductVectorMeta ProductDAO::getVectorMeta(int productId)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return ProductVectorMeta();
	auto row = db->executeQueryOne(
		R"(SELECT * FROM product_vector_meta WHERE product_id = $1)",
		{productId}
	);
	return ProductVectorMeta::fromVariantMap(row);
}

bool ProductDAO::saveRasterMeta(const ProductRasterMeta& meta)
{
	auto* db = PostgisConnector::instance();

	auto existing = db->executeQueryOne(
		"SELECT id FROM product_raster_meta WHERE product_id = $1",
		{meta.productId}
	);

	if (!existing.isEmpty()) {
		return db->executeNonQuery(R"(
			UPDATE product_raster_meta SET
				satellite_name   = $1,
				sensor_type      = $2,
				acquire_time     = $3,
				gsd              = $4,
				resolution_unit  = $5,
				color_type       = $6,
				bit_depth        = $7,
				band_count       = $8,
				nodata_value     = $9,
				cs_type          = $10,
				geodetic_datum   = $11,
				epsg_code        = $12,
				proj_desc        = $13,
				planar_unit      = $14,
				pixel_width      = $15,
				pixel_height     = $16,
				pixel_type       = $17,
				layer_table_name = $18
			WHERE product_id = $19
		)", {
			meta.satelliteName, meta.sensorType,
			meta.acquireTime.isValid() ? meta.acquireTime : QVariant(),
			meta.gsd, meta.resolutionUnit, meta.colorType,
			meta.bitDepth, meta.bandCount, meta.nodataValue,
			meta.csType, meta.geodeticDatum, meta.epsgCode, meta.projDesc,
			meta.planarUnit, meta.pixelWidth, meta.pixelHeight,
			meta.pixelType, meta.layerTableName,
			meta.productId
		});
	} else {
		return db->executeNonQuery(R"(
			INSERT INTO product_raster_meta
				(product_id, satellite_name, sensor_type, acquire_time,
				 gsd, resolution_unit, color_type,
				 bit_depth, band_count, nodata_value,
				 cs_type, geodetic_datum, epsg_code, proj_desc, planar_unit,
				 pixel_width, pixel_height, pixel_type, layer_table_name)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19)
		)", {
			meta.productId, meta.satelliteName, meta.sensorType,
			meta.acquireTime.isValid() ? meta.acquireTime : QVariant(),
			meta.gsd, meta.resolutionUnit, meta.colorType,
			meta.bitDepth, meta.bandCount, meta.nodataValue,
			meta.csType, meta.geodeticDatum, meta.epsgCode, meta.projDesc,
			meta.planarUnit, meta.pixelWidth, meta.pixelHeight,
			meta.pixelType, meta.layerTableName
		});
	}
}

ProductRasterMeta ProductDAO::getRasterMeta(int productId)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return ProductRasterMeta();
	auto row = db->executeQueryOne(
		R"(SELECT * FROM product_raster_meta WHERE product_id = $1)",
		{productId}
	);
	return ProductRasterMeta::fromVariantMap(row);
}

bool ProductDAO::saveDiagramMeta(const ProductDiagramMeta& meta)
{
	auto* db = PostgisConnector::instance();
	auto existing = db->executeQueryOne(
		"SELECT id FROM product_diagram_meta WHERE product_id = $1",
		{meta.productId}
	);
	if (!existing.isEmpty()) {
		return db->executeNonQuery(R"(
			UPDATE product_diagram_meta SET
				data_id          = $1,
				map_series       = $2,
				city_prefecture  = $3,
				map_scale        = $4,
				proj_desc        = $5,
				production_date  = $6,
				approval_no      = $7,
				carto_software   = $8,
				format           = $9,
				paper_size       = $10,
				legend_included  = $11,
				modifier         = $12,
				last_modified    = $13,
				print_ready      = $14,
				color_mode       = $15,
				dpi              = $16,
				proj_name        = $17,
				raster_ids       = $18,
				vector_ids       = $19,
				end_datetime     = $20,
				map_productor    = $21,
				has_math_base    = $22
			WHERE product_id = $23
		)", {
			meta.dataId, meta.mapSeries, meta.cityPrefecture, meta.mapScale,
			meta.projDesc, meta.productionDate.isValid() ? meta.productionDate : QVariant(),
			meta.approvalNo, meta.cartoSoftware, meta.format, meta.paperSize,
			meta.legendIncluded, meta.modifier,
			meta.lastModified.isValid() ? meta.lastModified : QVariant(),
			meta.printReady, meta.colorMode, meta.dpi, meta.projName,
			meta.rasterIds, meta.vectorIds,
			meta.endDatetime.isValid() ? meta.endDatetime : QVariant(),
			meta.mapProductor, meta.hasMathBase,
			meta.productId
		});
	} else {
		return db->executeNonQuery(R"(
			INSERT INTO product_diagram_meta
				(product_id, data_id, map_series, city_prefecture, map_scale,
				 proj_desc, production_date, approval_no, carto_software, format,
				 paper_size, legend_included, modifier, last_modified, print_ready,
				 color_mode, dpi, proj_name, raster_ids, vector_ids, end_datetime,
				 map_productor, has_math_base)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23)
		)", {
			meta.productId, meta.dataId, meta.mapSeries, meta.cityPrefecture, meta.mapScale,
			meta.projDesc, meta.productionDate.isValid() ? meta.productionDate : QVariant(),
			meta.approvalNo, meta.cartoSoftware, meta.format, meta.paperSize,
			meta.legendIncluded, meta.modifier,
			meta.lastModified.isValid() ? meta.lastModified : QVariant(),
			meta.printReady, meta.colorMode, meta.dpi, meta.projName,
			meta.rasterIds, meta.vectorIds,
			meta.endDatetime.isValid() ? meta.endDatetime : QVariant(),
			meta.mapProductor, meta.hasMathBase
		});
	}
}

ProductDiagramMeta ProductDAO::getDiagramMeta(int productId)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return ProductDiagramMeta();
	auto row = db->executeQueryOne(
		R"(SELECT * FROM product_diagram_meta WHERE product_id = $1)",
		{productId}
	);
	return ProductDiagramMeta::fromVariantMap(row);
}

bool ProductDAO::saveOutputMeta(const ProductOutputMeta& meta)
{
	auto* db = PostgisConnector::instance();
	auto existing = db->executeQueryOne(
		"SELECT id FROM product_output_meta WHERE product_id = $1",
		{meta.productId}
	);
	if (!existing.isEmpty()) {
		return db->executeNonQuery(R"(
			UPDATE product_output_meta SET
				page_count           = $1,
				color_mode           = $2,
				embedded_fonts       = $3,
				pdf_version          = $4,
				interactive_features = $5
			WHERE product_id = $6
		)", {
			meta.pageCount, meta.colorMode, meta.embeddedFonts,
			meta.pdfVersion, meta.interactiveFeatures,
			meta.productId
		});
	} else {
		return db->executeNonQuery(R"(
			INSERT INTO product_output_meta
				(product_id, page_count, color_mode, embedded_fonts,
				 pdf_version, interactive_features)
			VALUES ($1,$2,$3,$4,$5,$6)
		)", {
			meta.productId, meta.pageCount, meta.colorMode,
			meta.embeddedFonts, meta.pdfVersion, meta.interactiveFeatures
		});
	}
}

bool ProductDAO::saveDocumentMeta(const ProductDocumentMeta& meta)
{
	auto* db = PostgisConnector::instance();
	auto existing = db->executeQueryOne(
		"SELECT id FROM product_document_meta WHERE product_id = $1",
		{meta.productId}
	);
	if (!existing.isEmpty()) {
		return db->executeNonQuery(R"(
			UPDATE product_document_meta SET
				publisher       = $1,
				file_type       = $2,
				format          = $3,
				file_size       = $4,
				language_type   = $5,
				key_words       = $6,
				summary         = $7,
				quality_issues  = $8,
				collect_time    = $9,
				end_datetime    = $10,
				collector       = $11,
				collect_purpose = $12,
				project_name    = $13,
				is_compressed   = $14
			WHERE product_id = $15
		)", {
			meta.publisher, meta.fileType, meta.format, meta.fileSize,
			meta.languageType, meta.keyWords, meta.summary, meta.qualityIssues,
			meta.collectTime.isValid() ? meta.collectTime : QVariant(),
			meta.endDatetime.isValid() ? meta.endDatetime : QVariant(),
			meta.collector, meta.collectPurpose, meta.projectName,
			meta.isCompressed,
			meta.productId
		});
	} else {
		return db->executeNonQuery(R"(
			INSERT INTO product_document_meta
				(product_id, publisher, file_type, format, file_size,
				 language_type, key_words, summary, quality_issues,
				 collect_time, end_datetime, collector, collect_purpose,
				 project_name, is_compressed)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15)
		)", {
			meta.productId, meta.publisher, meta.fileType, meta.format,
			meta.fileSize, meta.languageType, meta.keyWords, meta.summary,
			meta.qualityIssues,
			meta.collectTime.isValid() ? meta.collectTime : QVariant(),
			meta.endDatetime.isValid() ? meta.endDatetime : QVariant(),
			meta.collector, meta.collectPurpose, meta.projectName,
			meta.isCompressed
		});
	}
}

ProductDocumentMeta ProductDAO::getDocumentMeta(int productId)
{
	auto* db = PostgisConnector::instance();
	if (!db || !db->isConnected())
		return ProductDocumentMeta();
	auto row = db->executeQueryOne(
		R"(SELECT * FROM product_document_meta WHERE product_id = $1)",
		{productId}
	);
	return ProductDocumentMeta::fromVariantMap(row);
}

void ProductDAO::enrichWithTypeMeta(int productId)
{
	auto* db = PostgisConnector::instance();

	// 读取产品类型和文件路径
	auto row = db->executeQueryOne(
		"SELECT product_type, file_path FROM product_metadata WHERE id = $1",
		{productId}
	);

	QString productType = row.value("product_type").toString();
	QString filePath = row.value("file_path").toString();

	if (filePath.isEmpty()) return;

	QFileInfo fi(filePath);
	if (!fi.exists()) return;

	MetadataExtractor extractor;

	if (productType == "矢量数据")
	{
		ProductVectorMeta vMeta = extractor.extractVectorTypeMeta(filePath);
		if (vMeta.featureCount > 0 || vMeta.fieldCount > 0)
		{
			vMeta.productId = productId;
			vMeta.layerTableName = getLayerTableName(productId);
			saveVectorMeta(vMeta);
		}
	}
	else if (productType == "栅格数据")
	{
		ProductRasterMeta rMeta = extractor.extractRasterTypeMeta(filePath);
		if (rMeta.bandCount > 0)
		{
			rMeta.productId = productId;
			rMeta.layerTableName = getLayerTableName(productId);
			saveRasterMeta(rMeta);
		}
	}
	else if (productType == "AI" || productType == "CAD" || productType == "CDR" || productType == "PDF")
	{
		ProductDiagramMeta dMeta = extractor.extractDiagramTypeMeta(filePath);
		dMeta.productId = productId;
		saveDiagramMeta(dMeta);
	}
	else if (productType == "Other" || productType == "文档文件" || productType == "压缩包")
	{
		ProductDocumentMeta docMeta = extractor.extractDocumentTypeMeta(filePath);
		docMeta.productId = productId;
		saveDocumentMeta(docMeta);
	}
}

void ProductDAO::getFullProductMetadata(int productId, ProductMetadata& meta,
	ProductVectorMeta& vectorMeta, ProductRasterMeta& rasterMeta)
{
	auto* db = PostgisConnector::instance();

	// 1. 取主表元数据
	auto mainRow = db->executeQueryOne(
		"SELECT * FROM product_metadata WHERE id = $1",
		{productId}
	);
	meta = ProductMetadata::fromVariantMap(mainRow);

	// 2. 根据 product_type 取专属元数据
	if (meta.productType == ProductType::Vector) {
		auto vRow = db->executeQueryOne(
			"SELECT * FROM product_vector_meta WHERE product_id = $1",
			{productId}
		);
		vectorMeta = ProductVectorMeta::fromVariantMap(vRow);
	}
	if (meta.productType == ProductType::Raster) {
		auto rRow = db->executeQueryOne(
			"SELECT * FROM product_raster_meta WHERE product_id = $1",
			{productId}
		);
		rasterMeta = ProductRasterMeta::fromVariantMap(rRow);
	}
}

