#include "data_importer.h"
#include "database/postgis_connector.h"
#include "database/schema_manager.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "ogr_spatialref.h"
#include "ogr_geometry.h"
#include "ogr_p.h"
#include "cpl_conv.h"
#include "cpl_string.h"

#include <libpq-fe.h>

#include <QFileInfo>
#include <QRegularExpression>
#include <QProcess>
#include <qgsmessagelog.h>

// ---------------------------------------------------------------------------
// 根据文件与编码参数构造 GDAL 打开选项并打开矢量数据集。
// - encoding 为 AUTO/AUTODETECT/空 时：shapefile 若没有配套 .cpg 侧文件，
//   则显式指定 ENCODING=GBK（国产数据常见无 .cpg 的 GBK shp，GDAL 默认
//   无法推断编码会中文乱码）；有 .cpg 或非 shp 文件则交给 GDAL 自动识别。
// - encoding 为具体编码时：显式传 ENCODING=<encoding>（如 GBK/UTF-8）。
// 返回成功打开的 dataset，失败返回 nullptr。
// ---------------------------------------------------------------------------
static GDALDataset* openVectorDataset(const QString& filePath, const QString& encoding)
{
	const QString enc = encoding.trimmed();
	QByteArray encOpt;   // 保活：GDALOpenEx 内部会立即复制选项串
	const char* papszOpenOpts[2] = { nullptr, nullptr };

	if (enc.isEmpty()
		|| enc.compare("AUTO", Qt::CaseInsensitive) == 0
		|| enc.compare("AUTODETECT", Qt::CaseInsensitive) == 0)
	{
		QFileInfo fi(filePath);
		if (fi.suffix().compare("shp", Qt::CaseInsensitive) == 0)
		{
			const QString cpgPath = fi.path() + "/" + fi.completeBaseName() + ".cpg";
			if (!QFileInfo::exists(cpgPath))
			{
				encOpt = QByteArray("ENCODING=GBK");   // 无 .cpg 的 shp 按 GBK 打开
			}
		}
	}
	else
	{
		encOpt = QByteArray("ENCODING=") + enc.toUtf8();
	}

	if (!encOpt.isEmpty())
		papszOpenOpts[0] = encOpt.constData();

	return static_cast<GDALDataset*>(
		GDALOpenEx(filePath.toUtf8().constData(),
				   GDAL_OF_VECTOR, nullptr,
				   papszOpenOpts[0] ? papszOpenOpts : nullptr,
				   nullptr));
}

DataImporter::DataImporter(QObject* parent)
	: QObject(parent)
{
	// 确保 GDAL 已注册
	static bool gdalRegistered = false;
	if (!gdalRegistered)
	{
		GDALAllRegister();
		gdalRegistered = true;
	}
}

