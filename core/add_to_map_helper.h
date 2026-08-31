#ifndef ADD_TO_MAP_HELPER_H
#define ADD_TO_MAP_HELPER_H

// 添加到地图通用辅助函数
// 从 PostGIS 数据库将地图成果加载到 QGIS 地图画布。
// 供成果存储与管理、元数据管理、成果检索与预览等多个对话框复用。

#include "database/product_metadata.h"
#include "database/postgis_connector.h"

#include "qgisinterface.h"
#include "qgsvectorlayer.h"
#include "qgsrasterlayer.h"
#include "qgsproject.h"

#include <QString>
#include <QStringList>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QMessageBox>
#include <libpq-fe.h>

namespace AddToMapHelper
{

// 大小写不敏感查找 PostgreSQL 中实际的表名
// 背景：PostgreSQL 对不带引号的标识符自动折叠为小写。
// 之前未加引号入库的数据以小写存储（如 j48g064093_resa），
// 但 QGIS URI 中带引号的表名是大小写敏感的。
// 此方法通过查询 pg_tables 做大小写不敏感匹配，返回实际存储的表名。
// 返回空字符串表示未找到。
inline QString resolveTableName(const QString& schema, const QString& candidateName)
{
	PostgisConnector* connector = PostgisConnector::instance();
	if (!connector || !connector->isConnected())
		return QString();

	PGconn* pg = connector->nativeConnection();
	if (!pg || PQstatus(pg) != CONNECTION_OK)
		return QString();

	// 用 LOWER() 做大小写不敏感匹配，找到 pg_tables 中实际存在的表名
	QString query = QString(
		"SELECT tablename FROM pg_tables "
		"WHERE schemaname = '%1' AND LOWER(tablename) = LOWER('%2') "
		"LIMIT 1")
		.arg(schema, candidateName);

	PGresult* res = PQexec(pg, query.toUtf8().constData());
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
	{
		QString found = QString::fromUtf8(PQgetvalue(res, 0, 0));
		PQclear(res);
		return found;
	}
	PQclear(res);
	return QString();
}

// 将指定地图成果加载到 QGIS 地图画布。
// iface 为 QGIS 接口，meta 为目标成果元数据。
// 成功返回 true，失败返回 false。
inline bool addLayerToMap(QgisInterface* iface, const ProductMetadata& meta)
{
	if (!iface)
		return false;

	PostgisConnector* connector = PostgisConnector::instance();
	if (!connector || !connector->isConnected())
		return false;

	PGconn* pg = connector->nativeConnection();
	if (!pg)
		return false;

	QString host = QString::fromUtf8(PQhost(pg));
	QString port = QString::fromUtf8(PQport(pg));
	QString dbName = QString::fromUtf8(PQdb(pg));
	QString user = QString::fromUtf8(PQuser(pg));
	QString password = QString::fromUtf8(PQpass(pg));
	QString schema = connector->searchPath();
	if (schema.isEmpty()) schema = "public";

	// 优先使用含版本后缀的表名（当前版本表名格式为 xxx_v{N}）
	QString baseName = meta.layerTableName;

	// 大小写不敏感查找实际表名：先按完整表名（含 _vN 后缀）匹配
	QString tableName = resolveTableName(schema, baseName);
	if (tableName.isEmpty())
	{
		// 回退：去掉 _vN 版本后缀，兼容历史数据（旧表名不含版本后缀）
		QString strippedName = baseName;
		strippedName.replace(QRegularExpression("_v\\d+$"), "");
		if (strippedName != baseName)
			tableName = resolveTableName(schema, strippedName);
	}
	if (tableName.isEmpty())
	{
		// 数据库中确实没有这张表
		tableName = baseName;
	}

	// 构建 PostGIS URI 并加载到 QGIS
	if (meta.productType == ProductType::Vector)
	{
		QString geomType = meta.geometryType.toUpper();
		if (geomType.isEmpty()) geomType = "GEOMETRY";

		QString uri = QString(
			"dbname='%1' host='%2' port='%3' user='%4' password='%5' "
			"sslmode=disable key='id' srid=4490 type=%6 table=\"%7\".\"%8\" (geom)")
			.arg(dbName, host, port, user, password)
			.arg(geomType)
			.arg(schema, tableName);

		QgsVectorLayer* layer = new QgsVectorLayer(uri, meta.productName, "postgres");
		if (!layer->isValid())
		{
			delete layer;
			return false;
		}
		QgsProject::instance()->addMapLayer(layer);
		return true;
	}
	else if (meta.productType == ProductType::Raster)
	{
		// 栅格数据：BLOB导出到本地temp文件，再用QgsRasterLayer加载
		// （当前不必用 PG: URI，因为栅格以 Large Object 形式存储而非 PostGIS raster 格式）
		QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
		QDir().mkpath(tempDir);

		// 老数据 fileOid 可能为 0，从栅格元数据表回退查询
		int rasterOid = meta.fileOid;
		if (rasterOid <= 0 && !meta.layerTableName.isEmpty())
		{
			QVariantMap row = PostgisConnector::instance()->executeQueryOne(
				QString("SELECT file_oid FROM \"%1\" LIMIT 1").arg(meta.layerTableName));
			if (!row.isEmpty())
				rasterOid = row.value("file_oid").toInt();
		}

		QString localRasterPath;
		if (rasterOid > 0)
		{
			localRasterPath = tempDir + "/" + meta.productName + "_oid"
				+ QString::number(rasterOid) + "." + meta.fileFormat.toLower();
		}
		else if (!meta.filePath.isEmpty())
		{
			QFileInfo fi(meta.filePath);
			QString targetFileName = fi.fileName();
			if (targetFileName.isEmpty())
				targetFileName = meta.productName + "." + meta.fileFormat.toLower();
			localRasterPath = tempDir + "/" + targetFileName;
		}
		else
		{
			return false;
		}

		// 如果缓存文件不存在，从 PG BLOB 导出或从本地文件复制
		if (!QFile::exists(localRasterPath))
		{
			if (rasterOid > 0)
			{
				if (!PostgisConnector::loExport(pg, rasterOid, localRasterPath))
				{
					return false;
				}
			}
			else
			{
				if (!QFile::exists(meta.filePath))
				{
					return false;
				}
				if (!QFile::copy(meta.filePath, localRasterPath))
				{
					return false;
				}
			}
		}

		QgsRasterLayer* layer = new QgsRasterLayer(localRasterPath, meta.productName);
		if (!layer->isValid())
		{
			delete layer;
			return false;
		}
		QgsProject::instance()->addMapLayer(layer);
		return true;
	}

	return false;
}

// 将成果添加到地图，并在弹窗中反馈结果。
// 内部先做支持性检查（制图格式/非矢量栅格/缺少图层表），再执行加载。
// 提示文案、弹窗样式、成功时是否提示均与成果存储与管理对话框完全一致，
// 供各对话框统一复用，保证三个入口行为一致。
// 返回 true 表示成功，false 表示不支持或加载失败（已弹出提示）。
inline bool addToMapWithFeedback(QWidget* parent, QgisInterface* iface, const ProductMetadata& meta)
{
	// 1) 制图文件格式（ai/cdr/pdf）不支持 → 普通提示
	QString ext = meta.fileFormat.toLower();
	static const QStringList unsupportedFormats = { "ai", "cdr", "pdf" };
	if (unsupportedFormats.contains(ext))
	{
		QMessageBox::information(parent, QStringLiteral("提示"),
			QStringLiteral("「%1」格式不支持添加到地图，该格式属于制图文件，不含空间数据。")
				.arg(ext.toUpper()));
		return false;
	}

	// 2) 非矢量/栅格类型不支持 → 普通提示
	if (meta.productType != ProductType::Vector && meta.productType != ProductType::Raster)
	{
		QMessageBox::information(parent, QStringLiteral("提示"),
			QStringLiteral("产品「%1」（类型: %2）不支持添加到地图，仅矢量和栅格数据支持。")
				.arg(meta.productName)
				.arg(productTypeToString(meta.productType)));
		return false;
	}

	// 3) 缺少空间图层表 → 警告
	if (meta.layerTableName.isEmpty())
	{
		QMessageBox::warning(parent, QStringLiteral("提示"),
			QStringLiteral("产品「%1」没有对应的空间图层表，无法添加到地图。")
				.arg(meta.productName));
		return false;
	}

	// 4) 实际加载失败 → 警告
	if (!addLayerToMap(iface, meta))
	{
		QMessageBox::warning(parent, QStringLiteral("错误"),
			QStringLiteral("产品「%1」添加到地图失败，请检查数据库连接和图层表「%2」是否存在。")
				.arg(meta.productName)
				.arg(meta.layerTableName));
		return false;
	}

	// 成功：与成果存储与管理一致，不额外弹窗
	return true;
}

} // namespace AddToMapHelper

#endif // ADD_TO_MAP_HELPER_H
