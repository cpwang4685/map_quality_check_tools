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

    // 【2026-09-11】解析 PostgreSQL 客户端工具（pg_dump / pg_restore / psql）的可用路径。
    //
    // 背景：原来 databaseBackup() 用一张写死的候选表找 pg_dump，第一条是开发者本机的
    // 安装位置；客户机上命中不了就退化成裸 "pg_dump" 交给 PATH 解析，而 LTZK 的
    // start_LTZK.bat 把自带 OSGeo4W 的 bin 前置到 PATH，于是抓到的是 15.2。客户机
    // 的数据库服务器是 18.6，pg_dump 不允许用低版本客户端连高版本服务器，备份在连接
    // 阶段就被驳回（"aborting because of server version mismatch"）。恢复侧的
    // pg_restore 更是一点路径查找都没有，直接裸调。
    //
    // 现在的策略（逐级降级，与盘符无关）：
    //   1) 用 libpq 连一次数据库问出服务器大版本号（连不上则跳过版本校验）；
    //   2) 收集候选 bin 目录：
    //      a. 注册表 HKLM\SOFTWARE\PostgreSQL\Installations\<实例> 的 "Base Directory"
    //         —— 官方安装器无论装在 C 盘还是 D 盘都会如实写在这里，是解决"不知道装哪"
    //         的主路径；
    //      b. 兜底：服务 HKLM\SYSTEM\CurrentControlSet\Services\postgresql-x64-<n> 的
    //         ImagePath（形如 "D:\...\bin\pg_ctl.exe" runservice ...），从 pg_ctl.exe
    //         反推 bin 目录；
    //      c. 各盘符的常见安装位置（C..Z:\Program Files\PostgreSQL\<版本>\bin 等）；
    //      d. 历史硬编码路径 + PATH 上的同名工具；
    //   3) 对每个候选跑一次 `<工具> --version`，只接受大版本 >= 服务器大版本的，并在
    //      够用的里面挑最贴近服务器的那个（同版本最稳）。
    //
    // 返回值语义：
    //   - 非空 = 可直接启动的绝对路径；此时 errMsg 若不为空，是一条"降级说明"，调用方
    //     记日志即可，流程继续；
    //   - 空串 = 确实没有可用工具，errMsg 是给用户看的完整中文原因（含本机已找到的版本
    //     清单），调用方应据此失败并提示。
    // enforceVersionGate：是否执行"工具大版本 >= 服务器大版本"的闸门。
    //   - pg_dump / pg_restore 必须为 true：它们遇到更高版本的服务器/存档会直接拒绝工作。
    //   - psql 传 false：psql 只是执行 SQL 文本的客户端，官方允许它连接任意版本的服务器，
    //     套闸门反而会在"本机只有旧 psql"时把原本能跑通的恢复挡掉（行为回归）。
    QString resolvePgClientTool(const QString& toolName,
                                const QString& host, int port, const QString& dbName,
                                const QString& user, const QString& password,
                                QString& errMsg,
                                bool enforceVersionGate = true);

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
