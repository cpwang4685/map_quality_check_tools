#ifndef PRODUCT_METADATA_H
#define PRODUCT_METADATA_H

#include <QString>
#include <QDateTime>
#include <QDate>
#include <QStringList>
#include <QVariantMap>
#include <QByteArray>
#include <QCryptographicHash>

/**
 * @brief 成果产品类型枚举
 */
enum class ProductType
{
	Vector,      // 矢量数据 (Shapefile, GeoJSON, GDB等)
	Raster,      // 栅格数据 (TIF, IMG, JPEG2000等)
	CAD,         // CAD制图源文件 (DWG, DXF)
	AI,          // Adobe Illustrator文件
	CDR,         // CorelDRAW文件
	PDF,         // PDF制图文件
	Document,    // 文档类 (DOC, DOCX, XLS, XLSX, XML, TXT 等)
	Archive,     // 压缩包 (ZIP, RAR, 7Z, TAR, GZ 等)
	Other        // 其他类型（未识别的格式兜底）
};

/**
 * @brief 密级枚举
 */
enum class SecurityLevel
{
	Unclassified = 0,   // 非密
	Internal = 1,       // 内部
	Confidential = 2,   // 机密
	Secret = 3,         // 秘密
	TopSecret = 4       // 绝密
};

/**
 * @brief 访问角色枚举（按岗位职责划分）
 * 
 * 数据操作员 (DataOperator):   上传成果、编辑元数据、查询预览、版本回溯与差异比对
 * 数据使用者 (DataUser):       查询预览数据、导出数据（只读，不能编辑元数据）
 * 数据库管理员 (DatabaseAdmin): 最高权限，可管理用户、增删人员及所有操作
 */
enum class AccessRole
{
	DataOperator = 1,  // 数据操作员
	DataUser = 2,      // 数据使用者
	DatabaseAdmin = 3, // 数据库管理员（最高权限）
};

// 辅助转换函数
QString productTypeToString(ProductType type);
ProductType stringToProductType(const QString& str);
QString productTypeToDisplayName(ProductType type);  // 表格展示用，文档/压缩包/其他→其它类型
QString securityLevelToString(SecurityLevel level);
SecurityLevel stringToSecurityLevel(const QString& str);
QString accessRoleToString(AccessRole role);
AccessRole stringToAccessRole(const QString& str);
QStringList getAllProductTypeStrings();

/**
 * @brief 成果产品元数据结构（按基础元数据.csv 27项定义）
 *
 * 字段顺序严格对齐 基础元数据.csv 中的序号 1~27
 */
struct ProductMetadata
{
	// ─── CSV 第 1~27 项：业务元数据 ───
	//  1: 数据唯一标识（原 uuid）
	QString dataId;
	//  2: 数据名称
	QString productName;
	//  3: 数据描述
	QString description;
	//  4: 数据来源
	QString source;
	//  5: 版本说明（currentVersion 由系统管理）
	QString versionNote;
	//  6: 数据格式（系统自动检测文件扩展名）
	QString fileFormat;
	//  7: 数据量（字节，系统自动计算）
	qint64 fileSize = 0;
	//  8: 是否压缩
	QString isCompressed;
	//  9: 空间范围（WKT 简要表达）
	QString bounds;
	// 10: 中心点经度
	QString centerLon;
	// 11: 中心点纬度
	QString centerLat;
	// 12: 数据现势性起始
	QDateTime startDatetime;
	// 13: 数据现势性截止
	QDateTime endDatetime;
	// 14: 密级
	SecurityLevel securityLevel = SecurityLevel::Unclassified;
	// 15: 创建人员
	QString createdBy;
	// 16: 创建时间
	QDateTime createdAt;
	// 17: 更新时间
	QDateTime updatedAt;
	// 18: 标签（多个 tag 使用英文分号分割）
	QString tags;
	// 19: 所在市州
	QString city;
	// 20: 比例尺分母
	QString scale;
	// 21: 项目名称
	QString projectName;
	// 22: 生产单位
	QString producer;
	// 23: 生产日期
	QDate productionDate;
	// 24: 坐标参考系
	QString crs;
	// 25: 编制信息
	QString compilationInfo;
	// 26: 汇交情况
	QString deliveryStatus;
	// 27: 汇交时间
	QDateTime deliveryTime;

