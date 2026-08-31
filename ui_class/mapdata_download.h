#pragma once

#include <QDialog>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>

#include "ui_mapdata_download.h"

class QCheckBox;

/**
 * @brief 地图成果下载对话框
 *
 * 连接 PostGIS 成果库，按条件检索产品元数据，并将用户勾选的成果
 * 下载到本地目录。矢量数据导出为 Shapefile，其余类型从 PostgreSQL
 * Large Object（file_oid）导出为原始文件。
 */
class mapdata_download : public QDialog
{
public:
	explicit mapdata_download(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~mapdata_download() override;

private:
	void onTestConnection();
	void onConnect();
	void onSearch();
	void onReset();
	void onChooseOutputDir();
	void onSelectAllToggled();
	void onDownloadSelected();

	struct DownloadItem
	{
		int id = -1;
		QString productName;
		QString productType;
		QString fileFormat;
		QString securityLevel;
		int currentVersion = 1;
		QDateTime updatedAt;
		int fileOid = 0;
		QString layerTableName;
	};

	void setupTable();
	void setupConnections();
	void loadSettings();
	void saveSettings();
	void setStatus(const QString& message, const QString& color);
	void performSearch();
	void updatePagination();
	void updateSelectionUi();
	void setAllCurrentPageChecked(bool checked);
	bool openDatabase(QString* errorMessage = nullptr);
	bool verifyProductTable(QString* errorMessage = nullptr) const;
	QString quotedSchema() const;
	QString metadataTable() const;
	QString buildWhereClause(QMap<QString, QVariant>* bindings) const;
	bool exportItem(const DownloadItem& item, const QString& outputDir, QString* errorMessage);
	bool exportVector(const DownloadItem& item, const QString& itemDir, QString* errorMessage);
	bool exportLargeObject(const DownloadItem& item, const QString& itemDir, QString* errorMessage);
	QString safeFileName(const QString& name) const;

	Ui::mapdata_download ui;
	QSqlDatabase mDatabase;
	QString mConnectionName;
	QString mConnectedSchema;

	QList<DownloadItem> mSearchResults;
	QMap<int, DownloadItem> mSelectedItems;
	int mCurrentPage = 1;
	int mTotalPages = 1;
	int mTotalResults = 0;
	const int mPageSize = 20;
	bool mUpdatingChecks = false;
};
