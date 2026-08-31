#include "metadata_extractor.h"
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

// GDAL headers
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "ogr_spatialref.h"
#include "ogr_geometry.h"
#include "cpl_conv.h"
#include "cpl_string.h"

MetadataExtractor::MetadataExtractor(QObject* parent)
	: QObject(parent)
{
	// 确保GDAL已注册
	static bool gdalRegistered = false;
	if (!gdalRegistered)
	{
		GDALAllRegister();
		gdalRegistered = true;
	}
}

ProductMetadata MetadataExtractor::extractMetadata(const QString& filePath)
{
	QFileInfo fi(filePath);
	if (!fi.exists())
	{
		emit extractionFailed(filePath, "文件不存在");
		return ProductMetadata();
	}

	ProductMetadata meta;
	meta.productName = fi.baseName();
	meta.filePath = filePath;
	meta.fileFormat = fi.suffix().toLower();
	meta.fileSize = fi.size();
	meta.fileHash = calculateFileHash(filePath);
	meta.productType = detectProductType(filePath);

	emit extractionProgress(10, "正在识别产品类型...");

	// 根据类型提取空间元数据
	switch (meta.productType)
	{
	case ProductType::Vector:
		meta = extractVectorMetadata(filePath);
		meta.productName = fi.baseName();
		meta.filePath = filePath;
		meta.fileFormat = fi.suffix().toLower();
		meta.fileSize = fi.size();
		meta.fileHash = calculateFileHash(filePath);
		meta.productType = ProductType::Vector;
		break;
	case ProductType::Raster:
		meta = extractRasterMetadata(filePath);
		meta.productName = fi.baseName();
		meta.filePath = filePath;
		meta.fileFormat = fi.suffix().toLower();
		meta.fileSize = fi.size();
		meta.fileHash = calculateFileHash(filePath);
		meta.productType = ProductType::Raster;
		break;
	case ProductType::PDF:
		meta = extractPDFMetadata(filePath);
		meta.productName = fi.baseName();
		meta.filePath = filePath;
		meta.fileFormat = fi.suffix().toLower();
		meta.fileSize = fi.size();
		meta.fileHash = calculateFileHash(filePath);
		meta.productType = ProductType::PDF;
		break;
	default:
		// 非空间数据，只提取基本文件信息
		break;
	}

	emit extractionProgress(100, "元数据提取完成");
	emit extractionCompleted(filePath);
	return meta;
}

ProductMetadata MetadataExtractor::extractVectorMetadata(const QString& filePath)
{
	ProductMetadata meta;

	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));

	if (!poDS)
	{
		emit extractionFailed(filePath, QString("GDAL无法打开矢量文件: %1").arg(CPLGetLastErrorMsg()));
		return meta;
	}

	emit extractionProgress(30, "正在提取矢量图层信息...");

	// 提取图层信息
	int layerCount = poDS->GetLayerCount();

	for (int i = 0; i < layerCount; ++i)
	{
		OGRLayer* poLayer = poDS->GetLayer(i);
		if (!poLayer) continue;

		// 空间参考系
		OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
		if (poSRS)
		{
			meta.crs = extractCRS(poSRS);
		}

		// 几何类型
		OGRwkbGeometryType geomType = poLayer->GetGeomType();
		meta.geometryType = QString::fromUtf8(OGRGeometryTypeToName(geomType));

		// 空间范围
		OGREnvelope oExt;
		if (poLayer->GetExtent(&oExt, TRUE) == OGRERR_NONE)
		{
			meta.minX = oExt.MinX;
			meta.minY = oExt.MinY;
			meta.maxX = oExt.MaxX;
			meta.maxY = oExt.MaxY;

			// 构建WKT范围
			meta.spatialExtentWKT = QString("POLYGON((%1 %2, %3 %2, %3 %4, %1 %4, %1 %2))")
				.arg(meta.minX, 0, 'f', 6)
				.arg(meta.minY, 0, 'f', 6)
				.arg(meta.maxX, 0, 'f', 6)
				.arg(meta.maxY, 0, 'f', 6);
		}

		break; // 只取第一个图层的元数据
	}

	emit extractionProgress(70, "矢量元数据提取完成");
	GDALClose(poDS);
	return meta;
}

