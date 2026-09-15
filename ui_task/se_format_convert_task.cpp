#define _HAS_STD_BYTE 0
#include "se_format_convert_task.h"
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qdiriterator.h>
#include <qdatetime.h>
#include <qset.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>

#include "commontype/se_commondef.h"

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <memory>
#include <set>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/ostream_sink.h"

// 包装目录：本身不是真 GDB（无 .gdbtable），内部却嵌套着 *.gdb 目录。
// 常见于解压包多套一层同名文件夹；扫描时应跳过，真正的 GDB 会被递归扫到。
static bool IsGdbWrapperDir(const QString& dir)
{
    QDir d(dir);
    if (!d.entryList(QStringList() << "*.gdbtable" << "*.gdbtablx",
                    QDir::Files | QDir::Readable | QDir::Hidden).isEmpty())
        return false;
    return !d.entryList(QStringList() << "*.gdb" << "*.GDB",
                        QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
}

static void CollectGdbDirsRecursive(const QString& path, QStringList& out)
{
    QDirIterator it(path, QStringList() << "*.gdb" << "*.GDB",
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString d = it.next();
        if (IsGdbWrapperDir(d)) continue;
        out << d;
    }
}

static void CollectFilesRecursive(const QString& path, const QStringList& nameFilters, QStringList& out)
{
    QDirIterator it(path, nameFilters, QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
}

static QString CanonicalPath(const QString& path)
{
    QString c = QFileInfo(path).canonicalFilePath();
    if (c.isEmpty()) c = QDir::cleanPath(path);
    return c.toLower();
}

// 严格 UTF-8 校验（与 merge/接边功能里处理字段名的逻辑一致）
static bool IsValidUtf8(const QByteArray& raw)
{
    int j = 0;
    while (j < raw.size()) {
        unsigned char c = raw[j];
        int len; unsigned int minCp;
        if (c < 0x80)           { len = 1; minCp = 0; }
        else if ((c & 0xE0) == 0xC0) { len = 2; minCp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; minCp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; minCp = 0x10000; }
        else return false;
        if (j + len > raw.size()) return false;
        for (int k = 1; k < len; ++k)
            if ((raw[j+k] & 0xC0) != 0x80) return false;
        unsigned int cp;
        if (len == 2) cp = ((c & 0x1F) << 6) | (raw[j+1] & 0x3F);
        else if (len == 3) cp = ((c & 0x0F) << 12) | ((raw[j+1] & 0x3F) << 6) | (raw[j+2] & 0x3F);
        else cp = ((c & 0x07) << 18) | ((raw[j+1] & 0x3F) << 12) | ((raw[j+2] & 0x3F) << 6) | (raw[j+3] & 0x3F);
        if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        j += len;
    }
    return true;
}

// GDAL 返回的文本（图层名/字段名/属性值）→ UTF-8 std::string
// bForceLocal=true：一律按本地编码(GBK)解码，专用于 PGeo。
// PGeo 经 ANSI ODBC 返回的必是 GBK 字节，但部分 GBK 双字节恰好构成合法 UTF-8 序列
// （如"时"=CA B1 会被当成 UTF-8），若走 UTF-8 校验回退会漏解，出现"部分乱码部分正确"。
// 其他驱动（GeoJSON/SHP/GPKG/GDB）返回 UTF-8，走严格校验：合法原样、非法按 GBK。
// 非法即 GBK 解（不能只看"含 UTF-8 多字节前缀"）：GBK 首字节 0x81~0xBF 段
// （如"高速"=B8 DF CB D9）不含合法 UTF-8 前缀，旧 hasMb 判据会漏判、把非法字节原样写出。
static std::string DecodeGdalTextUtf8(const char* sz, bool bForceLocal = false)
{
    if (!sz || !*sz) return std::string();
    const QByteArray raw(sz);
    if (bForceLocal)
        return QString::fromLocal8Bit(raw).toUtf8().toStdString();
    if (IsValidUtf8(raw))
        return std::string(sz);
    return QString::fromLocal8Bit(raw).toUtf8().toStdString();
}

// 打开源数据。SHP 源无 .cpg 时不做编码探测，直接 SHAPE_ENCODING="" 取 DBF 原始字节，
// 由 DecodeGdalTextUtf8 逐值判定（合法 UTF-8 原样、非法按 GBK）。
// 旧取样探测法在混合编码数据（同一 DBF 同时含 UTF-8 与 GBK 值，实测 公路.dbf
// NAME 20459 个值：UTF-8 10092 / GBK 6947 / 截断残值 3420）上会误判：
// 探测失败走 GDAL 默认（LDID）解码，UTF-8 字节被按 GBK 解成"合法 UTF-8 的乱码串"
// （如"鍞愬北"），逐值兜底无法识别，乱码被原样写入。
static GDALDataset* OpenSourceDataset(const std::string& srcFile, const std::string& srcDriver)
{
    const char* apszDrivers[2] = { srcDriver.empty() ? nullptr : srcDriver.c_str(), nullptr };
    if (srcDriver != "ESRI Shapefile")
    {
        return (GDALDataset*)GDALOpenEx(srcFile.c_str(), GDAL_OF_VECTOR,
            srcDriver.empty() ? nullptr : apszDrivers, nullptr, nullptr);
    }

    VSIStatBufL sStat;
    const char* pszCpgPath = CPLResetExtension(srcFile.c_str(), "cpg");
    const bool bHasCpg = (VSIStatL(pszCpgPath, &sStat) == 0);
    if (bHasCpg)
    {
        // 有 .cpg，GDAL 按 .cpg 处理，无需干预
        return (GDALDataset*)GDALOpenEx(srcFile.c_str(), GDAL_OF_VECTOR, apszDrivers, nullptr, nullptr);
    }

    // 无 .cpg：取原始字节，逐值兜底在 DecodeGdalTextUtf8 / CopyLayer 完成。
    // 用线程局部配置（任务线程），避免并行转换任务间全局配置竞态。
    const char* pszOld = CPLGetThreadLocalConfigOption("SHAPE_ENCODING", nullptr);
    CPLSetThreadLocalConfigOption("SHAPE_ENCODING", "");
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(srcFile.c_str(), GDAL_OF_VECTOR, apszDrivers, nullptr, nullptr);
    CPLSetThreadLocalConfigOption("SHAPE_ENCODING", pszOld);
    return poDS;
}

SeFormatConvertTask::SeFormatConvertTask(const QString& name,
    const string& strInputPath,
    const string& strOutputPath,
    const string& strSrcDriverName,
    const string& strTgtDriverName,
    const string& strSrcExtension,
    const string& strTgtExtension,
    int iLogLevel,
    const string& strOutputLogPath,
    bool bSingleInputFile,
    const string& strLayerName,
    const string& strGdbName)
    : QgsTask(name)
    , m_strInputPath(strInputPath)
    , m_strOutputPath(strOutputPath)
    , m_strSrcDriverName(strSrcDriverName)
    , m_strTgtDriverName(strTgtDriverName)
    , m_strSrcExtension(strSrcExtension)
    , m_strTgtExtension(strTgtExtension)
    , m_iLogLevel(iLogLevel)
    , m_strOutputLogPath(strOutputLogPath)
    , mProgress(0)
    , mCanceled(false)
    , m_bSingleInputFile(bSingleInputFile)
    , m_strLayerName(strLayerName)
    , m_strGdbName(strGdbName)
{
}

bool SeFormatConvertTask::run()
{
    CPLSetConfigOption("OGR_SHP_ESRI_WKT", "YES");
    GDALAllRegister();

    string strLogLevel;
    if (m_iLogLevel == SE_LOG_LEVEL_ERROR)      strLogLevel = "Error";
    else if (m_iLogLevel == SE_LOG_LEVEL_INFO)  strLogLevel = "Info";
    else if (m_iLogLevel == SE_LOG_LEVEL_DEBUG) strLogLevel = "Debug";

    string strLoggerName = "FormatConvert_" + m_strSrcExtension + "_to_" + m_strTgtExtension;
    string strLogFileFullPath = m_strOutputLogPath + "/System_Running_"
        + strLogLevel + "_FormatConvert.txt";
    // Use ostringstream sink – spdlog writes to memory, we write file once at end with BOM
    auto logStream = std::make_shared<std::ostringstream>();
    auto logSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*logStream);
    auto file_logger = std::make_shared<spdlog::logger>(strLoggerName, logSink);
    spdlog::register_logger(file_logger);

    if (m_iLogLevel == SE_LOG_LEVEL_ERROR)      file_logger->set_level(spdlog::level::err);
    else if (m_iLogLevel == SE_LOG_LEVEL_INFO)  file_logger->set_level(spdlog::level::info);
    else if (m_iLogLevel == SE_LOG_LEVEL_DEBUG) file_logger->set_level(spdlog::level::debug);

    char szLog[1000] = { 0 };
    snprintf(szLog, sizeof(szLog), "正在执行格式转换: %s -> %s", m_strSrcDriverName.c_str(), m_strTgtDriverName.c_str());
    file_logger->info(szLog);
    file_logger->flush();

    bool bOk = false;
    // GDB 批量：所有源数据汇入同一个 .gdb（每个源文件一个要素类，学长口径）；
    // GDB 单个：输出目录/<库名>.gdb；GPKG 批量：输出目录/<源名>.gpkg，单个：用户选定的文件
    bool bGdbTarget = (m_strTgtDriverName == "OpenFileGDB");
    bool bGpkgTarget = (m_strTgtDriverName == "GPKG");
    int successCount = 0;
    int totalCount = 0;
    QStringList tmpMdbFiles;
    GDALDataset* poBatchGdb = nullptr;
    string batchGdbName;

    do {
        vector<string> srcFileList;
        if (!m_srcFileList.isEmpty())
        {
            // 批量：清单里既有文件也有文件夹，逐项处理
            QStringList folderList;
            QStringList pickedFiles;
            for (const QString& f : m_srcFileList)
            {
                QFileInfo fi(f);
                if (fi.isDir()) folderList << f;
                else if (fi.isFile()) pickedFiles << f;
            }

            QStringList scanned;
            if (m_strSrcExtension == "gdb")
            {
                for (const QString& folder : folderList)
                {
                    // 包装目录（外层套娃）本身不是真 GDB，递归扫出内层真正的 GDB
                    if (folder.toLower().endsWith(QStringLiteral(".gdb"))
                        && !IsGdbWrapperDir(folder))
                        scanned << folder;
                    else
                        CollectGdbDirsRecursive(folder, scanned);
                }
            }
            else
            {
                QStringList nameFilters;
                nameFilters << ("*." + QString::fromStdString(m_strSrcExtension)).toLower()
                            << ("*." + QString::fromStdString(m_strSrcExtension)).toUpper();
                if (m_strSrcExtension == "geojson")
                    nameFilters << "*.json" << "*.JSON";
                for (const QString& folder : folderList)
                    CollectFilesRecursive(folder, nameFilters, scanned);
            }

            // 跳过已被勾选文件夹覆盖的文件，避免重复转换
            QSet<QString> coveredDirs;
            for (const QString& folder : folderList)
                coveredDirs.insert(CanonicalPath(folder));
            QStringList allPaths = scanned;
            for (const QString& f : pickedFiles)
            {
                const QString parentDir = CanonicalPath(QFileInfo(f).absolutePath());
                bool bCovered = false;
                for (const QString& d : coveredDirs)
                {
                    if (parentDir == d || parentDir.startsWith(d + QLatin1Char('/')))
                    {
                        bCovered = true;
                        break;
                    }
                }
                if (!bCovered) allPaths << f;
            }

            QSet<QString> seen;
            for (const QString& f : allPaths)
            {
                const QString canon = CanonicalPath(f);
                if (seen.contains(canon)) continue;
                seen.insert(canon);
                srcFileList.push_back(f.toUtf8().constData());
            }
        }
        else if (m_bSingleInputFile)
        {
            srcFileList.push_back(m_strInputPath);
        }
        else
        {
            if (m_strSrcExtension == "gdb")
            {
                // GDB 是目录型数据：批量模式扫描目录下的 *.gdb 子目录
                QDir dir(QString::fromUtf8(m_strInputPath.c_str()));
                QStringList dirList = dir.entryList(
                    QStringList() << "*.gdb" << "*.GDB",
                    QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                for (const QString& d : dirList)
                    srcFileList.push_back((m_strInputPath + "/" + d.toStdString()));
            }
            else
            {
                QStringList nameFilters;
                nameFilters << ("*." + QString::fromStdString(m_strSrcExtension)).toLower()
                            << ("*." + QString::fromStdString(m_strSrcExtension)).toUpper();
                if (m_strSrcExtension == "geojson")
                {
                    nameFilters << "*.json" << "*.JSON";
                }
                QStringList fileList = GetFileNames(
                    QString::fromUtf8(m_strInputPath.c_str()), nameFilters);

                for (const QString& f : fileList)
                    srcFileList.push_back((m_strInputPath + "/" + f.toStdString()));
            }

            if (srcFileList.empty())
            {
                QFileInfo fi(QString::fromUtf8(m_strInputPath.c_str()));
                if (fi.isFile())
                    srcFileList.push_back(m_strInputPath);
            }
        }

        // PGeo(ACE ODBC) 打不开含中文的路径/文件名：复制到纯 ASCII 临时目录再转换
        // 同时保留原始路径列表：图层名前缀仍用原始文件名，避免出现临时文件名
        vector<string> srcOrigList;
        if (m_strSrcDriverName == "PGeo")
        {
            srcOrigList = srcFileList;
            for (size_t i = 0; i < srcFileList.size(); i++)
            {
                const QString src = QString::fromUtf8(srcFileList[i].c_str());
                bool bHasNonAscii = false;
                for (const QChar& c : src)
                {
                    if (c.unicode() > 0x7F) { bHasNonAscii = true; break; }
                }
                if (!bHasNonAscii) continue;

                const QString tmpDir = QDir::tempPath() + QStringLiteral("/mdb_conv");
                QDir().mkpath(tmpDir);
                const QString tmpPath = tmpDir + QLatin1Char('/')
                    + QStringLiteral("mdb_conv_%1_%2.mdb")
                          .arg(i).arg(QDateTime::currentMSecsSinceEpoch());
                if (QFile::copy(src, tmpPath))
                {
                    tmpMdbFiles << tmpPath;
                    const QByteArray tmpUtf8 = tmpPath.toUtf8();
                    srcFileList[i] = tmpUtf8.constData();
                    snprintf(szLog, sizeof(szLog),
                        "源路径含中文，PGeo 无法直接读取，已复制到临时文件: %s",
                        tmpUtf8.constData());
                    file_logger->info(szLog);
                }
                else
                {
                    snprintf(szLog, sizeof(szLog),
                        "源路径含中文且复制到临时目录失败: %s",
                        tmpPath.toUtf8().constData());
                    file_logger->error(szLog);
                }
            }
        }

        totalCount = (int)srcFileList.size();
        snprintf(szLog, sizeof(szLog), "共 %d 个源文件待转换", totalCount);
        file_logger->info(szLog);
        file_logger->flush();

        if (totalCount == 0)
        {
            file_logger->error("未找到可转换的源文件");
            break;
        }

        // 用 GDAL VSIMkdir 创建输出目录
        // 单个 GPKG 输出时 m_strOutputPath 是完整文件路径，目录取其所在文件夹
        string strOutDir = m_strOutputPath;
        if (bGpkgTarget && m_bSingleInputFile)
            strOutDir = string(CPLGetPath(m_strOutputPath.c_str()));
        if (VSIMkdir(strOutDir.c_str(), 0777) != 0)
        {
            // VSIMkdir 可能因目录已存在返回非0，用 QDir 兜底
            QDir().mkpath(QString::fromUtf8(strOutDir.c_str()));
        }

        // 批量 GDB：循环前只创建一个目标库，各源文件逐个作为要素类汇入
        if (bGdbTarget && !m_bSingleInputFile)
        {
            batchGdbName = m_strGdbName;
            if (batchGdbName.empty())
                batchGdbName = "batch_convert";
            string gdbPath = strOutDir + "/" + batchGdbName + ".gdb";
            QDir gdbDir(QString::fromUtf8(gdbPath.c_str()));
            if (gdbDir.exists())
                gdbDir.removeRecursively();
            GDALDriver* poGdbDrv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
            if (!poGdbDrv)
                file_logger->error("GDAL 未找到 OpenFileGDB 驱动");
            else
                poBatchGdb = poGdbDrv->Create(gdbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
            if (!poBatchGdb)
                file_logger->error("创建目标库失败: " + gdbPath);
            else
                file_logger->info("目标库: " + gdbPath);
            file_logger->flush();
        }

        for (int i = 0; i < totalCount; i++)
        {
            if (isCanceled())
            {
                file_logger->warn("任务被用户取消");
                break;
            }

            const string& srcFile = srcFileList[i];
            string srcBaseName = CPLGetBasename(srcFile.c_str());
            // PGeo 中文路径临时复制后文件名变成 ASCII 临时名，输出命名改用原始文件名
            if (m_strSrcDriverName == "PGeo" && i < (int)srcOrigList.size())
                srcBaseName = CPLGetBasename(srcOrigList[i].c_str());

            m_strCopyError.clear();
            bool bConvOk = false;
            string outDesc;
            string renameNote;
            // 文件内进度：CopyLayer 按已处理要素数回报 0~1，叠加到"第 i 个文件"粒度
            std::function<void(double)> progCb = [this, i, totalCount](double frac) {
                setProgress((i + frac) * 100.0 / totalCount);
            };
            if (bGdbTarget && !m_bSingleInputFile)
            {
                // 批量：追加进同一个 GDB，每个源文件一个要素类
                if (!poBatchGdb)
                {
                    bConvOk = false;
                    outDesc = batchGdbName + ".gdb";
                }
                else
                {
                    bConvOk = CopyFileToGDB(srcFile, poBatchGdb, m_strSrcDriverName, renameNote, progCb);
                    outDesc = batchGdbName + ".gdb";
                }
            }
            else if (bGdbTarget)
            {
                string gdbName = (m_bSingleInputFile && !m_strGdbName.empty())
                    ? m_strGdbName : srcBaseName;
                string gdbPath = strOutDir + "/" + gdbName + ".gdb";
                bConvOk = ConvertToGDB(srcFile, gdbPath, m_strSrcDriverName, progCb);
                outDesc = gdbName + ".gdb";
            }
            else if (bGpkgTarget)
            {
                string outFile = m_bSingleInputFile
                    ? m_strOutputPath
                    : strOutDir + "/" + srcBaseName + ".gpkg";
                // 图层范围在拷贝时顺手累计、写库前直接刷 gpkg_contents，避免二次全表扫描
                bConvOk = ConvertToGPKG(srcFile, outFile, m_strSrcDriverName, progCb);
                outDesc = string(CPLGetFilename(outFile.c_str()));
            }
            else
            {
                // 学长口径（2026-09-10）：输出文件名 = 图层名，不拼 gdb 文件夹名前缀；
                // 重名（磁盘已有/本次已输出）时加源库名区分。baseName 仅单文件
                // 模式手填层名时传入，否则空串走自动命名。
                string shpBaseName = (m_bSingleInputFile && !m_strLayerName.empty())
                    ? m_strLayerName : "";
                // 每层 .cpg 由 GDAL 按 ENCODING 图层创建选项自动生成，无需手动补写
                // （手动按基名补写会在多图层源时留下无对应 .shp 的孤立 .cpg）
                bConvOk = ConvertToSHP(srcFile, strOutDir, shpBaseName, srcBaseName,
                                        m_strSrcDriverName, progCb, &outDesc, &renameNote);
            }

            if (bConvOk)
            {
                successCount++;
                snprintf(szLog, sizeof(szLog), "[%d/%d] %s -> %s 转换成功", i + 1, totalCount, srcBaseName.c_str(), outDesc.c_str());
                file_logger->info(szLog);
                if (!renameNote.empty())
                {
                    snprintf(szLog, sizeof(szLog), "  %s", renameNote.c_str());
                    file_logger->warn(szLog);
                }
            }
            else
            {
                m_failedNames << QString::fromUtf8(srcBaseName.c_str());
                string errDetail = m_strCopyError.empty()
                    ? string(CPLGetLastErrorMsg())
                    : m_strCopyError;
                snprintf(szLog, sizeof(szLog), "[%d/%d] %s 转换失败! %s", i + 1, totalCount, srcBaseName.c_str(), errDetail.c_str());
                file_logger->error(szLog);
            }

            file_logger->flush();
            setProgress((i + 1) * 100.0 / totalCount);
        }

        m_successCount = successCount;
        m_totalCount = totalCount;

        if (poBatchGdb)
        {
            GDALClose(poBatchGdb);
            poBatchGdb = nullptr;
        }

        snprintf(szLog, sizeof(szLog), "格式转换完毕! 成功:%d/%d", successCount, totalCount);
        file_logger->info(szLog);
        bOk = (successCount > 0 && !isCanceled());

    } while (false);

    // 清理 PGeo 中文路径的临时复制文件
    for (const QString& f : tmpMdbFiles)
        QFile::remove(f);
    if (!tmpMdbFiles.isEmpty())
        QDir().rmdir(QDir::tempPath() + QStringLiteral("/mdb_conv"));

    file_logger->flush();
    spdlog::drop(strLoggerName);
    file_logger.reset();
    // Write log in GBK (system default encoding on Chinese Windows, no BOM needed)
    {
        QFile file(QString::fromStdString(strLogFileFullPath));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::string utf8Content = logStream->str();
            QByteArray gbkBytes = QString::fromUtf8(utf8Content.c_str()).toLocal8Bit();
            file.write(gbkBytes);
            file.close();
        }
    }

    mProgress = 100;
    setProgress(mProgress);
    if (isCanceled()) return false;
    return bOk;
}

// ==========================================================
// ConvertToSHP: 源文件 → 输出目录下的 SHP 文件
// ==========================================================
bool SeFormatConvertTask::ConvertToSHP(const std::string& srcFile,
                                        const std::string& tgtDir,
                                        const std::string& baseName,
                                        const std::string& srcBaseName,
                                        const std::string& srcDriver,
                                        const std::function<void(double)>& progCb,
                                        std::string* pOutDesc,
                                        std::string* pRenameNote)
{
    GDALDataset* poSrcDS = OpenSourceDataset(srcFile, srcDriver);
    if (!poSrcDS)
    {
        const char* pszErr = CPLGetLastErrorMsg();
        m_strCopyError = "无法打开源数据";
        if (pszErr && pszErr[0])
        {
            m_strCopyError += "：";
            m_strCopyError += pszErr;
        }
        return false;
    }

    GDALDriver* poShpDrv = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poShpDrv) { GDALClose(poSrcDS); return false; }

    const bool bLocal = (srcDriver == "PGeo");
    bool bSuccess = true;
    int layerCount = poSrcDS->GetLayerCount();

    // 命名规则（学长口径）：输出名 = 图层名，不拼源库/文件夹前缀；
    // 目标名被占用（磁盘已有或本次已输出）时改加源库名区分，
    // 库名+图层名仍被占用则 _2/_3 兜底。Windows 文件名不区分大小写。
    auto toLowerName = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        return r;
    };
    std::set<std::string> usedNames;
    {
        QDir dir(QString::fromUtf8(tgtDir.c_str()));
        const QStringList entries = dir.entryList(QStringList() << "*.shp", QDir::Files | QDir::Readable);
        for (const QString& f : entries)
            usedNames.insert(toLowerName(QFileInfo(f).completeBaseName().toStdString()));
    }
    std::vector<std::string> writtenNames;

    for (int iLayer = 0; iLayer < layerCount; iLayer++)
    {
        OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
        if (!poSrcLayer) { bSuccess = false; continue; }

        const string layerName = DecodeGdalTextUtf8(poSrcLayer->GetName(), bLocal);
        // 自动命名（baseName 空）→ 图层名；显式命名（单文件手填层名）→ 层名[+_图层名]
        string desired = baseName.empty()
            ? layerName
            : (layerCount > 1 ? baseName + "_" + layerName : baseName);
        string outName = desired;
        if (usedNames.count(toLowerName(outName)))
        {
            outName = srcBaseName + "_" + layerName;
            int n = 2;
            while (usedNames.count(toLowerName(outName)))
                outName = srcBaseName + "_" + layerName + "_" + std::to_string(n++);
            if (pRenameNote)
            {
                *pRenameNote += desired + ".shp 已存在，改存为 " + outName + ".shp";
                *pRenameNote += "\n";
            }
        }
        usedNames.insert(toLowerName(outName));

        string outFile = tgtDir + "/" + outName + ".shp";

        OGRSpatialReference* poSRS = poSrcLayer->GetSpatialRef();
        // Flatten to 2D – SHP format does not support Z/M
        OGRwkbGeometryType eGeomType = wkbFlatten(poSrcLayer->GetGeomType());

        GDALDataset* poOutDS = poShpDrv->Create(outFile.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!poOutDS) { bSuccess = false; continue; }

        char** lco = nullptr;
        lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
        OGRLayer* poOutLayer = poOutDS->CreateLayer(outName.c_str(), poSRS, eGeomType, lco);
        CSLDestroy(lco);
        if (!poOutLayer) { GDALClose(poOutDS); bSuccess = false; continue; }
        writtenNames.push_back(outName);

        std::function<void(double)> layerCb;
        if (progCb)
            layerCb = [=](double frac) { progCb((iLayer + frac) / layerCount); };
        if (!CopyLayer(poSrcLayer, poOutLayer, srcDriver, layerCb))
            bSuccess = false;

        GDALClose(poOutDS);

        // Overwrite .prj with explicit ESRI WKT for ArcGIS compatibility
        if (poSRS) {
            OGRSpatialReference oTmpSRS(*poSRS);
            oTmpSRS.morphToESRI();
            char* pszWkt = nullptr;
            if (oTmpSRS.exportToWkt(&pszWkt) == OGRERR_NONE) {
                std::string prjPath = tgtDir + "/" + outName + ".prj";
                QFile f(QString::fromUtf8(prjPath.c_str()));
                if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    f.write(pszWkt);
                    f.close();
                }
                CPLFree(pszWkt);
            }
        }
    }

    if (pOutDesc)
    {
        if (writtenNames.size() == 1)
        {
            *pOutDesc = writtenNames[0] + ".shp";
        }
        else
        {
            std::string s;
            const int show = (int)writtenNames.size() > 3 ? 3 : (int)writtenNames.size();
            for (int i = 0; i < show; i++)
            {
                if (i) s += "、";
                s += writtenNames[i] + ".shp";
            }
            if ((int)writtenNames.size() > show)
                s += " 等 " + std::to_string(writtenNames.size()) + " 个文件";
            *pOutDesc = s;
        }
    }

    GDALClose(poSrcDS);
    return bSuccess;
}

