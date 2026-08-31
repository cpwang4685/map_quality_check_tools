#include "se_nmo_sdk_bridge.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QSettings>
#include <QTextStream>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>

#if !defined(SE_NMO_NO_SDK)
#include <MapBatchProcessing/FunctionsProcessing.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

static QString s_logPath;
static QTextStream& log()
{
    static QFile s_file;
    static QTextStream s_ts;
    if (!s_file.isOpen()) {
        s_logPath = QDir::tempPath() + "/nmo_bridge_debug.log";
        s_file.setFileName(s_logPath);
        s_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        s_ts.setDevice(&s_file);
        s_ts << "\n=== " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
    }
    return s_ts;
}

// NMO SDK 目录候选（优先外部配置，兜底插件目录）：
//   1) 环境变量 NMO_SDK_HOME
//   2) QSettings GarMap/MapProductManager 的 nmo/sdkDir
//   3) 可执行文件目录/plugins（/opt/ltzk/plugins 部署形态）
//   Windows 另加旧开发机硬编码路径作为最后兜底
static QStringList nmoSdkCandidateDirs()
{
    QStringList dirs;

    QByteArray env = qgetenv("NMO_SDK_HOME");
    if (!env.isEmpty())
        dirs << QString::fromLocal8Bit(env);

    QSettings settings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
    QString sdkDir = settings.value(QStringLiteral("nmo/sdkDir")).toString();
    if (!sdkDir.isEmpty())
        dirs << sdkDir;

    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    // /opt/ltzk 部署形态：exe 在 bin/，插件与 SDK 在 bin/../plugins
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/../plugins");

#ifdef _WIN32
    dirs << QStringLiteral("D:/GarMap/garmap_release/starmap/plugins");
#endif

    return dirs;
}

static QString nmoSdkDir()
{
    const QStringList dirs = nmoSdkCandidateDirs();
    for (const QString& d : dirs) {
        QString canonical = QDir(d).canonicalPath();
        if (!canonical.isEmpty() && QDir(canonical).exists())
            return canonical;
    }
    return dirs.isEmpty() ? QStringLiteral("/plugins") : dirs.first();
}

static void ensureNmoSearchPath()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

#ifdef _WIN32
    const QString sdkDir = nmoSdkDir();
    SetDllDirectoryW(reinterpret_cast<const wchar_t*>(sdkDir.utf16()));

    const QString dllPath = QDir(sdkDir).filePath(QStringLiteral("MapBatchProcessing.dll"));
    HMODULE hMod = LoadLibraryExW(reinterpret_cast<const wchar_t*>(dllPath.utf16()), NULL, 0);
    if (hMod) {
        log() << "MapBatchProcessing.dll pre-loaded OK\n";
    } else {
        log() << "MapBatchProcessing.dll pre-load FAILED, err=" << GetLastError() << "\n";
    }
#endif
}

bool SeNmoSdkBridge::executeMission(const QString& xmlPath, const QString& dataPath)
{
    log() << "--- executeMission ---\n";
    ensureNmoSearchPath();

    // Dump XML
    {
        QFile f(xmlPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            log() << "XML:\n" << QString::fromUtf8(f.readAll()) << "\n--- end XML ---\n";
            f.close();
        }
    }

    log() << "xmlPath: " << xmlPath << "\n";
    log() << "reletivePath: " << dataPath << "\n";

    // Files BEFORE
    {
        QDir dir(dataPath);
        log() << "Files BEFORE:\n";
        QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : list)
            log() << "  " << fi.fileName() << (fi.isDir() ? "/" : "") << "\n";
    }

#if !defined(SE_NMO_NO_SDK)
    QByteArray xmlBytes = xmlPath.toUtf8();
    QByteArray dataBytes = dataPath.toLocal8Bit();
    dataBytes.resize(4096);

    // missionPara as output buffer for error messages
    QByteArray missionBuf(4096, '\0');

    // SDK needs config files (AppConfigInfo.xml, MachineCode.txt, etc.)
    // relative to the current working directory. Save & restore to avoid
    // side effects on QGIS.
    QString oldDir;
#ifdef _WIN32
    WCHAR wOldDir[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, wOldDir);
    oldDir = QString::fromWCharArray(wOldDir);
    SetCurrentDirectoryW(reinterpret_cast<const wchar_t*>(nmoSdkDir().utf16()));
#else
    oldDir = QDir::currentPath();
    QDir::setCurrent(nmoSdkDir());
