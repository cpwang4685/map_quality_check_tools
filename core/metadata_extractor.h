#ifndef METADATA_EXTRACTOR_H
#define METADATA_EXTRACTOR_H

#include <QObject>
#include <QString>
#include <QVariantMap>
#include "database/product_metadata.h"

// 前置声明 GDAL 类型，避免头文件依赖
class OGRSpatialReference;

/**
 * @brief 元数据自动提取器
 * 
 * 使用GDAL库自动提取矢量、栅格等空间数据的元数据信息，
 * 包括空间范围、坐标系、几何类型、波段数、分辨率等。
 */
class MetadataExtractor : public QObject
{
	Q_OBJECT

public:
	explicit MetadataExtractor(QObject* parent = nullptr);

	/**
	 * @brief 从文件自动提取元数据
	 * @param filePath 文件路径
	 * @return 提取到的元数据结构
	 */
	ProductMetadata extractMetadata(const QString& filePath);

	/**
	 * @brief 提取矢量数据元数据
	 */
	ProductMetadata extractVectorMetadata(const QString& filePath);

	/**
	 * @brief 提取栅格数据元数据
	 */
	ProductMetadata extractRasterMetadata(const QString& filePath);

	/**
	 * @brief 提取制图成果专属元数据（AI/CAD/CDR 文件基本信息）
	 * @param filePath 制图源文件路径
	 * @return 制图成果专属元数据结构
	 */
	ProductDiagramMeta extractDiagramTypeMeta(const QString& filePath);

	/**
	 * @brief 提取文档数据专属元数据（PDF 文件基本信息）
	 * @param filePath 文档文件路径
	 * @return 文档专属元数据结构
	 */
	ProductDocumentMeta extractDocumentTypeMeta(const QString& filePath);

	/**
	 * @brief 提取PDF元数据（支持GeoPDF空间信息）
	 */
	ProductMetadata extractPDFMetadata(const QString& filePath);

	/**
	 * @brief 提取GDB数据集中的指定图层空间元数据
	 * @param datasetPath GDB路径
	 * @param layerName 图层名称
	 * @return 包含CRS、几何类型、空间范围的元数据
	 */
	ProductMetadata extractLayerMetadata(const QString& datasetPath, const QString& layerName);

	/**
	 * @brief 提取矢量专属元数据（要素数、字段详情等）
	 * @param filePath 矢量文件路径
	 * @return 矢量专属元数据结构
	 */
	ProductVectorMeta extractVectorTypeMeta(const QString& filePath);

	/**
	 * @brief 提取栅格专属元数据（位深、色彩空间、压缩方法等）
	 * @param filePath 栅格文件路径
	 * @return 栅格专属元数据结构
	 */
	ProductRasterMeta extractRasterTypeMeta(const QString& filePath);

	/**
	 * @brief 识别产品类型
	 */
	static ProductType detectProductType(const QString& filePath);

	/**
	 * @brief 生成缩略图
	 * @param filePath 源文件路径
	 * @param outputPath 缩略图输出路径
	 * @param maxSize 最大尺寸（像素）
	 * @return 是否成功
	 */
	static bool generateThumbnail(const QString& filePath, const QString& outputPath, int maxSize = 256);

	/**
	 * @brief 计算文件SHA256哈希
	 */
	static QString calculateFileHash(const QString& filePath);

	/**
	 * @brief 获取文件MIME类型
	 */
	static QString getFileMimeType(const QString& filePath);

signals:
	void extractionProgress(int percent, const QString& message);
	void extractionCompleted(const QString& filePath);
	void extractionFailed(const QString& filePath, const QString& error);

private:
	// GDAL相关辅助函数
	QVariantMap extractGDALMetadata(const QString& filePath);

	/**
	 * @brief 从 OGRSpatialReference 提取简短的 CRS 标识
	 * 
	 * 优先级：EPSG代码 > 坐标系统名称 > 截断WKT
	 * 保证返回值不超过 maxLen 字符，避免数据库列溢出。
	 */
	static QString extractCRS(OGRSpatialReference* poSRS, int maxLen = 200);
};

#endif // METADATA_EXTRACTOR_H