// ==========================================================
// ConvertToGPKG: 源文件 → 独立的 <名称>.gpkg，图层名直接迁移（ArcGIS 转换习惯）
// ==========================================================
bool SeFormatConvertTask::ConvertToGPKG(const std::string& srcFile,
                                         const std::string& outFile,
                                         const std::string& srcDriver,
                                         const std::function<void(double)>& progCb)
{
    GDALDataset* poSrcDS = OpenSourceDataset(srcFile, srcDriver);
    if (!poSrcDS)
    {
        const char* pszErr = CPLGetLastErrorMsg();
        m_strCopyError = "无法打开源数据";
        if (pszErr && pszErr[0])
        {
            m_strCopyError += "：";
            m_strCopyError += pszErr;
        }
        return false;
    }

    GDALDriver* poGpkgDrv = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (!poGpkgDrv) { GDALClose(poSrcDS); return false; }

    // 已存在则删除重建，避免残留旧图层
    VSIUnlink(outFile.c_str());

    GDALDataset* poTgtDS = poGpkgDrv->Create(outFile.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poTgtDS) { GDALClose(poSrcDS); return false; }

    const bool bLocal = (srcDriver == "PGeo");
    const int nLayers = poSrcDS->GetLayerCount();
    // 拷贝时顺手累计每层外包范围，写完直接刷 gpkg_contents，
    // 免去旧方案"重开文件 + SQL 全表再扫一遍"的第二次全量读取
    std::vector<OGREnvelope> envs(nLayers);
    std::vector<std::string> layerNames(nLayers);
    bool bSuccess = true;
    for (int iLayer = 0; iLayer < nLayers; iLayer++)
    {
        OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
        if (!poSrcLayer) { bSuccess = false; continue; }

        const string layerName = DecodeGdalTextUtf8(poSrcLayer->GetName(), bLocal);
        if (layerName.empty()) { bSuccess = false; continue; }
        layerNames[iLayer] = layerName;

        OGRSpatialReference* poSRS = poSrcLayer->GetSpatialRef();
        OGRwkbGeometryType eGeomType = poSrcLayer->GetGeomType();

        char** lco = nullptr;
        lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
        OGRLayer* poTgtLayer = poTgtDS->CreateLayer(layerName.c_str(), poSRS, eGeomType, lco);
        CSLDestroy(lco);
        if (!poTgtLayer) { bSuccess = false; continue; }

        std::function<void(double)> layerCb;
        if (progCb)
            layerCb = [=](double frac) { progCb((iLayer + frac) / nLayers); };
        if (!CopyLayer(poSrcLayer, poTgtLayer, srcDriver, layerCb, &envs[iLayer]))
            bSuccess = false;
    }

    // 刷 gpkg_contents（GDAL 2.x 无 SetExtent）
    for (int iLayer = 0; iLayer < nLayers; iLayer++)
    {
        const OGREnvelope& env = envs[iLayer];
        if (!env.IsInit() || layerNames[iLayer].empty()) continue;
        char szSql[512] = { 0 };
        snprintf(szSql, sizeof(szSql),
            "UPDATE gpkg_contents SET min_x=%.10f, min_y=%.10f, "
            "max_x=%.10f, max_y=%.10f WHERE table_name='%s'",
            env.MinX, env.MinY, env.MaxX, env.MaxY, layerNames[iLayer].c_str());
        poTgtDS->ExecuteSQL(szSql, nullptr, nullptr);
    }

    GDALClose(poTgtDS);
    GDALClose(poSrcDS);
    return bSuccess;
}