int DataImporter::importVectorToPostGIS(const QString& filePath,
										 const QString& targetTable,
										 int srcSrid,
										 int targetSrid,
										 const QString& encoding)
{
	QgsMessageLog::logMessage(
		QStringLiteral("[矢量导入] 开始: 文件=%1 目标表=%2 srcSrid=%3 targetSrid=%4")
			.arg(filePath).arg(targetTable).arg(srcSrid).arg(targetSrid),
		"MapProductTools", Qgis::Info);

	auto* db = PostgisConnector::instance();
	if (!db->isConnected())
	{
		m_lastError = "数据库未连接";
		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 数据库未连接"), "MapProductTools", Qgis::Critical);
		emit importFailed(targetTable, m_lastError);
		return -1;
	}

	// 检查文件是否存在
	{
		QFileInfo fi(filePath);
		if (!fi.exists())
		{
			m_lastError = QString("矢量文件不存在: %1").arg(filePath);
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 文件不存在: %1 (大小=%2)")
					.arg(filePath).arg(fi.size()),
				"MapProductTools", Qgis::Critical);
			return -1;
		}
		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 文件存在: %1 大小=%2 KB")
				.arg(filePath).arg(fi.size() / 1024.0, 0, 'f', 1),
			"MapProductTools", Qgis::Info);
	}

	emit importProgress(5, "正在打开矢量文件...");

	// 1. 先用 GDAL 打开源文件，获取图层信息
	// Shapefile 诊断：检查辅助文件是否存在
	if (filePath.endsWith(".shp", Qt::CaseInsensitive))
	{
		QFileInfo fiShp(filePath);
		QString shxPath = fiShp.path() + "/" + fiShp.completeBaseName() + ".shx";
		QString dbfPath = fiShp.path() + "/" + fiShp.completeBaseName() + ".dbf";
		bool shxExists = QFileInfo::exists(shxPath);
		bool dbfExists = QFileInfo::exists(dbfPath);
		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] Shapefile 诊断: .shx存在=%1 .dbf存在=%2")
				.arg(shxExists).arg(dbfExists),
			"MapProductTools", Qgis::Info);
		if (!shxExists)
		{
			m_lastError = QString("Shapefile 缺少 .shx 索引文件: %1。请确保 .shp/.shx/.dbf 三个文件完整。").arg(shxPath);
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] %1").arg(m_lastError), "MapProductTools", Qgis::Critical);
			return -1;
		}
		if (!dbfExists)
		{
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 警告: Shapefile 缺少 .dbf 属性文件"),
				"MapProductTools", Qgis::Warning);
		}
	}

	GDALDataset* poSrcDS = openVectorDataset(filePath, encoding);

	if (!poSrcDS)
	{
		m_lastError = QString("无法打开矢量文件: %1").arg(CPLGetLastErrorMsg());
		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] GDAL 打开失败: %1").arg(m_lastError),
			"MapProductTools", Qgis::Critical);
		return -1;
	}

	// 输出 GDAL 实际使用的驱动信息
	GDALDriver* poDriver = poSrcDS->GetDriver();
	int nLayers = poSrcDS->GetLayerCount();
	QString driverName = QString::fromUtf8(poDriver ? poDriver->GetDescription() : "Unknown");
	QgsMessageLog::logMessage(
		QStringLiteral("[矢量导入] GDAL 打开成功: 驱动=%1 图层数=%2")
			.arg(driverName).arg(nLayers),
		"MapProductTools", Qgis::Info);

	// 重置统计
	m_lastVectorStats = VectorImportStats();

	int totalFeatureCount = 0;

	for (int iLayer = 0; iLayer < poSrcDS->GetLayerCount(); ++iLayer)
	{
		OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
		if (!poSrcLayer) continue;

		emit importProgress(15 + iLayer * 10, QString("正在读取图层 %1...").arg(iLayer + 1));

		// ===== 诊断日志：输出图层详细信息 =====
		QString layerName = QString::fromUtf8(poSrcLayer->GetName());
		GIntBig estCount = poSrcLayer->GetFeatureCount(true);  // force=true 尝试精确计数
		OGRwkbGeometryType eGeomType = poSrcLayer->GetGeomType();
		QString geomTypeName = OGRGeometryTypeToName(eGeomType);
		OGRFeatureDefn* poSrcFDefn = poSrcLayer->GetLayerDefn();
		int nFields = poSrcFDefn ? poSrcFDefn->GetFieldCount() : 0;

		m_lastVectorStats.estimatedCount = (estCount >= 0) ? (int)estCount : -1;

		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 图层[%1]: 名称=%2 几何=%3 预估要素数=%4 字段数=%5")
				.arg(iLayer + 1).arg(layerName).arg(geomTypeName)
				.arg(estCount >= 0 ? QString::number(estCount) : "未知").arg(nFields),
			"MapProductTools", Qgis::Info);

		// 输出字段定义详情
		for (int f = 0; f < nFields; ++f)
		{
			OGRFieldDefn* pfDefn = poSrcFDefn->GetFieldDefn(f);
			if (pfDefn)
			{
				QgsMessageLog::logMessage(
					QStringLiteral("[矢量导入]   字段[%1]: %2 类型=%3 宽度=%4")
						.arg(f)
						.arg(QString::fromUtf8(pfDefn->GetNameRef()))
						.arg(OGRFieldDefn::GetFieldTypeName(pfDefn->GetType()))
						.arg(pfDefn->GetWidth()),
					"MapProductTools", Qgis::Info);
			}
		}

		// 多图层数据集（如 .gdb）：每个图层使用独立的 PostGIS 表名
		// 单图层文件：直接使用 targetTable
		QString layerTable = targetTable;
		if (poSrcDS->GetLayerCount() > 1)
		{
			QString safeLayer = layerName.toLower();
			safeLayer.replace(QRegularExpression("[^a-z0-9_\\x{4e00}-\\x{9fff}]"), "_");
			safeLayer.replace(QRegularExpression("_+"), "_");
			if (safeLayer.isEmpty())
				safeLayer = QString("layer_%1").arg(iLayer);

			// 当 targetTable 无效时（纯下划线/无有效字符），
			// 直接用 GDAL 图层名作为表名
			QString stripped = targetTable;
			stripped.remove(QRegularExpression("[^a-zA-Z0-9\\x{4e00}-\\x{9fff}]"));
			if (stripped.isEmpty())
				layerTable = safeLayer;
			else
				layerTable = targetTable + "_" + safeLayer;
		}

		// 如果几何类型是 wkbNone(0) 或 wkbUnknown(0)，说明文件没有空间几何
		if (eGeomType == wkbNone || eGeomType == wkbUnknown)
		{
			m_lastError = "矢量图层无有效几何类型，无法导入";
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 图层无有效几何类型"), "MapProductTools", Qgis::Critical);
			GDALClose(poSrcDS);
			return -1;
		}

		// 获取源图层的空间参考
		OGRSpatialReference* poSrcSRS = poSrcLayer->GetSpatialRef();
		if (poSrcSRS)
		{
			char* pszProj4 = nullptr;
			poSrcSRS->exportToProj4(&pszProj4);
			const char* pszAuthName = poSrcSRS->GetAuthorityName(nullptr);
			const char* pszAuthCode = poSrcSRS->GetAuthorityCode(nullptr);
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 源SRS: %1:%2 PROJ4=%3")
					.arg(pszAuthName ? pszAuthName : "?")
					.arg(pszAuthCode ? pszAuthCode : "?")
					.arg(pszProj4 ? QString::fromUtf8(pszProj4) : "?"),
				"MapProductTools", Qgis::Info);
			if (pszProj4) CPLFree(pszProj4);
		}
		else
		{
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 源图层无 SRS 信息，将使用 srcSrid=%1").arg(srcSrid),
				"MapProductTools", Qgis::Warning);
		}

		OGRSpatialReference oTargetSRS;
		oTargetSRS.importFromEPSG(targetSrid);

		// 是否需要坐标转换
		bool needTransform = false;
		OGRCoordinateTransformation* poCT = nullptr;
		if (poSrcSRS && !poSrcSRS->IsSame(&oTargetSRS))
		{
			needTransform = true;
			poCT = OGRCreateCoordinateTransformation(poSrcSRS, &oTargetSRS);
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 需要坐标转换: source -> EPSG:%1").arg(targetSrid),
				"MapProductTools", Qgis::Info);
		}
		else
		{
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 无需坐标转换 (SRS 相同或源无 SRS)"),
				"MapProductTools", Qgis::Info);
		}

		// 2. 创建 PostGIS 目标表（先删除再创建，确保干净）
		SchemaManager schemaMgr;
		schemaMgr.dropDataLayerTable(layerTable);
		if (!schemaMgr.createDataLayerTable(layerTable, geomTypeName, targetSrid))
		{
			m_lastError = schemaMgr.lastError();
			QgsMessageLog::logMessage(
				QStringLiteral("[矢量导入] 建表失败: %1 表=%2 几何=%3 SRID=%4")
					.arg(m_lastError).arg(layerTable).arg(geomTypeName).arg(targetSrid),
				"MapProductTools", Qgis::Critical);
			emit importFailed(layerTable, m_lastError);
			GDALClose(poSrcDS);
			return -1;
		}
		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 目标表创建成功: %1 几何=%2 SRID=%3")
				.arg(layerTable).arg(geomTypeName).arg(targetSrid),
			"MapProductTools", Qgis::Info);

		// 4. 动态添加属性列（非几何字段）
		int addedFieldCount = 0;
		int failedFieldCount = 0;
		for (int iField = 0; iField < poSrcFDefn->GetFieldCount(); ++iField)
		{
			OGRFieldDefn* poFieldDefn = poSrcFDefn->GetFieldDefn(iField);
			QString fieldName = QString::fromUtf8(poFieldDefn->GetNameRef());

			// 转义 PostgreSQL 保留字
			if (fieldName.compare("id", Qt::CaseInsensitive) == 0)
				fieldName = "fid";

			QString pgType;
			switch (poFieldDefn->GetType())
			{
			case OFTInteger:
			case OFTInteger64:
				pgType = "INTEGER";
				break;
			case OFTReal:
				pgType = "DOUBLE PRECISION";
				break;
			case OFTString:
				pgType = QString("VARCHAR(%1)").arg(qMax(poFieldDefn->GetWidth(), 254));
				break;
			case OFTDate:
				pgType = "DATE";
				break;
			case OFTTime:
				pgType = "TIME";
				break;
			case OFTDateTime:
				pgType = "TIMESTAMP";
				break;
			default:
				pgType = "VARCHAR(512)";
				break;
			}

			QString alterSql = QString("ALTER TABLE \"%1\" ADD COLUMN IF NOT EXISTS \"%2\" %3")
				.arg(layerTable, fieldName, pgType);

			if (!db->executeNonQuery(alterSql))
			{
				failedFieldCount++;
				QgsMessageLog::logMessage(
					QStringLiteral("[矢量导入] 添加字段失败: %1").arg(alterSql),
					"MapProductTools", Qgis::Warning);
			}
			else
			{
				addedFieldCount++;
			}
		}
		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 字段添加完成: 成功=%1 失败=%2")
				.arg(addedFieldCount).arg(failedFieldCount),
			"MapProductTools", Qgis::Info);

		emit importProgress(40, "正在写入要素数据...");

		// 5. 使用 libpq COPY 或逐条 INSERT 写入要素
		// 由于 libpq 参数化查询对几何数据支持有限，使用 ST_GeomFromText 方式逐条插入
		// 构建 INSERT 语句模板
		QStringList fieldNames;
		QStringList fieldPlaceholders;
		int paramIdx = 2; // $1 = geom

		// 先收集实际需要写入的字段
		QList<QString> actualFields;
		QList<OGRFieldType> actualFieldTypes;
		for (int iField = 0; iField < nFields; ++iField)
		{
			OGRFieldDefn* poFieldDefn = poSrcFDefn->GetFieldDefn(iField);
			QString fName = QString::fromUtf8(poFieldDefn->GetNameRef());
			if (fName.compare("id", Qt::CaseInsensitive) == 0)
				fName = "fid";

			actualFields.append(fName);
			actualFieldTypes.append(poFieldDefn->GetType());
			fieldNames.append(QString("\"%1\"").arg(fName));
			fieldPlaceholders.append(QString("$%1").arg(paramIdx++));
		}

		// 开始事务（批量提交，提升性能）
		db->beginTransaction();

		int featureCount = 0;
		int skipNoGeom = 0;
		int skipInsertFail = 0;
		int batchCount = 0;
		const int BATCH_SIZE = 500;  // 每批提交的要素数

		// 先获取一个要素来诊断几何情况
		poSrcLayer->ResetReading();
		{
			OGRFeature* poTestFeature = poSrcLayer->GetNextFeature();
			if (poTestFeature)
			{
				OGRGeometry* poTestGeom = poTestFeature->GetGeometryRef();
				if (poTestGeom)
				{
					char* pszTestWKT = nullptr;
					if (poTestGeom->exportToWkt(&pszTestWKT) == OGRERR_NONE && pszTestWKT)
					{
						QString testWkt = QString::fromUtf8(pszTestWKT).left(200);
						QgsMessageLog::logMessage(
							QStringLiteral("[矢量导入] 首要素几何 WKT(前200字符): %1").arg(testWkt),
							"MapProductTools", Qgis::Info);
						CPLFree(pszTestWKT);
					}
					// 输出首要素属性值
					QStringList attrInfo;
					for (int f = 0; f < poTestFeature->GetFieldCount() && f < 10; ++f)
					{
						if (poTestFeature->IsFieldSetAndNotNull(f))
						{
							attrInfo << QString("%1=%2")
								.arg(QString::fromUtf8(poTestFeature->GetFieldDefnRef(f)->GetNameRef()))
								.arg(QString::fromUtf8(poTestFeature->GetFieldAsString(f)));
						}
					}
					if (!attrInfo.isEmpty())
					{
						QgsMessageLog::logMessage(
							QStringLiteral("[矢量导入] 首要素属性(前10字段): %1").arg(attrInfo.join(", ")),
							"MapProductTools", Qgis::Info);
					}
				}
				else
				{
					// 首要素无几何 — 这是个警告信号
					QStringList fieldInfo;
					for (int f = 0; f < poTestFeature->GetFieldCount() && f < 5; ++f)
					{
						OGRFieldDefn* pFDefn = poTestFeature->GetFieldDefnRef(f);
						if (pFDefn)
							fieldInfo << QString("%1(%2)").arg(
								QString::fromUtf8(pFDefn->GetNameRef()),
								OGRFieldDefn::GetFieldTypeName(pFDefn->GetType()));
					}
					QgsMessageLog::logMessage(
						QStringLiteral("[矢量导入] 警告: 首要素无几何! 字段信息: %1").arg(fieldInfo.join(", ")),
						"MapProductTools", Qgis::Warning);
				}
				OGRFeature::DestroyFeature(poTestFeature);
			}
			else
			{
				QgsMessageLog::logMessage(
					QStringLiteral("[矢量导入] 图层无任何要素 (GetNextFeature 返回 nullptr)"),
					"MapProductTools", Qgis::Warning);
			}
		}

		// ===== 开始主要素循环（已修复：不再位于 else 块内）=====
		poSrcLayer->ResetReading();
		OGRFeature* poFeature = nullptr;
		int noGeomSampleCount = 0;

		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 开始读取要素: 表=%1 预估=%2 字段数=%3")
				.arg(layerTable)
				.arg(m_lastVectorStats.estimatedCount >= 0 ? QString::number(m_lastVectorStats.estimatedCount) : "未知")
				.arg(nFields),
			"MapProductTools", Qgis::Info);

		while ((poFeature = poSrcLayer->GetNextFeature()) != nullptr)
		{
			m_lastVectorStats.totalRead++;

			OGRGeometry* poGeom = poFeature->GetGeometryRef();
			if (!poGeom)
			{
				skipNoGeom++;
				m_lastVectorStats.skipNoGeom++;
				// 输出前3个无几何要素的 FID
				if (noGeomSampleCount < 3)
				{
					QgsMessageLog::logMessage(
						QStringLiteral("[矢量导入] 要素无几何 (第%1条 FID=%2)")
							.arg(m_lastVectorStats.totalRead)
							.arg(poFeature->GetFID()),
						"MapProductTools", Qgis::Warning);
					noGeomSampleCount++;
				}
				OGRFeature::DestroyFeature(poFeature);
				continue;
			}

			// 坐标转换
			if (needTransform && poCT)
			{
				poGeom->transform(poCT);
			}

			// 导出为 WKT
			// 注意：Shapefile 的 .prj 可能定义 lat/lon 轴顺序，
			// 导致 GDAL exportToWkt 输出 (lat, lon) 而非标准的 (lon, lat)。
			// 通过检查第一个坐标值是否在 ±90 范围外来判断是否需要 swapXY：
			// - 经度范围 ±180，纬度范围 ±90
			// - 如果第一个值 > 90 或 < -90，说明当前是 (lon, lat)，不需要交换
			// - 如果第一个值在 ±90 范围内，可能是 (lat, lon)，需要交换
			{
				OGREnvelope env;
				poGeom->getEnvelope(&env);
				// 检查 MinX：如果 MinX > 90 说明当前是 (lon, lat) 正确
				// 如果 MinX 在 ±90 内，说明是 (lat, lon) 需要交换
				if (env.MinX >= -90.0 && env.MinX <= 90.0 && env.MaxY > 90.0)
				{
					// 当前是 (lat, lon) 顺序，交换为 (lon, lat)
					poGeom->swapXY();
				}
				// 否则保持原样（已经是 (lon, lat) 或坐标范围无法判断）
			}

			char* pszWKT = nullptr;
			OGRErr eErr = poGeom->exportToWkt(&pszWKT);
			if (eErr != OGRERR_NONE || !pszWKT)
			{
				m_lastVectorStats.skipInsertFail++;
				QgsMessageLog::logMessage(
					QStringLiteral("[矢量导入] WKT 导出失败 (第%1条 FID=%2 err=%3)")
						.arg(m_lastVectorStats.totalRead)
						.arg(poFeature->GetFID())
						.arg(static_cast<int>(eErr)),
					"MapProductTools", Qgis::Warning);
				OGRFeature::DestroyFeature(poFeature);
				continue;
			}
			QString wkt = QString::fromUtf8(pszWKT);
			CPLFree(pszWKT);

			// 构建 INSERT SQL
			// ST_GeomFromText → ST_SetSRID → ST_Force2D → ST_MakeValid：
			//   ST_Force2D 剥离 Z/M 维度，确保所有几何统一为 2D，
			//   避免同一图层内要素维度不一致（部分有 Z/M、部分无）导致 PostGIS 类型检查失败
			//   ST_MakeValid 修复自相交、环方向错误等源数据固有的无效几何体
			QString geomExpr = QString("ST_MakeValid(ST_Force2D(ST_SetSRID(ST_GeomFromText($1), %1)))").arg(targetSrid);
			QString insertSql;
			if (fieldNames.isEmpty())
			{
				insertSql = QString("INSERT INTO \"%1\" (geom) VALUES (%2)")
					.arg(layerTable, geomExpr);
			}
			else
			{
				insertSql = QString("INSERT INTO \"%1\" (geom, %2) VALUES (%3, %4)")
					.arg(layerTable, fieldNames.join(", "),
						 geomExpr,
						 fieldPlaceholders.join(", "));
			}

			QVariantList params;
			params << wkt;

			// 添加属性字段值
			for (int iField = 0; iField < actualFields.size(); ++iField)
			{
				int ogrIdx = poFeature->GetFieldIndex(
					actualFields[iField].toUtf8().constData());
				if (ogrIdx < 0)
				{
					params << QVariant();
					continue;
				}

				if (!poFeature->IsFieldSetAndNotNull(ogrIdx))
				{
					params << QVariant();
					continue;
				}

				OGRFieldType ft = actualFieldTypes[iField];
				switch (ft)
				{
				case OFTInteger:
					params << poFeature->GetFieldAsInteger(ogrIdx);
					break;
				case OFTInteger64:
					params << QVariant(static_cast<qint64>(poFeature->GetFieldAsInteger64(ogrIdx)));
					break;
				case OFTReal:
					params << poFeature->GetFieldAsDouble(ogrIdx);
					break;
				case OFTString:
					params << QString::fromUtf8(poFeature->GetFieldAsString(ogrIdx));
					break;
				case OFTDate:
				case OFTTime:
				case OFTDateTime:
				{
					int year, month, day, hour, minute, second, tz;
					poFeature->GetFieldAsDateTime(ogrIdx, &year, &month, &day,
												   &hour, &minute, &second, &tz);
					params << QString("%1-%2-%3 %4:%5:%6")
						.arg(year, 4, 10, QChar('0'))
						.arg(month, 2, 10, QChar('0'))
						.arg(day, 2, 10, QChar('0'))
						.arg(hour, 2, 10, QChar('0'))
						.arg(minute, 2, 10, QChar('0'))
						.arg(second, 2, 10, QChar('0'));
					break;
				}
				default:
					params << QString::fromUtf8(poFeature->GetFieldAsString(ogrIdx));
					break;
				}
			}

			// 每条 INSERT 前创建独立保存点，失败只回滚这一条
			db->executeNonQuery("SAVEPOINT sp_row");

			if (db->executeNonQuery(insertSql, params))
			{
				db->executeNonQuery("RELEASE SAVEPOINT sp_row");
				featureCount++;
				batchCount++;
				m_lastVectorStats.featureCount++;
			}
			else
			{
				skipInsertFail++;
				m_lastVectorStats.skipInsertFail++;
				QString errMsg = db->lastError();

				// 回滚这一条，不影响其他已成功的 INSERT
				db->executeNonQuery("ROLLBACK TO sp_row");

				// 收集前几条错误信息用于诊断
				if (skipInsertFail <= 5 && !errMsg.isEmpty())
				{
					if (m_lastError.isEmpty())
						m_lastError = errMsg;
					else
						m_lastError += "; " + errMsg;

					QgsMessageLog::logMessage(
						QStringLiteral("INSERT 失败 (第%1条 FID=%2): %3")
							.arg(m_lastVectorStats.totalRead)
							.arg(poFeature->GetFID())
							.arg(errMsg),
						"MapProductTools", Qgis::Warning);
				}
			}

			OGRFeature::DestroyFeature(poFeature);

			// 每 BATCH_SIZE 条提交一次事务，防止内存占用过大
			if (batchCount >= BATCH_SIZE)
			{
				db->commitTransaction();
				db->beginTransaction();
				batchCount = 0;
				emit importProgress(40 + (featureCount % 10000) / 200,
					QString("已写入 %1 个要素...").arg(featureCount));
			}
		}
		// ===== 主要素循环结束 =====

		QgsMessageLog::logMessage(
			QStringLiteral("[矢量导入] 要素读取完成: 表=%1 总读取=%2 成功写入=%3 无几何=%4 INSERT失败=%5")
				.arg(layerTable)
				.arg(m_lastVectorStats.totalRead)
				.arg(featureCount)
				.arg(skipNoGeom)
				.arg(skipInsertFail),
			"MapProductTools", Qgis::Info);

		db->commitTransaction();

		if (poCT) OCTDestroyCoordinateTransformation(poCT);

		emit importProgress(90, QString("图层 %1 写入完成: %2 个要素")
			.arg(iLayer + 1).arg(featureCount));

		// 创建空间索引
		QString indexSql = QString("CREATE INDEX IF NOT EXISTS idx_%1_geom ON \"%1\" USING GIST(geom)")
			.arg(layerTable);
		db->executeNonQuery(indexSql);

		// 更新 geometry_columns 元数据
		QString updateGeomColSql = QString(
			"SELECT UpdateGeometrySRID('%1', 'geom', %2)")
			.arg(layerTable).arg(targetSrid);
		db->executeNonQuery(updateGeomColSql);

		totalFeatureCount += featureCount;
	}

	// 诊断：读取了要素但全部入库失败时，设置详细错误信息
	if (totalFeatureCount == 0 && m_lastVectorStats.totalRead > 0)
	{
		QString diag = QStringLiteral("所有要素入库失败 (读取 %1 个, 无几何 %2, INSERT失败 %3)")
			.arg(m_lastVectorStats.totalRead)
			.arg(m_lastVectorStats.skipNoGeom)
			.arg(m_lastVectorStats.skipInsertFail);

		if (m_lastError.isEmpty())
			m_lastError = diag;
		else
			m_lastError = diag + ": " + m_lastError;

		QgsMessageLog::logMessage(
			QStringLiteral("矢量导入无要素写入: 表=%1 %2").arg(targetTable).arg(diag),
			"MapProductTools", Qgis::Critical);
	}
	else if (m_lastVectorStats.skipInsertFail > 0)
	{
		QgsMessageLog::logMessage(
			QStringLiteral("矢量导入部分成功: 表=%1 成功=%2 失败=%3")
				.arg(targetTable)
				.arg(m_lastVectorStats.featureCount)
				.arg(m_lastVectorStats.skipInsertFail),
			"MapProductTools", Qgis::Warning);
	}

	GDALClose(poSrcDS);

	QgsMessageLog::logMessage(
		QStringLiteral("[矢量导入] 完成: 文件=%1 表=%2 总入库=%3 个要素")
			.arg(filePath).arg(targetTable).arg(totalFeatureCount),
		"MapProductTools", Qgis::Info);

	emit importProgress(100, QString("入库完成，共 %1 个要素").arg(totalFeatureCount));
	emit importCompleted(targetTable, totalFeatureCount);

	return totalFeatureCount;
}

