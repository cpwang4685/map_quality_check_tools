/*--------------QT---------------*/
#include "map_check_backup_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QDateTime>
#include <QTextStream>
#include <QCoreApplication>
#include <QProcess>
#include <QRegExp>
#include <QStandardPaths>

/*--------------QGIS---------------*/
#include "qgssettings.h"

/*-----------------------------------*/

static CMapCheckBackupManager* s_pInstance = nullptr;

CMapCheckBackupManager* CMapCheckBackupManager::instance()
{
    if (!s_pInstance)
    {
        s_pInstance = new CMapCheckBackupManager();
    }
    return s_pInstance;
}

CMapCheckBackupManager::CMapCheckBackupManager(QObject* parent)
    : QObject(parent)
{
    m_pCheckTimer = new QTimer(this);
    connect(m_pCheckTimer, &QTimer::timeout, this, &CMapCheckBackupManager::onCheckTimerTimeout);
    m_pCheckTimer->setInterval(60 * 1000); // 每分钟检查一次

    loadSettings();
}

CMapCheckBackupManager::~CMapCheckBackupManager()
{
    stop();
    saveSettings();
}

void CMapCheckBackupManager::start()
{
    if (m_pCheckTimer && !m_pCheckTimer->isActive())
    {
        m_pCheckTimer->start();
    }
}

void CMapCheckBackupManager::stop()
{
    if (m_pCheckTimer && m_pCheckTimer->isActive())
    {
        m_pCheckTimer->stop();
    }
}

void CMapCheckBackupManager::setSourcePath(const QString& path)
{
    m_qstrSourcePath = path;
}

void CMapCheckBackupManager::setTargetPath(const QString& path)
{
    m_qstrTargetPath = path;
}

void CMapCheckBackupManager::setStrategies(const vector<TimedBackupStrategy>& strategies)
{
    m_vecStrategies = strategies;
}

void CMapCheckBackupManager::setEnabled(bool enabled)
{
    m_bEnabled = enabled;
    if (m_bEnabled)
    {
        start();
    }
    else
    {
        stop();
    }
}

void CMapCheckBackupManager::setKeepExpired(bool keep)
{
    m_bKeepExpired = keep;
}

QString CMapCheckBackupManager::sourcePath() const
{
    return m_qstrSourcePath;
}

QString CMapCheckBackupManager::targetPath() const
{
    return m_qstrTargetPath;
}

bool CMapCheckBackupManager::isEnabled() const
{
    return m_bEnabled;
}

bool CMapCheckBackupManager::keepExpired() const
{
    return m_bKeepExpired;
}

const vector<TimedBackupStrategy>& CMapCheckBackupManager::getStrategies() const
{
    return m_vecStrategies;
}

void CMapCheckBackupManager::saveSettings()
{
    QgsSettings settings;
    settings.setValue(QStringLiteral("TimedBackup/SourcePath"), m_qstrSourcePath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("TimedBackup/TargetPath"), m_qstrTargetPath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("TimedBackup/EnableTimedBackup"), m_bEnabled, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("TimedBackup/KeepExpired"), m_bKeepExpired, QgsSettings::Section::Plugins);

    // 保存策略列表
    int strategyCount = static_cast<int>(m_vecStrategies.size());
    settings.setValue(QStringLiteral("TimedBackup/StrategyCount"), strategyCount, QgsSettings::Section::Plugins);
    for (int i = 0; i < strategyCount; ++i)
    {
        const TimedBackupStrategy& strategy = m_vecStrategies[i];
        QString prefix = QString("TimedBackup/Strategies/%1/").arg(i);
        settings.setValue(prefix + "BackupType", static_cast<int>(strategy.eBackupType), QgsSettings::Section::Plugins);
        settings.setValue(prefix + "Frequency", static_cast<int>(strategy.eFrequency), QgsSettings::Section::Plugins);
        settings.setValue(prefix + "ExecuteTime", strategy.strExecuteTime, QgsSettings::Section::Plugins);
        settings.setValue(prefix + "RetentionPeriod", strategy.strRetentionPeriod, QgsSettings::Section::Plugins);
        settings.setValue(prefix + "StorageLocation", static_cast<int>(strategy.eStorageLocation), QgsSettings::Section::Plugins);
        settings.setValue(prefix + "Enabled", strategy.bEnabled, QgsSettings::Section::Plugins);
        // 数据源配置
        settings.setValue(prefix + "DataSource", static_cast<int>(strategy.eDataSource), QgsSettings::Section::Plugins);
        if (strategy.eDataSource == BackupDataSource::Database || strategy.eDataSource == BackupDataSource::All)
        {
            settings.setValue(prefix + "DbType", static_cast<int>(strategy.eDbType), QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbHost", strategy.strDbHost, QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbPort", strategy.nDbPort, QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbName", strategy.strDbName, QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbUser", strategy.strDbUser, QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbPassword", strategy.strDbPassword, QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbSchema", strategy.strDbSchema, QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbBackupScope", static_cast<int>(strategy.eDbBackupScope), QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbBackupMethod", static_cast<int>(strategy.eDbBackupMethod), QgsSettings::Section::Plugins);
            settings.setValue(prefix + "DbBackupFormat", static_cast<int>(strategy.eDbBackupFormat), QgsSettings::Section::Plugins);
        }
    }

    // 保存最后执行时间
    for (auto it = m_mapLastBackupTime.begin(); it != m_mapLastBackupTime.end(); ++it)
    {
        QString key = QString("TimedBackup/LastTime/%1").arg(it.key());
        settings.setValue(key, it.value(), QgsSettings::Section::Plugins);
    }
}

