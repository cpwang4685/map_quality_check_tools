#ifndef WUJI_ENGINE_BRIDGE_H
#define WUJI_ENGINE_BRIDGE_H

// ============================================================
//  wuji_engine_bridge.h — 无极引擎（MapBatchProcessing）目录定位
//
//  本文件只负责"引擎装在哪、在不在"，不调用引擎本身：
//  真正的执行入口是 MapBatchProcessing.exe 子进程，由
//  WujiMissionRunner::runMissionXml() 以 QProcess 启动
//  （官方命令行入口：MapBatchProcessing.exe <xml> <dataPath> true 8）。
//
//  历史上本文件曾在进程内直接调用
//  Nmo::MapBatchProcessing::FunctionsProcessing::DoXMLFile()，
//  该路径已废弃删除——它引入 NMO SDK 头文件依赖（麒麟上默认
//  SE_NMO_SDK_ENABLED=OFF，头文件不可见会导致编译失败），
//  且需要 SetDllDirectoryW/LoadLibraryExW 等平台 API。
//
//  纯 Qt 实现，Windows / 麒麟双平台通用。
// ============================================================

#include <QString>

namespace WujiEngineBridge {

// 定位引擎目录：优先环境变量 WUJI_ENGINE_DIR；
// 否则取 QGIS 程序目录下的 WJEngine 子目录（需含 MapBatchProcessing.dll）；
// 再退回程序目录本身。
QString engineDir();

// 引擎是否可用：引擎目录下是否存在 MapBatchProcessing.exe。
// 子进程执行路径依赖该可执行文件；麒麟上没有它，调用方据此
// 回退到本地 QGIS 实现（见 se_mission454/455/456_check.cpp）。
bool engineAvailable();

} // namespace WujiEngineBridge

#endif // WUJI_ENGINE_BRIDGE_H