int DataImporter::importVectorLayerToPostGIS(const QString& filePath,
											 const QString& layerName,
											 const QString& targetTable,
											 int srcSrid,
											 int targetSrid,
											 const QString& encoding)
{
	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 开始: 文件=%1 图层=%2 目标表=%3 srcSrid=%4 targetSrid=%5")
			.arg(filePath).arg(layerName).arg(targetTable).arg(srcSrid).arg(targetSrid),
		"MapProductTools", Qgis::Info);

	auto* db = PostgisConnector::instance();
	if (!db->isConnected())
	{
		m_lastError = "数据库未连接";
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 数据库未连接"), "MapProductTools", Qgis::Critical);
		return -1;
	}

	emit importProgress(5, QString("正在打开矢量文件，准备导入图层 %1...").arg(layerName));

	GDALDataset* poSrcDS = openVectorDataset(filePath, encoding);

	if (!poSrcDS)
	{
		m_lastError = QString("无法打开矢量文件: %1").arg(CPLGetLastErrorMsg());
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] GDAL 打开失败: %1").arg(m_lastError),
			"MapProductTools", Qgis::Critical);
		emit importFailed(targetTable, m_lastError);
		return -1;
	}

	QString driverName = QString::fromUtf8(poSrcDS->GetDriverName());
	int totalLayerCount = poSrcDS->GetLayerCount();
	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] GDAL 打开成功: 驱动=%1 总图层数=%2")
			.arg(driverName).arg(totalLayerCount),
		"MapProductTools", Qgis::Info);

	// 查找指定图层
	OGRLayer* poSrcLayer = poSrcDS->GetLayerByName(layerName.toUtf8().constData());
	if (!poSrcLayer)
	{
		// 尝试遍历查找
		for (int i = 0; i < totalLayerCount; ++i)
		{
			OGRLayer* pLayer = poSrcDS->GetLayer(i);
			if (pLayer)
			{
				QString n = QString::fromUtf8(pLayer->GetName());
				QgsMessageLog::logMessage(
					QStringLiteral("[GDB矢量导入] 可用图层[%1]: %2").arg(i).arg(n),
					"MapProductTools", Qgis::Info);
				if (n.compare(layerName, Qt::CaseInsensitive) == 0)
				{
					poSrcLayer = pLayer;
				}
			}
		}
	}
	else
	{
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 精确匹配到图层: %1").arg(layerName),
			"MapProductTools", Qgis::Info);
	}

	if (!poSrcLayer)
	{
		m_lastError = QString("在文件中未找到图层: %1 (共 %2 个图层，驱动=%3)")
			.arg(layerName).arg(totalLayerCount).arg(driverName);
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] %1").arg(m_lastError),
			"MapProductTools", Qgis::Critical);
		emit importFailed(targetTable, m_lastError);
		GDALClose(poSrcDS);
		return -1;
	}

	// 重置统计
	m_lastVectorStats = VectorImportStats();

	emit importProgress(10, QString("正在读取图层 %1 的结构...").arg(layerName));

	OGRwkbGeometryType eGeomType = poSrcLayer->GetGeomType();
	QString geomTypeName = OGRGeometryTypeToName(eGeomType);
	GIntBig estCount = poSrcLayer->GetFeatureCount(true);
	OGRFeatureDefn* poSrcFDefn = poSrcLayer->GetLayerDefn();
	int nFields = poSrcFDefn ? poSrcFDefn->GetFieldCount() : 0;

	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 图层信息: 名称=%1 几何=%2 预估要素数=%3 字段数=%4")
			.arg(layerName).arg(geomTypeName)
			.arg(estCount >= 0 ? QString::number(estCount) : "未知").arg(nFields),
		"MapProductTools", Qgis::Info);

	// 输出字段定义详情
	for (int f = 0; f < nFields; ++f)
	{
		OGRFieldDefn* pfDefn = poSrcFDefn->GetFieldDefn(f);
		if (pfDefn)
		{
			QgsMessageLog::logMessage(
				QStringLiteral("[GDB矢量导入]   字段[%1]: %2 类型=%3 宽度=%4")
					.arg(f)
					.arg(QString::fromUtf8(pfDefn->GetNameRef()))
					.arg(OGRFieldDefn::GetFieldTypeName(pfDefn->GetType()))
					.arg(pfDefn->GetWidth()),
				"MapProductTools", Qgis::Info);
		}
	}

	if (eGeomType == wkbNone || eGeomType == wkbUnknown)
	{
		m_lastError = "矢量图层无有效几何类型，无法导入";
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 图层无有效几何类型"), "MapProductTools", Qgis::Critical);
		GDALClose(poSrcDS);
		return -1;
	}

	// 获取源图层的空间参考
	OGRSpatialReference* poSrcSRS = poSrcLayer->GetSpatialRef();
	if (poSrcSRS)
	{
		char* pszProj4 = nullptr;
		poSrcSRS->exportToProj4(&pszProj4);
		const char* pszAuthName = poSrcSRS->GetAuthorityName(nullptr);
		const char* pszAuthCode = poSrcSRS->GetAuthorityCode(nullptr);
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 源SRS: %1:%2 PROJ4=%3")
				.arg(pszAuthName ? pszAuthName : "?")
				.arg(pszAuthCode ? pszAuthCode : "?")
				.arg(pszProj4 ? QString::fromUtf8(pszProj4) : "?"),
			"MapProductTools", Qgis::Info);
		if (pszProj4) CPLFree(pszProj4);
	}
	else
	{
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 源图层无 SRS 信息，将使用 srcSrid=%1").arg(srcSrid),
			"MapProductTools", Qgis::Warning);
	}
	OGRSpatialReference oTargetSRS;
	oTargetSRS.importFromEPSG(targetSrid);

	bool needTransform = false;
	OGRCoordinateTransformation* poCT = nullptr;
	if (poSrcSRS && !poSrcSRS->IsSame(&oTargetSRS))
	{
		needTransform = true;
		poCT = OGRCreateCoordinateTransformation(poSrcSRS, &oTargetSRS);
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 需要坐标转换: source -> EPSG:%1").arg(targetSrid),
			"MapProductTools", Qgis::Info);
	}
	else
	{
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 无需坐标转换"),
			"MapProductTools", Qgis::Info);
	}

	// 创建 PostGIS 目标表
	SchemaManager schemaMgr;
	schemaMgr.dropDataLayerTable(targetTable);
	if (!schemaMgr.createDataLayerTable(targetTable, geomTypeName, targetSrid))
	{
		m_lastError = schemaMgr.lastError();
		QgsMessageLog::logMessage(
			QStringLiteral("[GDB矢量导入] 建表失败: %1 表=%2").arg(m_lastError).arg(targetTable),
			"MapProductTools", Qgis::Critical);
		GDALClose(poSrcDS);
		return -1;
	}
	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 目标表创建成功: %1 几何=%2 SRID=%3")
			.arg(targetTable).arg(geomTypeName).arg(targetSrid),
		"MapProductTools", Qgis::Info);

	// 动态添加属性列
	int addedFieldCount = 0;
	int failedFieldCount = 0;
	for (int iField = 0; iField < nFields; ++iField)
	{
		OGRFieldDefn* poFieldDefn = poSrcFDefn->GetFieldDefn(iField);
		QString fieldName = QString::fromUtf8(poFieldDefn->GetNameRef());

		if (fieldName.compare("id", Qt::CaseInsensitive) == 0)
			fieldName = "fid";

		QString pgType;
		switch (poFieldDefn->GetType())
		{
		case OFTInteger:
		case OFTInteger64:
			pgType = "INTEGER";
			break;
		case OFTReal:
			pgType = "DOUBLE PRECISION";
			break;
		case OFTString:
			pgType = QString("VARCHAR(%1)").arg(qMax(poFieldDefn->GetWidth(), 254));
			break;
		case OFTDate:
			pgType = "DATE";
			break;
		case OFTTime:
			pgType = "TIME";
			break;
		case OFTDateTime:
			pgType = "TIMESTAMP";
			break;
		default:
			pgType = "VARCHAR(512)";
			break;
		}

		QString alterSql = QString("ALTER TABLE \"%1\" ADD COLUMN IF NOT EXISTS \"%2\" %3")
			.arg(targetTable, fieldName, pgType);

		if (!db->executeNonQuery(alterSql))
		{
			failedFieldCount++;
			QgsMessageLog::logMessage(
				QStringLiteral("[GDB矢量导入] 添加字段失败: %1").arg(alterSql),
				"MapProductTools", Qgis::Warning);
		}
		else
		{
			addedFieldCount++;
		}
	}
	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 字段添加完成: 成功=%1 失败=%2")
			.arg(addedFieldCount).arg(failedFieldCount),
		"MapProductTools", Qgis::Info);

	emit importProgress(30, "正在写入要素数据...");

	// 构建 INSERT 语句模板
	QStringList fieldNames;
	QStringList fieldPlaceholders;
	int paramIdx = 2; // $1 = geom

	QList<QString> actualFields;
	QList<OGRFieldType> actualFieldTypes;
	for (int iField = 0; iField < nFields; ++iField)
	{
		OGRFieldDefn* poFieldDefn = poSrcFDefn->GetFieldDefn(iField);
		QString fName = QString::fromUtf8(poFieldDefn->GetNameRef());
		if (fName.compare("id", Qt::CaseInsensitive) == 0)
			fName = "fid";

		actualFields.append(fName);
		actualFieldTypes.append(poFieldDefn->GetType());
		fieldNames.append(QString("\"%1\"").arg(fName));
		fieldPlaceholders.append(QString("$%1").arg(paramIdx++));
	}

	// 开始事务
	db->beginTransaction();

	int featureCount = 0;
	int skipNoGeom = 0;
	int skipInsertFail = 0;
	int batchCount = 0;
	const int BATCH_SIZE = 500;

	poSrcLayer->ResetReading();
	OGRFeature* poFeature = nullptr;

	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 开始读取要素: 表=%1 预估=%2 字段数=%3")
			.arg(targetTable)
			.arg(estCount >= 0 ? QString::number(estCount) : "未知")
			.arg(nFields),
		"MapProductTools", Qgis::Info);

	while ((poFeature = poSrcLayer->GetNextFeature()) != nullptr)
	{
		m_lastVectorStats.totalRead++;

		OGRGeometry* poGeom = poFeature->GetGeometryRef();
		if (!poGeom)
		{
			skipNoGeom++;
			m_lastVectorStats.skipNoGeom++;
			OGRFeature::DestroyFeature(poFeature);
			continue;
		}

		// 坐标转换
		if (needTransform && poCT)
		{
			poGeom->transform(poCT);
		}

		// 检查轴序
		{
			OGREnvelope env;
			poGeom->getEnvelope(&env);
			if (env.MinX >= -90.0 && env.MinX <= 90.0 && env.MaxY > 90.0)
			{
				poGeom->swapXY();
			}
		}

		char* pszWKT = nullptr;
		OGRErr eErr = poGeom->exportToWkt(&pszWKT);
		if (eErr != OGRERR_NONE || !pszWKT)
		{
			m_lastVectorStats.skipInsertFail++;
			OGRFeature::DestroyFeature(poFeature);
			continue;
		}
		QString wkt = QString::fromUtf8(pszWKT);
		CPLFree(pszWKT);

		QString geomExpr = QString("ST_MakeValid(ST_Force2D(ST_SetSRID(ST_GeomFromText($1), %1)))").arg(targetSrid);
		QString insertSql;
		if (fieldNames.isEmpty())
		{
			insertSql = QString("INSERT INTO \"%1\" (geom) VALUES (%2)")
				.arg(targetTable, geomExpr);
		}
		else
		{
			insertSql = QString("INSERT INTO \"%1\" (geom, %2) VALUES (%3, %4)")
				.arg(targetTable, fieldNames.join(", "),
					 geomExpr,
					 fieldPlaceholders.join(", "));
		}

		QVariantList params;
		params << wkt;

		for (int iField = 0; iField < actualFields.size(); ++iField)
		{
			int ogrIdx = poFeature->GetFieldIndex(
				actualFields[iField].toUtf8().constData());
			if (ogrIdx < 0)
			{
				params << QVariant();
				continue;
			}

			if (!poFeature->IsFieldSetAndNotNull(ogrIdx))
			{
				params << QVariant();
				continue;
			}

			OGRFieldType ft = actualFieldTypes[iField];
			switch (ft)
			{
			case OFTInteger:
				params << poFeature->GetFieldAsInteger(ogrIdx);
				break;
			case OFTInteger64:
				params << QVariant(static_cast<qint64>(poFeature->GetFieldAsInteger64(ogrIdx)));
				break;
			case OFTReal:
				params << poFeature->GetFieldAsDouble(ogrIdx);
				break;
			case OFTString:
				params << QString::fromUtf8(poFeature->GetFieldAsString(ogrIdx));
				break;
			case OFTDate:
			case OFTTime:
			case OFTDateTime:
			{
				int year, month, day, hour, minute, second, tz;
				poFeature->GetFieldAsDateTime(ogrIdx, &year, &month, &day,
											   &hour, &minute, &second, &tz);
				params << QString("%1-%2-%3 %4:%5:%6")
					.arg(year, 4, 10, QChar('0'))
					.arg(month, 2, 10, QChar('0'))
					.arg(day, 2, 10, QChar('0'))
					.arg(hour, 2, 10, QChar('0'))
					.arg(minute, 2, 10, QChar('0'))
					.arg(second, 2, 10, QChar('0'));
				break;
			}
			default:
				params << QString::fromUtf8(poFeature->GetFieldAsString(ogrIdx));
				break;
			}
		}

		db->executeNonQuery("SAVEPOINT sp_row");

		if (db->executeNonQuery(insertSql, params))
		{
			db->executeNonQuery("RELEASE SAVEPOINT sp_row");
			featureCount++;
			batchCount++;
			m_lastVectorStats.featureCount++;
		}
		else
		{
			skipInsertFail++;
			m_lastVectorStats.skipInsertFail++;
			QString errMsg = db->lastError();
			db->executeNonQuery("ROLLBACK TO sp_row");

			// 收集前几条错误信息用于诊断
			if (skipInsertFail <= 5 && !errMsg.isEmpty())
			{
				if (m_lastError.isEmpty())
					m_lastError = errMsg;
				else
					m_lastError += "; " + errMsg;

				QgsMessageLog::logMessage(
					QStringLiteral("INSERT 失败 (第%1条): %2").arg(m_lastVectorStats.totalRead).arg(errMsg),
					"MapProductTools", Qgis::Warning);
			}
		}

		OGRFeature::DestroyFeature(poFeature);

		if (batchCount >= BATCH_SIZE)
		{
			db->commitTransaction();
			batchCount = 0;
			emit importProgress(40 + (featureCount % 10000) / 200,
				QString("已写入 %1 个要素...").arg(featureCount));
		}
	}

	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 要素读取完成: 表=%1 总读取=%2 成功写入=%3 无几何=%4 INSERT失败=%5")
			.arg(targetTable)
			.arg(m_lastVectorStats.totalRead)
			.arg(featureCount)
			.arg(skipNoGeom)
			.arg(skipInsertFail),
		"MapProductTools", Qgis::Info);

	db->commitTransaction();
	if (poCT) OCTDestroyCoordinateTransformation(poCT);
	GDALClose(poSrcDS);

	// 诊断：读取了要素但全部入库失败时，设置详细错误信息
	if (featureCount == 0 && m_lastVectorStats.totalRead > 0)
	{
		QString diag = QStringLiteral("所有要素入库失败 (读取 %1 个, 无几何 %2, INSERT失败 %3)")
			.arg(m_lastVectorStats.totalRead)
			.arg(m_lastVectorStats.skipNoGeom)
			.arg(m_lastVectorStats.skipInsertFail);

		if (m_lastError.isEmpty())
			m_lastError = diag;
		else
			m_lastError = diag + ": " + m_lastError;

		QgsMessageLog::logMessage(
			QStringLiteral("矢量图层导入无要素写入: 表=%1 图层=%2 %3").arg(targetTable).arg(layerName).arg(diag),
			"MapProductTools", Qgis::Critical);
	}
	else if (m_lastVectorStats.skipInsertFail > 0)
	{
		QgsMessageLog::logMessage(
			QStringLiteral("矢量图层导入部分成功: 表=%1 图层=%2 成功=%3 失败=%4")
				.arg(targetTable).arg(layerName)
				.arg(m_lastVectorStats.featureCount)
				.arg(m_lastVectorStats.skipInsertFail),
			"MapProductTools", Qgis::Warning);
	}

	// 创建空间索引
	QString indexSql = QString("CREATE INDEX IF NOT EXISTS idx_%1_geom ON \"%1\" USING GIST (geom)")
		.arg(targetTable);
	bool idxOk = db->executeNonQuery(indexSql);

	QgsMessageLog::logMessage(
		QStringLiteral("[GDB矢量导入] 完成: 文件=%1 图层=%2 表=%3 总入库=%4 空间索引=%5")
			.arg(filePath).arg(layerName).arg(targetTable).arg(featureCount)
			.arg(idxOk ? "成功" : "失败"),
		"MapProductTools", Qgis::Info);

	emit importProgress(95, QString("图层 %1 写入完成: %2 个要素").arg(layerName).arg(featureCount));
	emit importCompleted(targetTable, featureCount);

	return featureCount;
}

