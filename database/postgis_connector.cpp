#include "postgis_connector.h"
#include <libpq-fe.h>
#include <qgis.h>
#include <qgsmessagelog.h>
#include <QUuid>
#include <QSet>
#include <QStringList>
#include <QFile>

// libpq Large Object 访问模式常量（兼容部分 SDK 头文件未导出）
#ifndef INV_WRITE
#define INV_WRITE 0x00020000
#endif
#ifndef INV_READ
#define INV_READ  0x00040000
#endif

PostgisConnector* PostgisConnector::s_instance = nullptr;
std::mutex PostgisConnector::s_mutex;

PostgisConnector* PostgisConnector::instance()
{
	if (s_instance == nullptr)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_instance == nullptr)
		{
			s_instance = new PostgisConnector();
		}
	}
	return s_instance;
}

PostgisConnector::PostgisConnector(QObject* parent)
	: QObject(parent)
{
}

PostgisConnector::~PostgisConnector()
{
	disconnect();
}

bool PostgisConnector::connect(const QString& host, int port, const QString& dbName,
								const QString& user, const QString& password)
{
	QString connInfo = QString("host=%1 port=%2 dbname=%3 user=%4 password=%5")
		.arg(host)
		.arg(port)
		.arg(dbName)
		.arg(user)
		.arg(password);

	m_conn = PQconnectdb(connInfo.toUtf8().constData());

	if (PQstatus(m_conn) != CONNECTION_OK)
	{
		m_lastError = QString::fromUtf8(PQerrorMessage(m_conn));
		m_connected = false;
		emit connectionStatusChanged(false);
		emit errorOccurred(m_lastError);
		return false;
	}

	m_connected = true;

	// 连接成功后恢复之前设置的 search_path
	if (!m_searchPath.isEmpty())
	{
		QString setPathSQL = QString("SET search_path TO %1, public").arg(m_searchPath);
		PGresult* res = PQexec(m_conn, setPathSQL.toUtf8().constData());
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			QgsMessageLog::logMessage(
				QStringLiteral("恢复 search_path 失败: %1")
					.arg(QString::fromUtf8(PQerrorMessage(m_conn))),
				QStringLiteral("MapProductTools"), Qgis::Warning);
		}
		PQclear(res);  // 无论成败都释放结果，避免连接期间反复泄漏
	}

	// 自动运行 Schema 迁移，确保 service_records 表包含所有必要字段
	migrateServiceSchema();

	emit connectionStatusChanged(true);
	return true;
}

void PostgisConnector::setSearchPath(const QString& schema)
{
	QString oldPath = m_searchPath;
	m_searchPath = schema;
	if (isConnected() && !schema.isEmpty() && schema != "public")
	{
		executeNonQuery(QString("SET search_path TO %1, public").arg(schema));
		// 仅在实际发生切换时通知 UI 刷新数据
		if (oldPath != schema && !oldPath.isEmpty())
		{
			emit schemaChanged(schema);
		}
	}
}

QString PostgisConnector::searchPath() const
{
	return m_searchPath;
}

void PostgisConnector::disconnect()
{
	if (m_conn)
	{
		PQfinish(m_conn);
		m_conn = nullptr;
	}
	m_connected = false;
	emit connectionStatusChanged(false);
}

bool PostgisConnector::isConnected() const
{
	return m_connected && m_conn && PQstatus(m_conn) == CONNECTION_OK;
}

QString PostgisConnector::lastError() const
{
	return m_lastError;
}

