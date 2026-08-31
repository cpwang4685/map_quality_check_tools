#ifndef POSTGIS_CONNECTOR_H
#define POSTGIS_CONNECTOR_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <memory>
#include <mutex>

// libpq 前向声明
struct pg_conn;
typedef struct pg_conn PGconn;
struct pg_result;
typedef struct pg_result PGresult;

/**
 * @brief PostGIS数据库连接管理器（单例模式）
 * 
 * 使用libpq直接连接PostgreSQL/PostGIS数据库，
 * 提供元数据CRUD、版本管理、空间查询等功能。
 */
class PostgisConnector : public QObject
{
	Q_OBJECT

public:
	static PostgisConnector* instance();
	
	// 连接管理
	bool connect(const QString& host, int port, const QString& dbName,
				 const QString& user, const QString& password);
	void disconnect();
	bool isConnected() const;
	QString lastError() const;

	// Schema 搜索路径管理
	void setSearchPath(const QString& schema);
	QString searchPath() const;

	// 通用查询
	QVariantList executeQuery(const QString& sql, const QVariantList& params = QVariantList());
	QVariantMap executeQueryOne(const QString& sql, const QVariantList& params = QVariantList());
	bool executeNonQuery(const QString& sql, const QVariantList& params = QVariantList());
	
	// 事务管理
	bool beginTransaction();
	bool commitTransaction();
	bool rollbackTransaction();

	// 大对象操作（用于文件存储）
	int loImport(const QString& filePath);
	bool loExport(int oid, const QString& filePath);
	bool loDelete(int oid);

	// 静态重载：接受外部 PGconn*（供 data_importer 等自有连接使用）
	static int loImport(PGconn* conn, const QString& filePath, QString* outError = nullptr);
	static bool loExport(PGconn* conn, int oid, const QString& filePath, QString* outError = nullptr);

	// 获取 GDAL 兼容的连接字符串
	QString gdalConnectionString() const;
	// 获取原始 PGconn 指针（供内部模块使用）
	PGconn* nativeConnection() const { return m_conn; }

	// Schema 自动迁移
	void migrateServiceSchema();

signals:
	void connectionStatusChanged(bool connected);
	void errorOccurred(const QString& errorMsg);
	/// 当 search_path 发生变化时发出，通知 UI 刷新数据
	void schemaChanged(const QString& newSchema);

private:
	explicit PostgisConnector(QObject* parent = nullptr);
	~PostgisConnector() override;
	PostgisConnector(const PostgisConnector&) = delete;
	PostgisConnector& operator=(const PostgisConnector&) = delete;

	PGresult* executeSQL(const QString& sql, const QVariantList& params);
	void freeResult(PGresult* res);
	QVariantList resultToVariantList(PGresult* res);
	QVariantMap resultRowToMap(PGresult* res, int row);

	static PostgisConnector* s_instance;
	static std::mutex s_mutex;

	PGconn* m_conn = nullptr;
	QString m_lastError;
	QString m_searchPath;
	bool m_connected = false;
};

#endif // POSTGIS_CONNECTOR_H
