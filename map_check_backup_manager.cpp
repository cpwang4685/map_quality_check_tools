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
#include <QSettings>
#include <QMap>

/*--------------QGIS---------------*/
#include "qgssettings.h"

/*--------------PostgreSQL---------------*/
// 【2026-09-11】查服务器大版本号用（插件本就链接 libpq.lib + /DELAYLOAD:libpq.dll，
// database/data_importer.cpp、core/add_to_map_helper.h 同样直接包含本头文件）。
#include <libpq-fe.h>

/*--------------Windows---------------*/
// 【2026-09-11】取系统 ANSI 代码页（GetACP）用。见 postgresEncodingForSystemAnsi()。
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/*-----------------------------------*/

static CMapCheckBackupManager* s_pInstance = nullptr;

/*========== 【2026-09-11】pg_dump 客户端编码（中文表名修复） ==========
 * 背景：pg_dump.exe 是 ANSI 子系统的程序，Windows 会把 Qt 经 CreateProcessW
 * 传过去的 UTF-16 命令行按“系统 ANSI 代码页”转成字节（中文 Windows 上是 GBK）。
 * 单看这一步字节是对的，但 pg_dump 拿到 argv 后会把它们**当成客户端编码来解读**；
 * 连接编码默认是数据库编码（UTF8），于是每个非法字节被替换成 2 字节的坏序列
 * C0 20，再发给服务器，服务器直接拒绝：
 *
 *     ERROR: invalid byte sequence for encoding "UTF8": 0xc0 0x20
 *
 * 于是表名里只要有一个中文就备份失败（-t "public"."专用道_v1"）。表名全 ASCII 时
 * GBK 字节 == UTF-8 字节，问题不会暴露。
 *
 * 修法分两级：
 *
 *  1) 首选——把非 ASCII 从命令行里挪出去。表名含非 ASCII 时不再逐个用 -t 传，改成写出
 *     一份 --filter 文件（PostgreSQL 17 起提供）交给 pg_dump。文件内容是 UTF-8，pg_dump
 *     按客户端编码（=数据库编码 UTF8）正常读取；命令行里于是只剩 ASCII，客户端编码
 *     完全不必改动，备份文件仍是 UTF8。这一级没有任何副作用：既不会把库里带生僻字的
 *     数据（GBK 装不下的字符，如 U+4D16）逼到失败，也不改变输出编码。
 *
 *  2) 兜底——非 ASCII 仍然留在命令行里时（pg_dump 低于 17 用不了 --filter，或中文出现在
 *     库名/Schema/用户名上），把 PGCLIENTENCODING 设成“系统 ANSI 代码页对应的 PostgreSQL
 *     编码名”，让 pg_dump 正确地按该编码解读 argv。副作用是备份文件变成该编码（如 GBK），
 *     文件自带 SET client_encoding = 'GBK'; 属自描述，psql / pg_restore 恢复无碍；但该
 *     编码装不下的字符会让 pg_dump 明确报错退出（exit 1）——不会静默丢数据，却会备份失败。
 *
 * 触发判断只统计"会经 argv 送给服务器解析"的值（主机/用户/库名/Schema/表名），**不含**
 * -f 的输出文件路径：那是本地文件系统路径、不发给服务器，把它算进来会让"表名全 ASCII、
 * 只是目标目录名带中文"的机器误开 PGCLIENTENCODING，反而把原本正常的备份打挂。
 * Linux（麒麟）下 argv 以 UTF-8 原样传递，两级都不触发，行为与改动前一致。
 * ================================================================= */

// 命令行参数里是否出现非 ASCII 字符
static bool listHasNonAscii(const QStringList& list)
{
    for (const QString& s : list)
    {
        for (const QChar& c : s)
        {
            if (c.unicode() > 0x7F)
                return true;
        }
    }
    return false;
}