#endif
    log() << "Working dir set to plugins folder\n";

    // Use the 6-param overload: pass AppConfigInfo.xml as the appFile.
    // This is what the MapBatchProcessing.exe launcher does internally.
    QByteArray appFileBytes = QDir(nmoSdkDir())
        .filePath(QStringLiteral("AppConfigInfo.xml")).toUtf8();
    log() << "appFile: " << appFileBytes.constData() << "\n";
    log() << "Calling DoXMLFile (with appFile)...\n";
    bool ok = Nmo::MapBatchProcessing::FunctionsProcessing::DoXMLFile(
        appFileBytes.constData(),
        xmlBytes.constData(),
        dataBytes.data(),
        true,
        true,        // outTime
        missionBuf.data());
    log() << "DoXMLFile returned: " << (ok ? "true" : "false") << "\n";

#ifdef _WIN32
    SetCurrentDirectoryW(wOldDir);
#else
    QDir::setCurrent(oldDir);
#endif

    QString missionStr = QString::fromUtf8(missionBuf.constData());
    if (!missionStr.trimmed().isEmpty())
        log() << "missionPara output:\n" << missionStr << "\n";
#endif

    // Files AFTER
    {
        QDir dir(dataPath);
        log() << "Files AFTER:\n";
        QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : list)
            log() << "  " << fi.fileName() << (fi.isDir() ? "/" : "") << "\n";
    }

    log().flush();
#if defined(SE_NMO_NO_SDK)
    log() << "SE_NMO_NO_SDK: executeMission 未调用（NMO SDK 未就绪，对应功能已置灰）\n";
    return false;
#else
    return ok;
#endif
}

QStringList SeNmoSdkBridge::ensureGbkShapefiles(const QStringList& inputPaths, const QString& dataPath)
{
    QStringList result;
    for (const QString& shpPath : inputPaths) {
        bool needsConversion = false;
        bool sourceIsUtf8 = false;

        // 1) Check if .cpg file declares UTF-8 encoding (covers most QGIS exports)
        QFileInfo shpFi(shpPath);
        QString cpgPath = shpFi.absolutePath() + QStringLiteral("/") + shpFi.completeBaseName() + QStringLiteral(".cpg");
        QFile cpgFile(cpgPath);
        if (cpgFile.open(QIODevice::ReadOnly)) {
            QByteArray cpg = cpgFile.readAll().trimmed().toUpper();
            if (cpg == "UTF-8" || cpg == "UTF8") {
                needsConversion = true;
                sourceIsUtf8 = true;
            }
            cpgFile.close();
        }

        // 2) Fallback: inspect raw field name bytes for valid UTF-8 sequences
        if (!needsConversion) {
            GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
                shpPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
            if (poDS) {
                OGRLayer* poLayer = poDS->GetLayer(0);
                if (poLayer) {
                    OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
                    for (int f = 0; f < poDefn->GetFieldCount() && !needsConversion; ++f) {
                        const char* rawName = poDefn->GetFieldDefn(f)->GetNameRef();
                        QByteArray nameBytes = QByteArray::fromRawData(rawName, (int)strlen(rawName));

                        bool isUtf8 = true;
                        bool hasMultiByte = false;
                        int i = 0;
                        while (i < nameBytes.size() && isUtf8) {
                            unsigned char c = (unsigned char)nameBytes[i];
                            if (c < 0x80) { i++; continue; }
                            hasMultiByte = true;
                            int extra;
                            if      ((c & 0xE0) == 0xC0) extra = 1;
                            else if ((c & 0xF0) == 0xE0) extra = 2;
                            else if ((c & 0xF8) == 0xF0) extra = 3;
                            else { isUtf8 = false; break; }
                            for (int j = 1; j <= extra; ++j) {
                                if (i + j >= nameBytes.size() ||
                                    ((unsigned char)nameBytes[i + j] & 0xC0) != 0x80) {
                                    isUtf8 = false; break;
                                }
                            }
                            i += 1 + extra;
                        }
                        if (hasMultiByte) {
                            needsConversion = true;
                            sourceIsUtf8 = isUtf8;
                        }
                    }
                }
                GDALClose(poDS);
            }
        }

        if (!needsConversion) {
            result.append(shpPath);
            continue;
        }

        // Convert: create GBK temp copy in dataPath
        QFileInfo fi(shpPath);
        QString base = fi.completeBaseName();
        QString tmpPath = dataPath + QStringLiteral("/") + base + QStringLiteral("_gbk.shp");

        // Remove stale temp files first
        QDir tmpDir(dataPath);
        QStringList stale = tmpDir.entryList(
            QStringList() << (base + QStringLiteral("_gbk.*")), QDir::Files);
        for (const QString& s : stale)
            QFile::remove(dataPath + QStringLiteral("/") + s);

        // Use GDAL to re-encode.
        // If source is not UTF-8 (e.g. GBK with LDID=0), force GBK read so GDAL
        // decodes field names correctly before we re-encode them below.
        const char* apszOpenOpts[2] = { nullptr, nullptr };
        if (!sourceIsUtf8) {
            apszOpenOpts[0] = "ENCODING=GBK";
        }
        GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
            shpPath.toUtf8().constData(), GDAL_OF_VECTOR,
            nullptr, apszOpenOpts[0] ? apszOpenOpts : nullptr, nullptr);
        if (!poSrc) {
            result.append(shpPath);
            continue;
        }

        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (!poDriver) {
            GDALClose(poSrc);
            result.append(shpPath);
            continue;
        }

        char* papszOpts[] = { (char*)"ENCODING=GBK", nullptr };
        GDALDataset* poDst = poDriver->Create(
            tmpPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, papszOpts);
        if (!poDst) {
            GDALClose(poSrc);
            result.append(shpPath);
            continue;
        }

        OGRLayer* poSrcLayer = poSrc->GetLayer(0);
        if (poSrcLayer) {
            OGRLayer* poDstLayer = poDst->CreateLayer(
                poSrcLayer->GetName(),
                poSrcLayer->GetSpatialRef(),
                poSrcLayer->GetGeomType(),
                papszOpts);
            if (poDstLayer) {
                // Copy field definitions (names are re-encoded to GBK by the driver)
                OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
                for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f)
                    poDstLayer->CreateField(poSrcDefn->GetFieldDefn(f));

                // Copy features one by one so the driver recodes string
                // values from UTF-8 (GDAL API) to GBK (DBF storage).
                poSrcLayer->ResetReading();
                OGRFeature* poSrcFeat;
                while ((poSrcFeat = poSrcLayer->GetNextFeature()) != nullptr) {
                    OGRFeature* poDstFeat = OGRFeature::CreateFeature(
                        poDstLayer->GetLayerDefn());
                    if (poSrcFeat->GetGeometryRef())
                        poDstFeat->SetGeometry(poSrcFeat->GetGeometryRef());
                    for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f) {
                        if (!poSrcFeat->IsFieldSetAndNotNull(f)) {
                            if (poSrcDefn->GetFieldDefn(f)->GetType() == OFTString)
                                poDstFeat->SetField(f, "");
                            continue;
                        }
                        if (poSrcDefn->GetFieldDefn(f)->GetType() == OFTString)
                            poDstFeat->SetField(f, poSrcFeat->GetFieldAsString(f));
                        else
                            poDstFeat->SetField(f, poSrcFeat->GetRawFieldRef(f));
                    }
                    poDstLayer->CreateFeature(poDstFeat);
                    OGRFeature::DestroyFeature(poDstFeat);
                    OGRFeature::DestroyFeature(poSrcFeat);
                }
            }
        }
        GDALClose(poDst);
        GDALClose(poSrc);

        // Delete .cpg — SDK does not read it
        QFile::remove(dataPath + QStringLiteral("/") + base + QStringLiteral("_gbk.cpg"));

        result.append(tmpPath);
    }
    return result;
}

