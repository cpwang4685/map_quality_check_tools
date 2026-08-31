#include "product_metadata.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>

// 全局当前用户会话（定义在 product_metadata.h 中 extern 声明）
UserSession gCurrentUserSession;

// ===================== 产品类型转换 =====================
static QMap<ProductType, QString> g_productTypeMap = {
	{ProductType::Vector,   "矢量数据"},
	{ProductType::Raster,   "栅格数据"},
	{ProductType::CAD,      "CAD"},
	{ProductType::AI,       "AI"},
	{ProductType::CDR,      "CDR"},
	{ProductType::PDF,      "PDF"},
	{ProductType::Document, "文档文件"},
	{ProductType::Archive,  "压缩包"},
	{ProductType::Other,    "Other"}
};

QString productTypeToString(ProductType type)
{
	return g_productTypeMap.value(type, "Other");
}

ProductType stringToProductType(const QString& str)
{
	for (auto it = g_productTypeMap.begin(); it != g_productTypeMap.end(); ++it)
	{
		if (it.value().compare(str, Qt::CaseInsensitive) == 0)
			return it.key();
	}
	return ProductType::Other;
}

QString productTypeToDisplayName(ProductType type)
{
	// 表格展示：文档、压缩包、其它 统一归为"其它类型"
	switch (type) {
	case ProductType::Document:
	case ProductType::Archive:
	case ProductType::Other:
		return QString::fromUtf8("其它类型");
	default:
		return productTypeToString(type);
	}
}

// ===================== 密级转换 =====================
static QMap<SecurityLevel, QString> g_securityLevelMap = {
	{SecurityLevel::Unclassified, "非密"},
	{SecurityLevel::Internal,     "内部"},
	{SecurityLevel::Confidential, "机密"},
	{SecurityLevel::Secret,       "秘密"},
	{SecurityLevel::TopSecret,    "绝密"}
};

QString securityLevelToString(SecurityLevel level)
{
	return g_securityLevelMap.value(level, "非密");
}

SecurityLevel stringToSecurityLevel(const QString& str)
{
	for (auto it = g_securityLevelMap.begin(); it != g_securityLevelMap.end(); ++it)
	{
		if (it.value() == str)
			return it.key();
	}
	return SecurityLevel::Unclassified;
}

QStringList getAllProductTypeStrings()
{
	QStringList types;
	for (auto it = g_productTypeMap.begin(); it != g_productTypeMap.end(); ++it)
		types << it.value();
	return types;
}

// ===================== ProductMetadata 序列化 =====================
QVariantMap ProductMetadata::toVariantMap() const
{
	QVariantMap map;
	// CSV 第 1~27 项
	map["data_id"]          = dataId;
	map["product_name"]     = productName;
	map["description"]      = description;
	map["source"]           = source;
	map["version_note"]     = versionNote;
	map["file_format"]      = fileFormat;
	map["file_size"]        = fileSize;
	map["is_compressed"]    = isCompressed;
	map["bounds"]           = bounds;
	map["center_lon"]       = centerLon;
	map["center_lat"]       = centerLat;
	map["start_datetime"]   = startDatetime.isValid() ? startDatetime.toString(Qt::ISODate) : QVariant();
	map["end_datetime"]     = endDatetime.isValid() ? endDatetime.toString(Qt::ISODate) : QVariant();
	map["security_level"]   = securityLevelToString(securityLevel);
	map["created_by"]       = createdBy;
	map["created_at"]       = createdAt.toString(Qt::ISODate);
	map["updated_at"]       = updatedAt.toString(Qt::ISODate);
	map["tags"]             = tags;
	map["city"]             = city;
	map["scale"]            = scale;
	map["project_name"]     = projectName;
	map["producer"]         = producer;
	map["production_date"]  = productionDate.isValid() ? productionDate.toString(Qt::ISODate) : QVariant();
	map["crs"]              = crs;
	map["compilation_info"] = compilationInfo;
	map["delivery_status"]  = deliveryStatus;
	map["delivery_time"]    = deliveryTime.isValid() ? deliveryTime.toString(Qt::ISODate) : QVariant();

	// 系统内部字段
	map["id"]               = id;
	map["product_type"]     = productTypeToString(productType);
	map["file_path"]        = filePath;
	map["file_hash"]        = fileHash;
	map["file_oid"]         = fileOid;
	map["thumbnail_path"]   = thumbnailPath;
	map["min_x"]            = minX;
	map["min_y"]            = minY;
	map["max_x"]            = maxX;
	map["max_y"]            = maxY;
	map["spatial_extent_wkt"] = spatialExtentWKT;
	map["geometry_type"]    = geometryType;
	map["band_count"]       = bandCount;
	map["pixel_width"]      = pixelWidth;
	map["pixel_height"]     = pixelHeight;
	map["pixel_resolution"] = pixelResolution;
	map["approval_number"]  = approvalNumber;
	map["parent_dir_id"]    = parentDirId;
	map["directory_path"]   = directoryPath;
	map["updated_by"]       = updatedBy;
	map["layer_table_name"] = layerTableName;
	map["current_version"]  = currentVersion;

	// 兼容旧代码：uuid → data_id 别名
	map["uuid"] = dataId;

	return map;
}

