#ifndef WUJI_MISSION_XML_H
#define WUJI_MISSION_XML_H

// ============================================================
//  wuji_mission_xml.h — 任务 XML 构造器（id453-459）
//
//  各任务 XML 结构与官方质检测试.xml 逐字段一致；
//  FilePath 一律输出为 dataPath 相对路径——引擎内部按
//  dataPath 拼接 FilePath 解析数据，绝对路径会导致任务静默失败。
//  （实测：相对文件名与相对子路径 before/xxx.shp 均可正常执行）
// ============================================================

#include <QHash>
#include <QString>
#include <QStringList>

namespace WujiMissionXml {

// ---- 454 点拓扑规则 ----
// dataPath：任务执行目录（XML 内 FilePath 全部换算为该目录的相对路径）
// pointShp 必填；lineShp/polygonShp/refPointShp 可空（为空则不出对应 Layers）
// outPointShp：结果图层（源点要素副本 + info_NM 字段）
QString buildMission454(const QString& dataPath, const QString& pointShp,
                        const QString& lineShp, const QString& polygonShp,
                        const QString& refPointShp, int processMode,
                        double fuzzyTolerance, const QString& outPointShp);

// ---- 455 线拓扑规则 ----
// lineShp 必填；outLineShp：线结果层；outPointShp：错误位置点层
QString buildMission455(const QString& dataPath, const QString& lineShp,
                        const QString& pointShp, const QString& polygonShp,
                        const QString& refLineShp, int processMode,
                        double fuzzyTolerance, double bufferDistance,
                        const QString& outLineShp, const QString& outPointShp);

// ---- 456 面拓扑规则 ----
// polygonShp 必填；outPolygonShp：面结果层；outLineShp：线结果层；
// outPointShp：错误位置点层
QString buildMission456(const QString& dataPath, const QString& polygonShp,
                        const QString& lineShp, const QString& pointShp,
                        const QString& refPolygonShp, int processMode,
                        double fuzzyTolerance, double bufferDistance,
                        const QString& outPolygonShp, const QString& outLineShp,
                        const QString& outPointShp);

// ---- 457 关联表检查 ----
// srsShps：原始1w数据；resShps：结果5w数据（均位于 dataPath 下）；
// relationDb：relation.db 绝对路径；outShp：结果基础文件名
// （引擎自动添加后缀生成点线面三个层，读取时按快照差分发现）
QString buildMission457(const QString& dataPath, const QStringList& srsShps,
                        const QStringList& resShps, const QString& relationDb,
                        int processMode, const QString& outShp);

// ---- 458 综合前后缓冲匹配 ----
// beforeShps：原始数据；afterShps：成果数据（均位于 dataPath 下）；
// outShps：与 afterShps 一一对应的结果图层（成果要素副本 + match 字段）；
// relationDb / matchParamXml 可为空（为空则不出对应参数）
QString buildMission458(const QString& dataPath,
                        const QStringList& beforeShps,
                        const QStringList& afterShps, int processMode,
                        const QString& relationDb, const QString& matchParamXml,
                        const QStringList& outShps);

// ---- 459 图形规范性检查 ----
// srcShps：源图层（位于 dataPath 下）；thresholds 键同旧实现：
// FuzzyTolerance / AcuteAngle / NarrowWidth / SliverArea / MinNodeDistance
// outPolygonShp：结果层（error 标志字段）；outPointShp：错误位置点层
QString buildMission459(const QString& dataPath, const QStringList& srcShps,
                        int processMode,
                        const QHash<QString, double>& thresholds,
                        const QString& outPolygonShp,
                        const QString& outPointShp);

} // namespace WujiMissionXml

#endif // WUJI_MISSION_XML_H
