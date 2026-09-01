#define _HAS_STD_BYTE 0
#include "se_edge_merge_bridge.h"

#include <windows.h>
#include <cstring>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <qgis.h>

// base_geoextractandprocess.dll 部署目录候选（插件目录 → exe 目录）。
// 与 se_nmo_sdk_bridge 的 plugins 探测候选保持一致；此处独立实现，避免对 se_nmo_sdk_bridge 产生依赖。
static QStringList edgeMergePluginDirs()
{
    QStringList dirs;
    const QString exeDir = QCoreApplication::applicationDirPath();
    //  1) <exe>/plugins：exe 位于部署根目录的形态
    //  2) <exe>/../plugins：LTZK 运行时形态（exe 在 bin/，插件与 SDK 在 bin/../plugins）
    //  base_geoextractandprocess.dll 与 MapBatchProcessing.dll 等 NMO SDK DLL 同目录部署，
    //  候选必须与 se_nmo_sdk_bridge 对齐，否则接边 SDK 找不到而综合/合并正常。
    dirs << exeDir + QStringLiteral("/plugins")
         << QDir::cleanPath(exeDir + QStringLiteral("/../plugins"))
         << exeDir;
    return dirs;
}

#include "vector/cse_geo_extract_and_process.h"

#include "cpl_conv.h"
#include "cpl_error.h"

typedef int (*FnOpAutoMerge)(std::vector<LayerMatchParam>, int, double, std::string,
                             std::vector<LayerMergeRecord>&);

static HMODULE g_hMod = nullptr;
static FnOpAutoMerge g_fnMerge = nullptr;

// SDK 内部经 GDAL 输出的消息（实测 UTF-8 字节）若放行，会被 QGIS 全局
// GDAL 错误处理器按本地编码（GBK）解码转进消息面板成乱码。SDK 在工作线程
// 输出，线程局部的 CPLPushErrorHandler 拦不住；需全局拦截（CPLSetErrorHandler）
// 统一暂存，不进面板，由运行日志"SDK消息"小节记录。
static QStringList s_sdkGdalMessages;
static QMutex s_sdkMsgMutex;
static void sdkGdalErrHandler(CPLErr, int, const char* msg)
{
    if (!msg)
        return;
    // 严格 UTF-8 校验不过（含非法序列）再按本地编码兜底
    const QByteArray bytes(msg);
    QString text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes)
        text = QString::fromLocal8Bit(bytes);
    QMutexLocker lock(&s_sdkMsgMutex);
    s_sdkGdalMessages << text;
}

static FARPROC findExport(HMODULE hMod, const char* needle)
{
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + rva);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    WORD* ords = (WORD*)(base + exp->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* name = (const char*)(base + names[i]);
        if (strstr(name, needle))
            return (FARPROC)(base + funcs[ords[i]]);
    }
    return nullptr;
}

// —— SDK 面板消息 IAT 挂钩 ————————————————————————————————————
// 黑盒 SDK 直调 QgsMessageLog::logMessage（经 qgis_core.dll 导入表），
// 且把 UTF-8 字节按本地编码（GBK）解码成 QString 传入，面板显示花码。
// 此处补丁其 IAT 槽：模板法还原成正确中文后转发原函数（面板正常），
// 同时记入运行日志"SDK消息"小节。还原 key 由同机同 Qt 重演解码生成，
// 与 SDK 输出逐字节一致；占位符为纯 ASCII 层名，按前后缀定位取出。
typedef void (*FnLogMessage)(const QString&, const QString&, Qgis::MessageLevel, bool);

static const char kLogMessageName[] =
    "?logMessage@QgsMessageLog@@SAXAEBVQString@@0W4MessageLevel@Qgis@@_N@Z";

struct SdkLogHook
{
    FnLogMessage orig = nullptr;
};
static SdkLogHook* g_logHook = nullptr;  // 泄漏以跨插件重载保留原函数指针

static void fixGarbledTemplate(QString& text)
{
    struct Tmpl { QString text; };
    static const Tmpl kTmpls[] = {
        { QStringLiteral("正在生成图层%1的接边记录...") },
        { QStringLiteral("图层%1的接边记录生成完毕！") },
    };
    for (const Tmpl& t : kTmpls) {
        const QString garbled = QString::fromLocal8Bit(t.text.toUtf8());
        const int pos = garbled.indexOf(QLatin1String("%1"));
        const QString gPre = garbled.left(pos);
        const QString gSuf = garbled.mid(pos + 2);
        if (text.length() >= gPre.length() + gSuf.length()
            && text.startsWith(gPre) && text.endsWith(gSuf)) {
            const QString layer = text.mid(gPre.length(),
                text.length() - gPre.length() - gSuf.length());
            text = t.text;
            text.replace(QLatin1String("%1"), layer);
            return;
        }
    }
}

static void hookedLogMessage(const QString& message, const QString& tag,
                             Qgis::MessageLevel level, bool notifyUser)
{
    QString fixed = message;
    QString fixedTag = tag;
    fixGarbledTemplate(fixed);
    fixGarbledTemplate(fixedTag);
    {
        QMutexLocker lock(&s_sdkMsgMutex);
        s_sdkGdalMessages << fixed;
    }
    if (g_logHook && g_logHook->orig)
        g_logHook->orig(fixed, fixedTag, level, notifyUser);
}