// ==========================================================
// ConvertToGDB: 源文件 → 独立的 <名称>.gdb，图层名直接迁移（ArcGIS 转换习惯）
// ==========================================================
bool SeFormatConvertTask::ConvertToGDB(const std::string& srcFile,
                                        const std::string& gdbPath,
                                        const std::string& srcDriver,
                                        const std::function<void(double)>& progCb)
{
    GDALDataset* poSrcDS = OpenSourceDataset(srcFile, srcDriver);
    if (!poSrcDS)
    {
        const char* pszErr = CPLGetLastErrorMsg();
        m_strCopyError = "无法打开源数据";
        if (pszErr && pszErr[0])
        {
            m_strCopyError += "：";
            m_strCopyError += pszErr;
        }
        return false;
    }

    GDALDriver* poGdbDrv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!poGdbDrv) { GDALClose(poSrcDS); return false; }

    // 已存在则删除重建
    QDir gdbDir(QString::fromUtf8(gdbPath.c_str()));
    if (gdbDir.exists())
        gdbDir.removeRecursively();

    GDALDataset* poTgtDS = poGdbDrv->Create(gdbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poTgtDS) { GDALClose(poSrcDS); return false; }

    const bool bLocal = (srcDriver == "PGeo");
    const int nLayers = poSrcDS->GetLayerCount();
    bool bSuccess = true;
    for (int iLayer = 0; iLayer < nLayers; iLayer++)
    {
        OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
        if (!poSrcLayer) { bSuccess = false; continue; }

        const string layerName = DecodeGdalTextUtf8(poSrcLayer->GetName(), bLocal);
        if (layerName.empty()) { bSuccess = false; continue; }

        OGRSpatialReference* poSRS = poSrcLayer->GetSpatialRef();
        OGRwkbGeometryType eGeomType = poSrcLayer->GetGeomType();

        char** lco = nullptr;
        lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
        OGRLayer* poTgtLayer = poTgtDS->CreateLayer(layerName.c_str(), poSRS, eGeomType, lco);
        CSLDestroy(lco);
        if (!poTgtLayer) { bSuccess = false; continue; }

        std::function<void(double)> layerCb;
        if (progCb)
            layerCb = [=](double frac) { progCb((iLayer + frac) / nLayers); };
        if (!CopyLayer(poSrcLayer, poTgtLayer, srcDriver, layerCb))
            bSuccess = false;
    }

    GDALClose(poTgtDS);
    GDALClose(poSrcDS);
    return bSuccess;
}