// 系统 ANSI 代码页 -> PostgreSQL 编码名；未收录的代码页返回空串（调用方跳过设置）
static QString postgresEncodingForSystemAnsi()
{
    QString enc;
#ifdef Q_OS_WIN
    switch (GetACP())
    {
    case 874:   enc = QStringLiteral("WIN874");  break;
    case 932:   enc = QStringLiteral("SJIS");    break;   // 日文
    case 936:   enc = QStringLiteral("GBK");     break;   // 简体中文
    case 949:   enc = QStringLiteral("UHC");     break;   // 韩文
    case 950:   enc = QStringLiteral("BIG5");    break;   // 繁体中文
    case 1250:  enc = QStringLiteral("WIN1250"); break;
    case 1251:  enc = QStringLiteral("WIN1251"); break;
    case 1252:  enc = QStringLiteral("WIN1252"); break;
    case 1253:  enc = QStringLiteral("WIN1253"); break;
    case 1254:  enc = QStringLiteral("WIN1254"); break;
    case 1255:  enc = QStringLiteral("WIN1255"); break;
    case 1256:  enc = QStringLiteral("WIN1256"); break;
    case 1257:  enc = QStringLiteral("WIN1257"); break;
    case 1258:  enc = QStringLiteral("WIN1258"); break;
    case 65001: enc = QStringLiteral("UTF8");    break;
    default:    enc.clear();                     break;
    }
#endif
    return enc;
}

/*========== 【2026-09-11】PostgreSQL 客户端工具解析（辅助函数） ==========
 * 策略与背景见 map_check_backup_manager.h 里 resolvePgClientTool() 的注释。
 * ====================================================================== */

// 连一次数据库问出服务器大版本号（18.6 -> 18）。失败返回 false，调用方据此跳过版本校验。
static bool queryPostgresServerMajor(const QString& host, int port, const QString& dbName,
                                     const QString& user, const QString& password,
                                     int& majorOut, QString& versionTextOut)
{
    majorOut = -1;
    versionTextOut.clear();

    if (dbName.isEmpty())
        return false;

    const QByteArray baHost     = host.toUtf8();
    const QByteArray baPort     = QString::number(port).toUtf8();
    const QByteArray baDbName   = dbName.toUtf8();
    const QByteArray baUser     = user.toUtf8();
    const QByteArray baPassword = password.toUtf8();

    const char* keys[] = { "host", "port", "dbname", "user", "password",
                           "connect_timeout", "application_name", nullptr };
    const char* vals[] = { baHost.constData(), baPort.constData(), baDbName.constData(),
                           baUser.constData(), baPassword.constData(),
                           "5", "map_quality_check_tools", nullptr };

    PGconn* conn = PQconnectdbParams(keys, vals, 0);
    if (!conn || PQstatus(conn) != CONNECTION_OK)
    {
        if (conn) PQfinish(conn);
        return false;
    }

    const int verNum = PQserverVersion(conn);   // 例如 180006
    if (verNum > 0)
    {
        majorOut = verNum / 10000;
        // server_version 文本（如 "18.6 (Debian 18.6-1)"）仅用于提示，拿不到也不影响
        PGresult* res = PQexec(conn, "SHOW server_version");
        if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        {
            const QString text = QString::fromUtf8(PQgetvalue(res, 0, 0)).trimmed();
            // 只保留前面的版本号部分，去掉括号里的发行版说明
            QRegExp re(QStringLiteral("^([0-9][0-9.]*)"));
            versionTextOut = (re.indexIn(text) >= 0) ? re.cap(1) : text;
        }
        if (res) PQclear(res);
    }

    PQfinish(conn);
    return majorOut > 0;
}

// 跑一次 "<exe> --version"，取出版本号文本。例："pg_dump (PostgreSQL) 15.2" -> "15.2"。
// 探测失败（启动不了 / 非 0 退出 / 输出解析不出来）返回空串。
static QString probePgToolVersionText(const QString& exePath)
{
    QProcess proc;
    proc.start(exePath, QStringList() << QStringLiteral("--version"));
    if (!proc.waitForStarted(5000))
        return QString();
    if (!proc.waitForFinished(5000))
    {
        proc.kill();
        proc.waitForFinished(1000);
        return QString();
    }
    if (proc.exitCode() != 0)
        return QString();

    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput())
                      + QString::fromLocal8Bit(proc.readAllStandardError());

    QRegExp re(QStringLiteral("([0-9]+\\.[0-9]+)"));
    if (re.indexIn(out) < 0)
        return QString();
    return re.cap(1);
}

// 从版本号文本取大版本号（"15.2" -> 15）。取不到返回 -1。
static int majorFromVersionText(const QString& verText)
{
    QRegExp re(QStringLiteral("^([0-9]+)"));
    if (re.indexIn(verText) < 0)
        return -1;
    return re.cap(1).toInt();
}