static void patchIatSlot(void* slot, FARPROC fn)
{
    DWORD oldProt = 0;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt);
    *(FARPROC*)slot = fn;
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
}

static FARPROC sdkLogMessageExport()
{
    HMODULE qc = GetModuleHandleW(L"qgis_core.dll");
    return qc ? GetProcAddress(qc, kLogMessageName) : nullptr;
}

static void hookSdkMessageLog()
{
    HMODULE hMod = GetModuleHandleW(L"base_geoextractandprocess.dll");
    if (!hMod)
        return;
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + impRva);
    for (; imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), "qgis_core.dll") != 0)
            continue;
        ULONGLONG* iat = (ULONGLONG*)(base + imp->FirstThunk);
        DWORD* oft = imp->OriginalFirstThunk
            ? (DWORD*)(base + imp->OriginalFirstThunk) : nullptr;
        for (int i = 0; iat[i]; i++) {
            bool isTarget = false;
            if (oft && oft[i] && !(oft[i] & 0x80000000))
                isTarget = (strcmp((const char*)(base + oft[i] + 2), kLogMessageName) == 0);
            else if (!oft)
                isTarget = (reinterpret_cast<FARPROC>(iat[i]) == sdkLogMessageExport());
            if (!isTarget)
                continue;
            if (!g_logHook->orig) {
                if (iat[i] == reinterpret_cast<ULONGLONG>(&hookedLogMessage))
                    g_logHook->orig = reinterpret_cast<FnLogMessage>(sdkLogMessageExport());
                else
                    g_logHook->orig = reinterpret_cast<FnLogMessage>(iat[i]);
            }
            patchIatSlot(&iat[i], reinterpret_cast<FARPROC>(&hookedLogMessage));
            return;
        }
    }
}

namespace SeEdgeMergeBridge {

bool ensureLoaded(QString* err)
{
    if (g_fnMerge)
        return true;

    // Search plugin dir first, then exe dir; fall back to the bare name
    // (system search order) when the file is nowhere to be found.
    const QString dllName = QStringLiteral("base_geoextractandprocess.dll");
    QStringList candDirs;
    candDirs << edgeMergePluginDirs();
    QString foundPath;
    for (const QString& dir : candDirs) {
        if (dir.isEmpty())
            continue;
        const QString p = dir + QStringLiteral("/") + dllName;
        if (QFileInfo::exists(p)) {
            foundPath = p;
            break;
        }
    }

    if (!foundPath.isEmpty()) {
        const QString native = QDir::toNativeSeparators(foundPath);
        SetDllDirectoryW((LPCWSTR)QDir::toNativeSeparators(
            QFileInfo(foundPath).absolutePath()).utf16());
        g_hMod = LoadLibraryW((LPCWSTR)native.utf16());
    } else {
        g_hMod = LoadLibraryW((LPCWSTR)dllName.utf16());
    }
    if (!g_hMod) {
        if (err)
            *err = QStringLiteral("无法加载接边SDK（base_geoextractandprocess.dll），错误码 %1")
                       .arg(GetLastError());
        return false;
    }
    g_fnMerge = (FnOpAutoMerge)findExport(g_hMod, "?OpAutoMerge@");
    if (!g_fnMerge) {
        if (err)
            *err = QStringLiteral("接边SDK中未找到 OpAutoMerge 导出函数");
        return false;
    }
    if (!g_logHook)
        g_logHook = new SdkLogHook();
    hookSdkMessageLog();
    return true;
}

int opAutoMerge(const std::vector<LayerMatchParam>& params, int scaleType,
                double distMeters, const std::string& gpkgPath,
                std::vector<LayerMergeRecord>& records, QString* err)
{
    if (!ensureLoaded(err))
        return -1;
    {
        QMutexLocker lock(&s_sdkMsgMutex);
        s_sdkGdalMessages.clear();
    }
    // 主线程 + SDK 工作线程都要拦：线程局部 push 只覆盖主线程，全局
    // CPLSetErrorHandler 覆盖工作线程（SDK 消息实际从这里输出）。
    CPLPushErrorHandler(sdkGdalErrHandler);
    CPLErrorHandler pfnPrev = CPLSetErrorHandler(sdkGdalErrHandler);
    // SDK 内部会把 GDAL_FILENAME_IS_UTF8 改成 NO（旧版 GDAL 中文环境做法），
    // 导致之后插件/QGIS 以 UTF-8 打开中文路径全部失败（错误为空）。
    // 调用后立即恢复原值。
    const char* pszOldUtf8 = CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    int ret = g_fnMerge(params, scaleType, distMeters, gpkgPath, records);
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", pszOldUtf8);
    CPLSetErrorHandler(pfnPrev);
    CPLPopErrorHandler();
    return ret;
}

QStringList takeSdkMessages()
{
    QMutexLocker lock(&s_sdkMsgMutex);
    QStringList out;
    out.swap(s_sdkGdalMessages);
    return out;
}

} // namespace SeEdgeMergeBridge
