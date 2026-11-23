#ifndef SE_MISSION458_CHECK_H
#define SE_MISSION458_CHECK_H

#include <QString>
#include <QList>
#include <QPair>
#include <QHash>
#include <QStringList>

#include "qgsgeometry.h"

class QgsVectorLayer;
class QgsFeature;

// ---- Mission 458: 综合前后缓冲匹配检查 ----
// QGIS 本地实现：按图层映射/同名配对原始与成果图层，成果要素按缓冲距离
// 与原始要素求交（重叠面积/长度比），无原始来源或显著变形的记为异常。
// 需要 原始数据(综合前) 与 成果数据(综合后) 两个数据目录。
//
// M460 模式（focusMask≠0，供「综合前后拓扑检查」组M460使用）：
// 常规比对之外的增强检查，结果按 FocusError 结构化返回（检查项/说明/前后值/定位点），
// 由对话框写入 综合前后拓扑检查_点/线/面.shp + 定位点.shp；此时未匹配与未勾选的变形
// 不再写 allErrors。检查范围：道路/铁路图层参与②~⑦；河流/渠图层参与②③断裂、
// 化简检查（勾选④/⑥时）与⑦线要素属性一致性；行政境界图层（省界/市界/县界等）只参与⑦。
// 图层名单、属性比对字段与全部阈值均在 matchParameter.xml 配置。
namespace Mission458 {

// ---- M460 检查位（与 mission_config.xml 中 460 组 CheckItem 的 mode 一一对应） ----
enum FocusBit {
    FocusResidentRoad = 1,   // 居民点与道路拓扑（由对话框用引擎454执行，此处忽略）
    FocusBreakLong    = 2,   // 道路长距离连接：综合后断口 > LongGapThreshold
    FocusBreakShort   = 4,   // 道路·河流断裂：综合后断口 > MinBreakGap（默认50米）且 ≤ LongGapThreshold
    FocusDeform       = 8,   // 道路变形异常：长度/面积变化率超限或局部鼓包（顶点数仅写入说明）
    FocusDisplace     = 16,  // 道路过度偏移：成果线落在同路原始线缓冲外的长度占比超阈值
    FocusStraighten   = 32,  // 道路拉直异常：弯曲度（线长/首尾直线距离）下降超阈值；与④/⑤同要素时并入同一条记录
    FocusAttribute    = 64,  // 线要素属性一致性：按图层类别比对约定字段
                              // 道路/铁路 NAME/RN/CLASID/TYPE；境界 CLASID；水系 TYPE/LEVEL/NAME
    FocusSimplify     = 128  // 内部位（不是独立CheckItem，不写入mission_config）：
                              // 勾选④变形或⑥拉直任一即自动置位，使河流/渠图层也执行
                              // “线要素化简检查”；设置点在 Mission458::execute 入口。
};

// ---- M460 检查错误条目（对话框据此写 综合前后拓扑检查_点/线/面.shp） ----
struct FocusError {
    QgsGeometry geometry;   // 错误几何（按点/线/面类型分文件输出）
    QString checkItem;      // 检查项名称（与460组CheckItem一致，写入"检查项"字段）
    QString desc;           // 说明（含图层名/具体变化，写入"说明"字段）
    QString values;         // 前后值（写入"前后值"字段，含定位坐标）
    QgsPointXY locPoint;    // 定位点（②③断口/⑤偏移中心/④⑥最大偏离处；无定位时忽略）
    bool hasLoc = false;    // locPoint 是否有效（有效时另写 综合前后拓扑检查_定位点.shp）
};

// ---- M460 选取比例统计（成果要素数 / 原始要素数） ----
// 只做数量统计，不参与错误判定；由对话框写 选取比例统计.csv 并记入日志。
struct SelectionStat {
    QString resultLayer;    // 成果图层名（如 "国道"）
    QString origLayer;      // 原始图层名/代码（如 "LRDL"）
    qint64  origCount = 0;  // 原始要素数
    qint64  resultCount = 0;// 成果要素数
};

// ---- M460 ⑤ 真实移位值（2026-11-08 甲方口径） ----
// 用「化简后移位前」与「最终成果」两套分阶段数据直接对比，量出成果线相对
// 移位前线的真实偏移量：成果要素每个顶点到“同一条路”移位前要素的最近距离，
// 取最大值/平均值（投影坐标，单位米）。同一个成果要素可对应移位前多个
// 分段要素（综合时合并），因此按标识合并后的一组线参与取最近距离。
struct TrueShiftInfo {
    bool valid = false;      // 是否成功找到移位前参照
    QString layer;           // 成果图层名（如 "高速公路"）
    qint64 fid = -1;         // 成果要素 FID（用于 ⑤ 判定时回查）
    QString matchKey;        // 匹配依据（如 "OBJECTID=1327"、"RN=S102"、"按位置最近"）
    QString refLayers;       // 移位前参考图层名（多个用逗号分隔，最多列4个）
    double maxShift = 0.0;   // 最大移位值（米）
    double meanShift = 0.0;  // 平均移位值（米）
    int vertexCount = 0;     // 参与统计的成果顶点数
    int fallbackVertices = 0;// 其中按“同层就近”取参照的顶点数（标识覆盖不全时）
    QgsPointXY point;        // 最大移位所在位置（成果侧顶点）
    bool hasPoint = false;
};

// 计算「化简后移位前」→「最终成果」的真实移位值。
// beforeDir: 化简后移位前数据目录（SHP）
// afterDir:  最终成果数据目录（SHP，通常与成果数据路径相同）
// 返回 key = "<成果图层基名>#<FID>" 的逐要素结果；同一要素在 ⑤ 判定时按该
// key 回查。未找到移位前参照的成果要素不出现在返回值中（⑤ 退回缓冲法）。
// 图层对应关系按甲方 1107 口径内置：公路处理2_高速→高速公路/在建高速公路、
// 国道3→国道、省道3→省道、县道3→县道、乡道3+专用公路3→乡道（专用道）、
// 连接道3→连接道、快速路3→快速路、城市道路处理2→城市道路、村道处理2→村道、
// 铁路处理2→铁路。移位前数据缺某文件时，按同一道路标识在其他移位前图层中
// 跨图层查找（如省道文件里的快速路要素在成果中归入快速路）。
QHash<QString, TrueShiftInfo> computeTrueShifts(
    const QString& beforeDir, const QString& afterDir,
    const QString& matchParamPath, QStringList* log);

// origDir: 原始数据目录（综合前）
// resultDir: 成果数据目录（综合后）
// layerMapping: 成果图层名→源图层代码，用于原始数据图层配对（无映射时按同名规则回退）
// matchParamPath: matchParameter.xml 参数文件路径，读取失败时使用内置默认参数并记录提示
// focusMask: M460 检查位掩码（0=常规检查，行为与旧版一致）
// focusErrors: 非空且focusMask≠0时接收 M460 检查结果（未匹配/未勾选项不再进allErrors）
void execute(const QString& origDir, const QString& resultDir,
    int processMode,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks,
    const QHash<QString, QString>* layerMapping = nullptr,
    const QString& matchParamPath = QString(),
    int focusMask = 0,
    QList<FocusError>* focusErrors = nullptr,
    const QHash<QString, TrueShiftInfo>* trueShifts = nullptr);

void executeWithMapping(const QString& origDir, const QString& resultDir,
    int processMode,
    const QHash<QString, QString>& layerMapping,
    QList<QPair<QgsFeature, QString>>& allErrors,
    QStringList& executedChecks,
    const QString& matchParamPath = QString(),
    int focusMask = 0,
    QList<FocusError>* focusErrors = nullptr,
    const QHash<QString, TrueShiftInfo>* trueShifts = nullptr);

// ---- ① 村道/乡道端点连接（M460 专用） ----
// 2026-10-27 起替代原“居民点必须压盖道路”规则：成果侧村道/乡道线要素的每个
// 折线端点，必须距其他道路部件 ≤ EndpointBuffer 或距居民点 ≤ EndpointBuffer
// （matchParameter.xml，默认 50 米）；闭合折线不判定。
// subjectLayers: 成果侧村道/乡道线图层（检查目标）
// roadLayers: 成果侧全部道路线图层（连接参照）
// residentLayers: 成果侧居民点图层（压盖参照，可为空）
// boundaryLayers: 成果侧行政界线图层（县界/市界/省界等，可为空）。端点位于
//   数据外框或行政界线附近（≤ BoundaryTolerance，默认50米）时视为被数据范围
//   截断的边界端点，不报错，仅在日志中统计。
// out: 追加端点错误（点几何 + 定位点）；log: 可空，追加统计说明
void buildRoadEndpointFocusErrors(
    const QList<QgsVectorLayer*>& subjectLayers,
    const QList<QgsVectorLayer*>& roadLayers,
    const QList<QgsVectorLayer*>& residentLayers,
    const QList<QgsVectorLayer*>& boundaryLayers,
    const QString& matchParamPath,
    QList<FocusError>& out, QStringList* log);

// ---- ① 居民点道路左右换边（M460 专用） ----
// 对成果侧全部居民点：按 ELEMID（缺失时坐标/1米内最近点）找到原始同名点，
// 分别求“点到最近道路”的左右侧（道路方向被反转时先统一方向）；两侧不同即报
// “居民点与道路拓扑”异常，几何取成果居民点、定位点取居民点坐标。
// 只处理距最近道路≤PointSideTolerance 的点，超出容差视为远离道路不判定。
void buildResidentSideFlipFocusErrors(
    const QList<QgsVectorLayer*>& resultResidentLayers,
    const QList<QgsVectorLayer*>& resultRoadLayers,
    const QString& origDataPath,
    const QHash<QString, QString>& layerMapping,
    const QString& matchParamPath,
    QList<FocusError>& out, QStringList* log);

// ---- M460 选取比例统计 ----
// 按 pairLayers 的成果→原始图层配对，统计每个成果图层要素数占原始图层要素数的
// 比例（不判定错误）。origDir/resultDir 为空时跳过并记日志。
void collectSelectionStats(
    const QString& origDir, const QString& resultDir,
    const QHash<QString, QString>* layerMapping,
    QList<SelectionStat>& stats, QStringList* log);

} // namespace Mission458

#endif // SE_MISSION458_CHECK_H