// 收集本机 PostgreSQL 的 bin 目录候选（与盘符无关）。
static void collectPgBinDirs(QStringList& dirs)
{
#ifdef Q_OS_WIN
    // a) 官方安装器登记的安装位置。
    //    实测本机（装在 D 盘）：HKLM\SOFTWARE\PostgreSQL\Installations\postgresql-x64-17
    //      Base Directory = D:\Software\postgreSQL   Version = 17.5-1
    //    这正是"客户装在别的盘、我们不知道在哪"的解法——安装器会如实记录。
    {
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\PostgreSQL\\Installations"),
                      QSettings::NativeFormat);
        const QStringList groups = reg.childGroups();
        for (const QString& group : groups)
        {
            reg.beginGroup(group);
            const QString base = reg.value(QStringLiteral("Base Directory")).toString();
            reg.endGroup();
            if (base.isEmpty())
                continue;
            const QString bin = QDir::fromNativeSeparators(base) + QStringLiteral("/bin");
            if (QDir(bin).exists() && !dirs.contains(bin))
                dirs << bin;
        }
    }

    // b) 兜底：服务登记的可执行路径。
    //    ImagePath 形如 "D:\Software\postgreSQL\bin\pg_ctl.exe" runservice -N ... -D "..."
    //    取 "\bin\" 之前那段即为安装根目录。
    {
        QSettings services(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services"),
                           QSettings::NativeFormat);
        const QStringList groups = services.childGroups();
        for (const QString& group : groups)
        {
            if (!group.startsWith(QStringLiteral("postgresql"), Qt::CaseInsensitive))
                continue;
            services.beginGroup(group);
            const QString image = services.value(QStringLiteral("ImagePath")).toString();
            services.endGroup();
            if (image.isEmpty())
                continue;
            const int idx = image.lastIndexOf(QStringLiteral("\\bin\\"), -1, Qt::CaseInsensitive);
            if (idx <= 0)
                continue;
            const QString bin = QDir::fromNativeSeparators(image.left(idx)) + QStringLiteral("/bin");
            if (QDir(bin).exists() && !dirs.contains(bin))
                dirs << bin;
        }
    }

    // c) 各盘符的常见安装位置（覆盖注册表没登记的情况，如解压版、绿色版）。
    for (char drive = 'C'; drive <= 'Z'; ++drive)
    {
        const QString root = QString(QChar(drive)) + QStringLiteral(":/");
        if (!QDir(root).exists())
            continue;
        for (int ver = 9; ver <= 20; ++ver)
        {
            const QStringList bases = {
                root + QStringLiteral("Program Files/PostgreSQL/%1").arg(ver),
                root + QStringLiteral("Program Files (x86)/PostgreSQL/%1").arg(ver),
                root + QStringLiteral("PostgreSQL/%1").arg(ver),
            };
            for (const QString& b : bases)
            {
                const QString bin = b + QStringLiteral("/bin");
                if (QDir(bin).exists() && !dirs.contains(bin))
                    dirs << bin;
            }
        }
    }
#else
    // Linux（麒麟）：服务端工具在 /usr/lib/postgresql/<版本>/bin；
    // postgresql-client 装的 psql/pg_dump 一般在 /usr/bin。
    {
        QDir pgRoot(QStringLiteral("/usr/lib/postgresql"));
        if (pgRoot.exists())
        {
            const QStringList versions = pgRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString& v : versions)
            {
                const QString bin = pgRoot.filePath(v + QStringLiteral("/bin"));
                if (QDir(bin).exists() && !dirs.contains(bin))
                    dirs << bin;
            }
        }
    }
    const QStringList sysDirs = {
        QStringLiteral("/usr/bin"),
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/usr/local/pgsql/bin"),
        QStringLiteral("/usr/lib/postgresql/bin"),
    };
    for (const QString& d : sysDirs)
    {
        if (QDir(d).exists() && !dirs.contains(d))
            dirs << d;
    }
#endif
}