ProductMetadata ProductMetadata::fromVariantMap(const QVariantMap& map)
{
	ProductMetadata meta;
	// CSV 第 1~27 项
	// 兼容旧列名 uuid → data_id
	meta.dataId         = map.value("data_id", map.value("uuid").toString()).toString();
	meta.productName    = map.value("product_name").toString();
	meta.description    = map.value("description").toString();
	meta.source         = map.value("source").toString();
	meta.versionNote    = map.value("version_note").toString();
	meta.fileFormat     = map.value("file_format").toString();
	meta.fileSize       = map.value("file_size", 0).toLongLong();
	meta.isCompressed   = map.value("is_compressed").toString();
	meta.bounds         = map.value("bounds").toString();
	meta.centerLon      = map.value("center_lon").toString();
	meta.centerLat      = map.value("center_lat").toString();
	meta.startDatetime  = QDateTime::fromString(map.value("start_datetime").toString(), Qt::ISODate);
	meta.endDatetime    = QDateTime::fromString(map.value("end_datetime").toString(), Qt::ISODate);
	meta.securityLevel  = stringToSecurityLevel(map.value("security_level").toString());
	meta.createdBy      = map.value("created_by").toString();
	meta.createdAt      = QDateTime::fromString(map.value("created_at").toString(), Qt::ISODate);
	meta.updatedAt      = QDateTime::fromString(map.value("updated_at").toString(), Qt::ISODate);
	// 标签：兼容分号/逗号分割
	{
		QString rawTags = map.value("tags").toString();
		meta.tags = rawTags.replace(',', ';');  // 统一为分号分割
	}
	meta.city           = map.value("city").toString();
	meta.scale          = map.value("scale").toString();
	meta.projectName    = map.value("project_name").toString();
	meta.producer       = map.value("producer").toString();
	meta.productionDate = QDate::fromString(map.value("production_date").toString(), Qt::ISODate);
	meta.crs            = map.value("crs").toString();
	meta.compilationInfo= map.value("compilation_info").toString();
	meta.deliveryStatus = map.value("delivery_status").toString();
	meta.deliveryTime   = QDateTime::fromString(map.value("delivery_time").toString(), Qt::ISODate);

	// 系统内部字段
	meta.id             = map.value("id", -1).toInt();
	meta.productType    = stringToProductType(map.value("product_type").toString());
	meta.filePath       = map.value("file_path").toString();
	meta.fileHash       = map.value("file_hash").toString();
	meta.fileOid        = map.value("file_oid", 0).toInt();
	meta.thumbnailPath  = map.value("thumbnail_path").toString();
	meta.minX           = map.value("min_x", 0).toDouble();
	meta.minY           = map.value("min_y", 0).toDouble();
	meta.maxX           = map.value("max_x", 0).toDouble();
	meta.maxY           = map.value("max_y", 0).toDouble();
	meta.spatialExtentWKT = map.value("spatial_extent_wkt").toString();
	meta.geometryType   = map.value("geometry_type").toString();
	meta.bandCount      = map.value("band_count", 0).toInt();
	meta.pixelWidth     = map.value("pixel_width", 0).toInt();
	meta.pixelHeight    = map.value("pixel_height", 0).toInt();
	meta.pixelResolution = map.value("pixel_resolution", 0).toDouble();
	meta.approvalNumber = map.value("approval_number").toString();
	meta.parentDirId    = map.value("parent_dir_id", -1).toInt();
	meta.directoryPath  = map.value("directory_path").toString();
	meta.updatedBy      = map.value("updated_by").toString();
	meta.layerTableName = map.value("layer_table_name").toString();
	meta.currentVersion = map.value("current_version", 1).toInt();

	return meta;
}