ProductMetadata MetadataExtractor::extractLayerMetadata(const QString& datasetPath, const QString& layerName)
{
	ProductMetadata meta;

	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(datasetPath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));

	if (!poDS)
	{
		emit extractionFailed(datasetPath,
			QString("extractLayerMetadata: GDAL无法打开数据集: %1").arg(CPLGetLastErrorMsg()));
		return meta;
	}

	// 按名称查找图层
	OGRLayer* poLayer = poDS->GetLayerByName(layerName.toUtf8().constData());
	if (!poLayer)
	{
		// 按索引遍历查找
		int layerCount = poDS->GetLayerCount();
		for (int i = 0; i < layerCount; ++i)
		{
			OGRLayer* pL = poDS->GetLayer(i);
			if (pL && QString::fromUtf8(pL->GetName()) == layerName)
			{
				poLayer = pL;
				break;
			}
		}
	}

	if (!poLayer)
	{
		emit extractionFailed(datasetPath,
			QString("extractLayerMetadata: 未找到图层 \"%1\"").arg(layerName));
		GDALClose(poDS);
		return meta;
	}

	// 空间参考系
	OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
	if (poSRS)
	{
		meta.crs = extractCRS(poSRS);
	}

	// 几何类型
	OGRwkbGeometryType geomType = poLayer->GetGeomType();
	meta.geometryType = QString::fromUtf8(OGRGeometryTypeToName(geomType));

	// 空间范围
	OGREnvelope oExt;
	if (poLayer->GetExtent(&oExt, TRUE) == OGRERR_NONE)
	{
		meta.minX = oExt.MinX;
		meta.minY = oExt.MinY;
		meta.maxX = oExt.MaxX;
		meta.maxY = oExt.MaxY;

		meta.spatialExtentWKT = QString("POLYGON((%1 %2, %3 %2, %3 %4, %1 %4, %1 %2))")
			.arg(meta.minX, 0, 'f', 6)
			.arg(meta.minY, 0, 'f', 6)
			.arg(meta.maxX, 0, 'f', 6)
			.arg(meta.maxY, 0, 'f', 6);
	}

	GDALClose(poDS);
	return meta;
}

ProductMetadata MetadataExtractor::extractRasterMetadata(const QString& filePath)
{
	ProductMetadata meta;

	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_RASTER, nullptr, nullptr, nullptr));

	if (!poDS)
	{
		emit extractionFailed(filePath, QString("GDAL无法打开栅格文件: %1").arg(CPLGetLastErrorMsg()));
		return meta;
	}

	emit extractionProgress(30, "正在提取栅格信息...");

	// 尺寸
	meta.pixelWidth = poDS->GetRasterXSize();
	meta.pixelHeight = poDS->GetRasterYSize();

	// 波段数
	meta.bandCount = poDS->GetRasterCount();

	// 空间参考系
	const char* pszWKT = poDS->GetProjectionRef();
	if (pszWKT && strlen(pszWKT) > 0)
	{
		OGRSpatialReference oSRS(pszWKT);
		meta.crs = extractCRS(&oSRS);
	}

	// 空间范围和分辨率
	double adfGeoTransform[6];
	if (poDS->GetGeoTransform(adfGeoTransform) == CE_None)
	{
		meta.minX = adfGeoTransform[0];
		meta.maxY = adfGeoTransform[3];
		meta.maxX = adfGeoTransform[0] + adfGeoTransform[1] * meta.pixelWidth;
		meta.minY = adfGeoTransform[3] + adfGeoTransform[5] * meta.pixelHeight;
		meta.pixelResolution = qAbs(adfGeoTransform[1]);

		meta.spatialExtentWKT = QString("POLYGON((%1 %2, %3 %2, %3 %4, %1 %4, %1 %2))")
			.arg(meta.minX, 0, 'f', 6)
			.arg(meta.minY, 0, 'f', 6)
			.arg(meta.maxX, 0, 'f', 6)
			.arg(meta.maxY, 0, 'f', 6);
	}

	emit extractionProgress(70, "栅格元数据提取完成");
	GDALClose(poDS);
	return meta;
}

