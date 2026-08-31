#ifndef PRODUCT_DAO_H
#define PRODUCT_DAO_H

#include <QObject>
#include <QList>
#include "product_metadata.h"

/**
 * @brief 产品数据访问对象
 * 
 * 封装所有对product_metadata及相关表的CRUD操作。
 */
class ProductDAO : public QObject
{
	Q_OBJECT

public:
	explicit ProductDAO(QObject* parent = nullptr);

	// ===== 产品CRUD =====
	int insertProduct(const ProductMetadata& meta);
	bool updateProduct(const ProductMetadata& meta);
	bool deleteProduct(int productId);
	ProductMetadata getProduct(int productId);
	ProductMetadata getProductByUUID(const QString& uuid);
	QList<ProductMetadata> getAllProducts();
	QList<ProductMetadata> getProductsByType(ProductType type);
	QList<ProductMetadata> getProductsByDirectory(int dirId);

	// 收集某目录及其所有子孙目录 id（含自身），用于按分类检索整个子树
	QList<int> collectSubtreeDirectoryIds(int rootDirId);
	QList<ProductMetadata> getProductsBySecurityLevel(SecurityLevel level);

	// ===== 多条件检索 =====
	struct SearchCondition
	{
		QString keyword;            // 名称/描述关键词
		ProductType productType = ProductType::Other;    // 产品类型（默认不限）
		QList<ProductType> productTypes;  // 多类型同时筛选（优先级高于 productType）
		QString scale;              // 比例尺
		SecurityLevel securityLevel;// 密级（-1表示不限，用特殊值）
		QString producer;           // 生产单位
		QString approvalNumber;     // 审图号
		QString dateFrom;           // 起始日期
		QString dateTo;             // 结束日期
		QStringList tags;           // 标签
		int directoryId = -1;       // 目录ID
		QList<int> directoryIds;    // 目录集合（含子树，非空时优先用于过滤）
		double minX = 0, minY = 0, maxX = 0, maxY = 0; // 空间范围
		bool hasSpatialFilter = false;
		int limit = 100;
		int offset = 0;
	};
	QList<ProductMetadata> searchProducts(const SearchCondition& cond);
	int countSearchResults(const SearchCondition& cond);

	/**
	 * @brief 按文件哈希查找产品（用于去重）
	 * @param hash SHA256 哈希值
	 * @return 找到返回产品元数据，未找到返回空的 ProductMetadata
	 */
	ProductMetadata findByHash(const QString& hash);

	// ===== 版本管理 =====
	int insertVersionRecord(const VersionRecord& record);
	QList<VersionRecord> getVersionHistory(int productId);
	VersionRecord getVersionRecord(int productId, int versionNumber);
	bool rollbackToVersion(int productId, int versionNumber, const QString& operatorName);
	QList<VersionRecord> compareVersions(int productId, int versionA, int versionB);

	/**
	 * @brief 恢复目标版本的本地文件到当前工作目录
	 *
	 * 对于 PDF/CAD/AI/CDR 等仅本地存储的类型，从 FileStorageManager
	 * 版本化目录中复制目标版本的文件到新版本目录，确保 filePath 指向
	 * 可访问的实际文件。
	 * 对于栅格数据，同步恢复本地文件副本。
	 * 对于矢量数据，仅恢复本地文件（PostGIS 表通过 layerTableName 引用，无需恢复）。
	 *
	 * @param productUUID 产品 UUID
	 * @param targetVersion 目标版本号
	 * @param newVersion 新版本号（回退后生成的新版本）
	 * @param targetFilePath 目标版本记录的 filePath（从中提取文件名）
	 * @return 恢复后的本地文件路径，失败返回空字符串
	 */
	QString restoreLocalFile(const QString& productUUID, int targetVersion,
							  int newVersion, const QString& targetFilePath);

	/**
	 * @brief 复制旧版本 PostGIS 矢量表到新表名
	 *
	 * 为回退后的新版本创建独立的矢量数据副本，
	 * 避免新版本与目标版本共享同一张表导致后续修改互相影响。
	 *
	 * @param sourceTableName 源表名（目标版本的表）
	 * @param targetTableName 目标表名（为新版本创建的新表）
	 * @return 是否成功
	 */
	bool cloneVectorTable(const QString& sourceTableName, const QString& targetTableName);

	// ===== 目录管理 =====
	int insertDirectory(const DirectoryNode& node);
	bool updateDirectory(const DirectoryNode& node);
	bool deleteDirectory(int dirId);
	QList<DirectoryNode> getChildDirectories(int parentId);
	DirectoryNode getDirectory(int dirId);
	QList<DirectoryNode> getAllDirectories();

