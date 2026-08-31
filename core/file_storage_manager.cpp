#include "file_storage_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QDebug>

FileStorageManager::FileStorageManager(QObject* parent)
	: QObject(parent)
	, m_storageRoot("./product_storage")
{
}

void FileStorageManager::setStorageRoot(const QString& rootPath)
{
	m_storageRoot = rootPath;
	ensureDirectoryExists(m_storageRoot);
}

QString FileStorageManager::storageRoot() const
{
	return m_storageRoot;
}

bool FileStorageManager::ensureDirectoryExists(const QString& path)
{
	QDir dir(path);
	if (!dir.exists())
	{
		return dir.mkpath(".");
	}
	return true;
}

QString FileStorageManager::getProductStoragePath(const QString& productUUID, int version)
{
	QString subPath = productUUID.left(2) + "/" + productUUID;
	if (version > 0)
	{
		subPath += QString("/v%1").arg(version, 4, 10, QChar('0'));
	}
	return m_storageRoot + "/" + subPath;
}

QString FileStorageManager::getThumbnailPath(const QString& productUUID)
{
	return m_storageRoot + "/thumbnails/" + productUUID + ".png";
}

QString FileStorageManager::importFile(const QString& sourcePath, const QString& productUUID, int version)
{
	QFileInfo srcInfo(sourcePath);
	if (!srcInfo.exists())
	{
		emit importFailed(sourcePath, "源文件不存在");
		return QString();
	}

	emit importProgress(0, "准备导入文件...");

	// 生成目标路径
	QString storageDir = getProductStoragePath(productUUID, version);
	if (!ensureDirectoryExists(storageDir))
	{
		emit importFailed(sourcePath, "无法创建存储目录");
		return QString();
	}

	QString destPath = storageDir + "/" + srcInfo.fileName();

	emit importProgress(30, "正在复制文件...");

	bool ok = false;

	if (srcInfo.isDir())
	{
		// GDB 等目录格式：递归复制整个目录
		if (QDir(destPath).exists())
		{
			QDir(destPath).removeRecursively();
		}
		ok = copyDirectoryRecursively(sourcePath, destPath);
	}
	else
	{
		// 普通文件复制
		if (QFile::exists(destPath))
		{
			QFile::remove(destPath);
		}

		ok = QFile::copy(sourcePath, destPath);

		// 同时复制附属文件（如 .shx, .dbf, .prj, .cpg 等）
		// 这对于 Shapefile 等需要多文件的数据格式至关重要
		if (ok)
		{
			QString baseName = srcInfo.completeBaseName();
			QDir sourceDir(srcInfo.absolutePath());
			QStringList relatedFilters;
			relatedFilters << baseName + ".*";
			QStringList relatedFiles = sourceDir.entryList(relatedFilters, QDir::Files, QDir::Name);

			int copiedCount = 0;
			for (const QString& relatedFile : relatedFiles)
			{
				if (relatedFile == srcInfo.fileName()) continue;

				QString relatedSource = srcInfo.absolutePath() + "/" + relatedFile;
				QString relatedDest = storageDir + "/" + relatedFile;

				if (QFile::exists(relatedDest))
				{
					QFile::remove(relatedDest);
				}

				if (QFile::copy(relatedSource, relatedDest))
				{
					copiedCount++;
				}
			}
		}
	}

	if (!ok)
	{
		emit importFailed(sourcePath, srcInfo.isDir() ? "目录复制失败" : "文件复制失败");
		return QString();
	}

	emit importProgress(80, "验证文件完整性...");

	// 验证复制后的文件（目录跳过大小验证）
	if (!srcInfo.isDir())
	{
		QFileInfo destInfo(destPath);
		if (destInfo.size() != srcInfo.size())
		{
			QFile::remove(destPath);
			emit importFailed(sourcePath, "文件完整性验证失败");
			return QString();
		}
	}

	emit importProgress(100, "文件导入完成");
	emit importCompleted(destPath);

	return destPath;
}

bool FileStorageManager::exportFile(const QString& storagePath, const QString& outputPath)
{
	QFileInfo srcInfo(storagePath);
	if (!srcInfo.exists())
		return false;

	QString destDir = QFileInfo(outputPath).absolutePath();
	ensureDirectoryExists(destDir);

	if (QFile::exists(outputPath))
		QFile::remove(outputPath);

	return QFile::copy(storagePath, outputPath);
}

bool FileStorageManager::deleteStorageFile(const QString& storagePath)
{
	return QFile::remove(storagePath);
}

bool FileStorageManager::cleanupOldVersions(const QString& productUUID, int keepVersions)
{
	QString productPath = getProductStoragePath(productUUID);

	QDir dir(productPath);
	if (!dir.exists()) return false;

	// 获取所有版本目录
	QStringList versionDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

	// 保留最新N个版本
	if (versionDirs.size() <= keepVersions)
		return true;

	// 删除旧版本
	for (int i = 0; i < versionDirs.size() - keepVersions; ++i)
	{
		QString oldDir = productPath + "/" + versionDirs[i];
		QDir(oldDir).removeRecursively();
	}

	return true;
}

bool FileStorageManager::copyDirectoryRecursively(const QString& srcDir, const QString& dstDir)
{
	QDir src(srcDir);
	if (!src.exists())
		return false;

	// 创建目标目录
	QDir dst(dstDir);
	if (!dst.exists())
	{
		if (!dst.mkpath("."))
			return false;
	}

	// 复制所有文件
	QStringList files = src.entryList(QDir::Files | QDir::NoDotAndDotDot);
	for (const QString& file : files)
	{
		if (!QFile::copy(srcDir + "/" + file, dstDir + "/" + file))
			return false;
	}

	// 递归复制子目录
	QStringList subdirs = src.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString& subdir : subdirs)
	{
		if (!copyDirectoryRecursively(srcDir + "/" + subdir, dstDir + "/" + subdir))
			return false;
	}

	return true;
}