	// ─── 系统内部字段（不包含在 27 项中） ───
	int id = -1;
	ProductType productType = ProductType::Vector;
	QString filePath;            // 原始文件完整路径
	QString fileHash;            // SHA256 哈希值（去重用）
	int fileOid = 0;             // PostgreSQL Large Object OID（BLOB 存储）
	QString thumbnailPath;       // 缩略图路径

	// 空间元数据（自动提取）
	double minX = 0, minY = 0, maxX = 0, maxY = 0;
	QString spatialExtentWKT;    // 完整空间范围 WKT
	QString geometryType;        // 几何类型（矢量）
	int bandCount = 0;           // 波段数（栅格）
	int pixelWidth = 0;          // 像素宽度（栅格）
	int pixelHeight = 0;         // 像素高度（栅格）
	double pixelResolution = 0;  // 分辨率（栅格）

	// 附加系统字段
	QString approvalNumber;      // 审图号
	int parentDirId = -1;        // 父目录 ID（多级目录）
	QString directoryPath;       // 目录路径（冗余字段，便于检索）
	QString updatedBy;           // 更新人
	QString layerTableName;      // PostGIS 图层表名
	int currentVersion = 1;      // 版本号（系统自动管理）

	// 序列化
	QVariantMap toVariantMap() const;
	static ProductMetadata fromVariantMap(const QVariantMap& map);
};

// ============================================================================
// 数据类型专属元数据结构（扩展表 1:1 关联 product_metadata）
// ============================================================================

/**
 * @brief 矢量数据专属元数据（按「矢量数据元数据.csv」7项定义）
 * 对应 product_vector_meta 表
 */
struct ProductVectorMeta
{
	// ─── CSV 第 1~7 项 ───
	// 1: 几何类型
	QString geomType;
	// 2: 比例尺分母
	int invScale = 0;
	// 3: 坐标系类型（2000大地/投影）
	QString csType;
	// 4: 大地基准（CGCS2000）
	QString geodeticDatum;
	// 5: 地图投影代码
	QString epsgCode;
	// 6: 地图投影描述
	QString projDesc;
	// 7: 属性字段说明（JSON）
	QString fieldDesc;

	// ─── 系统内部字段 ───
	int id = -1;
	int productId = -1;
	qint64 featureCount = 0;     // 要素数量（自动提取）
	int fieldCount = 0;          // 字段数量（自动提取）
	QString layerTableName;      // PostGIS 入库表名

	QVariantMap toVariantMap() const;
	static ProductVectorMeta fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 栅格数据专属元数据（按「栅格数据元数据.csv」14项定义）
 * 对应 product_raster_meta 表
 */
struct ProductRasterMeta
{
	// ─── CSV 第 1~14 项 ───
	// 1: 卫星名称
	QString satelliteName;
	// 2: 传感器类型
	QString sensorType;
	// 3: 成像时间
	QDateTime acquireTime;
	// 4: 影像地面分辨率
	double gsd = 0;
	// 5: 分辨率单位
	QString resolutionUnit;
	// 6: 影像色彩类型
	QString colorType;
	// 7: 颜色级数
	int bitDepth = 0;
	// 8: 波段数
	int bandCount = 0;
	// 9: 无数据值
	double nodataValue = 0;
	// 10: 坐标系类型
	QString csType;
	// 11: 大地基准
	QString geodeticDatum;
	// 12: 地图投影代码
	QString epsgCode;
	// 13: 地图投影描述
	QString projDesc;
	// 14: 平面单位
	QString planarUnit;

	// ─── 系统内部字段 ───
	int id = -1;
	int productId = -1;
	int pixelWidth = 0;          // 像素宽度（自动提取）
	int pixelHeight = 0;         // 像素高度（自动提取）
	QString pixelType;           // 像元类型（自动提取）
	QString layerTableName;      // PostGIS 入库表名

