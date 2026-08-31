#ifndef FILE_STORAGE_MANAGER_H
#define FILE_STORAGE_MANAGER_H

#include <QObject>
#include <QString>

/**
 * @brief 文件存储管理器
 * 
 * 管理成果文件的本地存储和PostGIS大对象存储，
 * 提供文件导入、版本化存储、导出等功能。
 */
class FileStorageManager : public QObject
{
	Q_OBJECT

public:
	explicit FileStorageManager(QObject* parent = nullptr);

	/**
	 * @brief 设置存储根目录
	 */
	void setStorageRoot(const QString& rootPath);
	QString storageRoot() const;

	/**
	 * @brief 导入文件到存储
	 * @param sourcePath 源文件路径
	 * @param productUUID 产品UUID（用于生成子目录）
	 * @param version 版本号
	 * @return 存储后的文件路径
	 */
	QString importFile(const QString& sourcePath, const QString& productUUID, int version);

	/**
	 * @brief 导出文件
	 * @param storagePath 存储中的文件路径
	 * @param outputPath 输出目标路径
	 * @return 是否成功
	 */
	bool exportFile(const QString& storagePath, const QString& outputPath);

	/**
	 * @brief 删除存储文件
	 */
	bool deleteStorageFile(const QString& storagePath);

	/**
	 * @brief 获取缩略图存储路径
	 */
	QString getThumbnailPath(const QString& productUUID);

	/**
	 * @brief 生成产品存储子路径
	 */
	QString getProductStoragePath(const QString& productUUID, int version = 0);

	/**
	 * @brief 清理旧版本文件
	 * @param productUUID 产品UUID
	 * @param keepVersions 保留的版本数量
	 */
	bool cleanupOldVersions(const QString& productUUID, int keepVersions = 10);

signals:
	void importProgress(int percent, const QString& message);
	void importCompleted(const QString& filePath);
	void importFailed(const QString& filePath, const QString& error);

private:
	QString m_storageRoot;
	bool ensureDirectoryExists(const QString& path);
	bool copyDirectoryRecursively(const QString& srcDir, const QString& dstDir);
};

#endif // FILE_STORAGE_MANAGER_H