ProductMetadata MetadataExtractor::extractPDFMetadata(const QString& filePath)
{
	ProductMetadata meta;

	// ============================================================
	// 使用 GDAL PDF 矢量驱动 (GDAL_OF_VECTOR) 读取 GeoPDF
	// GeoPDF 矢量驱动直接解析 PDF 内部的矢量要素图层，
	// 通过 OGR 图层 API 获取 CRS、空间范围、几何类型等空间元数据。
	//
	// 注意：对于没有 PDF 逻辑结构标记的非结构化 PDF，
	// 需要设置配置选项 OGR_PDF_READ_NON_STRUCTURED=YES
	// 才能尝试读取其中的矢量几何图形。
	// ============================================================

	// 先尝试默认方式（结构化 PDF）打开
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));

	// 如果结构化方式打不开，设置非结构化读取标志后重试
	if (!poDS)
	{
		CPLSetConfigOption("OGR_PDF_READ_NON_STRUCTURED", "YES");
		poDS = static_cast<GDALDataset*>(
			GDALOpenEx(filePath.toUtf8().constData(),
					   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
		CPLSetConfigOption("OGR_PDF_READ_NON_STRUCTURED", nullptr);
	}

	if (!poDS)
	{
		emit extractionProgress(50, "PDF无法通过GDAL矢量驱动打开（可能为普通PDF或缺少Poppler/PoDoFo/PDFium后端），跳过空间元数据提取");
		return meta;
	}

	emit extractionProgress(30, "正在通过GDAL PDF矢量驱动提取GeoPDF空间信息...");

	// 遍历所有矢量图层，提取空间元数据
	int layerCount = poDS->GetLayerCount();
	bool hasSpatialInfo = false;

	for (int i = 0; i < layerCount; ++i)
	{
		OGRLayer* poLayer = poDS->GetLayer(i);
		if (!poLayer) continue;

		// 提取空间参考系 (CRS)
		OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
		if (poSRS && !hasSpatialInfo)
		{
			meta.crs = extractCRS(poSRS);
		}

		// 提取几何类型
		OGRwkbGeometryType geomType = poLayer->GetGeomType();
		if (geomType != wkbUnknown)
		{
			meta.geometryType = QString::fromUtf8(OGRGeometryTypeToName(geomType));
		}

		// 提取空间范围
		OGREnvelope oExt;
		if (poLayer->GetExtent(&oExt, TRUE) == OGRERR_NONE)
		{
			if (!hasSpatialInfo)
			{
				// 第一个有范围的图层
				meta.minX = oExt.MinX;
				meta.minY = oExt.MinY;
				meta.maxX = oExt.MaxX;
				meta.maxY = oExt.MaxY;
				hasSpatialInfo = true;
			}
			else
			{
				// 合并多图层范围
				if (oExt.MinX < meta.minX) meta.minX = oExt.MinX;
				if (oExt.MinY < meta.minY) meta.minY = oExt.MinY;
				if (oExt.MaxX > meta.maxX) meta.maxX = oExt.MaxX;
				if (oExt.MaxY > meta.maxY) meta.maxY = oExt.MaxY;
			}
		}

		// 如果有空间信息了就跳出（也可以继续遍历合并范围）
		// break; // 若只需首个图层可取消注释
	}

	if (!hasSpatialInfo)
	{
		emit extractionProgress(50, "PDF不含地理空间参考信息（非GeoPDF）");
		GDALClose(poDS);
		return meta;
	}

	// 构建空间范围 WKT
	meta.spatialExtentWKT = QString("POLYGON((%1 %2, %3 %2, %3 %4, %1 %4, %1 %2))")
		.arg(meta.minX, 0, 'f', 6)
		.arg(meta.minY, 0, 'f', 6)
		.arg(meta.maxX, 0, 'f', 6)
		.arg(meta.maxY, 0, 'f', 6);

	emit extractionProgress(70, "GeoPDF空间元数据提取完成");
	GDALClose(poDS);
	return meta;
}

// ===================== 数据类型专属元数据提取 =====================

ProductVectorMeta MetadataExtractor::extractVectorTypeMeta(const QString& filePath)
{
	ProductVectorMeta meta;

	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));

	if (!poDS)
		return meta;

	emit extractionProgress(50, "正在提取矢量专属元数据...");

	// 合计所有图层信息
	for (int i = 0; i < poDS->GetLayerCount(); ++i)
	{
		OGRLayer* poLayer = poDS->GetLayer(i);
		if (!poLayer) continue;

		// 要素数量
		GIntBig featureCount = poLayer->GetFeatureCount(TRUE);
		if (featureCount < 0) featureCount = 0;
		meta.featureCount += featureCount;

		// 几何类型（取第一个图层的）
		if (meta.geomType.isEmpty())
		{
			OGRwkbGeometryType geomType = poLayer->GetGeomType();
			meta.geomType = QString::fromUtf8(OGRGeometryTypeToName(geomType));
		}

		// 字段详情（属性字段说明）
		OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
		if (poDefn)
		{
			int fieldCount = poDefn->GetFieldCount();
			meta.fieldCount += fieldCount;

			QJsonArray fieldsArray;
			for (int j = 0; j < fieldCount; ++j)
			{
				OGRFieldDefn* poField = poDefn->GetFieldDefn(j);
				QJsonObject fieldObj;
				fieldObj["name"] = QString::fromUtf8(poField->GetNameRef());
				fieldObj["type"] = QString::fromUtf8(OGRFieldDefn::GetFieldTypeName(poField->GetType()));
				fieldObj["width"] = poField->GetWidth();
				fieldObj["precision"] = poField->GetPrecision();
				fieldsArray.append(fieldObj);
			}
			meta.fieldDesc = QString::fromUtf8(
				QJsonDocument(fieldsArray).toJson(QJsonDocument::Compact));
		}
	}

	emit extractionProgress(80, "矢量专属元数据提取完成");
	GDALClose(poDS);
	return meta;
}