	QVariantMap toVariantMap() const;
	static ProductRasterMeta fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 制图成果专属元数据（按「制图成果数据元数据.csv」22项定义）
 * 对应 product_diagram_meta 表
 */
struct ProductDiagramMeta
{
	// ─── CSV 第 1~22 项 ───
	// 1: 图名
	QString dataId;
	// 2: 图集系列名
	QString mapSeries;
	// 3: 制图区域
	QString cityPrefecture;
	// 4: 比例尺分母
	int mapScale = 0;
	// 5: 地图投影
	QString projDesc;
	// 6: 制图完成日期
	QDate productionDate;
	// 7: 审图号
	QString approvalNo;
	// 8: 制图软件及版本
	QString cartoSoftware;
	// 9: 数据格式
	QString format;
	// 10: 图件尺寸
	QString paperSize;
	// 11: 是否含图例
	bool legendIncluded = false;
	// 12: 修改人
	QString modifier;
	// 13: 最后修改时间
	QDate lastModified;
	// 14: 是否印刷级
	bool printReady = false;
	// 15: 色彩模式
	QString colorMode;
	// 16: 输出分辨率
	int dpi = 0;
	// 17: 项目名称
	QString projName;
	// 18: 关联栅格资产ID
	QString rasterIds;
	// 19: 关联矢量资产ID
	QString vectorIds;
	// 20: 数据现势性截止
	QDateTime endDatetime;
	// 21: 制图人
	QString mapProductor;
	// 22: 是否有数学基础
	bool hasMathBase = false;

	// ─── 系统内部字段 ───
	int id = -1;
	int productId = -1;

	QVariantMap toVariantMap() const;
	static ProductDiagramMeta fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 输出成果专属元数据 (PDF/图片等)
 * 对应 product_output_meta 表
 */
struct ProductOutputMeta
{
	int id = -1;
	int productId = -1;
	int pageCount = 0;                 // 页数
	QString colorMode;                 // 色彩模式 (CMYK/RGB/...)
	QString embeddedFonts;             // 嵌入字体列表
	QString pdfVersion;                // PDF版本
	QString interactiveFeatures;       // 交互功能 (图层/书签/表单)

	QVariantMap toVariantMap() const;
	static ProductOutputMeta fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 文档数据专属元数据（按「文档数据元数据.csv」14项定义）
 * 对应 product_document_meta 表
 */
struct ProductDocumentMeta
{
	// ─── CSV 第 1~14 项 ───
	// 1: 来源
	QString publisher;
	// 2: 文件类型
	QString fileType;
	// 3: 文件格式
	QString format;
	// 4: 文件大小
	QString fileSize;
	// 5: 语言类型
	QString languageType;
	// 6: 关键字
	QString keyWords;
	// 7: 内容摘要
	QString summary;
	// 8: 质量问题记录
	QString qualityIssues;
	// 9: 收集时间
	QDateTime collectTime;
	// 10: 数据现势性截止
	QDateTime endDatetime;
	// 11: 收集人
	QString collector;
	// 12: 收集目的
	QString collectPurpose;
	// 13: 项目名称
	QString projectName;
	// 14: 是否压缩
	bool isCompressed = false;

	// ─── 系统内部字段 ───
	int id = -1;
	int productId = -1;

	QVariantMap toVariantMap() const;
	static ProductDocumentMeta fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 版本记录结构
 */
struct VersionRecord
{
	int id = -1;
	int productId = -1;
	int versionNumber = 1;
	QString filePath;
	QString fileHash;
	qint64 fileSize = 0;
	int fileOid = 0;             // PostgreSQL Large Object OID
	QString fileFormat;          // 文件格式（扩展名，如 tif/shp/pdf）
	QString layerTableName;      // PostGIS 图层表名（矢量数据入库目标表）
	QString changeNote;          // 变更说明
	QString changedBy;
	QDateTime changedAt;
	int parentVersion = 0;       // 父版本号（用于版本树）
	QString diffInfo;            // 差异信息（JSON格式）

	QVariantMap toVariantMap() const;
	static VersionRecord fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 目录节点结构
 */
struct DirectoryNode
{
	int id = -1;
	int parentId = -1;
	QString name;
	QString description;
	int sortOrder = 0;
	QString createdBy;
	QDateTime createdAt;
	// 节点类型：0=普通目录，1=图层节点
	int nodeType = 0;

	QVariantMap toVariantMap() const;
	static DirectoryNode fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 用户权限结构（系统用户表）
 */
struct UserPermission
{
	int id = -1;
	QString userName;
	AccessRole role = AccessRole::DataOperator;
	QString passwordHash;        // SHA-256 哈希后的密码（初始密码由管理员设置）
	QString permissions;         // JSON格式的扩展权限
	QString grantedBy;
	QDateTime grantedAt;

