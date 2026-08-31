#ifndef MAP_CHECK_BACKUP_MANAGER_H
#define MAP_CHECK_BACKUP_MANAGER_H

#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QMap>
#include <QString>
#include <QDir>

#include "map_check_common.h"

#include <vector>
#include <fstream>

using namespace std;

// 备份记录信息（用于恢复功能）
struct BackupRecordInfo
{
    QString strName;          // 备份文件夹名称
    BackupType eBackupType;   // 备份类型
    QDateTime dtCreateTime;   // 创建时间
    qint64 nSizeBytes;        // 大小（字节）
    QString strFullPath;      // 完整路径
    BackupDataSource eDataSource;  // 数据源类型（新增）

    BackupRecordInfo()
        : eBackupType(BackupType::FullBackup)
        , nSizeBytes(0)
        , eDataSource(BackupDataSource::FileSystem)
    {}
};

// 定时备份管理器（单例）
// 负责在插件后台按策略执行定时备份，以及数据恢复
class CMapCheckBackupManager : public QObject
{
    Q_OBJECT

public:
    static CMapCheckBackupManager* instance();

    // 启动/停止定时检查
    void start();
    void stop();

    // 设置参数
    void setSourcePath(const QString& path);
    void setTargetPath(const QString& path);
    void setStrategies(const vector<TimedBackupStrategy>& strategies);
    void setEnabled(bool enabled);
    void setKeepExpired(bool keep);

    // 获取参数
    QString sourcePath() const;
    QString targetPath() const;
    bool isEnabled() const;
    bool keepExpired() const;

    // 获取策略列表
    const vector<TimedBackupStrategy>& getStrategies() const;

    // 保存/加载设置
    void saveSettings();
    void loadSettings();

    // 执行一次指定策略的备份
    bool executeBackup(const TimedBackupStrategy& strategy);

    // 获取最近一次失败的具体错误信息
    QString lastErrorString() const { return m_qstrLastError; }
    void clearLastError() { m_qstrLastError.clear(); }

    // ===== 恢复功能 =====
    // 扫描备份目录，获取所有可用备份记录
    vector<BackupRecordInfo> scanBackupRecords(const QString& backupDir);

    // 执行恢复操作（文件系统恢复）
    bool restoreBackup(const QString& backupPath, const QString& dstPath, bool overwrite);

    // 执行数据库恢复操作
    bool restoreBackupToDatabase(const QString& backupPath,
        const QString& host, int port, const QString& dbName,
        const QString& username, const QString& password,
        const QString& schema, bool overwrite);

signals:
    // 备份/恢复进度信号
    void backupProgress(const QString& message);
    void restoreProgress(const QString& message);

private slots:
    void onCheckTimerTimeout();

private:
    explicit CMapCheckBackupManager(QObject* parent = nullptr);
    ~CMapCheckBackupManager();

    CMapCheckBackupManager(const CMapCheckBackupManager&) = delete;
    CMapCheckBackupManager& operator=(const CMapCheckBackupManager&) = delete;

    // 判断是否需要执行备份
    bool shouldExecuteBackup(const TimedBackupStrategy& strategy, const QDateTime& lastTime) const;

    // 执行全量备份
    bool fullBackup(const QString& srcPath, const QString& dstPath, const QString& backupName);

    // 执行增量备份（只复制新增或修改的文件）
    bool incrementalBackup(const QString& srcPath, const QString& dstPath, const QString& backupName);

    // 执行数据库备份（新增）
    bool databaseBackup(const TimedBackupStrategy& strategy, const QString& dstPath, const QString& backupName);

    // 拷贝目录
    bool copyDirectory(const QDir& srcDir, const QDir& dstDir);

    // 拷贝目录（增量）
    bool copyDirectoryIncremental(const QDir& srcDir, const QDir& dstDir);

    // 生成备份目录名
    QString generateBackupFolderName(BackupType type) const;

    // 写入备份元数据
    void writeBackupInfo(const QString& backupPath, BackupType type, const QString& sourcePath,
                         BackupDataSource dataSource = BackupDataSource::FileSystem,
                         const QString& dbConnInfo = QString());

    // 清理过期备份
    void cleanupExpiredBackups();

    // 写入日志文件
    void writeLog(const QString& msg);

private:
    QTimer* m_pCheckTimer = nullptr;

    QString m_qstrSourcePath;
    QString m_qstrTargetPath;
    vector<TimedBackupStrategy> m_vecStrategies;
    bool m_bEnabled = false;
    bool m_bKeepExpired = true;

    // 记录每种备份类型的最后执行时间
    QMap<int, QDateTime> m_mapLastBackupTime;

    // 最近一次失败的具体错误信息
    QString m_qstrLastError;
};

#endif // MAP_CHECK_BACKUP_MANAGER_H