ProductRasterMeta MetadataExtractor::extractRasterTypeMeta(const QString& filePath)
{
	ProductRasterMeta meta;

	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_RASTER, nullptr, nullptr, nullptr));

	if (!poDS)
		return meta;

	emit extractionProgress(50, "正在提取栅格专属元数据...");

	// 波段数
	meta.bandCount = poDS->GetRasterCount();

	// 尺寸
	meta.pixelWidth = poDS->GetRasterXSize();
	meta.pixelHeight = poDS->GetRasterYSize();

	// 分辨率 → 影像地面分辨率 gsd
	double adfGeoTransform[6];
	if (poDS->GetGeoTransform(adfGeoTransform) == CE_None)
	{
		meta.gsd = qAbs(adfGeoTransform[1]);
	}

	// 波段详情
	if (meta.bandCount > 0)
	{
		GDALRasterBand* poBand = poDS->GetRasterBand(1);
		if (poBand)
		{
			// 像元类型
			meta.pixelType = QString::fromUtf8(
				GDALGetDataTypeName(poBand->GetRasterDataType()));

			// 无数据值
			int bNoData = 0;
			double noData = poBand->GetNoDataValue(&bNoData);
			if (bNoData)
				meta.nodataValue = noData;

			// 位深 (从 GDAL 数据类型推算)
			switch (poBand->GetRasterDataType())
			{
			case GDT_Byte:    meta.bitDepth = 8;  break;
			case GDT_UInt16:  meta.bitDepth = 16; break;
			case GDT_Int16:   meta.bitDepth = 16; break;
			case GDT_UInt32:  meta.bitDepth = 32; break;
			case GDT_Int32:   meta.bitDepth = 32; break;
			case GDT_Float32: meta.bitDepth = 32; break;
			case GDT_Float64: meta.bitDepth = 64; break;
			default:          meta.bitDepth = 0;  break;
			}

			// 色彩类型
			switch (poBand->GetColorInterpretation())
			{
			case GCI_RedBand:   meta.colorType = "RGB";   break;
			case GCI_GrayIndex: meta.colorType = "灰度";    break;
			case GCI_PaletteIndex: meta.colorType = "调色板"; break;
			default: meta.colorType = "未知"; break;
			}

			// 色彩类型精确判断（多波段）
			if (meta.bandCount >= 3)
			{
				GDALRasterBand* pB2 = poDS->GetRasterBand(2);
				GDALRasterBand* pB3 = poDS->GetRasterBand(3);
				if (pB2 && pB3 &&
					poBand->GetColorInterpretation() == GCI_RedBand &&
					pB2->GetColorInterpretation() == GCI_GreenBand &&
					pB3->GetColorInterpretation() == GCI_BlueBand)
				{
					meta.colorType = "RGB";
				}
				if (meta.bandCount >= 4)
				{
					meta.colorType = "RGBA";
				}
			}
		}
	}

	// 成像时间（从 GDAL 元数据读取）
	const char* pszDate = poDS->GetMetadataItem("ACQUISITIONDATETIME");
	if (!pszDate)
		pszDate = poDS->GetMetadataItem("TIFFTAG_DATETIME");
	if (pszDate)
		meta.acquireTime = QDateTime::fromString(QString::fromUtf8(pszDate), Qt::ISODate);

	emit extractionProgress(80, "栅格专属元数据提取完成");
	GDALClose(poDS);
	return meta;
}