// ===================== ProductVectorMeta 序列化（矢量数据元数据.csv 7项） =====================
QVariantMap ProductVectorMeta::toVariantMap() const
{
	QVariantMap map;
	// CSV 第 1~7 项
	map["geom_type"]       = geomType;
	map["inv_scale"]       = invScale;
	map["cs_type"]         = csType;
	map["geodetic_datum"]  = geodeticDatum;
	map["epsg_code"]       = epsgCode;
	map["proj_desc"]       = projDesc;
	map["field_desc"]      = fieldDesc;
	// 系统内部字段
	map["id"]              = id;
	map["product_id"]      = productId;
	map["feature_count"]   = featureCount;
	map["field_count"]     = fieldCount;
	map["layer_table_name"] = layerTableName;
	return map;
}

ProductVectorMeta ProductVectorMeta::fromVariantMap(const QVariantMap& map)
{
	ProductVectorMeta meta;
	// CSV 第 1~7 项
	meta.geomType       = map.value("geom_type").toString();
	meta.invScale       = map.value("inv_scale", 0).toInt();
	meta.csType         = map.value("cs_type").toString();
	meta.geodeticDatum  = map.value("geodetic_datum").toString();
	meta.epsgCode       = map.value("epsg_code").toString();
	meta.projDesc       = map.value("proj_desc").toString();
	meta.fieldDesc      = map.value("field_desc").toString();
	// 系统内部字段（兼容旧列名）
	meta.id             = map.value("id", -1).toInt();
	meta.productId      = map.value("product_id", -1).toInt();
	meta.featureCount   = map.value("feature_count", 0).toLongLong();
	meta.fieldCount     = map.value("field_count", 0).toInt();
	// 兼容旧字段名 geometry_type → geom_type
	if (meta.geomType.isEmpty())
		meta.geomType = map.value("geometry_type").toString();
	if (meta.fieldDesc.isEmpty())
		meta.fieldDesc = map.value("field_details").toString();
	meta.layerTableName = map.value("layer_table_name").toString();
	return meta;
}

// ===================== ProductRasterMeta 序列化（栅格数据元数据.csv 14项） =====================
QVariantMap ProductRasterMeta::toVariantMap() const
{
	QVariantMap map;
	// CSV 第 1~14 项
	map["satellite_name"]   = satelliteName;
	map["sensor_type"]      = sensorType;
	map["acquire_time"]     = acquireTime.isValid() ? acquireTime.toString(Qt::ISODate) : QVariant();
	map["gsd"]              = gsd;
	map["resolution_unit"]  = resolutionUnit;
	map["color_type"]       = colorType;
	map["bit_depth"]        = bitDepth;
	map["band_count"]       = bandCount;
	map["nodata_value"]     = nodataValue;
	map["cs_type"]          = csType;
	map["geodetic_datum"]   = geodeticDatum;
	map["epsg_code"]        = epsgCode;
	map["proj_desc"]        = projDesc;
	map["planar_unit"]      = planarUnit;
	// 系统内部字段
	map["id"]               = id;
	map["product_id"]       = productId;
	map["pixel_width"]      = pixelWidth;
	map["pixel_height"]     = pixelHeight;
	map["pixel_type"]       = pixelType;
	map["layer_table_name"] = layerTableName;
	return map;
}