void SeNmoSdkBridge::cleanupGbkTempFiles(const QStringList& tempPaths)
{
    for (const QString& path : tempPaths) {
        if (!path.contains(QStringLiteral("_gbk.")))
            continue;
        QFileInfo fi(path);
        QString dir = fi.absolutePath();
        QString base = fi.completeBaseName();
        QDir d(dir);
        QStringList files = d.entryList(
            QStringList() << (base + QStringLiteral(".*")), QDir::Files);
        for (const QString& f : files)
            QFile::remove(dir + QStringLiteral("/") + f);
    }
}

void SeNmoSdkBridge::deleteShapefile(const QString& shpPath)
{
    QFileInfo fi(shpPath);
    QString dir = fi.absolutePath();
    QString base = fi.completeBaseName();
    QDir d(dir);
    QStringList files = d.entryList(
        QStringList() << (base + QStringLiteral(".*")), QDir::Files);
    for (const QString& f : files)
        QFile::remove(dir + QStringLiteral("/") + f);
}

// 写 .cpg 编码声明文件（QGIS 优先读它而不是 DBF 的 LDID）。
static void writeCpg(const QString& shpPath, const char* enc)
{
    QFileInfo fi(shpPath);
    QString cpgPath = fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() + QStringLiteral(".cpg");
    QFile cpg(cpgPath);
    if (cpg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cpg.write(enc);
        cpg.close();
    }
}

// 将 DBF 第 29 字节（语言驱动 ID，LDID）写为 0x4D（GBK/CP936）。
// GDAL 用 ENCODING=GBK 创建时只写 .cpg、把 LDID 留成 0x00；ArcGIS 对 .cpg="GBK"
// 识别不可靠，但对 LDID=0x4D 与 .cpg="936" 都能正确按 GBK 解读，这里两者都写上。
static void patchDbfLdidGbk(const QString& shpPath)
{
    QFileInfo fi(shpPath);
    QString dbfPath = fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() + QStringLiteral(".dbf");
    QFile f(dbfPath);
    if (!f.open(QIODevice::ReadWrite))
        return;
    const char ldid = (char)0x4D;
    f.seek(29);
    f.write(&ldid, 1);
    f.close();
}