ProductDiagramMeta MetadataExtractor::extractDiagramTypeMeta(const QString& filePath)
{
	ProductDiagramMeta meta;
	QFileInfo fi(filePath);
	if (!fi.exists()) return meta;

	emit extractionProgress(30, "正在提取制图成果元数据...");

	// 图名 = 文件名
	meta.dataId = fi.baseName();

	// 制图完成日期 / 最后修改时间
	QDateTime lastMod = fi.lastModified();
	meta.productionDate = lastMod.date();
	meta.lastModified = lastMod.date();

	// 数据格式
	meta.format = fi.suffix().toLower();

	// 制图软件及版本（根据扩展名推测）
	QString ext = meta.format.toUpper();
	if (ext == "AI")
		meta.cartoSoftware = "Adobe Illustrator";
	else if (ext == "CDR")
		meta.cartoSoftware = "CorelDRAW";
	else if (ext == "CAD" || ext == "DWG")
		meta.cartoSoftware = "AutoCAD";
	else
		meta.cartoSoftware = ext;

	// 默认色彩模式
	meta.colorMode = "RGB";

	// 默认输出分辨率（可后期调整）
	meta.dpi = 300;

	// 尝试通过 GDAL 打开 AI 文件读取更多信息（AI 兼容 PDF 结构）
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
	if (poDS)
	{
		emit extractionProgress(60, "正在分析制图文件空间信息...");

		// 尝试提取空间参考和范围
		for (int i = 0; i < poDS->GetLayerCount(); ++i)
		{
			OGRLayer* poLayer = poDS->GetLayer(i);
			if (!poLayer) continue;

			OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
			if (poSRS && meta.projDesc.isEmpty())
			{
				meta.projDesc = extractCRS(poSRS);
			}

			OGREnvelope extent;
			if (poLayer->GetExtent(&extent, TRUE) == OGRERR_NONE)
			{
				// 尝试推算比例尺分母（粗略估算）
				double dx = extent.MaxX - extent.MinX;
				if (dx > 0 && meta.mapScale == 0)
				{
					// 近似：1 地图单位 ≈ 现实米
					meta.mapScale = static_cast<int>(dx / 0.3); // 粗略估算
				}
			}
			break;  // 只取第一个图层的参考信息
		}
		emit extractionProgress(80, "制图文件空间分析完成");
		GDALClose(poDS);
	}
	else
	{
		emit extractionProgress(70, "无法通过GDAL打开制图文件，仅提取基本信息");
	}

	emit extractionProgress(100, "制图成果元数据提取完成");
	return meta;
}