ProductRasterMeta ProductRasterMeta::fromVariantMap(const QVariantMap& map)
{
	ProductRasterMeta meta;
	// CSV 第 1~14 项
	meta.satelliteName  = map.value("satellite_name").toString();
	meta.sensorType     = map.value("sensor_type").toString();
	meta.acquireTime    = QDateTime::fromString(map.value("acquire_time").toString(), Qt::ISODate);
	meta.gsd            = map.value("gsd", 0).toDouble();
	meta.resolutionUnit = map.value("resolution_unit").toString();
	meta.colorType      = map.value("color_type").toString();
	meta.bitDepth       = map.value("bit_depth", 0).toInt();
	meta.bandCount      = map.value("band_count", 0).toInt();
	meta.nodataValue    = map.value("nodata_value", 0).toDouble();
	meta.csType         = map.value("cs_type").toString();
	meta.geodeticDatum  = map.value("geodetic_datum").toString();
	meta.epsgCode       = map.value("epsg_code").toString();
	meta.projDesc       = map.value("proj_desc").toString();
	meta.planarUnit     = map.value("planar_unit").toString();
	// 系统内部字段（兼容旧列名）
	meta.id             = map.value("id", -1).toInt();
	meta.productId      = map.value("product_id", -1).toInt();
	meta.pixelWidth     = map.value("pixel_width", 0).toInt();
	meta.pixelHeight    = map.value("pixel_height", 0).toInt();
	meta.pixelType      = map.value("pixel_type").toString();
	// 兼容旧字段 pixel_resolution → gsd
	if (meta.gsd == 0)
		meta.gsd = map.value("pixel_resolution", 0).toDouble();
	// 兼容旧字段 color_space → color_type
	if (meta.colorType.isEmpty())
		meta.colorType = map.value("color_space").toString();
	// 兼容旧字段 image_date → acquire_time
	if (!meta.acquireTime.isValid())
	{
		QString oldDate = map.value("image_date").toString();
		if (!oldDate.isEmpty())
			meta.acquireTime = QDateTime::fromString(oldDate, Qt::ISODate);
	}
	meta.layerTableName = map.value("layer_table_name").toString();
	return meta;
}

// ===================== ProductDiagramMeta 序列化（制图成果数据元数据.csv 22项） =====================
QVariantMap ProductDiagramMeta::toVariantMap() const
{
	QVariantMap map;
	// CSV 第 1~22 项
	map["data_id"]          = dataId;
	map["map_series"]       = mapSeries;
	map["city_prefecture"]  = cityPrefecture;
	map["map_scale"]        = mapScale;
	map["proj_desc"]        = projDesc;
	map["production_date"]  = productionDate.isValid() ? productionDate.toString(Qt::ISODate) : QVariant();
	map["approval_no"]      = approvalNo;
	map["carto_software"]   = cartoSoftware;
	map["format"]           = format;
	map["paper_size"]       = paperSize;
	map["legend_included"]  = legendIncluded;
	map["modifier"]         = modifier;
	map["last_modified"]    = lastModified.isValid() ? lastModified.toString(Qt::ISODate) : QVariant();
	map["print_ready"]      = printReady;
	map["color_mode"]       = colorMode;
	map["dpi"]              = dpi;
	map["proj_name"]        = projName;
	map["raster_ids"]       = rasterIds;
	map["vector_ids"]       = vectorIds;
	map["end_datetime"]     = endDatetime.isValid() ? endDatetime.toString(Qt::ISODate) : QVariant();
	map["map_productor"]    = mapProductor;
	map["has_math_base"]    = hasMathBase;
	// 系统内部字段
	map["id"]               = id;
	map["product_id"]       = productId;
	return map;
}