void CMapCheckBackupManager::loadSettings()
{
    const QgsSettings settings;
    m_qstrSourcePath = settings.value(QStringLiteral("TimedBackup/SourcePath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
    m_qstrTargetPath = settings.value(QStringLiteral("TimedBackup/TargetPath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
    m_bEnabled = settings.value(QStringLiteral("TimedBackup/EnableTimedBackup"), false, QgsSettings::Section::Plugins).toBool();
    m_bKeepExpired = settings.value(QStringLiteral("TimedBackup/KeepExpired"), true, QgsSettings::Section::Plugins).toBool();

    // 加载策略列表
    m_vecStrategies.clear();
    int strategyCount = settings.value(QStringLiteral("TimedBackup/StrategyCount"), 0, QgsSettings::Section::Plugins).toInt();
    for (int i = 0; i < strategyCount; ++i)
    {
        QString prefix = QString("TimedBackup/Strategies/%1/").arg(i);
        TimedBackupStrategy strategy;
        strategy.eBackupType = static_cast<BackupType>(settings.value(prefix + "BackupType", 0, QgsSettings::Section::Plugins).toInt());
        strategy.eFrequency = static_cast<BackupFrequency>(settings.value(prefix + "Frequency", 0, QgsSettings::Section::Plugins).toInt());
        strategy.strExecuteTime = settings.value(prefix + "ExecuteTime", QString(), QgsSettings::Section::Plugins).toString();
        strategy.strRetentionPeriod = settings.value(prefix + "RetentionPeriod", QString(), QgsSettings::Section::Plugins).toString();
        int storageLocationValue = settings.value(prefix + "StorageLocation", 0, QgsSettings::Section::Plugins).toInt();
        // 仅保留本地存储，旧版保存的异地/本地+异地值统一归一化为本地
        if (storageLocationValue != static_cast<int>(StorageLocation::Local))
        {
            storageLocationValue = static_cast<int>(StorageLocation::Local);
        }
        strategy.eStorageLocation = static_cast<StorageLocation>(storageLocationValue);
        strategy.bEnabled = settings.value(prefix + "Enabled", true, QgsSettings::Section::Plugins).toBool();
        // 数据源配置
        strategy.eDataSource = static_cast<BackupDataSource>(settings.value(prefix + "DataSource", 0, QgsSettings::Section::Plugins).toInt());
        if (settings.contains(prefix + "DbType", QgsSettings::Section::Plugins))
        {
            strategy.eDbType = static_cast<DatabaseType>(settings.value(prefix + "DbType", 0, QgsSettings::Section::Plugins).toInt());
            strategy.strDbHost = settings.value(prefix + "DbHost", QString(), QgsSettings::Section::Plugins).toString();
            strategy.nDbPort = settings.value(prefix + "DbPort", 5432, QgsSettings::Section::Plugins).toInt();
            strategy.strDbName = settings.value(prefix + "DbName", QString(), QgsSettings::Section::Plugins).toString();
            strategy.strDbUser = settings.value(prefix + "DbUser", QString(), QgsSettings::Section::Plugins).toString();
            strategy.strDbPassword = settings.value(prefix + "DbPassword", QString(), QgsSettings::Section::Plugins).toString();
            strategy.strDbSchema = settings.value(prefix + "DbSchema", "public", QgsSettings::Section::Plugins).toString();
            strategy.eDbBackupScope = static_cast<DatabaseBackupScope>(settings.value(prefix + "DbBackupScope", 0, QgsSettings::Section::Plugins).toInt());
            strategy.eDbBackupMethod = static_cast<DatabaseBackupMethod>(settings.value(prefix + "DbBackupMethod", 0, QgsSettings::Section::Plugins).toInt());
            strategy.eDbBackupFormat = static_cast<DatabaseBackupFormat>(settings.value(prefix + "DbBackupFormat", 0, QgsSettings::Section::Plugins).toInt());
        }
        m_vecStrategies.push_back(strategy);
    }

    // 加载最后执行时间
    for (int i = 0; i <= static_cast<int>(BackupType::LogicalExportBackup); ++i)
    {
        QString key = QString("TimedBackup/LastTime/%1").arg(i);
        QDateTime time = settings.value(key, QDateTime(), QgsSettings::Section::Plugins).toDateTime();
        if (!time.isNull())
        {
            m_mapLastBackupTime[i] = time;
        }
    }
}

bool CMapCheckBackupManager::executeBackup(const TimedBackupStrategy& strategy)
{
    // 数据库备份：只需要目标路径
    if (strategy.eBackupType == BackupType::DatabaseBackup ||
        strategy.eDataSource == BackupDataSource::Database ||
        strategy.eDataSource == BackupDataSource::All)
    {
        if (m_qstrTargetPath.isEmpty())
        {
            m_qstrLastError = tr("目标路径为空，无法执行备份。");
            writeLog(m_qstrLastError);
            return false;
        }

        QDir targetDir(m_qstrTargetPath);
        if (!targetDir.exists())
        {
            if (!targetDir.mkpath(m_qstrTargetPath))
            {
                m_qstrLastError = tr("创建目标路径失败: %1").arg(m_qstrTargetPath);
                writeLog(m_qstrLastError);
                return false;
            }
        }

        QString backupName = generateBackupFolderName(strategy.eBackupType);
        bool dbResult = databaseBackup(strategy, m_qstrTargetPath, backupName);

        // 如果数据源是 All，同时也要备份文件系统
        if (strategy.eDataSource == BackupDataSource::All)
        {
            bool fileResult = true;
            if (!m_qstrSourcePath.isEmpty())
            {
                QDir srcDir(m_qstrSourcePath);
                if (srcDir.exists())
                {
                    BackupType fileType = (strategy.eBackupType == BackupType::DatabaseBackup)
                        ? BackupType::FullBackup : strategy.eBackupType;
                    QString fileBackupName = "File_" + backupName;
                    switch (fileType)
                    {
                    case BackupType::FullBackup:
                        fileResult = fullBackup(m_qstrSourcePath, m_qstrTargetPath, fileBackupName);
                        break;
                    case BackupType::IncrementalBackup:
                        fileResult = incrementalBackup(m_qstrSourcePath, m_qstrTargetPath, fileBackupName);
                        break;
                    default:
                        fileResult = fullBackup(m_qstrSourcePath, m_qstrTargetPath, fileBackupName);
                        break;
                    }
                }
            }
            if (dbResult && fileResult)
            {
                int typeIndex = static_cast<int>(strategy.eBackupType);
                m_mapLastBackupTime[typeIndex] = QDateTime::currentDateTime();
                writeLog(tr("备份成功(数据库+文件): %1 -> %2").arg(strategy.backupTypeName(), m_qstrTargetPath));
                if (m_bKeepExpired) cleanupExpiredBackups();
                return true;
            }
            else
            {
                writeLog(tr("备份部分失败: %1 (数据库:%2, 文件:%3)")
                    .arg(strategy.backupTypeName())
                    .arg(dbResult ? tr("成功") : tr("失败"))
                    .arg(fileResult ? tr("成功") : tr("失败")));
                return false;
            }
        }

        if (dbResult)
        {
            int typeIndex = static_cast<int>(strategy.eBackupType);
            m_mapLastBackupTime[typeIndex] = QDateTime::currentDateTime();
            writeLog(tr("备份成功: %1 -> %2").arg(strategy.backupTypeName(), m_qstrTargetPath));
            if (m_bKeepExpired) cleanupExpiredBackups();
        }
        else
        {
            writeLog(tr("备份失败: %1").arg(strategy.backupTypeName()));
        }
        return dbResult;
    }

    // 文件系统备份（原逻辑）
    if (m_qstrSourcePath.isEmpty() || m_qstrTargetPath.isEmpty())
    {
        writeLog(tr("源路径或目标路径为空，无法执行备份。"));
        return false;
    }

    QDir srcDir(m_qstrSourcePath);
    if (!srcDir.exists())
    {
        writeLog(tr("源路径不存在: %1").arg(m_qstrSourcePath));
        return false;
    }

    QDir targetDir(m_qstrTargetPath);
    if (!targetDir.exists())
    {
        if (!targetDir.mkpath(m_qstrTargetPath))
        {
            writeLog(tr("创建目标路径失败: %1").arg(m_qstrTargetPath));
            return false;
        }
    }

    QString backupName = generateBackupFolderName(strategy.eBackupType);
    QString backupPath = m_qstrTargetPath + QDir::separator() + backupName;

    bool result = false;
    switch (strategy.eBackupType)
    {
    case BackupType::FullBackup:
        result = fullBackup(m_qstrSourcePath, m_qstrTargetPath, backupName);
        break;
    case BackupType::IncrementalBackup:
        result = incrementalBackup(m_qstrSourcePath, m_qstrTargetPath, backupName);
        break;
    case BackupType::WALArchive:
        result = incrementalBackup(m_qstrSourcePath, m_qstrTargetPath, backupName);
        break;
    case BackupType::LogicalExportBackup:
        result = fullBackup(m_qstrSourcePath, m_qstrTargetPath, backupName);
        break;
    default:
        result = fullBackup(m_qstrSourcePath, m_qstrTargetPath, backupName);
        break;
    }

    if (result)
    {
        int typeIndex = static_cast<int>(strategy.eBackupType);
        m_mapLastBackupTime[typeIndex] = QDateTime::currentDateTime();
        writeLog(tr("备份成功: %1 -> %2").arg(strategy.backupTypeName(), backupPath));

        if (m_bKeepExpired)
        {
            cleanupExpiredBackups();
        }
    }
    else
    {
        writeLog(tr("备份失败: %1").arg(strategy.backupTypeName()));
    }

    return result;
}

void CMapCheckBackupManager::onCheckTimerTimeout()
{
    if (!m_bEnabled || m_qstrSourcePath.isEmpty() || m_qstrTargetPath.isEmpty())
    {
        return;
    }

    QDateTime now = QDateTime::currentDateTime();
    for (const auto& strategy : m_vecStrategies)
    {
        if (!strategy.bEnabled)
        {
            continue;
        }

        int typeIndex = static_cast<int>(strategy.eBackupType);
        QDateTime lastTime = m_mapLastBackupTime.value(typeIndex, QDateTime());
        if (shouldExecuteBackup(strategy, lastTime))
        {
            executeBackup(strategy);
        }
    }
}

bool CMapCheckBackupManager::shouldExecuteBackup(const TimedBackupStrategy& strategy, const QDateTime& lastTime) const
{
    if (lastTime.isNull())
    {
        return true;
    }

    QDateTime now = QDateTime::currentDateTime();
    int secondsSinceLast = lastTime.secsTo(now);

    switch (strategy.eFrequency)
    {
    case BackupFrequency::RealTime:
        return secondsSinceLast >= 60; // 实时至少间隔1分钟
    case BackupFrequency::Hourly:
        return secondsSinceLast >= 60 * 60; // 每小时
    case BackupFrequency::Daily:
        return now.date() != lastTime.date() && now.time() >= QTime(2, 0); // 每天02:00
    case BackupFrequency::Weekly:
        return now.date().dayOfWeek() == 7 && now.time() >= QTime(2, 0) && lastTime.date() != now.date(); // 周日02:00
    case BackupFrequency::Monthly:
        return now.date().day() == 1 && now.time() >= QTime(3, 0) && lastTime.date() != now.date(); // 每月1日03:00
    default:
        return false;
    }
}

bool CMapCheckBackupManager::fullBackup(const QString& srcPath, const QString& dstPath, const QString& backupName)
{
    QDir srcDir(srcPath);
    QDir dstDir(dstPath + QDir::separator() + backupName);
    if (!dstDir.exists())
    {
        dstDir.mkpath(dstDir.absolutePath());
    }

    bool result = copyDirectory(srcDir, dstDir);
    if (result)
    {
        writeBackupInfo(dstDir.absolutePath(), BackupType::FullBackup, srcPath);
    }
    return result;
}

bool CMapCheckBackupManager::incrementalBackup(const QString& srcPath, const QString& dstPath, const QString& backupName)
{
    QDir srcDir(srcPath);
    QDir dstDir(dstPath + QDir::separator() + backupName);
    if (!dstDir.exists())
    {
        dstDir.mkpath(dstDir.absolutePath());
    }

    bool result = copyDirectoryIncremental(srcDir, dstDir);
    if (result)
    {
        writeBackupInfo(dstDir.absolutePath(), BackupType::IncrementalBackup, srcPath);
    }
    return result;
}

bool CMapCheckBackupManager::databaseBackup(const TimedBackupStrategy& strategy, const QString& dstPath, const QString& backupName)
{
    // 查找 pg_dump（与数据库连接配置 UI 相同策略）
    QStringList candidates = {
        "D:/Software/postgreSQL/bin/pg_dump.exe",
        "/usr/bin/pg_dump",
        "/usr/lib/postgresql/16/bin/pg_dump",
    };
    QString pgDumpPath;
    for (const auto& c : candidates) {
        if (QFile::exists(c)) { pgDumpPath = c; break; }
    }
    // 也尝试在 PATH 中查找
    if (pgDumpPath.isEmpty()) pgDumpPath = "pg_dump";
    if (pgDumpPath.isEmpty())
    {
        m_qstrLastError = tr("未找到 pg_dump 工具。\n"
                             "请确认 PostgreSQL 已安装，或手动将 PostgreSQL 的 bin 目录加入系统 PATH 环境变量后重启程序。");
        writeLog(m_qstrLastError);
        return false;
    }

    QDir dstDir(dstPath + QDir::separator() + backupName);
    if (!dstDir.exists())
    {
        if (!dstDir.mkpath(dstDir.absolutePath()))
        {
            m_qstrLastError = tr("创建备份目录失败: %1").arg(dstDir.absolutePath());
            writeLog(m_qstrLastError);
            return false;
        }
    }

    // 检查数据库名称是否为空
    if (strategy.strDbName.isEmpty())
    {
        m_qstrLastError = tr("数据库名称为空，请输入数据库名称。");
        writeLog(m_qstrLastError);
        return false;
    }

    // 构建数据库连接信息字符串
    QString dbConnInfo = QString("host=%1 port=%2 dbname=%3 user=%4 schema=%5")
        .arg(strategy.strDbHost.isEmpty() ? "127.0.0.1" : strategy.strDbHost)
        .arg(QString::number(strategy.nDbPort))
        .arg(strategy.strDbName)
        .arg(strategy.strDbUser)
        .arg(strategy.strDbSchema.isEmpty() ? "public" : strategy.strDbSchema);

    // 确定备份文件名和扩展名
    QString fileExt;
    switch (strategy.eDbBackupFormat)
    {
    case DatabaseBackupFormat::SQL:     fileExt = ".sql"; break;
    case DatabaseBackupFormat::SQL_GZ:  fileExt = ".sql.gz"; break;
    case DatabaseBackupFormat::PG_DUMP: fileExt = ".dump"; break;
    case DatabaseBackupFormat::GPKG:    fileExt = ".gpkg"; break;
    default: fileExt = ".sql"; break;
    }

    QString backupFile = dstDir.absolutePath() + QDir::separator() +
        (strategy.strDbName.isEmpty() ? "database_backup" : strategy.strDbName);
    backupFile += "_" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + fileExt;

    // 构建 pg_dump 命令参数
    QStringList args;
    args << "-h" << (strategy.strDbHost.isEmpty() ? "127.0.0.1" : strategy.strDbHost);
    args << "-p" << QString::number(strategy.nDbPort);
    args << "-U" << (strategy.strDbUser.isEmpty() ? "postgres" : strategy.strDbUser);
    args << "-d" << strategy.strDbName;

    // 根据备份范围构建参数
    if (strategy.eDbBackupScope == DatabaseBackupScope::Schema &&
        !strategy.strDbSchema.isEmpty())
    {
        args << "-n" << strategy.strDbSchema;
    }
    else if (strategy.eDbBackupScope == DatabaseBackupScope::SelectedTables &&
        !strategy.lstDbTables.isEmpty())
    {
        for (const QString& table : strategy.lstDbTables)
        {
            args << "-t" << ("\"" + strategy.strDbSchema + "\".\"" + table + "\"");
        }
    }

    // 根据备份格式构建参数
    if (strategy.eDbBackupFormat == DatabaseBackupFormat::PG_DUMP)
    {
        args << "-Fc"; // 自定义二进制格式
        args << "-f" << backupFile;
    }
    else if (strategy.eDbBackupFormat == DatabaseBackupFormat::SQL_GZ)
    {
        // SQL格式 + gzip压缩需要用管道
        args << "-f" << backupFile;
        // 注意：pg_dump 本身不支持 .sql.gz，需要 pipe 到 gzip
        // 这里简化处理，使用 -f 输出到临时文件，然后 gzip
    }
    else
    {
        args << "-f" << backupFile;
    }

    // 设置环境变量（密码）
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!strategy.strDbPassword.isEmpty())
    {
        env.insert("PGPASSWORD", strategy.strDbPassword);
    }
    process.setProcessEnvironment(env);

    writeLog(tr("开始数据库备份: pg_dump %1").arg(args.join(" ")));
    emit backupProgress(tr("正在备份数据库 %1 ...").arg(strategy.strDbName));

    process.start(pgDumpPath, args);
    if (!process.waitForStarted(10000))
    {
        m_qstrLastError = tr("pg_dump 启动失败: %1").arg(process.errorString());
        writeLog(m_qstrLastError);
        return false;
    }

    if (!process.waitForFinished(3600000)) // 最多等待1小时
    {
        m_qstrLastError = tr("pg_dump 执行超时");
        writeLog(m_qstrLastError);
        process.kill();
        return false;
    }

    if (process.exitCode() == 0)
    {
        QString stdErr = QString::fromLocal8Bit(process.readAllStandardError());
        if (!stdErr.isEmpty())
        {
            writeLog(tr("pg_dump 警告: %1").arg(stdErr.trimmed()));
        }

        // 如果是 .sql.gz 格式，进行 gzip 压缩
        if (strategy.eDbBackupFormat == DatabaseBackupFormat::SQL_GZ)
        {
            QProcess gzipProcess;
            gzipProcess.setWorkingDirectory(dstDir.absolutePath());
            gzipProcess.start("gzip", QStringList() << "-f" << QFileInfo(backupFile).fileName());
            if (gzipProcess.waitForFinished(60000) && gzipProcess.exitCode() == 0)
            {
                writeLog(tr("数据库备份压缩完成: %1.gz").arg(backupFile));
            }
            else
            {
                writeLog(tr("gzip 压缩失败，保留原始 .sql 文件"));
            }
        }

        writeBackupInfo(dstDir.absolutePath(), BackupType::DatabaseBackup, strategy.strDbName,
            BackupDataSource::Database, dbConnInfo);
        writeLog(tr("数据库备份成功: %1 -> %2").arg(strategy.strDbName, backupFile));
        m_qstrLastError.clear();
        return true;
    }
    else
    {
        QString error = QString::fromLocal8Bit(process.readAllStandardError());
        m_qstrLastError = tr("pg_dump 失败 (exit=%1): %2").arg(process.exitCode()).arg(error.trimmed());
        writeLog(m_qstrLastError);
        return false;
    }
}

void CMapCheckBackupManager::writeBackupInfo(const QString& backupPath, BackupType type, const QString& sourcePath,
                                             BackupDataSource dataSource, const QString& dbConnInfo)
{
    using json = nlohmann::json;
    json info;
    info["backup_type"] = static_cast<int>(type);
    info["source_path"] = sourcePath.toStdString();
    info["timestamp"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss").toStdString();
    info["source_data_type"] = (dataSource == BackupDataSource::Database ||
                                dataSource == BackupDataSource::All) ? "database" : "file";
    if (!dbConnInfo.isEmpty())
    {
        info["db_connection"] = dbConnInfo.toStdString();
    }

    QString infoPath = backupPath + QDir::separator() + "backup_info.json";
    try
    {
        std::ofstream ofs(infoPath.toStdString());
        ofs << info.dump(4);
    }
    catch (...)
    {
        writeLog(tr("写入备份元数据失败: %1").arg(infoPath));
    }
}

QString CMapCheckBackupManager::generateBackupFolderName(BackupType type) const
{
    QString prefix;
    switch (type)
    {
    case BackupType::FullBackup:        prefix = "FullBackup"; break;
    case BackupType::IncrementalBackup: prefix = "IncrementalBackup"; break;
    case BackupType::WALArchive:        prefix = "WALArchive"; break;
    case BackupType::LogicalExportBackup:prefix = "LogicalExportBackup"; break;
    default: prefix = "Backup"; break;
    }

    return QString("%1_%2").arg(prefix, QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
}

bool CMapCheckBackupManager::copyDirectory(const QDir& srcDir, const QDir& dstDir)
{
    if (!srcDir.exists())
    {
        return false;
    }

    if (!dstDir.exists())
    {
        dstDir.mkpath(dstDir.absolutePath());
    }

    QFileInfoList fileInfoList = srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);
    for (const QFileInfo& fileInfo : fileInfoList)
    {
        if (fileInfo.isDir())
        {
            QDir subSrcDir(fileInfo.filePath());
            QDir subDstDir(dstDir.absolutePath() + QDir::separator() + fileInfo.fileName());
            if (!copyDirectory(subSrcDir, subDstDir))
            {
                return false;
            }
        }
        else
        {
            QString dstFilePath = dstDir.absolutePath() + QDir::separator() + fileInfo.fileName();
            if (!QFile::copy(fileInfo.filePath(), dstFilePath))
            {
                // 如果文件已存在，尝试删除后重新复制
                QFile::remove(dstFilePath);
                if (!QFile::copy(fileInfo.filePath(), dstFilePath))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

bool CMapCheckBackupManager::copyDirectoryIncremental(const QDir& srcDir, const QDir& dstDir)
{
    if (!srcDir.exists())
    {
        return false;
    }

    if (!dstDir.exists())
    {
        dstDir.mkpath(dstDir.absolutePath());
    }

    QFileInfoList fileInfoList = srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);
    for (const QFileInfo& srcFileInfo : fileInfoList)
    {
        QString dstFilePath = dstDir.absolutePath() + QDir::separator() + srcFileInfo.fileName();

        if (srcFileInfo.isDir())
        {
            QDir subSrcDir(srcFileInfo.filePath());
            QDir subDstDir(dstFilePath);
            if (!copyDirectoryIncremental(subSrcDir, subDstDir))
            {
                return false;
            }
        }
        else
        {
            QFileInfo dstFileInfo(dstFilePath);
            // 如果目标文件不存在或源文件更新，则复制
            if (!dstFileInfo.exists() || srcFileInfo.lastModified() > dstFileInfo.lastModified())
            {
                if (dstFileInfo.exists())
                {
                    QFile::remove(dstFilePath);
                }
                if (!QFile::copy(srcFileInfo.filePath(), dstFilePath))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

void CMapCheckBackupManager::cleanupExpiredBackups()
{
    QDir targetDir(m_qstrTargetPath);
    if (!targetDir.exists())
    {
        return;
    }

    QFileInfoList fileInfoList = targetDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    QDateTime now = QDateTime::currentDateTime();

    for (const QFileInfo& fileInfo : fileInfoList)
    {
        QString name = fileInfo.fileName();
        QDateTime backupTime = fileInfo.birthTime();
        if (!backupTime.isValid())
        {
            backupTime = fileInfo.lastModified();
        }

        // 根据备份类型前缀判断保留天数
        int retentionDays = -1;
        if (name.startsWith("FullBackup"))        retentionDays = 28;     // 4周
        else if (name.startsWith("IncrementalBackup")) retentionDays = 7; // 7天
        else if (name.startsWith("WALArchive")) retentionDays = 3;      // 3天
        else if (name.startsWith("LogicalExportBackup")) retentionDays = 365; // 12个月

        if (retentionDays > 0)
        {
            int daysSinceBackup = backupTime.daysTo(now);
            if (daysSinceBackup > retentionDays)
            {
                QDir oldDir(fileInfo.filePath());
                oldDir.removeRecursively();
                writeLog(tr("已清理过期备份: %1").arg(fileInfo.fileName()));
            }
        }
    }
}

void CMapCheckBackupManager::writeLog(const QString& msg)
{
    QDir logDir(m_qstrTargetPath);
    if (!logDir.exists())
    {
        logDir.mkpath(m_qstrTargetPath);
    }

    QString logFilePath = m_qstrTargetPath + QDir::separator() + "timed_backup.log";
    QFile logFile(logFilePath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        QTextStream stream(&logFile);
        stream << "[" << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << "] " << msg << "\n";
        logFile.close();
    }
}

// ========== 恢复功能实现 ==========

static qint64 calculateDirectorySize(const QString& dirPath)
{
    qint64 totalSize = 0;
    QDir dir(dirPath);
    QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : list)
    {
        if (info.isDir())
            totalSize += calculateDirectorySize(info.absoluteFilePath());
        else
            totalSize += info.size();
    }
    return totalSize;
}

vector<BackupRecordInfo> CMapCheckBackupManager::scanBackupRecords(const QString& backupDir)
{
    vector<BackupRecordInfo> records;
    QDir dir(backupDir);
    if (!dir.exists())
        return records;

    QFileInfoList dirList = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);

    for (const QFileInfo& dirInfo : dirList)
    {
        QString name = dirInfo.fileName();
        BackupRecordInfo record;
        record.strName = name;
        record.strFullPath = dirInfo.absoluteFilePath();
        record.dtCreateTime = dirInfo.birthTime();
        if (!record.dtCreateTime.isValid())
            record.dtCreateTime = dirInfo.lastModified();
        record.nSizeBytes = calculateDirectorySize(dirInfo.absoluteFilePath());

        // 根据文件夹前缀判断备份类型
        if (name.startsWith("FullBackup"))
            record.eBackupType = BackupType::FullBackup;
        else if (name.startsWith("IncrementalBackup"))
            record.eBackupType = BackupType::IncrementalBackup;
        else if (name.startsWith("WALArchive"))
            record.eBackupType = BackupType::WALArchive;
        else if (name.startsWith("LogicalExportBackup"))
            record.eBackupType = BackupType::LogicalExportBackup;
        else
            record.eBackupType = BackupType::FullBackup; // 默认按全量备份处理

        records.push_back(record);
    }

    return records;
}

static bool restoreDirectoryRecursive(const QDir& srcDir, const QDir& dstDir, bool overwrite)
{
    if (!srcDir.exists())
        return false;

    if (!dstDir.exists())
        dstDir.mkpath(dstDir.absolutePath());

    QFileInfoList fileInfoList = srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);
    for (const QFileInfo& fileInfo : fileInfoList)
    {
        QString dstFilePath = dstDir.absolutePath() + QDir::separator() + fileInfo.fileName();

        if (fileInfo.isDir())
        {
            QDir subSrcDir(fileInfo.filePath());
            QDir subDstDir(dstFilePath);
            if (!restoreDirectoryRecursive(subSrcDir, subDstDir, overwrite))
                return false;
        }
        else
        {
            QFileInfo dstFileInfo(dstFilePath);
            if (dstFileInfo.exists())
            {
                if (!overwrite)
                    continue;
                QFile::remove(dstFilePath);
            }

            if (!QFile::copy(fileInfo.filePath(), dstFilePath))
                return false;
        }
    }

    return true;
}

bool CMapCheckBackupManager::restoreBackup(const QString& backupPath, const QString& dstPath, bool overwrite)
{
    QDir srcDir(backupPath);
    if (!srcDir.exists())
    {
        writeLog(tr("备份目录不存在: %1").arg(backupPath));
        return false;
    }

    QDir dstDir(dstPath);
    if (!dstDir.exists())
    {
        if (!dstDir.mkpath(dstPath))
        {
            writeLog(tr("创建目标目录失败: %1").arg(dstPath));
            return false;
        }
    }

    bool result = restoreDirectoryRecursive(srcDir, dstDir, overwrite);
    if (result)
    {
        writeLog(tr("恢复成功: %1 -> %2").arg(backupPath, dstPath));
    }
    else
    {
        writeLog(tr("恢复失败: %1 -> %2").arg(backupPath, dstPath));
    }
    return result;
}

bool CMapCheckBackupManager::restoreBackupToDatabase(const QString& backupPath,
    const QString& host, int port, const QString& dbName,
    const QString& username, const QString& password,
    const QString& schema, bool overwrite)
{
    QDir backupDir(backupPath);
    if (!backupDir.exists())
    {
        writeLog(tr("备份目录不存在: %1").arg(backupPath));
        return false;
    }

    // 查找备份目录中的数据库备份文件 (.sql, .sql.gz, .dump)
    QStringList sqlFiles;
    QStringList dumpFiles;
    QStringList gzFiles;

    QFileInfoList fileInfoList = backupDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fileInfo : fileInfoList)
    {
        if (fileInfo.suffix().compare("dump", Qt::CaseInsensitive) == 0)
            dumpFiles.append(fileInfo.absoluteFilePath());
        else if (fileInfo.suffix().compare("sql", Qt::CaseInsensitive) == 0)
            sqlFiles.append(fileInfo.absoluteFilePath());
        else if (fileInfo.fileName().endsWith(".sql.gz", Qt::CaseInsensitive))
            gzFiles.append(fileInfo.absoluteFilePath());
    }

    if (sqlFiles.isEmpty() && dumpFiles.isEmpty() && gzFiles.isEmpty())
    {
        writeLog(tr("备份目录中未找到数据库备份文件(.sql/.dump/.sql.gz)"));
        return false;
    }

    // 构建密码环境变量
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!password.isEmpty())
    {
        env.insert("PGPASSWORD", password);
    }

    bool allSuccess = true;

    // 1. 恢复 .dump 文件 (pg_restore)
    for (const QString& dumpFile : dumpFiles)
    {
        writeLog(tr("正在恢复 dump 文件: %1").arg(dumpFile));

        QProcess process;
        process.setProcessEnvironment(env);

        QStringList args;
        args << "-h" << host
             << "-p" << QString::number(port)
             << "-U" << username
             << "-d" << dbName;

        if (overwrite)
        {
            args << "-c" << "--if-exists"; // 清理已存在的对象
        }

        if (!schema.isEmpty())
        {
            args << "-n" << schema;
        }

        args << dumpFile;

        emit restoreProgress(tr("正在恢复: %1").arg(QFileInfo(dumpFile).fileName()));
        process.start("pg_restore", args);

        if (!process.waitForStarted(10000))
        {
            writeLog(tr("pg_restore 启动失败: %1").arg(process.errorString()));
            allSuccess = false;
            continue;
        }

        if (!process.waitForFinished(3600000))
        {
            writeLog(tr("pg_restore 执行超时: %1").arg(QFileInfo(dumpFile).fileName()));
            process.kill();
            allSuccess = false;
            continue;
        }

        if (process.exitCode() == 0)
        {
            QString stdErr = QString::fromLocal8Bit(process.readAllStandardError());
            if (!stdErr.isEmpty())
            {
                writeLog(tr("pg_restore 警告: %1").arg(stdErr.trimmed()));
            }
            writeLog(tr("dump 文件恢复成功: %1").arg(QFileInfo(dumpFile).fileName()));
        }
        else
        {
            QString error = QString::fromLocal8Bit(process.readAllStandardError());
            writeLog(tr("pg_restore 失败 (exit=%1): %2").arg(process.exitCode()).arg(error.trimmed()));
            allSuccess = false;
        }
    }

    // 2. 恢复 .sql.gz 文件 (先解压再 psql)
    for (const QString& gzFile : gzFiles)
    {
        writeLog(tr("正在恢复压缩 SQL 文件: %1").arg(gzFile));

        // 解压到临时文件
        QProcess gunzipProcess;
        QString tempSqlFile = QDir::tempPath() + QDir::separator() +
            QFileInfo(gzFile).baseName() + "_restore.sql";

        // 先尝试用 gzip 解压
        gunzipProcess.start("gzip", QStringList() << "-d" << "-c" << gzFile);
        if (!gunzipProcess.waitForFinished(60000) || gunzipProcess.exitCode() != 0)
        {
            writeLog(tr("gzip 解压失败: %1").arg(gunzipProcess.errorString()));
            allSuccess = false;
            continue;
        }

        // 将解压内容写入临时文件
        QByteArray decompressed = gunzipProcess.readAllStandardOutput();
        QFile tempFile(tempSqlFile);
        if (!tempFile.open(QIODevice::WriteOnly))
        {
            writeLog(tr("无法创建临时文件: %1").arg(tempSqlFile));
            allSuccess = false;
            continue;
        }
        tempFile.write(decompressed);
        tempFile.close();

        // 用 psql 恢复
        QProcess psqlProcess;
        psqlProcess.setProcessEnvironment(env);
        QStringList psqlArgs;
        psqlArgs << "-h" << host
                 << "-p" << QString::number(port)
                 << "-U" << username
                 << "-d" << dbName
                 << "-f" << tempSqlFile;

        emit restoreProgress(tr("正在恢复: %1").arg(QFileInfo(gzFile).fileName()));
        psqlProcess.start("psql", psqlArgs);

        if (!psqlProcess.waitForStarted(10000))
        {
            writeLog(tr("psql 启动失败: %1").arg(psqlProcess.errorString()));
            QFile::remove(tempSqlFile);
            allSuccess = false;
            continue;
        }

        if (!psqlProcess.waitForFinished(3600000))
        {
            writeLog(tr("psql 执行超时: %1").arg(QFileInfo(gzFile).fileName()));
            psqlProcess.kill();
            QFile::remove(tempSqlFile);
            allSuccess = false;
            continue;
        }

        QFile::remove(tempSqlFile); // 清理临时文件

        if (psqlProcess.exitCode() == 0)
        {
            QString stdErr = QString::fromLocal8Bit(psqlProcess.readAllStandardError());
            if (!stdErr.isEmpty())
            {
                writeLog(tr("psql 警告: %1").arg(stdErr.trimmed()));
            }
            writeLog(tr("压缩 SQL 文件恢复成功: %1").arg(QFileInfo(gzFile).fileName()));
        }
        else
        {
            QString error = QString::fromLocal8Bit(psqlProcess.readAllStandardError());
            writeLog(tr("psql 失败 (exit=%1): %2").arg(psqlProcess.exitCode()).arg(error.trimmed()));
            allSuccess = false;
        }
    }

    // 3. 恢复 .sql 文件 (psql)
    for (const QString& sqlFile : sqlFiles)
    {
        writeLog(tr("正在恢复 SQL 文件: %1").arg(sqlFile));

        QProcess process;
        process.setProcessEnvironment(env);

        QStringList args;
        args << "-h" << host
             << "-p" << QString::number(port)
             << "-U" << username
             << "-d" << dbName
             << "-f" << sqlFile;

        emit restoreProgress(tr("正在恢复: %1").arg(QFileInfo(sqlFile).fileName()));
        process.start("psql", args);

        if (!process.waitForStarted(10000))
        {
            writeLog(tr("psql 启动失败: %1").arg(process.errorString()));
            allSuccess = false;
            continue;
        }

        if (!process.waitForFinished(3600000))
        {
            writeLog(tr("psql 执行超时: %1").arg(QFileInfo(sqlFile).fileName()));
            process.kill();
            allSuccess = false;
            continue;
        }

        if (process.exitCode() == 0)
        {
            QString stdErr = QString::fromLocal8Bit(process.readAllStandardError());
            if (!stdErr.isEmpty())
            {
                writeLog(tr("psql 警告: %1").arg(stdErr.trimmed()));
            }
            writeLog(tr("SQL 文件恢复成功: %1").arg(QFileInfo(sqlFile).fileName()));
        }
        else
        {
            QString error = QString::fromLocal8Bit(process.readAllStandardError());
            writeLog(tr("psql 失败 (exit=%1): %2").arg(process.exitCode()).arg(error.trimmed()));
            allSuccess = false;
        }
    }

    if (allSuccess)
    {
        writeLog(tr("数据库恢复全部成功: %1 -> %2").arg(backupPath, QString("%1:%2/%3").arg(host).arg(port).arg(dbName)));
    }
    else
    {
        writeLog(tr("数据库恢复部分失败: %1").arg(backupPath));
    }
    return allSuccess;
}