PGresult* PostgisConnector::executeSQL(const QString& sql, const QVariantList& params)
{
	if (!isConnected())
	{
		m_lastError = "数据库未连接";
		return nullptr;
	}

	// 构建参数数组
	int nParams = params.size();
	const char** paramValues = nullptr;
	int* paramLengths = nullptr;
	int* paramFormats = nullptr;
	QByteArray** paramBuffers = nullptr;  // 持有参数数据的实际缓冲区

	if (nParams > 0)
	{
		paramValues = new const char*[nParams];
		paramLengths = new int[nParams];
		paramFormats = new int[nParams];
		paramBuffers = new QByteArray*[nParams];

		for (int i = 0; i < nParams; ++i)
		{
			paramBuffers[i] = nullptr;

			if (params[i].isNull())
			{
				// null QVariant → SQL NULL（传 nullptr + 长度为 0）
				paramValues[i] = nullptr;
				paramLengths[i] = 0;
				paramFormats[i] = 0;
			}
			else
			{
				paramBuffers[i] = new QByteArray(params[i].toString().toUtf8());
				paramValues[i] = paramBuffers[i]->constData();
				paramLengths[i] = paramBuffers[i]->size();
				paramFormats[i] = 0; // 文本格式
			}
		}
	}

	PGresult* res = PQexecParams(m_conn,
		sql.toUtf8().constData(),
		nParams,
		nullptr,        // paramTypes
		paramValues,
		paramLengths,
		paramFormats,
		0);             // 文本结果格式

	// 清理参数缓冲区（必须在 PQexecParams 之后）
	if (paramBuffers)
	{
		for (int i = 0; i < nParams; ++i)
		{
			delete paramBuffers[i];
		}
		delete[] paramBuffers;
	}
	if (paramValues)
	{
		delete[] paramValues;
		delete[] paramLengths;
		delete[] paramFormats;
	}

	if (PQresultStatus(res) != PGRES_TUPLES_OK &&
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		QString errMsg = QString::fromUtf8(PQresultErrorMessage(res));
		m_lastError = errMsg;

		// 构建参数信息用于日志
		QStringList paramStr;
		for (int i = 0; i < nParams && i < 20; ++i) {
			paramStr << QString("$%1=%2").arg(i + 1).arg(params.value(i).toString());
		}
		QString paramInfo = paramStr.isEmpty() ? QStringLiteral("(无参数)") : paramStr.join(QStringLiteral(", "));

		// SQL 截断（前 500 字符），避免日志过长
		QString sqlPreview = sql.length() > 500
			? sql.left(500) + QStringLiteral("...[截断, 总长=%1]").arg(sql.length())
			: sql;

		QgsMessageLog::logMessage(
			QStringLiteral("[SQL错误] %1\n参数: %2\nSQL: %3")
				.arg(errMsg.trimmed(), paramInfo, sqlPreview),
			QStringLiteral("MapProductTools"), Qgis::Critical);

		PQclear(res);
		return nullptr;
	}

	int nRows = PQntuples(res);
	return res;
}

void PostgisConnector::freeResult(PGresult* res)
{
	if (res) PQclear(res);
}

QVariantList PostgisConnector::resultToVariantList(PGresult* res)
{
	QVariantList results;
	if (!res) return results;

	int nRows = PQntuples(res);
	int nCols = PQnfields(res);

	for (int r = 0; r < nRows; ++r)
	{
		QVariantMap row;
		for (int c = 0; c < nCols; ++c)
		{
			QString colName = QString::fromUtf8(PQfname(res, c));
			QString value = QString::fromUtf8(PQgetvalue(res, r, c));
			row[colName] = value;
		}
		results.append(row);
	}
	return results;
}

QVariantMap PostgisConnector::resultRowToMap(PGresult* res, int row)
{
	QVariantMap map;
	if (!res || row >= PQntuples(res)) return map;

	int nCols = PQnfields(res);
	for (int c = 0; c < nCols; ++c)
	{
		QString colName = QString::fromUtf8(PQfname(res, c));
		QString value = QString::fromUtf8(PQgetvalue(res, row, c));
		map[colName] = value;
	}
	return map;
}

QVariantList PostgisConnector::executeQuery(const QString& sql, const QVariantList& params)
{
	PGresult* res = executeSQL(sql, params);
	if (!res) return QVariantList();

	QVariantList results = resultToVariantList(res);
	freeResult(res);
	return results;
}

QVariantMap PostgisConnector::executeQueryOne(const QString& sql, const QVariantList& params)
{
	PGresult* res = executeSQL(sql, params);
	if (!res) return QVariantMap();

	QVariantMap result;
	if (PQntuples(res) > 0)
		result = resultRowToMap(res, 0);
	freeResult(res);
	return result;
}

bool PostgisConnector::executeNonQuery(const QString& sql, const QVariantList& params)
{
	PGresult* res = executeSQL(sql, params);
	if (!res) return false;
	freeResult(res);
	return true;
}

bool PostgisConnector::beginTransaction()
{
	return executeNonQuery("BEGIN");
}