ProductDiagramMeta ProductDiagramMeta::fromVariantMap(const QVariantMap& map)
{
	ProductDiagramMeta meta;
	// CSV 第 1~22 项
	meta.dataId         = map.value("data_id").toString();
	meta.mapSeries      = map.value("map_series").toString();
	meta.cityPrefecture = map.value("city_prefecture").toString();
	meta.mapScale       = map.value("map_scale", 0).toInt();
	meta.projDesc       = map.value("proj_desc").toString();
	meta.productionDate = QDate::fromString(map.value("production_date").toString(), Qt::ISODate);
	meta.approvalNo     = map.value("approval_no").toString();
	meta.cartoSoftware  = map.value("carto_software").toString();
	meta.format         = map.value("format").toString();
	meta.paperSize      = map.value("paper_size").toString();
	meta.legendIncluded = map.value("legend_included").toBool();
	meta.modifier       = map.value("modifier").toString();
	meta.lastModified   = QDate::fromString(map.value("last_modified").toString(), Qt::ISODate);
	meta.printReady     = map.value("print_ready").toBool();
	meta.colorMode      = map.value("color_mode").toString();
	meta.dpi            = map.value("dpi", 0).toInt();
	meta.projName       = map.value("proj_name").toString();
	meta.rasterIds      = map.value("raster_ids").toString();
	meta.vectorIds      = map.value("vector_ids").toString();
	meta.endDatetime    = QDateTime::fromString(map.value("end_datetime").toString(), Qt::ISODate);
	meta.mapProductor   = map.value("map_productor").toString();
	meta.hasMathBase    = map.value("has_math_base").toBool();
	// 系统内部字段（兼容旧列名）
	meta.id             = map.value("id", -1).toInt();
	meta.productId      = map.value("product_id", -1).toInt();
	// 兼容旧字段 sheet_size → paper_size
	if (meta.paperSize.isEmpty())
		meta.paperSize = map.value("sheet_size").toString();
	// 兼容旧字段 output_dpi → dpi
	if (meta.dpi == 0)
		meta.dpi = map.value("output_dpi", 0).toInt();
	return meta;
}

// ===================== ProductOutputMeta 序列化 =====================
QVariantMap ProductOutputMeta::toVariantMap() const
{
	QVariantMap map;
	map["id"] = id;
	map["product_id"] = productId;
	map["page_count"] = pageCount;
	map["color_mode"] = colorMode;
	map["embedded_fonts"] = embeddedFonts;
	map["pdf_version"] = pdfVersion;
	map["interactive_features"] = interactiveFeatures;
	return map;
}

ProductOutputMeta ProductOutputMeta::fromVariantMap(const QVariantMap& map)
{
	ProductOutputMeta meta;
	meta.id = map.value("id", -1).toInt();
	meta.productId = map.value("product_id", -1).toInt();
	meta.pageCount = map.value("page_count", 0).toInt();
	meta.colorMode = map.value("color_mode").toString();
	meta.embeddedFonts = map.value("embedded_fonts").toString();
	meta.pdfVersion = map.value("pdf_version").toString();
	meta.interactiveFeatures = map.value("interactive_features").toString();
	return meta;
}

// ===================== ProductDocumentMeta 序列化（文档数据元数据.csv 14项） =====================
QVariantMap ProductDocumentMeta::toVariantMap() const
{
	QVariantMap map;
	// CSV 第 1~14 项
	map["publisher"]        = publisher;
	map["file_type"]        = fileType;
	map["format"]           = format;
	map["file_size"]        = fileSize;
	map["language_type"]    = languageType;
	map["key_words"]        = keyWords;
	map["summary"]          = summary;
	map["quality_issues"]   = qualityIssues;
	map["collect_time"]     = collectTime.isValid() ? collectTime.toString(Qt::ISODate) : QVariant();
	map["end_datetime"]     = endDatetime.isValid() ? endDatetime.toString(Qt::ISODate) : QVariant();
	map["collector"]        = collector;
	map["collect_purpose"]  = collectPurpose;
	map["project_name"]     = projectName;
	map["is_compressed"]    = isCompressed;
	// 系统内部字段
	map["id"]               = id;
	map["product_id"]       = productId;
	return map;
}

ProductDocumentMeta ProductDocumentMeta::fromVariantMap(const QVariantMap& map)
{
	ProductDocumentMeta meta;
	// CSV 第 1~14 项
	meta.publisher        = map.value("publisher").toString();
	meta.fileType         = map.value("file_type").toString();
	meta.format           = map.value("format").toString();
	meta.fileSize         = map.value("file_size").toString();
	meta.languageType     = map.value("language_type").toString();
	meta.keyWords         = map.value("key_words").toString();
	meta.summary          = map.value("summary").toString();
	meta.qualityIssues    = map.value("quality_issues").toString();
	meta.collectTime      = QDateTime::fromString(map.value("collect_time").toString(), Qt::ISODate);
	meta.endDatetime      = QDateTime::fromString(map.value("end_datetime").toString(), Qt::ISODate);
	meta.collector        = map.value("collector").toString();
	meta.collectPurpose   = map.value("collect_purpose").toString();
	meta.projectName      = map.value("project_name").toString();
	meta.isCompressed     = map.value("is_compressed").toBool();
	// 系统内部字段（兼容旧列名）
	meta.id               = map.value("id", -1).toInt();
	meta.productId        = map.value("product_id", -1).toInt();
	// 兼容旧字段 author → publisher
	if (meta.publisher.isEmpty())
		meta.publisher = map.value("author").toString();
	return meta;
}