ProductDocumentMeta MetadataExtractor::extractDocumentTypeMeta(const QString& filePath)
{
	ProductDocumentMeta meta;
	QFileInfo fi(filePath);
	if (!fi.exists()) return meta;

	emit extractionProgress(30, "正在提取文档元数据...");

	// 文件类型
	meta.fileType = fi.suffix().toLower();

	// 文件格式
	meta.format = fi.suffix().toLower();

	// 文件大小
	qint64 size = fi.size();
	if (size >= 1024 * 1024)
		meta.fileSize = QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
	else if (size >= 1024)
		meta.fileSize = QString("%1 KB").arg(size / 1024.0, 0, 'f', 1);
	else
		meta.fileSize = QString("%1 B").arg(size);

	// 收集时间 = 文件最后修改时间
	QDateTime lastMod = fi.lastModified();
	meta.collectTime = lastMod;
	meta.endDatetime = lastMod;

	// 来源（默认文件路径所在目录名）
	QDir parentDir = fi.absoluteDir();
	meta.publisher = parentDir.dirName();

	// 默认语言
	meta.languageType = "中文";

	// 是否压缩（后缀名不可直接读则为压缩）
	QString ext = fi.suffix().toLower();
	if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "gz" || ext == "tar")
		meta.isCompressed = "是";
	else
		meta.isCompressed = "否";

	emit extractionProgress(70, "正在分析PDF文档信息...");

	// 尝试通过 GDAL PDF 驱动获取更多信息
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));

	if (!poDS)
	{
		// 尝试非结构化读取
		CPLSetConfigOption("OGR_PDF_READ_NON_STRUCTURED", "YES");
		poDS = static_cast<GDALDataset*>(
			GDALOpenEx(filePath.toUtf8().constData(),
					   GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
		CPLSetConfigOption("OGR_PDF_READ_NON_STRUCTURED", nullptr);
	}

	if (poDS)
	{
		emit extractionProgress(85, "正在提取文档结构信息...");

		// 内容摘要：图层列表
		QStringList layerNames;
		for (int i = 0; i < poDS->GetLayerCount(); ++i)
		{
			OGRLayer* poLayer = poDS->GetLayer(i);
			if (poLayer)
				layerNames << QString::fromUtf8(poLayer->GetName());
		}
		if (!layerNames.isEmpty())
			meta.summary = QString("包含图层: %1").arg(layerNames.join(", "));

		GDALClose(poDS);
	}

	emit extractionProgress(100, "文档元数据提取完成");
	return meta;
}

QString MetadataExtractor::extractCRS(OGRSpatialReference* poSRS, int maxLen)
{
	if (!poSRS) return QString();

	// 1. 优先取 EPSG 代码（如 "EPSG:4490"），一定不超过 maxLen
	const char* pszAuthCode = poSRS->GetAuthorityCode(nullptr);
	if (pszAuthCode)
		return QString("EPSG:%1").arg(pszAuthCode);

	// 2. 取坐标系统名称（如 "CGCS 2000 / 3-degree Gauss-Kruger CM 114E"）
	const char* pszName = poSRS->GetName();
	if (pszName && strlen(pszName) > 0 && strlen(pszName) <= maxLen)
		return QString::fromUtf8(pszName);

	// 3. 回退到 WKT，但截断防止溢出
	char* pszWKT = nullptr;
	poSRS->exportToWkt(&pszWKT);
	if (pszWKT)
	{
		QString wkt = QString::fromUtf8(pszWKT);
		CPLFree(pszWKT);
		if (wkt.length() <= maxLen)
			return wkt;
		// 截断：保留前 maxLen-3 字符 + "..."
		return wkt.left(maxLen - 3) + "...";
	}

	return QString();
}

ProductType MetadataExtractor::detectProductType(const QString& filePath)
{
	QString ext = QFileInfo(filePath).suffix().toLower();

	// 矢量格式
	static QSet<QString> vectorExts = {
		"shp", "geojson", "json", "kml", "kmz", "gpx",
		"tab", "mif", "mid", "gml", "dgn", "vct"
	};
	if (vectorExts.contains(ext))
		return ProductType::Vector;

	// 栅格格式
	static QSet<QString> rasterExts = {
		"tif", "tiff", "img", "jpg", "jpeg", "jp2", "j2k",
		"png", "bmp", "gif", "ecw", "sid", "dem", "asc", "grd"
	};
	if (rasterExts.contains(ext))
		return ProductType::Raster;

	// 专业制图格式
	static QSet<QString> cadExts = {"dwg", "dxf"};
	if (cadExts.contains(ext))
		return ProductType::CAD;

	if (ext == "ai")
		return ProductType::AI;

	if (ext == "cdr")
		return ProductType::CDR;

	if (ext == "pdf")
		return ProductType::PDF;

	// 文档格式
	static QSet<QString> docExts = {
		"doc", "docx", "xls", "xlsx", "ppt", "pptx",
		"xml", "txt", "csv", "rtf", "odt", "ods", "odp"
	};
	if (docExts.contains(ext))
		return ProductType::Document;

	// 压缩包格式
	static QSet<QString> archiveExts = {
		"zip", "rar", "7z", "tar", "gz", "bz2", "xz"
	};
	if (archiveExts.contains(ext))
		return ProductType::Archive;

	// File Geodatabase / Personal Geodatabase
	if (ext == "gdb" || ext == "mdb")
	{
		// GDB 是一个目录，以文件夹路径传入，用 GDAL 进一步判断
		GDALDataset* gdbDS = static_cast<GDALDataset*>(
			GDALOpenEx(filePath.toUtf8().constData(),
					   GDAL_OF_VECTOR,
					   nullptr, nullptr, nullptr));
		if (gdbDS)
		{
			ProductType gdbType = ProductType::Other;
			if (gdbDS->GetLayerCount() > 0)
				gdbType = ProductType::Vector;
			GDALClose(gdbDS);
			return gdbType;
		}
	}

	// MDB (Access Personal Geodatabase)
	if (ext == "mdb")
	{
		GDALDataset* mdbDS = static_cast<GDALDataset*>(
			GDALOpenEx(filePath.toUtf8().constData(),
					   GDAL_OF_VECTOR,
					   nullptr, nullptr, nullptr));
		if (mdbDS)
		{
			ProductType mdbType = ProductType::Other;
			if (mdbDS->GetLayerCount() > 0)
				mdbType = ProductType::Vector;
			GDALClose(mdbDS);
			return mdbType;
		}
	}

	// 尝试用GDAL打开判断
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR | GDAL_OF_RASTER,
				   nullptr, nullptr, nullptr));
	if (poDS)
	{
		ProductType type = ProductType::Other;
		if (poDS->GetLayerCount() > 0)
			type = ProductType::Vector;
		else if (poDS->GetRasterCount() > 0)
			type = ProductType::Raster;
		GDALClose(poDS);
		return type;
	}

	return ProductType::Other;
}