bool PostgisConnector::commitTransaction()
{
	return executeNonQuery("COMMIT");
}

bool PostgisConnector::rollbackTransaction()
{
	return executeNonQuery("ROLLBACK");
}

int PostgisConnector::loImport(const QString& filePath)
{
	if (!isConnected())
	{
		m_lastError = QStringLiteral("数据库未连接");
		return -1;
	}

	QString errMsg;
	int oid = loImport(m_conn, filePath, &errMsg);
	if (oid < 0)
	{
		m_lastError = errMsg;
		emit errorOccurred(m_lastError);
	}
	return oid;
}

// 局部工具：在 PGconn 上执行一条 SQL，返回 false 表示失败
static bool pgExecSimple(PGconn* conn, const char* sql, QString* outError)
{
	PGresult* res = PQexec(conn, sql);
	ExecStatusType st = PQresultStatus(res);
	bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
	if (!ok && outError)
		*outError = QString::fromUtf8("%1 失败: %2").arg(QString::fromUtf8(sql)).arg(QString::fromUtf8(PQerrorMessage(conn)));
	PQclear(res);
	return ok;
}

int PostgisConnector::loImport(PGconn* conn, const QString& filePath, QString* outError)
{
	if (!conn)
	{
		if (outError) *outError = QStringLiteral("PGconn 为空");
		return -1;
	}

	// 判断是否需要自己管理事务（lo_open/write/close 必须在显式事务中）
	bool ownTx = (PQtransactionStatus(conn) == PQTRANS_IDLE);
	if (ownTx && !pgExecSimple(conn, "BEGIN", outError))
		return -1;

	// 1. 客户端读取文件（而非让 PostgreSQL 服务器去读，支持跨机器部署）
	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly))
	{
		if (outError) *outError = QString::fromUtf8("无法打开文件: %1 (%2)").arg(filePath, file.errorString());
		if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
		return -1;
	}

	// 2. 在服务器端创建 Large Object
	int oid = lo_create(conn, 0);
	if (oid == InvalidOid)
	{
		if (outError) *outError = QString::fromUtf8("lo_create 失败: %1").arg(QString::fromUtf8(PQerrorMessage(conn)));
		if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
		return -1;
	}

	// 3. 以写入模式打开 LO（需要事务上下文）
	int fd = lo_open(conn, oid, INV_WRITE);
	if (fd < 0)
	{
		if (outError) *outError = QString::fromUtf8("lo_open(WRITE) 失败: %1").arg(QString::fromUtf8(PQerrorMessage(conn)));
		lo_unlink(conn, oid);
		if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
		return -1;
	}

	// 4. 分块读取客户端文件，流式写入服务器 LO
	const int CHUNK_SIZE = 8192;
	char buffer[8192];
	while (!file.atEnd())
	{
		qint64 bytesRead = file.read(buffer, CHUNK_SIZE);
		if (bytesRead <= 0) break;

		int bytesWritten = lo_write(conn, fd, buffer, static_cast<size_t>(bytesRead));
		if (bytesWritten < 0)
		{
			if (outError) *outError = QString::fromUtf8("lo_write 失败: %1").arg(QString::fromUtf8(PQerrorMessage(conn)));
			lo_close(conn, fd);
			lo_unlink(conn, oid);
			if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
			return -1;
		}
	}

	// 5. 关闭 LO、提交事务
	lo_close(conn, fd);
	if (ownTx && !pgExecSimple(conn, "COMMIT", outError))
	{
		lo_unlink(conn, oid);
		return -1;
	}
	return oid;
}

bool PostgisConnector::loExport(int oid, const QString& filePath)
{
	if (!isConnected())
	{
		m_lastError = QStringLiteral("数据库未连接");
		return false;
	}

	QString errMsg;
	bool ok = loExport(m_conn, oid, filePath, &errMsg);
	if (!ok)
	{
		m_lastError = errMsg;
		emit errorOccurred(m_lastError);
	}
	return ok;
}