// ==========================================================
// CopyFileToGDB: 批量模式 — 源文件的图层作为要素类追加进已打开的 GDB
// 要素类名 = 源图层名；与库中已有图层重名时自动加 _2/_3 后缀（renameNote 记录改名）
// ==========================================================
bool SeFormatConvertTask::CopyFileToGDB(const std::string& srcFile,
                                         GDALDataset* poTgtDS,
                                         const std::string& srcDriver,
                                         std::string& renameNote,
                                         const std::function<void(double)>& progCb)
{
    renameNote.clear();
    if (!poTgtDS) return false;

    GDALDataset* poSrcDS = OpenSourceDataset(srcFile, srcDriver);
    if (!poSrcDS)
    {
        const char* pszErr = CPLGetLastErrorMsg();
        m_strCopyError = "无法打开源数据";
        if (pszErr && pszErr[0])
        {
            m_strCopyError += "：";
            m_strCopyError += pszErr;
        }
        return false;
    }

    // 库中已有要素类名，用于重名检测
    std::set<std::string> existing;
    for (int i = 0; i < poTgtDS->GetLayerCount(); i++)
    {
        OGRLayer* l = poTgtDS->GetLayer(i);
        if (l && l->GetName()) existing.insert(l->GetName());
    }

    const bool bLocal = (srcDriver == "PGeo");
    const int nLayers = poSrcDS->GetLayerCount();
    bool bSuccess = true;
    for (int iLayer = 0; iLayer < nLayers; iLayer++)
    {
        OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
        if (!poSrcLayer) { bSuccess = false; continue; }

        string layerName = DecodeGdalTextUtf8(poSrcLayer->GetName(), bLocal);
        if (layerName.empty()) { bSuccess = false; continue; }

        string uniqueName = layerName;
        int suffix = 2;
        while (existing.count(uniqueName))
            uniqueName = layerName + "_" + std::to_string(suffix++);
        if (uniqueName != layerName)
        {
            if (!renameNote.empty()) renameNote += ", ";
            renameNote += "图层 " + layerName + " 重名，改存为 " + uniqueName;
        }
        existing.insert(uniqueName);

        OGRSpatialReference* poSRS = poSrcLayer->GetSpatialRef();
        OGRwkbGeometryType eGeomType = poSrcLayer->GetGeomType();

        char** lco = nullptr;
        lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
        OGRLayer* poTgtLayer = poTgtDS->CreateLayer(uniqueName.c_str(), poSRS, eGeomType, lco);
        CSLDestroy(lco);
        if (!poTgtLayer) { bSuccess = false; continue; }

        std::function<void(double)> layerCb;
        if (progCb)
            layerCb = [=](double frac) { progCb((iLayer + frac) / nLayers); };
        if (!CopyLayer(poSrcLayer, poTgtLayer, srcDriver, layerCb))
            bSuccess = false;
    }

    GDALClose(poSrcDS);
    return bSuccess;
}