	/**
	 * @brief 按名称查找或创建目录（用于自动分类）
	 * @param name 目录名称
	 * @param parentId 父目录ID（0=根级）
	 * @return 目录ID，失败返回 -1
	 *
	 * 如果指定名称的目录已存在，直接返回其ID；
	 * 否则自动创建该目录并返回新ID。
	 */
	int findOrCreateDirectory(const QString& name, int parentId = 0, int nodeType = 0);

	// ===== 标签管理 =====
	bool addTag(const QString& name, const QString& color = "", const QString& desc = "");
	bool removeTag(int tagId);
	QVariantList getAllTags();
	bool setProductTags(int productId, const QStringList& tags);
	QStringList getProductTags(int productId);

	// ===== 权限管理 =====
	bool setUserPermission(const UserPermission& perm);
	bool removeUserPermission(const QString& userName);
	UserPermission getUserPermission(const QString& userName);
	QList<UserPermission> getAllPermissions();
	AccessRole getUserAccessRole(const QString& userName);

	// ===== 用户认证 =====
	/**
	 * @brief 验证用户登录（用户名+密码）
	 * @return 成功返回填充的 UserPermission（不含密码哈希），失败返回 id=-1 的空结构
	 */
	UserPermission authenticateUser(const QString& userName, const QString& password);

	/**
	 * @brief 添加用户并设置初始密码（仅数据库管理员可调用）
	 * @return 成功返回 true
	 */
	bool addUserWithPassword(const QString& userName, const QString& password,
		AccessRole role, const QString& grantedBy);

	/**
	 * @brief 修改用户密码
	 */
	bool changePassword(const QString& userName, const QString& newPassword);

	/**
	 * @brief 修改用户角色
	 */
	bool changeUserRole(const QString& userName, AccessRole newRole, const QString& grantedBy);

	// ===== 审计日志 =====
	bool writeAuditLog(int productId, const QString& action, const QString& detail, const QString& operatorName);
	QVariantList getAuditLogs(int productId, int limit = 50);

	// ===== 图层注册（完整入库） =====
	/**
	 * @brief 注册入库的 PostGIS 图层表
	 * @param productId 产品 ID
	 * @param tableName PostGIS 图层表名
	 * @param geometryType 几何类型
	 * @param srid 坐标系 SRID
	 * @param featureCount 要素数量
	 */
	bool registerLayer(int productId, const QString& tableName,
					   const QString& geometryType, int srid, int featureCount);

	/**
	 * @brief 注销图层（删除产品时同时删除关联的图层表）
	 */
	bool unregisterLayer(int productId);

	/**
	 * @brief 获取产品关联的图层表名
	 */
	QString getLayerTableName(int productId);

	// ===== 数据类型专属元数据 =====
	/**
	 * @brief 保存矢量专属元数据（INSERT 或 UPDATE）
	 */
	bool saveVectorMeta(const ProductVectorMeta& meta);
	/**
	 * @brief 获取矢量专属元数据
	 */
	ProductVectorMeta getVectorMeta(int productId);

	/**
	 * @brief 保存栅格专属元数据
	 */
	bool saveRasterMeta(const ProductRasterMeta& meta);
	/**
	 * @brief 获取栅格专属元数据
	 */
	ProductRasterMeta getRasterMeta(int productId);

	/**
	 * @brief 保存制图成果专属元数据
	 */
	bool saveDiagramMeta(const ProductDiagramMeta& meta);
	/**
	 * @brief 获取制图成果专属元数据
	 */
	ProductDiagramMeta getDiagramMeta(int productId);
	/**
	 * @brief 保存输出成果专属元数据
	 */
	bool saveOutputMeta(const ProductOutputMeta& meta);
	/**
	 * @brief 保存文档专属元数据
	 */
	bool saveDocumentMeta(const ProductDocumentMeta& meta);
	/**
	 * @brief 获取文档专属元数据
	 */
	ProductDocumentMeta getDocumentMeta(int productId);

	/**
	 * @brief 获取完整的元数据（主表 + 类型专属扩展表 JOIN）
	 * @param productId 产品 ID
	 * @param meta 输出主表元数据
	 * @param vectorMeta 输出矢量专属元数据（若类型非矢量则为空）
	 * @param rasterMeta 输出栅格专属元数据（若类型非栅格则为空）
	 */
	void getFullProductMetadata(int productId, ProductMetadata& meta,
		ProductVectorMeta& vectorMeta, ProductRasterMeta& rasterMeta);

	/**
	 * @brief 入库后提取并写入类型专属扩展元数据
	 *        根据 product_metadata 表中的 product_type 和 file_path 
	 *        自动提取矢量/栅格专属字段写入对应扩展表
	 * @param productId 产品 ID
	 */
	void enrichWithTypeMeta(int productId);

signals:
	void operationCompleted(const QString& operation);
	void operationFailed(const QString& operation, const QString& error);

private:
	QString buildSearchSQL(const SearchCondition& cond, bool countOnly);
	QVariantList buildSearchParams(const SearchCondition& cond);
};

#endif // PRODUCT_DAO_H