bool PostgisConnector::loExport(PGconn* conn, int oid, const QString& filePath, QString* outError)
{
	if (!conn)
	{
		if (outError) *outError = QStringLiteral("PGconn 为空");
		return false;
	}

	// lo_open/read/close 必须在显式事务中
	bool ownTx = (PQtransactionStatus(conn) == PQTRANS_IDLE);
	if (ownTx && !pgExecSimple(conn, "BEGIN", outError))
		return false;

	// 1. 以读取模式打开 LO
	int fd = lo_open(conn, oid, INV_READ);
	if (fd < 0)
	{
		if (outError) *outError = QString::fromUtf8("lo_open(READ) 失败 (oid=%1): %2")
			.arg(oid).arg(QString::fromUtf8(PQerrorMessage(conn)));
		if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
		return false;
	}

	// 移动到开头
	lo_lseek(conn, fd, 0, SEEK_SET);

	// 2. 客户端创建输出文件
	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly))
	{
		if (outError) *outError = QString::fromUtf8("无法写入文件: %1 (%2)").arg(filePath, file.errorString());
		lo_close(conn, fd);
		if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
		return false;
	}

	// 3. 分块从服务器 LO 读取，写入客户端文件
	const int CHUNK_SIZE = 8192;
	char buffer[8192];
	while (true)
	{
		int bytesRead = lo_read(conn, fd, buffer, CHUNK_SIZE);
		if (bytesRead < 0)
		{
			if (outError) *outError = QString::fromUtf8("lo_read 失败: %1").arg(QString::fromUtf8(PQerrorMessage(conn)));
			lo_close(conn, fd);
			if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
			return false;
		}
		if (bytesRead == 0) break; // EOF

		if (file.write(buffer, bytesRead) != bytesRead)
		{
			if (outError) *outError = QString::fromUtf8("写入文件失败: %1 (%2)").arg(filePath, file.errorString());
			lo_close(conn, fd);
			if (ownTx) pgExecSimple(conn, "ROLLBACK", nullptr);
			return false;
		}
	}

	// 4. 关闭 LO，提交事务
	lo_close(conn, fd);
	if (ownTx && !pgExecSimple(conn, "COMMIT", outError))
		return false;
	return true;
}

bool PostgisConnector::loDelete(int oid)
{
	if (!isConnected()) return false;

	int result = lo_unlink(m_conn, oid);
	if (result != 1)
	{
		m_lastError = QString::fromUtf8(PQerrorMessage(m_conn));
		emit errorOccurred(m_lastError);
		return false;
	}
	return true;
}

QString PostgisConnector::gdalConnectionString() const
{
	if (!isConnected()) return QString();

	// 通过 libpq 获取连接参数，构建 GDAL PG 连接字符串
	QString host = QString::fromUtf8(PQhost(m_conn));
	QString port = QString::fromUtf8(PQport(m_conn));
	QString dbName = QString::fromUtf8(PQdb(m_conn));
	QString user = QString::fromUtf8(PQuser(m_conn));
	QString password = QString::fromUtf8(PQpass(m_conn));

	// 转义单引号（密码中可能包含特殊字符）
	auto escape = [](const QString& s) -> QString {
		QString escaped = s;
		escaped.replace("\\", "\\\\");
		escaped.replace("'", "\\'");
		return escaped;
	};

	// GDAL PostgreSQL 连接字符串格式:
	// PG:dbname='xxx' host='xxx' port='xxx' user='xxx' password='xxx' [active_schema='xxx']
	QString connStr = QString("PG:dbname='%1' host='%2' port='%3' user='%4' password='%5'")
		.arg(escape(dbName), escape(host), escape(port), escape(user), escape(password));

	// 如果设置了非默认 schema，添加到连接字符串中
	// GDAL 要求 active_schema 值带单引号
	if (!m_searchPath.isEmpty() && m_searchPath != "public")
	{
		connStr += QString(" active_schema='%1'").arg(m_searchPath);
	}

	return connStr;
}