// 诊断用：把 DBF 的字段描述区与第一条记录的原始字节写入日志，用于排查编码问题。
static void logDbfSample(const QString& shpPath, const char* tag)
{
    QFileInfo fi(shpPath);
    QString dbfPath = fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() + QStringLiteral(".dbf");
    QFile f(dbfPath);
    if (!f.open(QIODevice::ReadOnly)) {
        log() << tag << ": cannot open dbf\n";
        return;
    }
    QByteArray raw = f.readAll();
    f.close();
    int headerLen = (raw.size() >= 10) ? ((unsigned char)raw[8] | ((unsigned char)raw[9] << 8)) : 0;
    int recLen = (raw.size() >= 12) ? ((unsigned char)raw[10] | ((unsigned char)raw[11] << 8)) : 0;
    log() << tag << " dbf size=" << raw.size() << " headerLen=" << headerLen << " recLen=" << recLen << "\n";
    if (headerLen > 32)
        log() << tag << " fields: " << raw.mid(32, qMin(headerLen - 32, 160)).toHex().constData() << "\n";
    if (headerLen > 0 && recLen > 0 && headerLen + recLen <= raw.size())
        log() << tag << " rec0:   " << raw.mid(headerLen, qMin(recLen, 160)).toHex().constData() << "\n";
}