bool MetadataExtractor::generateThumbnail(const QString& filePath, const QString& outputPath, int maxSize)
{
	// 对于矢量数据，使用GDAL创建预览
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR | GDAL_OF_RASTER,
				   nullptr, nullptr, nullptr));

	if (!poDS) return false;

	// 使用GDAL生成缩略图
	// 先缩放到合理尺寸
	int nRasterX = poDS->GetRasterXSize();
	int nRasterY = poDS->GetRasterYSize();

	if (nRasterX > 0 && nRasterY > 0)
	{
		// 栅格数据：直接缩放
		double scale = qMin((double)maxSize / nRasterX, (double)maxSize / nRasterY);
		int outX = (int)(nRasterX * scale);
		int outY = (int)(nRasterY * scale);

		// 使用GDAL Warp进行缩略图生成
		GDALDatasetH hSrcDS = poDS;
		GDALDriverH hDriver = GDALGetDriverByName("PNG");
		GDALDatasetH hDstDS = GDALCreateCopy(hDriver,
			outputPath.toUtf8().constData(),
			hSrcDS, FALSE, nullptr, nullptr, nullptr);

		if (hDstDS)
		{
			GDALClose(hDstDS);
			GDALClose(poDS);
			return true;
		}
	}

	GDALClose(poDS);
	return false;
}

