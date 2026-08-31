#ifndef DATA_IMPORTER_H
#define DATA_IMPORTER_H

#include <QObject>
#include <QString>
#include "database/product_metadata.h"

/**
 * @brief 矢量导入统计信息
 */
struct VectorImportStats
{
	int featureCount = 0;      // 成功写入的要素数
	int skipNoGeom = 0;        // 因无几何跳过的要素数
	int skipInsertFail = 0;    // INSERT 失败的要素数
	int totalRead = 0;         // 总共尝试读取的要素数
	int estimatedCount = 0;    // GDAL 估计的要素数

	/** 是否完全没有任何要素被处理 */
	bool noFeatures() const { return totalRead == 0; }

	/** 是否所有要素都被跳过（无几何） */
	bool allSkipped() const { return totalRead > 0 && featureCount == 0; }

	/** 是否所有INSERT都失败了 */
	bool allInsertFailed() const { return totalRead > 0 && featureCount == 0 && skipInsertFail > 0; }
};

/**
 * @brief 数据完整入库器
 *
 * 使用 GDAL/OGR 将矢量、栅格文件的实际空间数据完整导入 PostGIS，
 * 而不仅仅是存储元数据。入库后在 PostGIS 中创建独立的图层表，
 * QGIS 可通过 PostGIS 连接直接加载和可视化。
 */
class DataImporter : public QObject
{
	Q_OBJECT

public:
	explicit DataImporter(QObject* parent = nullptr);

	/**
	 * @brief 将矢量文件完整导入 PostGIS
	 * @param filePath 源文件路径
	 * @param targetTable 目标 PostGIS 表名
	 * @param srcSrid 源坐标系 EPSG 代码（0 表示自动检测）
	 * @param targetSrid 目标坐标系 EPSG 代码（默认 4490，即 CGCS2000）
	 * @param encoding 源文件编码（默认 AUTO 自动识别：无 .cpg 的 shp 按 GBK 打开）
	 * @return 成功导入的要素数量，-1 表示失败
	 */
	int importVectorToPostGIS(const QString& filePath,
							  const QString& targetTable,
							  int srcSrid = 0,
							  int targetSrid = 4490,
							  const QString& encoding = "AUTO");

	/**
	 * @brief 将矢量文件中的指定图层导入 PostGIS
	 * @param filePath 源文件路径（如 .gdb 目录路径）
	 * @param layerName 要导入的图层名称
	 * @param targetTable 目标 PostGIS 表名
	 * @param srcSrid 源坐标系 EPSG 代码（0 表示自动检测）
	 * @param targetSrid 目标坐标系 EPSG 代码（默认 4490）
	 * @param encoding 字符编码（默认 AUTO 自动识别：无 .cpg 的 shp 按 GBK 打开）
	 * @return 成功导入的要素数量，-1 表示失败
	 */
	int importVectorLayerToPostGIS(const QString& filePath,
								   const QString& layerName,
								   const QString& targetTable,
								   int srcSrid = 0,
								   int targetSrid = 4490,
								   const QString& encoding = "AUTO");

	/**
	 * @brief 将栅格文件完整导入 PostGIS（使用 raster2pgsql 或 GDAL 写入）
	 * @param filePath 源文件路径
	 * @param targetTable 目标 PostGIS 表名
	 * @param targetSrid 目标坐标系 EPSG 代码
	 * @return 是否成功
	 *
	 * 注意：栅格入库需要 PostGIS raster 扩展支持。
	 * 如果 PostGIS 没有 raster 支持，则降级为只存储元数据。
	 */
	bool importRasterToPostGIS(const QString& filePath,
							   const QString& targetTable,
							   int targetSrid = 4490);

	/**
	 * @brief 获取最后一次错误信息
	 */
	QString lastError() const { return m_lastError; }

	/**
	 * @brief 获取最近一次矢量导入的详细统计信息
	 */
	const VectorImportStats& lastVectorStats() const { return m_lastVectorStats; }

signals:
	void importProgress(int percent, const QString& message);
	void importCompleted(const QString& tableName, int featureCount);
	void importFailed(const QString& tableName, const QString& error);

private:
	/**
	 * @brief 通过 OGR 创建 PostGIS 图层表并写入数据
	 *
	 * 使用 GDAL PG driver 直接操作 PostGIS，
	 * 自动创建表结构（字段映射）并逐要素写入。
	 */
	int importViaOGRPGDriver(const QString& filePath,
							 const QString& targetTable,
							 int targetSrid,
							 const QString& encoding);

	/**
	 * @brief 获取 PostGIS 连接字符串
	 */
	QString getPGConnectionString() const;

	QString m_lastError;
	VectorImportStats m_lastVectorStats;
};

#endif // DATA_IMPORTER_H