// 统计一段文本中"解码为 CJK 的多字节序列数"与"非 CJK 多字节序列数"。
// GBK 双字节（如 省 = 0xCA 0xA1）按 UTF-8 解会落在 U+0080~U+07FF（拉丁/IPA 等区），
// 真 UTF-8 中文落在 U+4E00 以上（CJK 区）；据此区分二者。
static void countCjk(const unsigned char* data, int len, int& cjk, int& nonCjk)
{
    int i = 0;
    while (i < len) {
        unsigned char c = data[i];
        if (c < 0x80) { i++; continue; }

        int seqLen;
        unsigned int cp;
        if      ((c & 0xE0) == 0xC0) { seqLen = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { seqLen = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { seqLen = 4; cp = c & 0x07; }
        else { nonCjk++; i++; continue; } // 非法 UTF-8 前导

        if (i + seqLen > len) { nonCjk++; i++; continue; }
        bool ok = true;
        for (int j = 1; j < seqLen; ++j) {
            unsigned char cc = data[i + j];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { nonCjk++; i++; continue; }

        if ((cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 统一汉字
            (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK 扩展 A
            (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容汉字
            (cp >= 0x3000 && cp <= 0x303F) ||   // CJK 标点
            (cp >= 0xFF00 && cp <= 0xFFEF))     // 全角字符
            cjk++;
        else
            nonCjk++;
        i += seqLen;
    }
}

// 判断 DBF 字段名是否为真正的 UTF-8。
// 只读文件头里的字段名（纯文本），不扫记录区——大值数值字段（Shape_Area 等
// double 高位字节）是随机二进制，会把统计带偏，导致 UTF-8 被误判成 GBK。
static bool dbfRawIsUtf8(const QString& shpPath)
{
    QFileInfo fi(shpPath);
    QString dbfPath = fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() + QStringLiteral(".dbf");
    QFile f(dbfPath);
    if (!f.open(QIODevice::ReadOnly))
        return true; // 读不到就按 UTF-8 处理
    QByteArray raw = f.readAll();
    f.close();

    // 文件头：字节 8-9 为头长度（小端 uint16）；字段描述符自字节 32 起、每个 32 字节，
    // 每个描述符前 11 字节是字段名（\0 结尾）。
    if (raw.size() < 33)
        return true;
    int headerLen = (unsigned char)raw[8] | ((unsigned char)raw[9] << 8);
    if (headerLen < 33 || headerLen > raw.size())
        return true;
    int nFields = (headerLen - 33) / 32;
    if (nFields < 1)
        return true;

    int cjk = 0, nonCjk = 0;
    for (int k = 0; k < nFields; ++k) {
        int off = 32 + k * 32;
        int nameLen = 0;
        while (nameLen < 11 && (unsigned char)raw[off + nameLen] != 0)
            ++nameLen;
        if (nameLen > 0)
            countCjk((const unsigned char*)raw.constData() + off, nameLen, cjk, nonCjk);
    }

    // 全 ASCII 字段名（cjk==nonCjk==0）按 UTF-8 处理；否则 CJK 占优才判 UTF-8。
    return cjk >= nonCjk;
}

bool SeNmoSdkBridge::normalizeOutputToGbk(const QString& shpPath)
{
    if (!QFileInfo::exists(shpPath))
        return false;

    // SDK 输出编码不稳定：首次可能 GBK、后续 UTF-8。这里统一转成 GBK(CP936)。
    // GBK 是中文 Windows 下 QGIS 与 ArcGIS 都能正确识别的编码；ArcGIS 对 UTF-8
    // shapefile 支持差，且输出 DBF 的 LDID=0x57 会使其按 ANSI/GBK 误解码 UTF-8 字节。
    bool srcIsUtf8 = dbfRawIsUtf8(shpPath);
    log() << "normalizeOutputToGbk: " << shpPath
          << " detected as " << (srcIsUtf8 ? "UTF-8" : "GBK") << "\n";
    logDbfSample(shpPath, "  src");

    QFileInfo fi(shpPath);
    QString dir = fi.absolutePath();
    QString base = fi.completeBaseName();
    QString tmpPath = dir + QStringLiteral("/") + base + QStringLiteral("_cp936.shp");

    QDir d(dir);
    QStringList stale = d.entryList(
        QStringList() << (base + QStringLiteral("_cp936.*")), QDir::Files);
    for (const QString& s : stale)
        QFile::remove(dir + QStringLiteral("/") + s);

    // 手工重编码：SDK 输出没有 .cpg 声明，若不强制编码，GDAL 会按系统 ANSI/GBK
    // 误读 UTF-8 字节（UTF-8 字节被当 GBK 解→写回 GBK 后仍还原成原 UTF-8 字节，
    // 表现为"没有重编码"）。这里按检测到的实际字节编码强制打开源，再用 ENCODING=GBK
    // 新建目标并逐字段/逐要素拷贝，让 shapefile 驱动完成 UTF-8→GBK 重编码。
    // （GDALVectorTranslate 的 "-oo/-lco" 参数形式在进程内调用时，因把源放在选项
    // 位置参数里、目标放在 pszDest 参数里，会被当成"缺源"而报 "hSrcDS == NULL"；
    // 手工 Copy 路径与 ogr2ogr -lco ENCODING=GBK 的行为一致，实测能正确重编码。）
    const char* apszOpenOpts[2] = { nullptr, nullptr };
    apszOpenOpts[0] = srcIsUtf8 ? "ENCODING=UTF-8" : "ENCODING=GBK";

    GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
        shpPath.toUtf8().constData(), GDAL_OF_VECTOR,
        nullptr, apszOpenOpts, nullptr);
    if (!poSrc) {
        log() << "  GDALOpenEx src FAILED: " << CPLGetLastErrorMsg() << "\n";
        return false;
    }

    OGRLayer* poSrcLayer = poSrc->GetLayer(0);
    if (!poSrcLayer) {
        log() << "  no layer in src\n";
        GDALClose(poSrc);
        return false;
    }

    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver) {
        GDALClose(poSrc);
        return false;
    }

    char* papszOpts[] = { (char*)"ENCODING=GBK", nullptr };
    GDALDataset* poDst = poDriver->Create(
        tmpPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, papszOpts);
    if (!poDst) {
        log() << "  Create dst FAILED: " << CPLGetLastErrorMsg() << "\n";
        GDALClose(poSrc);
        return false;
    }

    OGRLayer* poDstLayer = poDst->CreateLayer(
        poSrcLayer->GetName(), poSrcLayer->GetSpatialRef(),
        poSrcLayer->GetGeomType(), papszOpts);
    if (!poDstLayer) {
        log() << "  CreateLayer FAILED: " << CPLGetLastErrorMsg() << "\n";
        GDALClose(poDst);
        GDALClose(poSrc);
        return false;
    }

    OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
    int nFields = poSrcDefn->GetFieldCount();
    for (int f = 0; f < nFields; ++f)
        poDstLayer->CreateField(poSrcDefn->GetFieldDefn(f));

    poSrcLayer->ResetReading();
    OGRFeature* poSrcFeat;
    while ((poSrcFeat = poSrcLayer->GetNextFeature()) != nullptr) {
        OGRFeature* poDstFeat = OGRFeature::CreateFeature(
            poDstLayer->GetLayerDefn());
        if (poSrcFeat->GetGeometryRef())
            poDstFeat->SetGeometry(poSrcFeat->GetGeometryRef());
        for (int f = 0; f < nFields; ++f) {
            if (!poSrcFeat->IsFieldSetAndNotNull(f)) {
                if (poSrcDefn->GetFieldDefn(f)->GetType() == OFTString)
                    poDstFeat->SetField(f, "");
                continue;
            }
            if (poSrcDefn->GetFieldDefn(f)->GetType() == OFTString)
                poDstFeat->SetField(f, poSrcFeat->GetFieldAsString(f));
            else
                poDstFeat->SetField(f, poSrcFeat->GetRawFieldRef(f));
        }
        poDstLayer->CreateFeature(poDstFeat);
        OGRFeature::DestroyFeature(poDstFeat);
        OGRFeature::DestroyFeature(poSrcFeat);
    }

    GDALClose(poDst);
    GDALClose(poSrc);

    // 诊断：重编码产物 _cp936 的原始字节（确认 GDALVectorTranslate 是否真的把
    // UTF-8 转成了 GBK，还是原样拷贝）。
    logDbfSample(tmpPath, "  tmp");

    // 用 GBK 版本替换原文件（.shp/.shx/.dbf/.prj/.cpg）
    deleteShapefile(shpPath);
    log() << "  after deleteShapefile, base.dbf exists="
          << QFileInfo::exists(dir + QStringLiteral("/") + base + QStringLiteral(".dbf")) << "\n";
    QString baseCp936 = base + QStringLiteral("_cp936");
    QStringList files = d.entryList(
        QStringList() << (baseCp936 + QStringLiteral(".*")), QDir::Files);
    for (const QString& f : files) {
        QString suffix = f.mid(baseCp936.length());
        bool ok = QFile::rename(dir + QStringLiteral("/") + f,
                                dir + QStringLiteral("/") + base + suffix);
        log() << "  rename " << f << " -> " << (base + suffix)
              << " ok=" << ok << "\n";
    }

    // 写 .cpg="936"（GDAL 与 ArcGIS 都认的 CP936 代码页号）+ LDID=0x4D（GBK），
    // 二者一致声明 GBK，保证 QGIS/LTZK 与 ArcGIS 都能正确显示中文。
    writeCpg(shpPath, "936");
    patchDbfLdidGbk(shpPath);
    logDbfSample(shpPath, "  dst");
    return true;
}

bool SeNmoSdkBridge::mergeShapefiles(const QStringList& inputPaths, const QString& outputPath)
{
    int n = inputPaths.count();
    if (n < 2) return false;

    // Phase 1: capture schema from first input
    GDALDataset* poFirst = (GDALDataset*)GDALOpenEx(
        inputPaths[0].toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poFirst) return false;

    OGRLayer* poFirstLayer = poFirst->GetLayer(0);
    if (!poFirstLayer) { GDALClose(poFirst); return false; }

    OGRFeatureDefn* poDefn = poFirstLayer->GetLayerDefn();
    OGRSpatialReference* poSRS = poFirstLayer->GetSpatialRef();
    OGRwkbGeometryType eType = poFirstLayer->GetGeomType();
    int nFields = poDefn->GetFieldCount();

    // Phase 2: create output
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver) { GDALClose(poFirst); return false; }

    char* papszOpts[] = { (char*)"ENCODING=GBK", nullptr };
    GDALDataset* poDst = poDriver->Create(
        outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, papszOpts);
    if (!poDst) { GDALClose(poFirst); return false; }

    OGRLayer* poDstLayer = poDst->CreateLayer("merged", poSRS, eType, papszOpts);
    if (!poDstLayer) {
        GDALClose(poDst); GDALClose(poFirst);
        deleteShapefile(outputPath);
        return false;
    }

    // Forward field definitions from first input
    for (int f = 0; f < nFields; ++f)
        poDstLayer->CreateField(poDefn->GetFieldDefn(f));

    GDALClose(poFirst);

    // Phase 3: copy features from all inputs
    for (const QString& inPath : inputPaths) {
        GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
            inPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poSrc) continue;

        OGRLayer* poSrcLayer = poSrc->GetLayer(0);
        if (!poSrcLayer) { GDALClose(poSrc); continue; }

        poSrcLayer->ResetReading();
        OGRFeature* poFeat;
        while ((poFeat = poSrcLayer->GetNextFeature()) != nullptr) {
            OGRFeature* poDstFeat = OGRFeature::CreateFeature(
                poDstLayer->GetLayerDefn());
            if (poFeat->GetGeometryRef())
                poDstFeat->SetGeometry(poFeat->GetGeometryRef());
            for (int f = 0; f < nFields; ++f) {
                if (poFeat->IsFieldSetAndNotNull(f))
                    poDstFeat->SetField(f, poFeat->GetRawFieldRef(f));
            }
            if (poDstLayer->CreateFeature(poDstFeat) != OGRERR_NONE) {
                OGRFeature::DestroyFeature(poDstFeat);
                OGRFeature::DestroyFeature(poFeat);
                GDALClose(poSrc); GDALClose(poDst);
                deleteShapefile(outputPath);
                return false;
            }
            OGRFeature::DestroyFeature(poDstFeat);
            OGRFeature::DestroyFeature(poFeat);
        }
        GDALClose(poSrc);
    }

    GDALClose(poDst);
    return true;
}

QStringList SeNmoSdkBridge::ensureProjectedInputs(const QStringList& inputPaths,
                                                    const QString& dataPath,
                                                    QString& outOriginalSrsWkt)
{
    outOriginalSrsWkt.clear();
    QStringList result;

    // Check the first input's CRS to decide
    GDALDataset* poFirst = (GDALDataset*)GDALOpenEx(
        inputPaths[0].toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poFirst) return inputPaths;

    OGRLayer* poLayer = poFirst->GetLayer(0);
    bool needsProjection = false;
    if (poLayer) {
        OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
        if (poSRS && poSRS->IsGeographic()) {
            needsProjection = true;
            char* pszWkt = nullptr;
            poSRS->exportToWkt(&pszWkt);
            outOriginalSrsWkt = QString::fromUtf8(pszWkt);
            CPLFree(pszWkt);
            // GCS_WGS_1984 and GCS_WGS_84_CRS84 differ in axis order
            // interpretation (lat-lon vs lon-lat). Normalize to CRS84 so
            // the round-trip projection uses consistent axis semantics.
            outOriginalSrsWkt.replace(QStringLiteral("GCS_WGS_1984"),
                                       QStringLiteral("GCS_WGS_84_CRS84"));
        }
    }
    GDALClose(poFirst);

    if (!needsProjection) return inputPaths;

    // Target: Web Mercator (meters), universally applicable
    OGRSpatialReference oTargetSRS;
    oTargetSRS.importFromEPSG(3857);
    char* pszTargetWkt = nullptr;
    oTargetSRS.exportToWkt(&pszTargetWkt);

    for (const QString& shpPath : inputPaths) {
        QFileInfo fi(shpPath);
        QString base = fi.completeBaseName();
        QString tmpPath = dataPath + QStringLiteral("/") + base + QStringLiteral("_proj.shp");

        // Clean stale temp files
        QDir d(dataPath);
        QStringList stale = d.entryList(
            QStringList() << (base + QStringLiteral("_proj.*")), QDir::Files);
        for (const QString& s : stale)
            QFile::remove(dataPath + QStringLiteral("/") + s);

        GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
            shpPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poSrc) { result.append(shpPath); continue; }

        OGRLayer* poSrcLayer = poSrc->GetLayer(0);
        if (!poSrcLayer) { GDALClose(poSrc); result.append(shpPath); continue; }

        OGRCoordinateTransformation* poCT =
            OGRCreateCoordinateTransformation(poSrcLayer->GetSpatialRef(), &oTargetSRS);
        if (!poCT) { GDALClose(poSrc); result.append(shpPath); continue; }

        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (!poDriver) { OCTDestroyCoordinateTransformation(poCT); GDALClose(poSrc); result.append(shpPath); continue; }

        char* papszOpts[] = { (char*)"ENCODING=GBK", nullptr };
        GDALDataset* poDst = poDriver->Create(
            tmpPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, papszOpts);
        if (!poDst) { OCTDestroyCoordinateTransformation(poCT); GDALClose(poSrc); result.append(shpPath); continue; }

        OGRLayer* poDstLayer = poDst->CreateLayer(
            poSrcLayer->GetName(), &oTargetSRS, poSrcLayer->GetGeomType(), papszOpts);
        if (!poDstLayer) {
            GDALClose(poDst); OCTDestroyCoordinateTransformation(poCT);
            GDALClose(poSrc); result.append(shpPath); continue;
        }

        OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
        for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f)
            poDstLayer->CreateField(poSrcDefn->GetFieldDefn(f));

        poSrcLayer->ResetReading();
        OGRFeature* poSrcFeat;
        while ((poSrcFeat = poSrcLayer->GetNextFeature()) != nullptr) {
            OGRFeature* poDstFeat = OGRFeature::CreateFeature(poDstLayer->GetLayerDefn());
            if (poSrcFeat->GetGeometryRef()) {
                OGRGeometry* poGeom = poSrcFeat->GetGeometryRef()->clone();
                if (poGeom->transform(poCT) == OGRERR_NONE)
                    poDstFeat->SetGeometry(poGeom);
                delete poGeom;
            }
            for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f) {
                if (poSrcFeat->IsFieldSetAndNotNull(f))
                    poDstFeat->SetField(f, poSrcFeat->GetRawFieldRef(f));
            }
            poDstLayer->CreateFeature(poDstFeat);
            OGRFeature::DestroyFeature(poDstFeat);
            OGRFeature::DestroyFeature(poSrcFeat);
        }

        GDALClose(poDst);
        OCTDestroyCoordinateTransformation(poCT);
        GDALClose(poSrc);

        // Delete .cpg — SDK does not read it
        QFile::remove(dataPath + QStringLiteral("/") + base + QStringLiteral("_proj.cpg"));

        result.append(tmpPath);
    }

    CPLFree(pszTargetWkt);
    return result;
}