bool DataImporter::importRasterToPostGIS(const QString& filePath,
										  const QString& targetTable,
										  int targetSrid)
{
	auto* db = PostgisConnector::instance();
	if (!db->isConnected())
	{
		m_lastError = "数据库未连接";
		emit importFailed(targetTable, m_lastError);
		return false;
	}

	// 验证文件存在
	QFileInfo fi(filePath);
	if (!fi.exists())
	{
		m_lastError = QString("栅格文件不存在: %1").arg(filePath);
		return false;
	}

	emit importProgress(10, "正在将栅格数据写入 PostGIS...");

	PGconn* conn = db->nativeConnection();
	qint64 fileSize = fi.size();
	QString fileName = fi.fileName();

	// 使用配置的 schema（默认为 public）
	QString schema = db->searchPath();
	if (schema.isEmpty()) schema = "public";

	// 在事务中执行：建表 + BLOB 导入 + 元数据写入
	PGresult* beginRes = PQexec(conn, "BEGIN");
	if (PQresultStatus(beginRes) != PGRES_COMMAND_OK)
	{
		m_lastError = QString("事务开启失败: %1").arg(PQresultErrorMessage(beginRes));
		emit importFailed(targetTable, m_lastError);
		return false;
	}
	PQclear(beginRes);

	// 删除旧表（如果存在）
	QString dropSql = QString("DROP TABLE IF EXISTS \"%1\".\"%2\" CASCADE").arg(schema, targetTable);
	PGresult* dropRes = PQexec(conn, dropSql.toUtf8().constData());
	if (PQresultStatus(dropRes) != PGRES_COMMAND_OK)
	{
	}
	PQclear(dropRes);

	// 创建 BLOB 元数据表
	QString createSql = QString(
		"CREATE TABLE \"%1\".\"%2\" ("
		"  id SERIAL PRIMARY KEY,"
		"  file_name TEXT NOT NULL,"
		"  file_oid OID NOT NULL,"
		"  file_size BIGINT,"
		"  srid INTEGER DEFAULT %3,"
		"  created_at TIMESTAMP DEFAULT NOW()"
		")").arg(schema, targetTable).arg(targetSrid);

	PGresult* createRes = PQexec(conn, createSql.toUtf8().constData());
	if (PQresultStatus(createRes) != PGRES_COMMAND_OK)
	{
		m_lastError = QString("建表失败: %1").arg(PQresultErrorMessage(createRes));
		PQexec(conn, "ROLLBACK");
		emit importFailed(targetTable, m_lastError);
		return false;
	}
	PQclear(createRes);
	emit importProgress(40, "正在导入栅格文件...");

	// 使用客户端流式上传（支持跨机器部署）
	Oid fileOid = PostgisConnector::loImport(conn, filePath);
	if (fileOid == InvalidOid)
	{
		m_lastError = QString("BLOB 导入失败: %1").arg(QString::fromUtf8(PQerrorMessage(conn)));
		PQexec(conn, "ROLLBACK");
		emit importFailed(targetTable, m_lastError);
		return false;
	}
	emit importProgress(70, "正在写入元数据...");

	// 插入元数据行
	QByteArray insertSql = QString(
		"INSERT INTO \"%1\".\"%2\" (file_name, file_oid, file_size, srid) "
		"VALUES ($1, $2::oid, $3::bigint, $4::integer)").arg(schema, targetTable).toUtf8();

	QByteArray fileNameUtf8 = fileName.toUtf8();
	QByteArray oidStr = QByteArray::number(fileOid);
	QByteArray sizeStr = QByteArray::number(fileSize);
	QByteArray sridStr = QByteArray::number(targetSrid);

	const char* params[4] = {
		fileNameUtf8.constData(),
		oidStr.constData(),
		sizeStr.constData(),
		sridStr.constData()
	};
	int lengths[4] = {
		(int)fileNameUtf8.size(),
		(int)oidStr.size(),
		(int)sizeStr.size(),
		(int)sridStr.size()
	};
	int formats[4] = { 0, 0, 0, 0 };

	PGresult* insertRes = PQexecParams(conn, insertSql.constData(),
		4, nullptr, params, lengths, formats, 0);

	if (PQresultStatus(insertRes) != PGRES_COMMAND_OK)
	{
		m_lastError = QString("元数据写入失败: %1").arg(PQresultErrorMessage(insertRes));
		PQexec(conn, "ROLLBACK");
		emit importFailed(targetTable, m_lastError);
		return false;
	}
	PQclear(insertRes);

	// 提交事务
	PGresult* commitRes = PQexec(conn, "COMMIT");
	if (PQresultStatus(commitRes) != PGRES_COMMAND_OK)
	{
		m_lastError = QString("事务提交失败: %1").arg(PQresultErrorMessage(commitRes));
		emit importFailed(targetTable, m_lastError);
		return false;
	}
	PQclear(commitRes);

	// 验证表确实存在
	QString verifySql = QString(
		"SELECT COUNT(*) AS cnt FROM \"%1\".\"%2\"").arg(schema, targetTable);
	auto verify = db->executeQueryOne(verifySql);
	emit importProgress(100, "栅格数据入库完成");
	emit importCompleted(targetTable, 1);
	return true;
}

int DataImporter::importViaOGRPGDriver(const QString& filePath,
										const QString& targetTable,
										int targetSrid,
										const QString& encoding)
{
	// 备用方法：使用 OGR PG driver（当前主流程使用 libpq 方式，此方法备用）
	Q_UNUSED(filePath)
	Q_UNUSED(targetTable)
	Q_UNUSED(targetSrid)
	Q_UNUSED(encoding)
	return -1;
}

QString DataImporter::getPGConnectionString() const
{
	auto* db = PostgisConnector::instance();
	return db->gdalConnectionString();
}