// ==========================================================
// CopyLayer: 复制字段定义 + 全部要素
// srcDriver == "PGeo" 时字段名与字符串值一律按 GBK 解码（ANSII ODBC 返回本地编码）
// progCb：按已处理要素数回报文件内进度 0~1（每 1/200 粒度，避免刷屏）
// pEnv：非空时顺手累计要素外包范围（GPKG 用它免二次全表扫描）
// ==========================================================
bool SeFormatConvertTask::CopyLayer(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer,
                                     const std::string& srcDriver,
                                     const std::function<void(double)>& progCb,
                                     OGREnvelope* pEnv)
{
    if (!poSrcLayer || !poTgtLayer) return false;

    const bool bLocalText = (srcDriver == "PGeo");
    OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
    int nSrcFields = poSrcDefn->GetFieldCount();

    std::vector<int> validSrcIdx;
    for (int i = 0; i < nSrcFields; i++)
    {
        OGRFieldDefn* poSrcField = poSrcDefn->GetFieldDefn(i);
        const char* szName = poSrcField->GetNameRef();
        if (szName != nullptr && strlen(szName) > 0)
        {
            const std::string utf8Name = DecodeGdalTextUtf8(szName, bLocalText);
            if (utf8Name == szName)
            {
                poTgtLayer->CreateField(poSrcField);
            }
            else
            {
                OGRFieldDefn oNewField(poSrcField);
                oNewField.SetName(utf8Name.c_str());
                poTgtLayer->CreateField(&oNewField);
            }
        }
        else
        {
            OGRFieldDefn oNewField(("field_" + std::to_string(i)).c_str(), poSrcField->GetType());
            oNewField.SetWidth(poSrcField->GetWidth());
            oNewField.SetPrecision(poSrcField->GetPrecision());
            poTgtLayer->CreateField(&oNewField);
        }
        validSrcIdx.push_back(i);
    }

    const GIntBig nTotal = poSrcLayer->GetFeatureCount();
    const GIntBig nStep = (nTotal > 0) ? std::max<GIntBig>(1, nTotal / 200) : 0;

    // 事务包裹：SQLite 系目标（GPKG）默认 synchronous=FULL，自动提交模式下每要素一次 fsync，
    // 慢盘上单要素可达十余毫秒；事务内全部写完后只提交一次
    bool bUseTxn = poTgtLayer->TestCapability(OLCTransactions);
    if (bUseTxn && poTgtLayer->StartTransaction() != OGRERR_NONE)
        bUseTxn = false;

    m_strCopyError.clear();
    poSrcLayer->ResetReading();
    OGRFeature* poFeature;
    GIntBig iFeature = 0;
    int iFailed = 0;
    while ((poFeature = poSrcLayer->GetNextFeature()) != nullptr)
    {
        iFeature++;
        OGRFeature* poTgtFeature = OGRFeature::CreateFeature(poTgtLayer->GetLayerDefn());
        poTgtFeature->SetGeometry(poFeature->GetGeometryRef());
        if (pEnv)
        {
            OGRGeometry* pGeom = poFeature->GetGeometryRef();
            if (pGeom)
            {
                OGREnvelope env;
                pGeom->getEnvelope(&env);
                if (env.IsInit())
                {
                    if (!pEnv->IsInit()) *pEnv = env;
                    else pEnv->Merge(env);
                }
            }
        }
        for (int t = 0; t < (int)validSrcIdx.size(); t++)
        {
            int srcIdx = validSrcIdx[t];
            if (poFeature->IsFieldSet(srcIdx))
            {
                OGRFieldDefn* poFieldDefn = poSrcDefn->GetFieldDefn(srcIdx);
                if (poFieldDefn && poFieldDefn->GetType() == OFTString)
                {
                    // 字符串值按源编码解码成 UTF-8（PGeo 为 GBK，其他驱动已是 UTF-8 原样通过）
                    const char* pszVal = poFeature->GetFieldAsString(srcIdx);
                    const std::string utf8Val = DecodeGdalTextUtf8(pszVal, bLocalText);
                    poTgtFeature->SetField(t, utf8Val.c_str());
                }
                else
                {
                    poTgtFeature->SetField(t, poFeature->GetRawFieldRef(srcIdx));
                }
            }
        }
        OGRErr eErr = poTgtLayer->CreateFeature(poTgtFeature);
        if (eErr != OGRERR_NONE)
        {
            iFailed++;
            if (!m_strCopyError.empty()) m_strCopyError += ", ";
            QString qGdalErr = QString::fromLocal8Bit(CPLGetLastErrorMsg());
            m_strCopyError += "FID=" + std::to_string((long long)poFeature->GetFID())
                + "(" + qGdalErr.toStdString() + ")";
        }
        OGRFeature::DestroyFeature(poTgtFeature);
        OGRFeature::DestroyFeature(poFeature);
        if (progCb && nStep > 0 && (iFeature % nStep == 0))
            progCb((double)iFeature / (double)nTotal);
    }
    if (bUseTxn)
        poTgtLayer->CommitTransaction();
    if (progCb)
        progCb(1.0);
    if (iFailed > 0)
    {
        m_strCopyError = std::to_string(iFailed) + "/" + std::to_string((long long)iFeature)
            + " features failed: " + m_strCopyError;
        return false;
    }
    return true;
}

QStringList SeFormatConvertTask::GetFileNames(const QString& path, const QStringList& nameFilters)
{
    QDir dir(path);
    return dir.entryList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
}

bool SeFormatConvertTask::isCanceled() { return mCanceled; }
void SeFormatConvertTask::cancel()     { mCanceled = true; }
int  SeFormatConvertTask::progress() const { return mProgress; }
void SeFormatConvertTask::finished(bool result) { emit taskFinished(result); }
