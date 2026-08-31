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
#include <sstream>
#include <memory>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/ostream_sink.h"

static void CollectGdbDirsRecursive(const QString& path, QStringList& out)
{
    QDirIterator it(path, QStringList() << "*.gdb" << "*.GDB",
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
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

// PGeo 经 ANSI ODBC 接口返回本地编码（中文 Windows 为 GBK）字节。
// 严格 UTF-8 校验：合法 UTF-8 原样返回；含多字节但不是合法 UTF-8 的按本地编码(GBK)解码。
// 与 merge/接边功能里处理字段名的逻辑一致。
static bool IsValidUtf8(const QByteArray& raw, bool* hasMb)
{
    *hasMb = false;
    int j = 0;
    while (j < raw.size()) {
        unsigned char c = raw[j];
        int len; unsigned int minCp;
        if (c < 0x80)           { len = 1; minCp = 0; }
        else if ((c & 0xE0) == 0xC0) { len = 2; minCp = 0x80; *hasMb = true; }
        else if ((c & 0xF0) == 0xE0) { len = 3; minCp = 0x800; *hasMb = true; }
        else if ((c & 0xF8) == 0xF0) { len = 4; minCp = 0x10000; *hasMb = true; }
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
static std::string DecodeGdalTextUtf8(const char* sz, bool bForceLocal = false)
{
    if (!sz || !*sz) return std::string();
    const QByteArray raw(sz);
    if (bForceLocal)
        return QString::fromLocal8Bit(raw).toUtf8().toStdString();
    bool hasMb = false;
    if (IsValidUtf8(raw, &hasMb))
        return std::string(sz);
    if (hasMb)
        return QString::fromLocal8Bit(raw).toUtf8().toStdString();
    return std::string(sz);
}

// GPKG 图层外包范围刷到 gpkg_contents（GDAL 2.x 无 SetExtent，改用 GPKG SQL 空间函数）
static void UpdateGpkgExtents(GDALDataset* poDS, const std::shared_ptr<spdlog::logger>& logger)
{
    if (!poDS) return;
    char szLog[1000] = { 0 };
    for (int i = 0; i < poDS->GetLayerCount(); i++)
    {
        OGRLayer* pLayer = poDS->GetLayer(i);
        if (!pLayer) continue;

        const char* pszTable = pLayer->GetName();
        const char* pszGeom  = pLayer->GetGeometryColumn();
        if (!pszTable || !pszGeom) continue;

        snprintf(szLog, sizeof(szLog),
            "SELECT MIN(ST_MinX(\"%s\")), MAX(ST_MaxX(\"%s\")), "
            "MIN(ST_MinY(\"%s\")), MAX(ST_MaxY(\"%s\")) FROM \"%s\"",
            pszGeom, pszGeom, pszGeom, pszGeom, pszTable);

        OGRLayer* pResult = poDS->ExecuteSQL(szLog, nullptr, nullptr);
        if (pResult)
        {
            OGRFeature* pFeat = pResult->GetNextFeature();
            if (pFeat && !pFeat->IsFieldNull(0))
            {
                double minX = pFeat->GetFieldAsDouble(0);
                double maxX = pFeat->GetFieldAsDouble(1);
                double minY = pFeat->GetFieldAsDouble(2);
                double maxY = pFeat->GetFieldAsDouble(3);

                poDS->ReleaseResultSet(pResult);
                pResult = nullptr;

                snprintf(szLog, sizeof(szLog),
                    "UPDATE gpkg_contents SET min_x=%.10f, min_y=%.10f, "
                    "max_x=%.10f, max_y=%.10f WHERE table_name='%s'",
                    minX, minY, maxX, maxY, pszTable);
                poDS->ExecuteSQL(szLog, nullptr, nullptr);

                snprintf(szLog, sizeof(szLog), "图层 [%s] 范围已刷新: %.3f, %.3f ~ %.3f, %.3f",
                    pszTable, minX, minY, maxX, maxY);
                logger->info(szLog);

                OGRFeature::DestroyFeature(pFeat);
            }
            else
            {
                poDS->ReleaseResultSet(pResult);
                pResult = nullptr;
                if (pFeat) OGRFeature::DestroyFeature(pFeat);
                snprintf(szLog, sizeof(szLog), "图层 [%s] 无要素，跳过范围更新", pszTable);
                logger->info(szLog);
            }
        }
    }
    logger->flush();
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
    // 与 ArcGIS 转换习惯一致：每个源文件 → 各自的目标输出
    // GDB: 输出目录/<源名>.gdb；GPKG: 批量时输出目录/<源名>.gpkg，单个时用户选定的文件
    bool bGdbTarget = (m_strTgtDriverName == "OpenFileGDB");
    bool bGpkgTarget = (m_strTgtDriverName == "GPKG");
    int successCount = 0;
    int totalCount = 0;
    QStringList tmpMdbFiles;

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
                    if (folder.toLower().endsWith(QStringLiteral(".gdb")))
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
            if (bGdbTarget)
            {
                string gdbName = (m_bSingleInputFile && !m_strGdbName.empty())
                    ? m_strGdbName : srcBaseName;
                string gdbPath = strOutDir + "/" + gdbName + ".gdb";
                bConvOk = ConvertToGDB(srcFile, gdbPath, m_strSrcDriverName);
                outDesc = gdbName + ".gdb";
            }
            else if (bGpkgTarget)
            {
                string outFile = m_bSingleInputFile
                    ? m_strOutputPath
                    : strOutDir + "/" + srcBaseName + ".gpkg";
                bConvOk = ConvertToGPKG(srcFile, outFile, m_strSrcDriverName);
                if (bConvOk)
                {
                    GDALDataset* poDs = (GDALDataset*)GDALOpenEx(outFile.c_str(),
                        GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr);
                    if (poDs)
                    {
                        UpdateGpkgExtents(poDs, file_logger);
                        GDALClose(poDs);
                    }
                }
                outDesc = string(CPLGetFilename(outFile.c_str()));
            }
            else
            {
                string shpBaseName = (m_bSingleInputFile && !m_strLayerName.empty())
                    ? m_strLayerName : srcBaseName;
                bConvOk = ConvertToSHP(srcFile, strOutDir, shpBaseName, m_strSrcDriverName);
                if (bConvOk)
                    CreateShapefileCPG(strOutDir + "/" + shpBaseName + ".cpg", "UTF-8");
                outDesc = shpBaseName + ".shp";
            }

            if (bConvOk)
            {
                successCount++;
                snprintf(szLog, sizeof(szLog), "[%d/%d] %s -> %s 转换成功", i + 1, totalCount, srcBaseName.c_str(), outDesc.c_str());
                file_logger->info(szLog);
            }
            else
            {
                string errDetail = m_strCopyError.empty()
                    ? string(CPLGetLastErrorMsg())
                    : m_strCopyError;
                snprintf(szLog, sizeof(szLog), "[%d/%d] %s 转换失败! %s", i + 1, totalCount, srcBaseName.c_str(), errDetail.c_str());
                file_logger->error(szLog);
            }

            file_logger->flush();
            setProgress((i + 1) * 100.0 / totalCount);
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
                                        const std::string& srcDriver)
{
    const char* apszDrivers[2] = { srcDriver.c_str(), nullptr };
    GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(
        srcFile.c_str(), GDAL_OF_VECTOR,
        srcDriver.empty() ? nullptr : apszDrivers,
        nullptr, nullptr);
    if (!poSrcDS) return false;

    GDALDriver* poShpDrv = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poShpDrv) { GDALClose(poSrcDS); return false; }

    const bool bLocal = (srcDriver == "PGeo");
    bool bSuccess = true;
    int layerCount = poSrcDS->GetLayerCount();

    for (int iLayer = 0; iLayer < layerCount; iLayer++)
    {
        OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
        if (!poSrcLayer) { bSuccess = false; continue; }

        // 多图层时，每个图层单独命名
        string outName = baseName;
        if (layerCount > 1)
            outName = baseName + "_" + DecodeGdalTextUtf8(poSrcLayer->GetName(), bLocal);

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

        if (!CopyLayer(poSrcLayer, poOutLayer, srcDriver))
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

    GDALClose(poSrcDS);
    return bSuccess;
}

// ==========================================================
// ConvertToGPKG: 源文件 → 独立的 <名称>.gpkg，图层名直接迁移（ArcGIS 转换习惯）
// ==========================================================
bool SeFormatConvertTask::ConvertToGPKG(const std::string& srcFile,
                                         const std::string& outFile,
                                         const std::string& srcDriver)
{
    const char* apszDrivers[2] = { srcDriver.empty() ? nullptr : srcDriver.c_str(), nullptr };
    GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(
        srcFile.c_str(), GDAL_OF_VECTOR,
        srcDriver.empty() ? nullptr : apszDrivers,
        nullptr, nullptr);
    if (!poSrcDS) return false;

    GDALDriver* poGpkgDrv = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (!poGpkgDrv) { GDALClose(poSrcDS); return false; }

    // 已存在则删除重建，避免残留旧图层
    VSIUnlink(outFile.c_str());

    GDALDataset* poTgtDS = poGpkgDrv->Create(outFile.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poTgtDS) { GDALClose(poSrcDS); return false; }

    const bool bLocal = (srcDriver == "PGeo");
    bool bSuccess = true;
    for (int iLayer = 0; iLayer < poSrcDS->GetLayerCount(); iLayer++)
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

        if (!CopyLayer(poSrcLayer, poTgtLayer, srcDriver))
            bSuccess = false;
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
                                        const std::string& srcDriver)
{
    const char* apszDrivers[2] = { srcDriver.empty() ? nullptr : srcDriver.c_str(), nullptr };
    GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(
        srcFile.c_str(), GDAL_OF_VECTOR,
        srcDriver.empty() ? nullptr : apszDrivers,
        nullptr, nullptr);
    if (!poSrcDS) return false;

    GDALDriver* poGdbDrv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!poGdbDrv) { GDALClose(poSrcDS); return false; }

    // 已存在则删除重建
    QDir gdbDir(QString::fromUtf8(gdbPath.c_str()));
    if (gdbDir.exists())
        gdbDir.removeRecursively();

    GDALDataset* poTgtDS = poGdbDrv->Create(gdbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poTgtDS) { GDALClose(poSrcDS); return false; }

    const bool bLocal = (srcDriver == "PGeo");
    bool bSuccess = true;
    for (int iLayer = 0; iLayer < poSrcDS->GetLayerCount(); iLayer++)
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

        if (!CopyLayer(poSrcLayer, poTgtLayer, srcDriver))
            bSuccess = false;
    }

    GDALClose(poTgtDS);
    GDALClose(poSrcDS);
    return bSuccess;
}

// ==========================================================
// CopyLayer: 复制字段定义 + 全部要素
// srcDriver == "PGeo" 时字段名与字符串值一律按 GBK 解码（ANSII ODBC 返回本地编码）
// ==========================================================
bool SeFormatConvertTask::CopyLayer(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer,
                                     const std::string& srcDriver)
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

    m_strCopyError.clear();
    poSrcLayer->ResetReading();
    OGRFeature* poFeature;
    int iFeature = 0;
    int iFailed = 0;
    while ((poFeature = poSrcLayer->GetNextFeature()) != nullptr)
    {
        iFeature++;
        OGRFeature* poTgtFeature = OGRFeature::CreateFeature(poTgtLayer->GetLayerDefn());
        poTgtFeature->SetGeometry(poFeature->GetGeometryRef());
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
            m_strCopyError += "FID=" + std::to_string(poFeature->GetFID())
                + "(" + qGdalErr.toStdString() + ")";
        }
        OGRFeature::DestroyFeature(poTgtFeature);
        OGRFeature::DestroyFeature(poFeature);
    }
    if (iFailed > 0)
    {
        m_strCopyError = std::to_string(iFailed) + "/" + std::to_string(iFeature)
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

bool SeFormatConvertTask::CreateShapefileCPG(string strCPGFilePath, string strEncoding)
{
    QFile f(QString::fromUtf8(strCPGFilePath.c_str()));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(strEncoding.c_str());
    f.close();
    return true;
}

bool SeFormatConvertTask::isCanceled() { return mCanceled; }
void SeFormatConvertTask::cancel()     { mCanceled = true; }
int  SeFormatConvertTask::progress() const { return mProgress; }
void SeFormatConvertTask::finished(bool result) { emit taskFinished(result); }
