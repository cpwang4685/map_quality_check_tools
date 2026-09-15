#ifndef WUJI_MISSION_RUNNER_H
#define WUJI_MISSION_RUNNER_H

// ============================================================
//  wuji_mission_runner.h — Mission 执行公共辅助
//
//  负责：
//   - 图层/文件准备：导出为 GBK 编码 SHP（无极引擎只读 GBK 的 DBF）；
//   - 任务 XML 落盘，并以独立子进程运行 MapBatchProcessing.exe
//     （引擎遇异常数据会崩溃/挂死，子进程隔离避免带崩 QGIS）；
//   - 结果 SHP 读取：按标志字段语义挑出错误要素，转成 QgsFeature。
// ============================================================

#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>

class QgsVectorLayer;
class QgsFeature;

namespace WujiMissionRunner {

// ---- 数据准备 ----

// 将图层导出为指定目录下的 SHP（GBK 编码 DBF）。
// 成功返回导出文件绝对路径；失败返回空串（errOut 输出原因）。
QString exportLayerShp(QgsVectorLayer* layer, const QString& dir,
                       const QString& baseName, QString* errOut = nullptr);

// 将磁盘 SHP（含附属文件）复制到 dir，并在副本上做 GBK 编码校正：
// 源为 UTF-8（.cpg 声明或 QGIS 探测）时转出 *_gbk.shp。
// 返回副本路径列表（绝对路径，可能为 *_gbk.shp）。
QStringList stageShapefiles(const QStringList& srcPaths, const QString& dir);

// 合并多个同几何类型图层为单个纯几何 SHP（仅保留几何，属性不参与）：
// 454/455/456 扩展为全部图层逐层检查时，其余类型的参考图层合并后，
// 一次引擎调用即可覆盖全部参考要素。单图层直接导出；无图层返回空串。
QString mergeLayersShp(const QList<QgsVectorLayer*>& layers, const QString& dir,
                       const QString& baseName, QString* errOut = nullptr);

// ---- 执行 ----

// 将任务 XML 写入 dataPath 下临时文件，并以子进程运行引擎
// （MapBatchProcessing.exe <xml> <dataPath> true 8，超时 300 秒）。
// 执行期间持续读取子进程输出（防输出管道写满死锁）；超时强制终止，
// 并把控制台输出尾部带回错误信息，用于定位引擎挂起点。
// 成功判定：退出码 0 且控制台含"任务执行成功"且不含"任务执行失败"。
// 失败时向 executedChecks 追加"执行失败"说明（含引擎控制台输出）。
bool runMissionXml(const QString& xml, const QString& dataPath,
                   const QString& label, int processMode,
                   QStringList& executedChecks);

// ---- 结果读取 ----

// 结果读取模式
enum ResultReadMode {
    ReadInfoField,   // 信息字段非空即错误（454/455/456/457：info_NM）
    ReadFlagInt,     // 整型标志字段非 0 即错误（459：error）
    ReadMatchString, // 字符串匹配字段 != "1" 即错误（458：match）
    ReadAll          // 全部要素均为错误（错误位置点层）
};

// 读取引擎结果 SHP：按字段语义挑出错误要素，优先按 FID 从 srcLayer
// 取回原始要素（保留属性），取回失败则直接用结果要素。
void readResultErrors(const QString& resultShpPath, QgsVectorLayer* srcLayer,
                      const QString& label, ResultReadMode mode,
                      const QString& fieldName,
                      QList<QPair<QgsFeature, QString>>& allErrors);

// 扫描 dir 中相对执行前快照新建的 SHP，逐个按 info_NM 字段收集错误。
// 用于引擎自定义输出文件名的情况（457 自动加后缀生成点线面结果层）。
void readNewResultShps(const QString& dir, const QStringList& beforeFiles,
                       QgsVectorLayer* srcLayer, const QString& label,
                       QList<QPair<QgsFeature, QString>>& allErrors);

// 快照目录下当前全部 .shp 文件名
QStringList shpNamesIn(const QString& dir);

// 判断数据源路径是否为 FileGDB（.gdb 结尾的文件或目录）。
// FileGDB 本质是文件夹，用户可在目录选择框里直接选中 "xxx.gdb" 目录。
bool isFileGdbSource(const QString& path);

} // namespace WujiMissionRunner

#endif // WUJI_MISSION_RUNNER_H