void PostgisConnector::migrateServiceSchema()
{
	if (!m_conn || PQstatus(m_conn) != CONNECTION_OK) return;

	// 1. 检查 service_records 表是否存在
	{
		PGresult* r = PQexec(m_conn, "SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_name = 'service_records')");
		if (!r || PQresultStatus(r) != PGRES_TUPLES_OK)
		{
			if (r) PQclear(r);
			return;
		}
		char* val = PQgetvalue(r, 0, 0);
		bool exists = (strcmp(val, "t") == 0);
		PQclear(r);
		if (!exists)
			return;
	}

	// 2. 查询已有列
	QSet<QString> existingCols;
	{
		PGresult* r = PQexec(m_conn, "SELECT column_name FROM information_schema.columns WHERE table_name = 'service_records'");
		if (!r || PQresultStatus(r) != PGRES_TUPLES_OK)
		{
			if (r) PQclear(r);
			return;
		}
		int nRows = PQntuples(r);
		for (int i = 0; i < nRows; ++i)
			existingCols.insert(QString::fromUtf8(PQgetvalue(r, i, 0)));
		PQclear(r);
	}

	// 3. 定义全部期望的列及其类型
	struct ColDef { const char* name; const char* type; };
	static const ColDef expected[] = {
		{"record_date",          "VARCHAR(32)"},
		{"service_content",      "TEXT"},
		{"format",               "VARCHAR(64)"},
		{"sheet_count",          "INTEGER DEFAULT 0"},
		{"compiled_sheet_count", "INTEGER DEFAULT 0"},
		{"mounted_count",        "INTEGER DEFAULT 0"},
		{"gov_map_pack_count",   "INTEGER DEFAULT 0"},
		{"gansu_work_map",       "INTEGER DEFAULT 0"},
		{"atlas",                "INTEGER DEFAULT 0"},
		{"calendar_stationery_map", "INTEGER DEFAULT 0"},
		{"data_count",           "INTEGER DEFAULT 0"},
		{"category",             "VARCHAR(128)"},
		{"service_form",         "VARCHAR(256)"},
		{"service_target",       "VARCHAR(256)"},
		{"arranged_leader",      "VARCHAR(128)"},
		{"funding",              "NUMERIC(12,2) DEFAULT 0"},
		// UNV2 新增列
		{"service_level",        "VARCHAR(64)"},
		{"submit_time",          "VARCHAR(32)"},
		{"arranged_office",      "VARCHAR(256)"},
		{"other_service_form",   "VARCHAR(256)"},
		{"map_name",             "VARCHAR(512)"},
		{"mapping_region",       "VARCHAR(512)"},
		{"size",                 "VARCHAR(128)"},
		{"material",             "VARCHAR(128)"},
		{"compilation_details",  "TEXT"},
		// UNV3 新增列 — 各服务形式Tab详情
		{"print_details",        "TEXT"},
		{"mounting_details",     "TEXT"},
		{"product_details",      "TEXT"},
		{"data_details",         "TEXT"},
		{"other_details",        "TEXT"},
		{"product_ids",          "TEXT"},
		{"product_names",        "TEXT"},
		{"approval_file_path",   "TEXT"},
	{"approval_file_oid",    "INTEGER DEFAULT 0"},
	};

	int addedCount = 0;
	for (const auto& col : expected)
	{
		if (existingCols.contains(QString::fromUtf8(col.name)))
			continue;

		QString sql = QString("ALTER TABLE service_records ADD COLUMN IF NOT EXISTS %1 %2")
			.arg(QString::fromUtf8(col.name), QString::fromUtf8(col.type));
		PGresult* r = PQexec(m_conn, sql.toUtf8().constData());
		if (!r || PQresultStatus(r) != PGRES_COMMAND_OK)
		{
			if (r) PQclear(r);
			continue;
		}
		PQclear(r);
		++addedCount;
	}

	// 4. 修正列类型：service_level 早期可能是 INTEGER，需要改为 VARCHAR(64)
	{
		PGresult* r = PQexec(m_conn,
			"SELECT data_type FROM information_schema.columns "
			"WHERE table_name = 'service_records' AND column_name = 'service_level'");
		if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
		{
			QString dt = QString::fromUtf8(PQgetvalue(r, 0, 0));
			if (dt.compare(QStringLiteral("character varying"), Qt::CaseInsensitive) != 0)
			{
				PQclear(r);
				r = PQexec(m_conn,
					"ALTER TABLE service_records ALTER COLUMN service_level TYPE VARCHAR(64)");
				if (r && PQresultStatus(r) == PGRES_COMMAND_OK)
				{
					QgsMessageLog::logMessage(
						QStringLiteral("[Schema迁移] service_level 类型已修正为 VARCHAR(64)"),
						QStringLiteral("MapProductTools"), Qgis::Info);
				}
			}
		}
		if (r) PQclear(r);
	}
}