// 解析客户端工具的可用路径。语义见头文件注释。
QString CMapCheckBackupManager::resolvePgClientTool(const QString& toolName,
                                                    const QString& host, int port, const QString& dbName,
                                                    const QString& user, const QString& password,
                                                    QString& errMsg,
                                                    bool enforceVersionGate)
{
    errMsg.clear();

#ifdef Q_OS_WIN
    const QString exeSuffix = QStringLiteral(".exe");
#else
    const QString exeSuffix = QString();
#endif

    // ---- 1) 先问服务器版本。连不上不算失败，只是退化成"选能找到的最高版本"。----
    int serverMajor = -1;
    QString serverVerText;
    queryPostgresServerMajor(host, port, dbName, user, password, serverMajor, serverVerText);

    // 结果按 (工具名, 服务器大版本, 是否开闸门) 缓存：同一进程内反复备份不必每次重扫磁盘 + 重跑子进程。
    static QMap<QString, QString> s_mapResolved;
    const QString cacheKey = toolName + QLatin1Char('|') + QString::number(serverMajor)
                           + QLatin1Char('|') + (enforceVersionGate ? QLatin1Char('1') : QLatin1Char('0'));
    if (s_mapResolved.contains(cacheKey))
        return s_mapResolved.value(cacheKey);

    // ---- 2) 收集候选可执行文件，按可靠性从高到低 ----
    QStringList candidates;

    QStringList binDirs;
    collectPgBinDirs(binDirs);
    for (const QString& d : binDirs)
    {
        const QString exe = QDir(d).filePath(toolName + exeSuffix);
        if (QFileInfo::exists(exe) && !candidates.contains(exe))
            candidates << exe;
    }

    // 历史硬编码路径（保留兼容，但要过下面的版本闸门才可能被选中）
#ifdef Q_OS_WIN
    {
        const QString legacy = QStringLiteral("D:/Software/postgreSQL/bin/") + toolName + exeSuffix;
        if (QFileInfo::exists(legacy) && !candidates.contains(legacy))
            candidates << legacy;
    }
#endif

    // PATH 上的同名工具也纳入候选——但同样必须过版本闸门。
    // 客户机上的 15.2 就是这样被抓到的；在这里它会被服务器版本淘汰掉。
    const QString pathExe = QStandardPaths::findExecutable(toolName);
    if (!pathExe.isEmpty() && !candidates.contains(pathExe))
        candidates << pathExe;

    // ---- 3) 版本闸门 ----
    // gate 为真：只接受大版本 >= 服务器大版本的候选（pg_dump / pg_restore 的硬性要求）。
    // gate 为假：不设下限，直接选能找到的最高版本（psql，或没能问到服务器版本时）。
    const bool gate = (enforceVersionGate && serverMajor > 0);

    int bestMajor = -1;
    QString bestPath;
    QStringList foundTexts;   // 用于失败时的提示

    for (const QString& exe : candidates)
    {
        const QString verText = probePgToolVersionText(exe);
        if (verText.isEmpty())
            continue;
        const int major = majorFromVersionText(verText);
        if (major < 0)
            continue;

        const QString mark = (gate && major < serverMajor) ? tr("  [版本过低]") : QString();
        foundTexts << QStringLiteral("%1%2  %3")
                          .arg(verText, mark, QDir::toNativeSeparators(exe));

        if (gate)
        {
            // pg_dump / pg_restore 要求工具大版本 >= 目标服务器大版本，否则会在连接阶段
            // 直接报 "aborting because of server version mismatch"。
            if (major < serverMajor)
                continue;
            // 够用的里面挑最贴近服务器的（同版本最稳），避免无谓地用一个过新的工具
            if (bestMajor < 0 || major < bestMajor)
            {
                bestMajor = major;
                bestPath = exe;
            }
        }
        else
        {
            if (major > bestMajor)
            {
                bestMajor = major;
                bestPath = exe;
            }
        }
    }

    if (!bestPath.isEmpty())
    {
        writeLog(tr("选用 %1: %2 (v%3)")
                     .arg(toolName, QDir::toNativeSeparators(bestPath), QString::number(bestMajor)));
        if (serverMajor > 0)
            writeLog(tr("数据库服务器版本: %1").arg(
                serverVerText.isEmpty() ? QString::number(serverMajor) : serverVerText));
        s_mapResolved.insert(cacheKey, bestPath);
        return bestPath;
    }

    // ---- 4) 一个候选都没通过闸门 ----
    if (!foundTexts.isEmpty())
    {
        // 情况 A：探测到了版本，但都不够新。这正是客户机上发生的那种失败，
        // 必须把"服务器是几、本机只有几"说清楚，而不是像原来那样闷头用旧工具。
        if (gate)
        {
            errMsg = tr("数据库服务器为 PostgreSQL %1，但本机没有版本足够新的 %2 工具。\n"
                        "PostgreSQL 不允许用低版本客户端访问高版本服务器，操作会直接失败。\n"
                        "请在本机安装 PostgreSQL %3 的客户端工具后重试。")
                         .arg(serverVerText.isEmpty() ? QString::number(serverMajor) : serverVerText,
                              toolName, QString::number(serverMajor));
        }
        else
        {
            errMsg = tr("未找到可用的 %1 工具。").arg(toolName);
        }
        errMsg += tr("\n\n本机已找到：\n%1").arg(foundTexts.join(QStringLiteral("\n")));
        writeLog(errMsg);
        // 不缓存失败结果：用户装好工具后无需重启即可重试。
        return QString();
    }

    // 情况 B：所有候选都探测不出有效版本（被安全软件拦截、非标准构建等）。
    // 此时无从判断版本，保留改动前的行为——PATH 上有同名工具就继续用，总比直接判死好。
    if (!pathExe.isEmpty())
    {
        errMsg = tr("未能识别 %1 的版本号，将直接使用 PATH 上的 %2（可能因版本不匹配而失败）。")
                     .arg(toolName, QDir::toNativeSeparators(pathExe));
        writeLog(errMsg);
        s_mapResolved.insert(cacheKey, pathExe);
        return pathExe;
    }

    // 情况 C：本机连一个同名工具都没有。
    errMsg = tr("未找到 %1 工具。\n"
                "请确认 PostgreSQL 已安装，或手动将 PostgreSQL 的 bin 目录加入系统 PATH 环境变量后重启程序。")
                 .arg(toolName);
    writeLog(errMsg);
    return QString();
}

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
            // 【2026-09-11】选中的表列表原先没有落盘，定时备份取到的策略里
            // lstDbTables 恒为空 -> 不加任何 -t -> 静默退化成整库备份。补上。
            settings.setValue(prefix + "DbTables", strategy.lstDbTables, QgsSettings::Section::Plugins);
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
            // 【2026-09-11】与 saveSettings 对应：恢复选中的表列表（旧版无此项时为空表）
            strategy.lstDbTables = settings.value(prefix + "DbTables", QStringList(), QgsSettings::Section::Plugins).toStringList();
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
    // 检查数据库名称是否为空
    // 【2026-09-11】这一检查由原先的"查完 pg_dump 之后"提到了最前面：解析 pg_dump 需要
    // 先拿到库名才能连库问出服务器版本，再据此挑一个版本够新的 pg_dump
    // （详见 resolvePgClientTool 的注释）。原顺序下库名为空会先建出空目录再失败。
    if (strategy.strDbName.isEmpty())
    {
        m_qstrLastError = tr("数据库名称为空，请输入数据库名称。");
        writeLog(m_qstrLastError);
        return false;
    }

    // 解析 pg_dump：客户端大版本必须 >= 服务器大版本，否则连接阶段就会被驳回。
    QString toolErr;
    const QString pgDumpPath = resolvePgClientTool(
        QStringLiteral("pg_dump"),
        strategy.strDbHost.isEmpty() ? QStringLiteral("127.0.0.1") : strategy.strDbHost,
        strategy.nDbPort,
        strategy.strDbName,
        strategy.strDbUser.isEmpty() ? QStringLiteral("postgres") : strategy.strDbUser,
        strategy.strDbPassword,
        toolErr);

    if (pgDumpPath.isEmpty())
    {
        m_qstrLastError = toolErr;   // resolvePgClientTool 内部已写日志
        return false;
    }
    if (!toolErr.isEmpty())
    {
        // 拿到了可用路径、但附带一条降级说明（例如没能识别出版本号）：记日志后照常继续
        writeLog(toolErr);
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
    // 【2026-09-11】另记一份"会经 argv 送给服务器解析"的值（主机/用户/库名/Schema/表名），
    // 只有它们与 pg_dump 的 argv 编码问题有关（见文件头说明）。-f 的输出文件路径是本地
    // 文件系统路径、不发给服务器，**绝不能**参与判断——否则目标目录名里带中文（例如桌面
    // 上名为"备份"的文件夹）就会在表名全 ASCII 的库上误开 PGCLIENTENCODING，把那台机器
    // 上原本正常的备份打挂。
    QStringList serverSideValues;

    const QString strHost   = strategy.strDbHost.isEmpty() ? QStringLiteral("127.0.0.1") : strategy.strDbHost;
    const QString strUser   = strategy.strDbUser.isEmpty() ? QStringLiteral("postgres") : strategy.strDbUser;
    const QString strSchema = strategy.strDbSchema;

    args << "-h" << strHost;
    args << "-p" << QString::number(strategy.nDbPort);
    args << "-U" << strUser;
    args << "-d" << strategy.strDbName;
    serverSideValues << strHost << strUser << strategy.strDbName;

    // 表名含非 ASCII 时临时写出、用完即删的 --filter 文件
    QString strTableFilterFile;

    // 根据备份范围构建参数
    if (strategy.eDbBackupScope == DatabaseBackupScope::Schema &&
        !strategy.strDbSchema.isEmpty())
    {
        args << "-n" << strSchema;
        serverSideValues << strSchema;
    }
    else if (strategy.eDbBackupScope == DatabaseBackupScope::SelectedTables &&
        !strategy.lstDbTables.isEmpty())
    {
        QStringList tablePatterns;
        for (const QString& table : strategy.lstDbTables)
        {
            tablePatterns << ("\"" + strSchema + "\".\"" + table + "\"");
        }

        // 表名/Schema 全是 ASCII 时走原来的 -t 路径，行为与改动前逐字一致。
        // 含非 ASCII 时优先把表名从命令行里挪走，改用一份 --filter 文件传递：
        // 文件内容是 UTF-8，pg_dump 按客户端编码（=数据库编码 UTF8）正常读取，命令行里
        // 于是不再有非 ASCII，客户端编码也就不必改动——备份文件仍是 UTF8，不会因为数据里
        // 有 GBK 装不下的字符（生僻字如 U+4D16）而失败。--filter 自 PostgreSQL 17 起提供。
        bool bUseFilter = false;
#ifdef Q_OS_WIN
        if (listHasNonAscii(tablePatterns))
        {
            const int nDumpMajor = majorFromVersionText(probePgToolVersionText(pgDumpPath));
            bUseFilter = (nDumpMajor >= 17);
        }
#endif
        if (bUseFilter)
        {
            strTableFilterFile = QDir::tempPath() + QDir::separator() +
                QString("ltzk_pgdump_filter_%1.txt")
                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmsszzz"));

            QFile fileFilter(strTableFilterFile);
            QStringList filterLines;
            for (const QString& pattern : tablePatterns)
            {
                filterLines << QString("include table %1").arg(pattern);
            }

            if (fileFilter.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                fileFilter.write(filterLines.join("\n").toUtf8() + "\n") > 0)
            {
                fileFilter.close();
                args << QString("--filter=%1").arg(strTableFilterFile);
            }
            else
            {
                // 写不出临时文件就退回 -t（老办法，由下面的 PGCLIENTENCODING 兜底）
                fileFilter.close();
                writeLog(tr("表名含非 ASCII，但无法写入过滤文件 %1，退回 -t 参数方式")
                             .arg(strTableFilterFile));
                QFile::remove(strTableFilterFile);
                strTableFilterFile.clear();
                bUseFilter = false;
            }
        }

        if (!bUseFilter)
        {
            for (const QString& pattern : tablePatterns)
            {
                args << "-t" << pattern;
                serverSideValues << pattern;
            }
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

    // 【2026-09-11】--filter 临时文件用完即删；下面每个提前返回的分支都要调一次。
    auto removeTableFilterFile = [&strTableFilterFile]()
    {
        if (!strTableFilterFile.isEmpty())
        {
            QFile::remove(strTableFilterFile);
            strTableFilterFile.clear();
        }
    };

    // 【2026-09-11】兜底路径：只有在"非 ASCII 仍然留在命令行里"时才设客户端编码
    // （表名已改走 --filter 的情形不算；剩下的可能是中文库名/Schema/用户名，或 pg_dump
    // 低于 17 而没法用 --filter）。不设的话 pg_dump 会把 ANSI 字节当 UTF-8 解读并生成
    // 非法序列，见文件头说明。注意这里判断的是 serverSideValues，**不含** -f 输出路径。
    QString strPgClientEnc;
#ifdef Q_OS_WIN
    if (listHasNonAscii(serverSideValues))
    {
        strPgClientEnc = postgresEncodingForSystemAnsi();
    }
#endif

    // 设置环境变量（密码 / 客户端编码）
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!strategy.strDbPassword.isEmpty())
    {
        env.insert("PGPASSWORD", strategy.strDbPassword);
    }
    if (!strPgClientEnc.isEmpty())
    {
        env.insert("PGCLIENTENCODING", strPgClientEnc);
    }
    process.setProcessEnvironment(env);

    // 【2026-09-11】日志里带上实际选用的 pg_dump 路径：排查版本问题时一眼能看出用的是哪一个。
    // 非 ASCII 时同时记下 PGCLIENTENCODING，便于区分“编码修复是否生效”。
    if (!strTableFilterFile.isEmpty())
    {
        writeLog(tr("表名含非 ASCII，已改用 --filter 文件传递表名（备份文件仍为 UTF8）: %1")
                     .arg(strTableFilterFile));
    }
    writeLog(tr("开始数据库备份: %1 %2%3")
                 .arg(QDir::toNativeSeparators(pgDumpPath), args.join(" "),
                      strPgClientEnc.isEmpty()
                          ? QString()
                          : tr("  [PGCLIENTENCODING=%1]").arg(strPgClientEnc)));
    emit backupProgress(tr("正在备份数据库 %1 ...").arg(strategy.strDbName));

    process.start(pgDumpPath, args);
    if (!process.waitForStarted(10000))
    {
        m_qstrLastError = tr("pg_dump 启动失败: %1").arg(process.errorString());
        writeLog(m_qstrLastError);
        removeTableFilterFile();
        return false;
    }

    if (!process.waitForFinished(3600000)) // 最多等待1小时
    {
        m_qstrLastError = tr("pg_dump 执行超时");
        writeLog(m_qstrLastError);
        process.kill();
        removeTableFilterFile();
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
        removeTableFilterFile();
        return true;
    }
    else
    {
        QString error = QString::fromLocal8Bit(process.readAllStandardError());
        m_qstrLastError = tr("pg_dump 失败 (exit=%1): %2").arg(process.exitCode()).arg(error.trimmed());
        writeLog(m_qstrLastError);
        removeTableFilterFile();
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

    // 【2026-09-11】解析恢复所需的客户端工具路径。
    // 原来 pg_restore / psql 都是裸调 PATH 上的同名程序，客户机上会抓到 LTZK 自带
    // OSGeo4W 里的 15.2——它读不了 18.6 服务器导出的 .dump，恢复同样会失败。
    // pg_restore 要过版本闸门；psql 只执行 SQL 文本，官方允许连任意版本服务器，不设下限。
    QString toolErr;
    QString pgRestorePath;
    QString psqlPath;
    if (!dumpFiles.isEmpty())
    {
        pgRestorePath = resolvePgClientTool(QStringLiteral("pg_restore"), host, port, dbName,
                                            username, password, toolErr);
        if (pgRestorePath.isEmpty())
        {
            writeLog(tr("无法恢复 .dump 文件：%1").arg(toolErr));
            allSuccess = false;
        }
    }
    if (!sqlFiles.isEmpty() || !gzFiles.isEmpty())
    {
        psqlPath = resolvePgClientTool(QStringLiteral("psql"), host, port, dbName,
                                       username, password, toolErr, false);
        if (psqlPath.isEmpty())
        {
            writeLog(tr("无法恢复 .sql / .sql.gz 文件：%1").arg(toolErr));
            allSuccess = false;
        }
    }

    // 1. 恢复 .dump 文件 (pg_restore)
    for (const QString& dumpFile : dumpFiles)
    {
        if (pgRestorePath.isEmpty())
            continue;   // 找不到可用的 pg_restore，原因已在上面记录

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
        process.start(pgRestorePath, args);

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
        if (psqlPath.isEmpty())
            continue;   // 找不到可用的 psql，原因已在上面记录（提前跳过，免得白解压一遍）

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
        psqlProcess.start(psqlPath, psqlArgs);

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
        if (psqlPath.isEmpty())
            continue;   // 找不到可用的 psql，原因已在上面记录

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
        process.start(psqlPath, args);

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
