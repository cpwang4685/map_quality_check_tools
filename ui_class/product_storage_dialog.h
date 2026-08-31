#ifndef PRODUCT_STORAGE_DIALOG_H
#define PRODUCT_STORAGE_DIALOG_H

#include <QDialog>
#include <QThread>
#include "database/product_metadata.h"
#include "ui_product_storage_dialog.h"

class MetadataExtractor;
class FileStorageManager;
class ProductDAO;
class DataImporter;
class QgisInterface;

/**
 * @brief 成果存储与管理对话框
 * 
 * 实现成果文件的上传、自动元数据提取、版本管理功能。
 */
class ProductStorageDialog : public QDialog
{
	Q_OBJECT

public:
	explicit ProductStorageDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~ProductStorageDialog() override;

	void setQgisInterface(QgisInterface* iface) { mQGisIface = iface; }

private slots:
	void onSelectFile();
	void onSelectFolder();
	void onUploadFile();
	void onDeleteProduct();
	void onAddToMap();
	void onRefreshList();
	void onProductSelected(int row, int col);
	void onExtractionProgress(int percent, const QString& message);

private:
	void setupConnections();
	void setupTableColumns();
	void loadProductList();
	void clearForm();
	void fillForm(const ProductMetadata& meta);
	void updateRasterFieldsVisibility();
	void processBatchFile(const QString& filePath, int& importedCount, int& skippedCount);
	void processGdbLayers(const QString& gdbPath, const QStringList& layerNames,
		const QString& productType, const QString& versionNote);
	void updateProgressText(const QString& text, int value = -1);
	bool loadLayerToMap(const ProductMetadata& meta);
	QString resolveTableName(const QString& schema, const QString& candidateName);

	// 核心组件
	MetadataExtractor* mExtractor = nullptr;
	FileStorageManager* mStorageManager = nullptr;
	ProductDAO* mDAO = nullptr;
	DataImporter* mImporter = nullptr;

	// QGIS 接口
	QgisInterface* mQGisIface = nullptr;

	QList<ProductMetadata> mProducts;
	ProductMetadata mCurrentMeta;

	// GDB 图层选择相关
	QString mSelectedGdbPath;
	QStringList mSelectedGdbLayers;
	bool mIsGdbMultiLayer = false;

	Ui::ProductStorageDialog ui;
};

#endif // PRODUCT_STORAGE_DIALOG_H