void SeNmoSdkBridge::reprojectOutputsToOriginal(const QStringList& outputPaths,
                                                  const QString& originalSrsWkt)
{
    if (originalSrsWkt.isEmpty()) return;

    OGRSpatialReference oSrcSRS, oDstSRS;
    oSrcSRS.importFromEPSG(3857);
    oDstSRS.importFromWkt(originalSrsWkt.toUtf8().constData());

    for (const QString& outPath : outputPaths) {
        if (!QFileInfo::exists(outPath)) continue;

        QFileInfo fi(outPath);
        QString dir = fi.absolutePath();
        QString base = fi.completeBaseName();
        QString tmpPath = dir + QStringLiteral("/") + base + QStringLiteral("_reproj.shp");

        GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
            outPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poSrc) continue;

        OGRLayer* poSrcLayer = poSrc->GetLayer(0);
        if (!poSrcLayer) { GDALClose(poSrc); continue; }

        OGRCoordinateTransformation* poCT =
            OGRCreateCoordinateTransformation(&oSrcSRS, &oDstSRS);
        if (!poCT) { GDALClose(poSrc); continue; }

        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (!poDriver) { OCTDestroyCoordinateTransformation(poCT); GDALClose(poSrc); continue; }

        char* papszOpts[] = { (char*)"ENCODING=GBK", nullptr };
        GDALDataset* poDst = poDriver->Create(
            tmpPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, papszOpts);
        if (!poDst) { OCTDestroyCoordinateTransformation(poCT); GDALClose(poSrc); continue; }

        OGRLayer* poDstLayer = poDst->CreateLayer(
            base.toUtf8().constData(), &oDstSRS, poSrcLayer->GetGeomType(), papszOpts);
        if (!poDstLayer) {
            GDALClose(poDst); OCTDestroyCoordinateTransformation(poCT);
            GDALClose(poSrc); continue;
        }

        OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
        for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f)
            poDstLayer->CreateField(poSrcDefn->GetFieldDefn(f));

        poSrcLayer->ResetReading();
        OGRFeature* poSrcFeat;
        while ((poSrcFeat = poSrcLayer->GetNextFeature()) != nullptr) {
            OGRFeature* poDstFeat = OGRFeature::CreateFeature(poDstLayer->GetLayerDefn());
            if (poSrcFeat->GetGeometryRef()) {
                OGRGeometry* poGeom = poSrcFeat->GetGeometryRef()->clone();
                if (poGeom->transform(poCT) == OGRERR_NONE)
                    poDstFeat->SetGeometry(poGeom);
                delete poGeom;
            }
            for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f) {
                if (poSrcFeat->IsFieldSetAndNotNull(f))
                    poDstFeat->SetField(f, poSrcFeat->GetRawFieldRef(f));
            }
            poDstLayer->CreateFeature(poDstFeat);
            OGRFeature::DestroyFeature(poDstFeat);
            OGRFeature::DestroyFeature(poSrcFeat);
        }

        GDALClose(poDst);
        OCTDestroyCoordinateTransformation(poCT);
        GDALClose(poSrc);

        // Replace original with reprojected version
        deleteShapefile(outPath);
        QDir d(dir);
        QString baseReproj = base + QStringLiteral("_reproj");
        QStringList files = d.entryList(
            QStringList() << (baseReproj + QStringLiteral(".*")), QDir::Files);
        for (const QString& f : files) {
            QString suffix = f.mid(baseReproj.length());
            QFile::rename(dir + QStringLiteral("/") + f,
                          dir + QStringLiteral("/") + base + suffix);
        }
    }
}

void SeNmoSdkBridge::cleanupProjectedTempFiles(const QStringList& tempPaths)
{
    for (const QString& path : tempPaths) {
        if (!path.contains(QStringLiteral("_proj.")))
            continue;
        QFileInfo fi(path);
        QString dir = fi.absolutePath();
        QString base = fi.completeBaseName();
        QDir d(dir);
        QStringList files = d.entryList(
            QStringList() << (base + QStringLiteral(".*")), QDir::Files);
        for (const QString& f : files)
            QFile::remove(dir + QStringLiteral("/") + f);
    }
}

// 运行日志：logDir/System_Running_<levelTag>_<funcTag>.txt，GBK 编码（记事本直接可读），截断重写
void SeNmoSdkBridge::writeRunLog(const QString& logDir, const QString& levelTag,
                                 const QString& funcTag, const QStringList& lines)
{
    if (logDir.trimmed().isEmpty() || lines.isEmpty()) return;
    QDir().mkpath(logDir);
    QString path = logDir + QStringLiteral("/System_Running_") + levelTag
        + QStringLiteral("_") + funcTag + QStringLiteral(".txt");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(lines.join(QStringLiteral("\n")).toLocal8Bit());
    file.close();
}