// ===================== VersionRecord 序列化 =====================
QVariantMap VersionRecord::toVariantMap() const
{
	QVariantMap map;
	map["id"] = id;
	map["product_id"] = productId;
	map["version_number"] = versionNumber;
	map["file_path"] = filePath;
	map["file_hash"] = fileHash;
	map["file_size"] = fileSize;
	map["file_oid"] = fileOid;
	map["file_format"] = fileFormat;
	map["layer_table_name"] = layerTableName;
	map["change_note"] = changeNote;
	map["changed_by"] = changedBy;
	map["changed_at"] = changedAt.toString(Qt::ISODate);
	map["parent_version"] = parentVersion;
	map["diff_info"] = diffInfo;
	return map;
}

VersionRecord VersionRecord::fromVariantMap(const QVariantMap& map)
{
	VersionRecord rec;
	rec.id = map.value("id", -1).toInt();
	rec.productId = map.value("product_id", -1).toInt();
	rec.versionNumber = map.value("version_number", 1).toInt();
	rec.filePath = map.value("file_path").toString();
	rec.fileHash = map.value("file_hash").toString();
	rec.fileSize = map.value("file_size", 0).toLongLong();
	rec.fileOid = map.value("file_oid", 0).toInt();
	rec.fileFormat = map.value("file_format").toString();
	rec.layerTableName = map.value("layer_table_name").toString();
	rec.changeNote = map.value("change_note").toString();
	rec.changedBy = map.value("changed_by").toString();
	rec.changedAt = QDateTime::fromString(map.value("changed_at").toString(), Qt::ISODate);
	rec.parentVersion = map.value("parent_version", 0).toInt();
	rec.diffInfo = map.value("diff_info").toString();
	return rec;
}

// ===================== DirectoryNode 序列化 =====================
QVariantMap DirectoryNode::toVariantMap() const
{
	QVariantMap map;
	map["id"] = id;
	map["parent_id"] = parentId;
	map["name"] = name;
	map["description"] = description;
	map["sort_order"] = sortOrder;
	map["created_by"] = createdBy;
	map["created_at"] = createdAt.toString(Qt::ISODate);
	return map;
}

DirectoryNode DirectoryNode::fromVariantMap(const QVariantMap& map)
{
	DirectoryNode node;
	node.id = map.value("id", -1).toInt();
	node.parentId = map.value("parent_id", -1).toInt();
	node.name = map.value("name").toString();
	node.description = map.value("description").toString();
	node.sortOrder = map.value("sort_order", 0).toInt();
	node.createdBy = map.value("created_by").toString();
	node.createdAt = QDateTime::fromString(map.value("created_at").toString(), Qt::ISODate);
	return node;
}

// ===================== UserPermission 序列化 =====================
QVariantMap UserPermission::toVariantMap() const
{
	QVariantMap map;
	map["id"] = id;
	map["user_name"] = userName;
	map["role"] = accessRoleToString(role);
	map["password_hash"] = passwordHash;
	map["permissions"] = permissions;
	map["granted_by"] = grantedBy;
	map["granted_at"] = grantedAt.toString(Qt::ISODate);
	return map;
}

UserPermission UserPermission::fromVariantMap(const QVariantMap& map)
{
	UserPermission perm;
	perm.id = map.value("id", -1).toInt();
	perm.userName = map.value("user_name").toString();
	perm.role = stringToAccessRole(map.value("role").toString());
	perm.passwordHash = map.value("password_hash").toString();
	perm.permissions = map.value("permissions").toString();
	perm.grantedBy = map.value("granted_by").toString();
	perm.grantedAt = QDateTime::fromString(map.value("granted_at").toString(), Qt::ISODate);
	return perm;
}
