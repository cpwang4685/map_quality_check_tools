#include "wuji_engine_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace WujiEngineBridge {

// ---- 引擎目录定位 ----

QString engineDir()
{
    // 1) 环境变量优先（部署调整用；启动脚本里设 WUJI_ENGINE_DIR=%APP_DIR%plugins）
    const QString env = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("WUJI_ENGINE_DIR"));
    if (!env.isEmpty())
        return QDir::cleanPath(env);

    // 2) 程序目录下 WJEngine 子目录（引擎 DLL/配置/授权所在）
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString wj = QDir(appDir).filePath(QStringLiteral("WJEngine"));
    if (QFileInfo::exists(QDir(wj).filePath(QStringLiteral("MapBatchProcessing.dll"))))
        return wj;

    // 3) 退回程序目录
    return appDir;
}

bool engineAvailable()
{
    return QFileInfo::exists(
        QDir(engineDir()).filePath(QStringLiteral("MapBatchProcessing.exe")));
}

} // namespace WujiEngineBridge