	QVariantMap toVariantMap() const;
	static UserPermission fromVariantMap(const QVariantMap& map);
};

/**
 * @brief 当前登录用户会话
 */
struct UserSession
{
	QString userName;
	AccessRole role = AccessRole::DataOperator;
	bool isLoggedIn = false;

	void clear()
	{
		userName.clear();
		role = AccessRole::DataOperator;
		isLoggedIn = false;
	}

	bool canUploadProduct() const
	{
		return isLoggedIn && (role == AccessRole::DataOperator || role == AccessRole::DatabaseAdmin);
	}
	bool canEditMetadata() const
	{
		return isLoggedIn && (role == AccessRole::DataOperator || role == AccessRole::DatabaseAdmin);
	}
	bool canManageTags() const
	{
		return isLoggedIn && (role == AccessRole::DataOperator || role == AccessRole::DatabaseAdmin);
	}
	bool canVersionDiff() const
	{
		return isLoggedIn && (role == AccessRole::DataOperator || role == AccessRole::DatabaseAdmin);
	}
	bool canManageUsers() const
	{
		return isLoggedIn && role == AccessRole::DatabaseAdmin;
	}
	bool canExportData() const
	{
		return isLoggedIn;  // 所有登录用户均可导出数据
	}
	bool canQueryPreview() const
	{
		return isLoggedIn;  // 所有登录用户均可查询预览
	}
};

// ============================================================================
// 工具函数
// ============================================================================

/** @brief 角色转显示字符串 */
inline QString accessRoleToString(AccessRole role)
{
	switch (role)
	{
	case AccessRole::DataOperator:  return QStringLiteral("数据操作员");
	case AccessRole::DataUser:      return QStringLiteral("数据使用者");
	case AccessRole::DatabaseAdmin: return QStringLiteral("数据库管理员");
	default:                        return QStringLiteral("未知");
	}
}

/** @brief 字符串转角色 */
inline AccessRole stringToAccessRole(const QString& str)
{
	if (str == QStringLiteral("数据操作员"))   return AccessRole::DataOperator;
	if (str == QStringLiteral("数据使用者"))   return AccessRole::DataUser;
	if (str == QStringLiteral("数据库管理员")) return AccessRole::DatabaseAdmin;
	return AccessRole::DataOperator;
}

/** @brief 角色转数字（数据库存储用） */
inline int accessRoleToInt(AccessRole role) { return static_cast<int>(role); }

/** @brief 数字转角色 */
inline AccessRole intToAccessRole(int v)
{
	switch (v)
	{
	case 1: return AccessRole::DataOperator;
	case 2: return AccessRole::DataUser;
	case 3: return AccessRole::DatabaseAdmin;
	default: return AccessRole::DataOperator;
	}
}

/** @brief 计算密码的 SHA-256 哈希 */
inline QString hashPassword(const QString& password)
{
	return QString::fromLatin1(
		QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex()
	);
}

/**
 * @brief 格式化文件大小为可读字符串
 * 
 * 将字节数转换为适合显示的字符串，支持浮点数精度：
 * - 小于 1 KB: "512 B"（显示字节）
 * - 小于 1 MB: "1.5 KB"（保留1位小数）
 * - 大于等于 1 MB: "15.30 MB"（保留2位小数）
 * - 大于等于 1 GB: "1.50 GB"
 */
inline QString formatFileSize(qint64 bytes)
{
	if (bytes < 0) return "N/A";
	if (bytes < 1024)
		return QString::number(bytes) + " B";

	double value = static_cast<double>(bytes);
	if (bytes < 1024 * 1024)
		return QString::number(value / 1024.0, 'f', 1) + " KB";

	value /= 1024.0; // KB
	if (bytes < 1024LL * 1024 * 1024)
		return QString::number(value / 1024.0, 'f', 2) + " MB";

	return QString::number(value / (1024.0 * 1024.0), 'f', 2) + " GB";
}

// ============================================================================
// 全局当前用户会话
// ============================================================================
// 登录成功后由 LoginDialog 填充，各页面读取此变量判断权限
// 使用时需包含此头文件，直接访问 gCurrentUserSession
extern UserSession gCurrentUserSession;

#endif // PRODUCT_METADATA_H
