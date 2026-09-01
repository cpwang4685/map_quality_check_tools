#ifndef SE_EDGE_MERGE_BRIDGE_H
#define SE_EDGE_MERGE_BRIDGE_H

#include <QString>
#include <QStringList>
#include <string>
#include <vector>

struct LayerMatchParam;
struct LayerMergeRecord;

namespace SeEdgeMergeBridge {

// 加载 base_geoextractandprocess.dll 并定位 OpAutoMerge（无导入库，直接查导出表）。
// 失败返回 false 并在 err 中给出原因。
bool ensureLoaded(QString* err);

// 调用 CSE_GeoExtractAndProcess::OpAutoMerge。
// 返回 0 成功；1 比例尺不合法；2 接边距离不合法；3 图层匹配参数不合法；
//       4 gpkg数据库全路径不合法；5 其它错误；-1 调用前失败（err 已设置）。
// 调用期间 SDK 直发 QGIS 面板的消息会被 IAT 挂钩拦下、花码还原成正确中文后
// 转发面板（面板正常显示），并暂存供 takeSdkMessages() 取出记入运行日志。
int opAutoMerge(const std::vector<LayerMatchParam>& params, int scaleType,
                double distMeters, const std::string& gpkgPath,
                std::vector<LayerMergeRecord>& records, QString* err);

// 取出并清空最近一次 opAutoMerge 期间 SDK 输出的消息（已转 UTF-8）。
QStringList takeSdkMessages();

} // namespace SeEdgeMergeBridge

#endif // SE_EDGE_MERGE_BRIDGE_H