QString MetadataExtractor::calculateFileHash(const QString& filePath)
{
	QFileInfo fi(filePath);

	// 目录类型（GDB 等）：递归哈希所有文件（排序保证确定性）
	if (fi.isDir())
	{
		QCryptographicHash hash(QCryptographicHash::Sha256);

		// 收集目录下所有文件，按完整路径排序
		QDir dir(filePath);
		QStringList allFiles;
		QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot;
		QDirIterator it(filePath, filters, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			allFiles << it.next();
		}
		allFiles.sort(Qt::CaseInsensitive);

		for (const QString& fPath : allFiles)
		{
			// 先写入文件相对路径（保证文件增删能被检测）
			QString relPath = dir.relativeFilePath(fPath);
			hash.addData(relPath.toUtf8());

			QFile f(fPath);
			if (f.open(QIODevice::ReadOnly))
			{
				// 分块读取，避免大文件一次加载
				while (!f.atEnd())
				{
					QByteArray chunk = f.read(64 * 1024);  // 64KB 块
					hash.addData(chunk);
				}
				f.close();
			}
		}
		return hash.result().toHex();
	}

	// 普通文件
	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly))
		return QString();

	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (hash.addData(&file))
	{
		return hash.result().toHex();
	}
	return QString();
}

QString MetadataExtractor::getFileMimeType(const QString& filePath)
{
	QString ext = QFileInfo(filePath).suffix().toLower();

	static QMap<QString, QString> mimeMap = {
		{"shp", "application/x-esri-shapefile"},
		{"geojson", "application/geo+json"},
		{"tif", "image/tiff"},
		{"tiff", "image/tiff"},
		{"img", "application/x-erdas-img"},
		{"pdf", "application/pdf"},
		{"dwg", "application/x-autocad"},
		{"dxf", "application/x-autocad"},
		{"ai", "application/postscript"},
		{"cdr", "application/x-coreldraw"},
		{"png", "image/png"},
		{"jpg", "image/jpeg"},
		{"jpeg", "image/jpeg"},
		{"jp2", "image/jp2"}
	};

	return mimeMap.value(ext, "application/octet-stream");
}

QVariantMap MetadataExtractor::extractGDALMetadata(const QString& filePath)
{
	QVariantMap result;
	GDALDataset* poDS = static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_RASTER | GDAL_OF_VECTOR,
				   nullptr, nullptr, nullptr));

	if (poDS)
	{
		char** papszMetadata = poDS->GetMetadata();
		if (papszMetadata)
		{
			for (int i = 0; papszMetadata[i] != nullptr; ++i)
			{
				QString entry = QString::fromUtf8(papszMetadata[i]);
				int eqPos = entry.indexOf('=');
				if (eqPos > 0)
				{
					result[entry.left(eqPos)] = entry.mid(eqPos + 1);
				}
			}
		}
		GDALClose(poDS);
	}
	return result;
}
