// 必须先于任何 QGIS 头文件：修正本机 QGIS 库与 json_fwd.hpp 的 nlohmann 版本差，
// 否则链接 QgsGeometry::asJsonObject 会报 LNK2001（详见该头文件里的说明）。
#include "qgis_json_compat.h"

#include "se_mission458_check.h"

#include "../ui_task/wuji_mission_runner.h"

#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgscurve.h"
#include "qgscurvepolygon.h"
#include "qgsgeometrycollection.h"
#include "qgsgeometryutils.h"
#include "qgsspatialindex.h"
#include "qgspoint.h"
#include "qgsvectorlayer.h"
#include "qgsproviderregistry.h"
#include "qgsprovidermetadata.h"
#include "qgsprovidersublayerdetails.h"
#include "qgspointxy.h"
#include "qgsrectangle.h"
#include "qgswkbtypes.h"

#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QVector>
#include <cmath>
#include <limits>
#include <memory>

// ---- Mission 458 本地实现说明 ----
// 引擎路线（DataBufferMatch/MapBatchProcessing.exe）在成果数据上稳定失败
// （引擎写中间文件丢 .shx 后重开失败），已改为 QGIS 本地缓冲匹配：
//   1. 按图层映射表（成果图层名→源图层代码）或同名规则配对原始/成果图层；
//   2. 成果要素按缓冲距离与原始要素求交（重叠面积/长度比），
//      无原始来源的成果要素记为"未匹配"，显著变形的记为"变形"；
//   3. 原始要素被综合掉（成果中找不到对应）属综合正常现象，只统计不报错。
// 匹配参数优先从 matchParameter.xml 读取（config 目录随包提供），
// 未找到时使用下列默认值（成果为投影坐标，单位米）。
namespace Mission458 {

static const double DEFAULT_BUFFER_DISTANCE = 10.0;   // 默认缓冲匹配距离（米）
static const double DEFAULT_MIN_OVERLAP_RATIO = 0.5;  // 重叠面积/长度比低于该值视为未匹配
static const double DEFAULT_MAX_CHANGE_RATIO = 0.3;   // 面积/长度变化率超过 30% 视为显著变形
static const int    DEFAULT_MAX_VERTEX_CHANGE = 10;   // 顶点数变化超过 10 个视为显著变形
// M460 检查补充参数默认值
static const double DEFAULT_MAX_DISPLACEMENT = 100.0;    // 过度偏移缓冲半径（米）
static const double DEFAULT_MAX_OUTSIDE_RATIO = 0.2;     // 超出偏移缓冲的长度占比阈值
static const double DEFAULT_MAX_SINUOSITY_DROP = 0.5;    // 弯曲度下降阈值（疑拉直）
static const double DEFAULT_CONNECT_TOLERANCE = 1.0;     // 连通判定容差（米）
static const double DEFAULT_MIN_BREAK_GAP = 50.0;        // ③综合后短断口报错下限（米）
static const double DEFAULT_LONG_GAP_THRESHOLD = 100.0;  // 长距离断口阈值（米）
// ②③ 隧道断口豁免：成果道路端点距隧道线端点（隧道口）≤该值视为经隧道连通（米）。
// 隧道按标准字段识别（CLASID 4506xx / TYPE 含“隧道”），不依赖图层名。
static const double DEFAULT_TUNNEL_PORTAL_TOLERANCE = 5.0;
// ⑥ 直角拉直（重点）：成果拐点转角在 [Min,Max] 内、原始同位置转角≤OrigMaxTurn、
//    拐点两侧线段均≥MinSegment 时，单独输出“道路直角拉直（重点）”。
static const double DEFAULT_RIGHT_ANGLE_MIN_ANGLE = 70.0;
static const double DEFAULT_RIGHT_ANGLE_MAX_ANGLE = 110.0;
static const double DEFAULT_RIGHT_ANGLE_ORIG_MAX_TURN = 30.0;
static const double DEFAULT_RIGHT_ANGLE_MIN_SEGMENT = 5.0;
static const double DEFAULT_RIGHT_ANGLE_SEARCH_RADIUS = 60.0;
// ① 村道/乡道端点连接：端点距其他道路或居民点≤该值视为连接/压盖（米）。
// 默认 0.001 米 = 1mm（成果为投影坐标、毫米精度）；可在 matchParameter.xml
// 的 <EndpointBuffer> 调整。
static const double DEFAULT_ENDPOINT_BUFFER = 0.001;
// ① 边界端点排除距离（米）：端点位于数据外框或县界/市界/省界附近时，
// 视为被数据范围截断（不是综合造成的断头），不报错。默认 50 米。
static const double DEFAULT_BOUNDARY_TOLERANCE = 50.0;
// ① 左右换边：只在居民点距最近道路≤该值时判定左右侧（米），避免远处点噪声
static const double DEFAULT_POINT_SIDE_TOLERANCE = 100.0;
// ④ 化简检查：原始顶点数≥MinSimplifyVertexCount 且顶点减少比例<MinVertexReduceRatio
//    时视为"未化简/化简不足"（阈值可按数据密度调整）
static const int    DEFAULT_MIN_SIMPLIFY_VERTEX_COUNT = 20;
static const double DEFAULT_MIN_VERTEX_REDUCE_RATIO = 0.05;
// ④ 局部鼓包/突刺：顶点到相邻顶点弦线的垂距>MaxLocalBulge，且该点距原始线
//    >MaxLocalDeviation 时报告（米）
static const double DEFAULT_MAX_LOCAL_BULGE = 20.0;
static const double DEFAULT_MAX_LOCAL_DEVIATION = 20.0;

// M460 图层范围默认名单（matchParameter.xml 的 RoadLayerKeywords/RiverLayerKeywords
// 未配置时使用；名称为包含匹配）：道路含铁路；河流/渠图层只参与②③断裂检查。
static QStringList defaultRoadLayerKeywords()
{
    return QStringList() << QStringLiteral("国道") << QStringLiteral("省道")
        << QStringLiteral("县道") << QStringLiteral("乡道") << QStringLiteral("村道")
        << QStringLiteral("专用道") << QStringLiteral("高速公路")
        << QStringLiteral("高速铁路") << QStringLiteral("普通铁路");
}

static QStringList defaultRiverLayerKeywords()
{
    return QStringList() << QStringLiteral("河流") << QStringLiteral("干渠")
        << QStringLiteral("支渠");
}

// ⑦ 线要素属性一致性：境界图层名单（默认只认行政界线；名称包含匹配）
static QStringList defaultBoundaryLayerKeywords()
{
    return QStringList() << QStringLiteral("省界") << QStringLiteral("未定省界")
        << QStringLiteral("市界") << QStringLiteral("县界");
}

// ⑦ 各类线要素参与比对的属性字段（大小写不敏感；CLASSID/CLASID 互为别名）。
// 只比对原始/成果两边都存在的字段，缺失字段自动跳过。
static QStringList defaultRoadAttrFields()
{
    return QStringList() << QStringLiteral("NAME") << QStringLiteral("RN")
        << QStringLiteral("CLASID") << QStringLiteral("TYPE");
}

static QStringList defaultBoundaryAttrFields()
{
    return QStringList() << QStringLiteral("CLASID");
}

static QStringList defaultWaterAttrFields()
{
    return QStringList() << QStringLiteral("TYPE") << QStringLiteral("LEVEL")
        << QStringLiteral("NAME");
}

// 匹配参数集（matchParameter.xml 提供，缺文件时取默认值）
struct MatchParams {
    double bufferDistance = DEFAULT_BUFFER_DISTANCE;
    double minOverlapRatio = DEFAULT_MIN_OVERLAP_RATIO;
    double maxChangeRatio = DEFAULT_MAX_CHANGE_RATIO;
    int maxVertexChange = DEFAULT_MAX_VERTEX_CHANGE;
    double maxDisplacement = DEFAULT_MAX_DISPLACEMENT;
    double maxOutsideRatio = DEFAULT_MAX_OUTSIDE_RATIO;      // ⑤缓冲外长度占比阈值
    double maxSinuosityDrop = DEFAULT_MAX_SINUOSITY_DROP;
    double connectTolerance = DEFAULT_CONNECT_TOLERANCE;
    double minBreakGap = DEFAULT_MIN_BREAK_GAP;              // ③综合后断口≤该值不报（米）
    double longGapThreshold = DEFAULT_LONG_GAP_THRESHOLD;
    double tunnelPortalTolerance = DEFAULT_TUNNEL_PORTAL_TOLERANCE; // ②③隧道口豁免（米）
    // ⑥ 直角拉直（重点）判定参数（度/米）
    double rightAngleMinAngle = DEFAULT_RIGHT_ANGLE_MIN_ANGLE;
    double rightAngleMaxAngle = DEFAULT_RIGHT_ANGLE_MAX_ANGLE;
    double rightAngleOrigMaxTurn = DEFAULT_RIGHT_ANGLE_ORIG_MAX_TURN;
    double rightAngleMinSegment = DEFAULT_RIGHT_ANGLE_MIN_SEGMENT;
    double rightAngleSearchRadius = DEFAULT_RIGHT_ANGLE_SEARCH_RADIUS;
    double endpointBuffer = DEFAULT_ENDPOINT_BUFFER;         // ①村道/乡道端点连接缓冲（米）
    double boundaryTolerance = DEFAULT_BOUNDARY_TOLERANCE;    // ①边界端点排除距离（米）
    double pointSideTolerance = DEFAULT_POINT_SIDE_TOLERANCE; // ①左右换边判定距离（米）
    int minSimplifyVertexCount = DEFAULT_MIN_SIMPLIFY_VERTEX_COUNT;   // ④化简检查最少原始顶点
    double minVertexReduceRatio = DEFAULT_MIN_VERTEX_REDUCE_RATIO;    // ④顶点最少减少比例
    double maxLocalBulge = DEFAULT_MAX_LOCAL_BULGE;           // ④局部鼓包弦高阈值（米）
    double maxLocalDeviation = DEFAULT_MAX_LOCAL_DEVIATION;   // ④鼓包点距原始线阈值（米）
    QStringList roadKeywords = defaultRoadLayerKeywords();   // 道路图层名关键字（含铁路）
    QStringList riverKeywords = defaultRiverLayerKeywords(); // 河流/渠图层名关键字
    QStringList boundaryKeywords = defaultBoundaryLayerKeywords(); // 境界（行政界线）图层名关键字
    // ⑦ 线要素属性一致性：按图层类别参与比对的字段
    QStringList roadAttrFields = defaultRoadAttrFields();         // 道路/铁路
    QStringList boundaryAttrFields = defaultBoundaryAttrFields(); // 境界
    QStringList waterAttrFields = defaultWaterAttrFields();       // 水系
    bool loadedFromFile = false;
};

// 解析 matchParameter.xml：
// <MapGeneBatchProcessing><Mission id="458"><Parameter>
//   <BufferDistance>10</BufferDistance><MinOverlapRatio>0.5</MinOverlapRatio>
//   <MaxChangeRatio>0.3</MaxChangeRatio><MaxVertexChange>10</MaxVertexChange>
// </Parameter></Mission></MapGeneBatchProcessing>
static MatchParams loadMatchParams(const QString& path)
{
    MatchParams p;
    if (path.isEmpty() || !QFileInfo::exists(path))
        return p;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return p;
    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        return p;
    }
    file.close();
    const QDomElement param = doc.documentElement()
        .firstChildElement(QStringLiteral("Mission"))
        .firstChildElement(QStringLiteral("Parameter"));
    if (param.isNull())
        return p;
    const auto readDouble = [&param](const QString& tag, double def) -> double {
        const QDomElement e = param.firstChildElement(tag);
        if (e.isNull())
            return def;
        bool ok = false;
        const double v = e.text().trimmed().toDouble(&ok);
        return ok ? v : def;
    };
    p.bufferDistance  = readDouble(QStringLiteral("BufferDistance"), DEFAULT_BUFFER_DISTANCE);
    p.minOverlapRatio = readDouble(QStringLiteral("MinOverlapRatio"), DEFAULT_MIN_OVERLAP_RATIO);
    p.maxChangeRatio  = readDouble(QStringLiteral("MaxChangeRatio"), DEFAULT_MAX_CHANGE_RATIO);
    p.maxVertexChange = static_cast<int>(readDouble(QStringLiteral("MaxVertexChange"),
                                                    DEFAULT_MAX_VERTEX_CHANGE));
    p.maxDisplacement = readDouble(QStringLiteral("MaxDisplacement"), DEFAULT_MAX_DISPLACEMENT);
    p.maxOutsideRatio = readDouble(QStringLiteral("MaxOutsideRatio"), DEFAULT_MAX_OUTSIDE_RATIO);
    p.maxSinuosityDrop = readDouble(QStringLiteral("MaxSinuosityDrop"), DEFAULT_MAX_SINUOSITY_DROP);
    p.connectTolerance = readDouble(QStringLiteral("ConnectTolerance"), DEFAULT_CONNECT_TOLERANCE);
    p.minBreakGap = readDouble(QStringLiteral("MinBreakGap"), DEFAULT_MIN_BREAK_GAP);
    p.longGapThreshold = readDouble(QStringLiteral("LongGapThreshold"), DEFAULT_LONG_GAP_THRESHOLD);
    p.tunnelPortalTolerance = readDouble(QStringLiteral("TunnelPortalTolerance"),
                                         DEFAULT_TUNNEL_PORTAL_TOLERANCE);
    p.rightAngleMinAngle = readDouble(QStringLiteral("RightAngleMinAngle"),
                                      DEFAULT_RIGHT_ANGLE_MIN_ANGLE);
    p.rightAngleMaxAngle = readDouble(QStringLiteral("RightAngleMaxAngle"),
                                      DEFAULT_RIGHT_ANGLE_MAX_ANGLE);
    p.rightAngleOrigMaxTurn = readDouble(QStringLiteral("RightAngleOrigMaxTurn"),
                                         DEFAULT_RIGHT_ANGLE_ORIG_MAX_TURN);
    p.rightAngleMinSegment = readDouble(QStringLiteral("RightAngleMinSegment"),
                                        DEFAULT_RIGHT_ANGLE_MIN_SEGMENT);
    p.rightAngleSearchRadius = readDouble(QStringLiteral("RightAngleSearchRadius"),
                                          DEFAULT_RIGHT_ANGLE_SEARCH_RADIUS);
    if (p.rightAngleMinAngle > p.rightAngleMaxAngle)
        qSwap(p.rightAngleMinAngle, p.rightAngleMaxAngle);
    if (p.rightAngleMinSegment < 0.0) p.rightAngleMinSegment = 0.0;
    if (p.rightAngleSearchRadius < p.rightAngleMinSegment)
        p.rightAngleSearchRadius = p.rightAngleMinSegment;
    p.endpointBuffer = readDouble(QStringLiteral("EndpointBuffer"),
                                  DEFAULT_ENDPOINT_BUFFER);
    p.boundaryTolerance = readDouble(QStringLiteral("BoundaryTolerance"),
                                     DEFAULT_BOUNDARY_TOLERANCE);
    p.pointSideTolerance = readDouble(QStringLiteral("PointSideTolerance"),
                                      DEFAULT_POINT_SIDE_TOLERANCE);
    p.minSimplifyVertexCount = static_cast<int>(readDouble(QStringLiteral("MinSimplifyVertexCount"),
                                                           DEFAULT_MIN_SIMPLIFY_VERTEX_COUNT));
    p.minVertexReduceRatio = readDouble(QStringLiteral("MinVertexReduceRatio"),
                                        DEFAULT_MIN_VERTEX_REDUCE_RATIO);
    p.maxLocalBulge = readDouble(QStringLiteral("MaxLocalBulge"), DEFAULT_MAX_LOCAL_BULGE);
    p.maxLocalDeviation = readDouble(QStringLiteral("MaxLocalDeviation"),
                                     DEFAULT_MAX_LOCAL_DEVIATION);
    // 图层范围名单：逗号（中英文均可）分隔的关键字列表
    const auto readList = [&param](const QString& tag, const QStringList& def) -> QStringList {
        const QDomElement e = param.firstChildElement(tag);
        if (e.isNull()) return def;
        QString text = e.text();
        text.replace(QStringLiteral("，"), QStringLiteral(","));
        text.replace(QStringLiteral("、"), QStringLiteral(","));
        QStringList out;
        for (const QString& s : text.split(QLatin1Char(','))) {
            const QString t = s.trimmed();
            if (!t.isEmpty()) out << t;
        }
        return out.isEmpty() ? def : out;
    };
    p.roadKeywords  = readList(QStringLiteral("RoadLayerKeywords"), defaultRoadLayerKeywords());
    p.riverKeywords = readList(QStringLiteral("RiverLayerKeywords"), defaultRiverLayerKeywords());
    p.boundaryKeywords = readList(QStringLiteral("BoundaryLayerKeywords"),
                                  defaultBoundaryLayerKeywords());
    p.roadAttrFields = readList(QStringLiteral("RoadAttrFields"), defaultRoadAttrFields());
    p.boundaryAttrFields = readList(QStringLiteral("BoundaryAttrFields"),
                                    defaultBoundaryAttrFields());
    p.waterAttrFields = readList(QStringLiteral("WaterAttrFields"), defaultWaterAttrFields());
    p.loadedFromFile  = true;
    return p;
}

// 递归收集目录下全部 .shp 绝对路径（原始数据按分类子目录存放，需要递归）
static QStringList shpPathsIn(const QString& dir)
{
    QStringList paths;
    QDirIterator it(dir, QStringList() << QStringLiteral("*.shp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) paths.append(it.next());
    return paths;
}

// 枚举 FileGDB 中全部矢量图层：返回 图层名(代码) → 图层URI列表
// URI 形如 "D:/xxx/永登县.gdb|layername=LRDL"，可直接交给 QgsVectorLayer 打开；
// 图层名即标准图层代码（LRDL/HYDL_HL/...），与图层映射表 sourceCode 列天然对应。
static QHash<QString, QStringList> gdbLayersByCode(const QString& gdbPath)
{
    QHash<QString, QStringList> byCode;
    QgsProviderMetadata* ogrMd =
        QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("ogr"));
    if (!ogrMd) return byCode;
    const QList<QgsProviderSublayerDetails> subs = ogrMd->querySublayers(gdbPath);
    for (const QgsProviderSublayerDetails& s : subs) {
        if (s.type() != QgsMapLayerType::VectorLayer) continue;   // 跳过非矢量子层
        if (s.wkbType() == QgsWkbTypes::Unknown
            || s.wkbType() == QgsWkbTypes::NoGeometry) continue;   // 跳过纯属性表
        const QString name = s.name();
        if (name.isEmpty()) continue;
        byCode[name].append(s.uri());
    }
    return byCode;
}

// 在字段表中按字段名查找下标（大小写不敏感；CLASSID/CLASID 视为同一字段）
static int findFieldIndexCI(const QgsFields& fields, const QString& wanted)
{
    const QString w = wanted.trimmed().toUpper();
    if (w.isEmpty()) return -1;
    for (int i = 0; i < fields.count(); ++i) {
        if (fields.at(i).name().trimmed().toUpper() == w) return i;
    }
    const QString alias = (w == QStringLiteral("CLASID"))
        ? QStringLiteral("CLASSID")
        : (w == QStringLiteral("CLASSID")) ? QStringLiteral("CLASID") : QString();
    if (!alias.isEmpty()) {
        for (int i = 0; i < fields.count(); ++i) {
            if (fields.at(i).name().trimmed().toUpper() == alias) return i;
        }
    }
    return -1;
}

// 在图层字段中查找标识字段（候选名大小写无关，找不到返回空）
static QString findField(const QgsVectorLayer* layer, const QStringList& candidates)
{
    if (!layer) return QString();
    const QgsFields& fields = layer->fields();
    for (const QString& c : candidates) {
        if (fields.indexOf(c) >= 0) return c;
    }
    for (int i = 0; i < fields.count(); i++) {
        const QString fn = fields.at(i).name();
        for (const QString& c : candidates) {
            if (fn.compare(c, Qt::CaseInsensitive) == 0) return fn;
        }
    }
    return QString();
}

// 去掉常见综合后缀（_选取/_名称/_点/_方向点/_注记/_跳绘），用于图层配对回退
static QString stripSuffix(const QString& base)
{
    const QStringList suffixes = {
        QStringLiteral("_选取"), QStringLiteral("_名称"), QStringLiteral("_点"),
        QStringLiteral("_方向点"), QStringLiteral("_注记"), QStringLiteral("_跳绘")};
    for (const QString& sfx : suffixes) {
        if (base.endsWith(sfx))
            return base.left(base.length() - sfx.length());
    }
    return base;
}

// 配对得分：原始要素落在成果要素缓冲范围内的比例。
//  线：缓冲内长度 ÷ 两者长度较大值 —— 分母取较长者，避免"短要素整段落在
//      缓冲内"得分虚高（同一条链上的另一段、只是交叉经过的线因此不会被误选）；
//  面：交集面积 ÷ 原始面积（沿用原口径）；
//  点等其余类型：落入缓冲 = 1，否则 0。
static double pairScore(const QgsGeometry& bufGeom, double resultLen,
                        const QgsGeometry& origGeom, double* matchedLenOut = nullptr)
{
    if (matchedLenOut) *matchedLenOut = 0.0;
    if (bufGeom.isNull() || origGeom.isNull()) return 0.0;
    const QgsWkbTypes::GeometryType ot = origGeom.type();
    if (ot == QgsWkbTypes::LineGeometry) {
        const double ob = origGeom.length();
        if (ob <= 0.0) return 0.0;
        const double matched = bufGeom.intersection(origGeom).length();
        if (matchedLenOut) *matchedLenOut = matched;
        const double denom = qMax(ob, resultLen);
        return denom > 0.0 ? matched / denom : 0.0;
    }
    if (ot == QgsWkbTypes::PolygonGeometry) {
        const double ob = origGeom.area();
        if (ob <= 0.0) return 0.0;
        return bufGeom.intersection(origGeom).area() / ob;
    }
    return bufGeom.intersects(origGeom) ? 1.0 : 0.0;
}

// 统计顶点数（多部件递归；面=外环+内环点数——此前 QgsPolygon 既不是
// curve 也不是 collection，会落到 return 0，导致面的顶点变化永不触发）
static int countVertices(const QgsAbstractGeometry* g)
{
    if (!g) return 0;
    if (g->dimension() == 0) return 1;
    if (const QgsCurve* curve = dynamic_cast<const QgsCurve*>(g))
        return curve->numPoints();
    if (const QgsCurvePolygon* poly = dynamic_cast<const QgsCurvePolygon*>(g)) {
        int n = poly->exteriorRing() ? poly->exteriorRing()->numPoints() : 0;
        for (int i = 0; i < poly->numInteriorRings(); ++i)
            if (const QgsCurve* ring = poly->interiorRing(i))
                n += ring->numPoints();
        return n;
    }
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        int n = 0;
        for (int i = 0; i < gc->numGeometries(); i++)
            n += countVertices(gc->geometryN(i));
        return n;
    }
    return 0;
}

// 统计线几何的曲线部件数（与 countVertices 配套：线∩缓冲面得到的每个裁剪
// 部件两端各有一个合成端点，估算"原始顶点落在缓冲内"时需扣除 2×部件数）
static int countCurveParts(const QgsAbstractGeometry* g)
{
    if (!g) return 0;
    if (dynamic_cast<const QgsCurve*>(g)) return 1;
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        int n = 0;
        for (int i = 0; i < gc->numGeometries(); i++)
            n += countCurveParts(gc->geometryN(i));
        return n;
    }
    return 0;
}

// 取图层要素的文本属性；空值及综合数据占位符 "-" 一律视为空
static QString attrText(const QgsVectorLayer& layer, const QgsFeature& f, const QString& name)
{
    const int idx = layer.fields().lookupField(name);
    if (idx < 0) return QString();
    const QString s = f.attribute(idx).toString().trimmed();
    if (s.isEmpty() || s == QStringLiteral("-")) return QString();
    return s;
}

// 道路同路标识：优先 ELEMID（调用方单独处理）；无 ELEMID 时按
// RN > NAME1 > NAME > TYPE 取第一个非空属性。综合后的国道/省道/县道/乡道
// 图层没有 ELEMID，只能靠这些业务属性判断是否为同一条路。
struct RoadIdentity {
    QString key;
    QString value;
};

static RoadIdentity identityFromAttrs(const QgsVectorLayer& layer, const QgsFeature& f)
{
    static const QStringList keys = QStringList()
        << QStringLiteral("RN") << QStringLiteral("NAME1")
        << QStringLiteral("NAME") << QStringLiteral("TYPE");
    for (const QString& k : keys) {
        const QString v = attrText(layer, f, k);
        if (!v.isEmpty()) return RoadIdentity{k, v};
    }
    return RoadIdentity();
}

// 匹配循环内收集的原始候选（缓冲内、同几何类型），供同路合计复用：
// score=pairScore，interLen=缓冲∩原始线长度（面/点恒 0）
struct CandRef {
    const QgsFeature* feat = nullptr;
    double score = 0.0;
    double interLen = 0.0;
};

// 同路原始要素集合与"成果缓冲内同路原始顶点合计"。
// 综合后一个成果要素常合并同一条路的多个原始要素（原始按链分段），
// 只与 bestOrig 一对一比较会把"合并后顶点正常"误报成变形/化简不足。
struct SameRoadRef {
    QList<QgsGeometry> geoms;   // 同路原始要素（整条）
    int unionVerts = 0;         // 成果缓冲内的同路原始顶点合计（估算）
    double coverage = 0.0;      // 成果线被同路原始要素覆盖的比例（0~1）
    double unionLen = 0.0;      // 同路原始线合并后的总长（2026-10-30 ④长度基准）
    bool hasIdentity = false;
    QString key;
    QString value;
};

// 收集同路原始要素并计算合计指标：
//   标识优先 ELEMID（eidMatched 时即 bestOrig 与成果同号）；否则用 bestOrig 的
//   RN/NAME1/NAME/TYPE；同路候选取缓冲∩原始长度≥50米的要素（极短擦边不算同路），
//   bestOrig 无论长度都计入。coverage<50% 时调用方不做化简/顶点基准判定。
static SameRoadRef collectSameRoadRefs(
    const QgsVectorLayer& origLayer, const QgsFeature& bestOrig,
    const QVector<CandRef>& cands, const QgsGeometry& bufGeom,
    double resultLen, const QString& eid, int origEidIdx)
{
    SameRoadRef ref;
    if (resultLen <= 0.0 || cands.isEmpty()) return ref;

    // 成果要素自带 ELEMID 时以它为准（与原始侧同号要素即同一条链）；无 ELEMID
    // 的国道/省道/县道/乡道成果图层退回 RN/NAME1/NAME/TYPE 业务标识。
    if (!eid.isEmpty()) {
        ref.hasIdentity = true;
        ref.key = QStringLiteral("ELEMID");
        ref.value = eid;
    } else {
        const RoadIdentity id = identityFromAttrs(origLayer, bestOrig);
        if (!id.value.isEmpty()) {
            ref.hasIdentity = true;
            ref.key = id.key;
            ref.value = id.value;
        }
    }
    const int keyIdx = (ref.hasIdentity && ref.key != QStringLiteral("ELEMID"))
        ? origLayer.fields().lookupField(ref.key) : -1;

    double sumLen = 0.0;
    int sumVerts = 0;
    for (const CandRef& c : cands) {
        if (!c.feat) continue;
        const bool isBest = (c.feat->id() == bestOrig.id());
        bool same = isBest;
        if (!same && ref.hasIdentity) {
            if (ref.key == QStringLiteral("ELEMID")) {
                if (origEidIdx >= 0)
                    same = (c.feat->attribute(origEidIdx).toString().trimmed() == ref.value);
            } else if (keyIdx >= 0) {
                const QString v = c.feat->attribute(keyIdx).toString().trimmed();
                same = (!v.isEmpty() && v != QStringLiteral("-") && v == ref.value);
            }
        }
        if (!same) continue;
        if (!isBest && c.interLen < 50.0) continue;

        const QgsGeometry g = c.feat->geometry();
        ref.geoms.append(g);
        const QgsGeometry inter = bufGeom.intersection(g);
        if (inter.isNull() || inter.isEmpty()) continue;
        sumLen += inter.length();
        // 裁剪件两端各有一个合成端点；部件数×2 扣掉后即为落在缓冲内的原始顶点数（近似）
        const int v = countVertices(inter.constGet()) - 2 * countCurveParts(inter.constGet());
        if (v > 0) sumVerts += v;
    }
    if (ref.geoms.isEmpty()) ref.geoms.append(bestOrig.geometry());
    ref.unionVerts = sumVerts;
    ref.coverage = qMin(1.0, sumLen / resultLen);
    // 2026-10-30 ④长度基准：同路原始线先合并再取长度（重叠段不重复计），
    // 用于替代“只与 bestOrig 一条比长度”——成果线合并同路多段时会虚增。
    {
        QgsGeometry all;
        for (const QgsGeometry& g : ref.geoms) {
            if (g.isNull() || g.isEmpty()) continue;
            all = all.isNull() ? g : all.combine(g);
        }
        if (!all.isNull() && !all.isEmpty())
            ref.unionLen = all.length();
    }
    return ref;
}

// 比较综合前后几何变化（移植自 7-27 集成版）
static void compareGeometry(const QgsFeature& origFeat, const QgsFeature& resultFeat,
                            double& areaChange, double& lengthChange, int& vertexChange)
{
    areaChange = 0.0;
    lengthChange = 0.0;
    vertexChange = 0;

    const QgsGeometry origGeom = origFeat.geometry();
    const QgsGeometry resultGeom = resultFeat.geometry();
    if (origGeom.isNull() || resultGeom.isNull()) return;

    const double origArea = origGeom.area();
    const double resultArea = resultGeom.area();
    if (origArea > 0.0 && resultArea > 0.0)
        areaChange = (resultArea - origArea) / origArea;

    const double origLen = origGeom.length();
    const double resultLen = resultGeom.length();
    if (origLen > 0.0 && resultLen > 0.0)
        lengthChange = (resultLen - origLen) / origLen;

    const int origVerts = countVertices(origGeom.constGet());
    const int resultVerts = countVertices(resultGeom.constGet());
    if (origVerts > 0 && resultVerts > 0)
        vertexChange = resultVerts - origVerts;
}

// ==================== M460 检查辅助 ====================

// 线要素首尾端点（多部件取首部件起点、末部件终点）；非线几何返回 false
static bool lineEndpoints(const QgsAbstractGeometry* g, QgsPointXY& p1, QgsPointXY& p2)
{
    if (!g) return false;
    if (const QgsCurve* curve = dynamic_cast<const QgsCurve*>(g)) {
        if (curve->numPoints() < 1) return false;
        const QgsPoint sp = curve->startPoint();
        const QgsPoint ep = curve->endPoint();
        p1 = QgsPointXY(sp.x(), sp.y());
        p2 = QgsPointXY(ep.x(), ep.y());
        return true;
    }
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        if (gc->numGeometries() < 1) return false;
        QgsPointXY a1, a2, b1, b2;
        if (!lineEndpoints(gc->geometryN(0), a1, a2)) return false;
        if (!lineEndpoints(gc->geometryN(gc->numGeometries() - 1), b1, b2)) return false;
        p1 = a1;
        p2 = b2;
        return true;
    }
    return false;
}

// 线弯曲度 = 线长 / 首尾直线距离（直线=1，越弯越大）。
// 非线 / 首尾重合（直线距离<0.5米）/ 零长时返回 -1（不适用）
static double sinuosity(const QgsGeometry& geom)
{
    if (geom.isNull() || geom.type() != QgsWkbTypes::LineGeometry) return -1.0;
    QgsPointXY p1, p2;
    if (!lineEndpoints(geom.constGet(), p1, p2)) return -1.0;
    const double straight = p1.distance(p2);
    const double len = geom.length();
    if (straight < 0.5 || len <= 0.0) return -1.0;
    return len / straight;
}

// 折线在 b 点的转角（度）：0=直行，90=直角，180=掉头；零长边返回 -1。
static double turnAngleDegrees(const QgsPointXY& a, const QgsPointXY& b,
                               const QgsPointXY& c)
{
    const double v1x = b.x() - a.x(), v1y = b.y() - a.y();
    const double v2x = c.x() - b.x(), v2y = c.y() - b.y();
    if (std::hypot(v1x, v1y) <= 1e-9 || std::hypot(v2x, v2y) <= 1e-9)
        return -1.0;
    double deg = std::fabs(std::atan2(v2y, v2x) - std::atan2(v1y, v1x))
        * 180.0 / 3.14159265358979323846;
    while (deg >= 360.0) deg -= 360.0;
    if (deg > 180.0) deg = 360.0 - deg;
    return deg;
}

// 原始线在成果拐点附近的局部转角：取 radius 内最近的有效顶点（两侧线段均
// ≥minSeg）的转角。找不到返回 false（无法判断原始走向）。
static bool localOriginalTurnAngle(const QgsGeometry& origGeom, const QgsPointXY& corner,
                                   double radius, double minSeg, double& angle)
{
    if (origGeom.isNull() || radius <= 0.0) return false;
    const QgsMultiPolylineXY parts = origGeom.asMultiPolyline();
    double bestDist = -1.0;
    bool found = false;
    for (const QgsPolylineXY& pts : parts) {
        for (int i = 1; i + 1 < pts.size(); ++i) {
            const double d = pts.at(i).distance(corner);
            if (d > radius) continue;
            if (pts.at(i).distance(pts.at(i - 1)) < minSeg
                || pts.at(i).distance(pts.at(i + 1)) < minSeg)
                continue;
            const double a = turnAngleDegrees(pts.at(i - 1), pts.at(i), pts.at(i + 1));
            if (a < 0.0) continue;
            if (bestDist < 0.0 || d < bestDist) {
                bestDist = d;
                angle = a;
                found = true;
            }
        }
    }
    return found;
}

struct RightAngleHit {
    QgsPointXY corner;
    double resultAngle = 0.0;   // 成果拐点转角（度）
    double origAngle = 0.0;     // 原始同位置转角（度）
    double seg1 = 0.0;          // 拐点前一段长度（米）
    double seg2 = 0.0;          // 拐点后一段长度（米）
};

// ⑥ 直角拉直（重点）判定：成果线存在近直角拐点，而原始同名线在同一位置
// 接近直行 → 该直角是综合后新增的。返回最接近 90° 的一处。
static bool detectRightAngleStraightening(const QgsGeometry& resultGeom,
                                          const QgsGeometry& origGeom,
                                          const MatchParams& mp, RightAngleHit& hit)
{
    if (resultGeom.isNull() || origGeom.isNull() || mp.rightAngleMaxAngle <= 0.0)
        return false;
    const QgsMultiPolylineXY parts = resultGeom.asMultiPolyline();
    double bestDelta = 1e18;
    bool found = false;
    for (const QgsPolylineXY& pts : parts) {
        for (int i = 1; i + 1 < pts.size(); ++i) {
            const double s1 = pts.at(i).distance(pts.at(i - 1));
            const double s2 = pts.at(i).distance(pts.at(i + 1));
            if (s1 < mp.rightAngleMinSegment || s2 < mp.rightAngleMinSegment) continue;
            const double a = turnAngleDegrees(pts.at(i - 1), pts.at(i), pts.at(i + 1));
            if (a < mp.rightAngleMinAngle || a > mp.rightAngleMaxAngle) continue;
            double oa = 0.0;
            if (!localOriginalTurnAngle(origGeom, pts.at(i), mp.rightAngleSearchRadius,
                                        mp.rightAngleMinSegment, oa))
                continue;
            if (oa > mp.rightAngleOrigMaxTurn) continue;   // 原始本来就有这个直角
            const double delta = std::fabs(a - 90.0);
            if (delta < bestDelta) {
                bestDelta = delta;
                found = true;
                hit.corner = pts.at(i);
                hit.resultAngle = a;
                hit.origAngle = oa;
                hit.seg1 = s1;
                hit.seg2 = s2;
            }
        }
    }
    return found;
}

// 点 p 到最近隧道口（隧道线端点）的距离；无隧道口返回 -1。
static double nearestPortalDistance(const QList<QgsPointXY>& portals, const QgsPointXY& p)
{
    double best = -1.0;
    for (const QgsPointXY& q : portals) {
        const double d = p.distance(q);
        if (best < 0.0 || d < best) best = d;
    }
    return best;
}

// 从成果数据中收集隧道口坐标与隧道线几何。隧道按标准字段识别（跨县一致）：
//   CLASID 以 4506 开头（450601 火车隧道 / 450602 汽车隧道），
//   或 TYPE 含“隧道”；图层名仅用于快速筛选候选，最终以字段为准。
// 每条隧道线取首尾两个端点作为隧道口（不另建隧道口点图层）；
// 隧道线几何一并返回（2026-10-30），供⑤过度偏移把“道路走隧道”的段落从
// 缓冲外长度里剔除（原始道路在隧道处常被隧道线打断）。
static QList<QgsPointXY> collectTunnelPortals(
    const QStringList& resultShps,
    const QMap<QString, QString>& resultByBase,
    const QHash<QString, QString>* layerMapping,
    int& tunnelLayers, int& tunnelFeatures, QStringList* log,
    QList<QgsGeometry>* tunnelLines = nullptr)
{
    tunnelLayers = 0;
    tunnelFeatures = 0;

    QSet<QString> candidates;
    for (const QString& p : resultShps) {
        if (QFileInfo(p).completeBaseName().contains(QStringLiteral("隧道")))
            candidates.insert(p);
    }
    // 图层映射中源代码为 LFCL 的成果图层（标准名“隧道”“汽车隧道口”等），
    // 其中点图层会在下面按几何类型跳过。
    if (layerMapping) {
        for (auto it = layerMapping->constBegin(); it != layerMapping->constEnd(); ++it) {
            if (it.value().compare(QStringLiteral("LFCL"), Qt::CaseInsensitive) != 0)
                continue;
            const QString p = resultByBase.value(it.key());
            if (!p.isEmpty()) candidates.insert(p);
        }
    }

    QList<QgsPointXY> portals;
    for (const QString& path : candidates) {
        QgsVectorLayer lyr(path, QStringLiteral("m460_tunnel"), QStringLiteral("ogr"));
        if (!lyr.isValid()) continue;
        if (lyr.geometryType() != QgsWkbTypes::LineGeometry) continue; // 隧道口点层跳过
        const int clasidIdx = findFieldIndexCI(lyr.fields(), QStringLiteral("CLASID"));
        const int typeIdx = findFieldIndexCI(lyr.fields(), QStringLiteral("TYPE"));
        if (clasidIdx < 0 && typeIdx < 0) {
            if (log)
                log->append(QStringLiteral("[%1] 未找到CLASID/TYPE字段，未作为隧道图层")
                    .arg(QFileInfo(path).fileName()));
            continue;
        }
        int n = 0;
        QgsFeatureIterator fit = lyr.getFeatures();
        QgsFeature f;
        while (fit.nextFeature(f)) {
            bool isTunnel = false;
            if (clasidIdx >= 0) {
                const QString v = f.attribute(clasidIdx).toString().trimmed();
                if (v.startsWith(QStringLiteral("4506"))) isTunnel = true;
            }
            if (!isTunnel && typeIdx >= 0) {
                const QString v = f.attribute(typeIdx).toString().trimmed();
                if (v.contains(QStringLiteral("隧道"))) isTunnel = true;
            }
            if (!isTunnel) continue;
            if (tunnelLines) {
                const QgsGeometry tg = f.geometry();
                if (!tg.isNull() && !tg.isEmpty()) tunnelLines->append(tg);
            }
            // 多部件隧道逐部件取首尾端点，全部作为隧道口；单部件即首尾两点
            const QgsMultiPolylineXY parts = f.geometry().asMultiPolyline();
            bool added = false;
            for (const QgsPolylineXY& pts : parts) {
                if (pts.size() < 2) continue;
                portals << pts.first() << pts.last();
                added = true;
            }
            if (added) ++n;
        }
        if (n > 0) {
            ++tunnelLayers;
            tunnelFeatures += n;
        }
    }

    if (log) {
        if (tunnelFeatures > 0)
            log->append(QStringLiteral(
                "隧道识别（②③断口豁免/⑤偏移补充）：识别隧道图层%1个、隧道线%2条、隧道口%3个（按CLASID/TYPE字段）")
                .arg(tunnelLayers).arg(tunnelFeatures).arg(portals.size()));
        else
            log->append(QStringLiteral(
                "隧道识别：成果数据未识别到隧道线（按CLASID/TYPE字段），②③豁免与⑤隧道补充均未启用"));
    }
    return portals;
}

// ---- M460 图层范围过滤 ----
// 名称包含匹配；"XX_名称/XX代码/XX注记/方向点/选取"等注记与代码类图层虽带关键字，
// 但不是被检查的道路/河流要素本身，一律不参与（如"国道代码""三级河流_名称"）。
static bool nameContainsAny(const QString& name, const QStringList& keywords)
{
    for (const QString& k : keywords) {
        if (!k.isEmpty() && name.contains(k))
            return true;
    }
    return false;
}

static const QStringList& annotationMarkers()
{
    static const QStringList markers = QStringList()
        << QStringLiteral("名称") << QStringLiteral("注记") << QStringLiteral("代码")
        << QStringLiteral("方向点") << QStringLiteral("选取");
    return markers;
}

// 该成果图层在 M460 模式下的有效检查位（0=不参与本次M460检查）：
//   道路图层（名单含铁路）→ 全部已勾选项；
//   河流/渠图层 → ②③断裂 + 化简检查（FocusSimplify，勾选④/⑥时派生）+ ⑦线要素属性一致性
//     （水系不做④变形/⑤偏移/⑥拉直，只做化简）；
//   境界图层（省界/市界/县界等）→ 仅⑦线要素属性一致性；
//   其余图层（面状地物、注记代码类等）→ 0。
// 1107 图层对应表中未列入 RoadLayerKeywords 的道路族：
// 城市道路/快速路/连接道/铁路（合并层）。只参与化简与偏移，不扩②③④⑥⑦。
static bool isSimplifyDisplaceOnlyRoad(const QString& layerName)
{
    return layerName.contains(QStringLiteral("城市道路"))
        || layerName.contains(QStringLiteral("快速路"))
        || layerName.contains(QStringLiteral("连接道"))
        || layerName == QStringLiteral("铁路");
}

static int focusLayerMask(const QString& layerName, const MatchParams& mp, int focus)
{
    if (nameContainsAny(layerName, annotationMarkers())) return 0;
    if (nameContainsAny(layerName, mp.roadKeywords)) return focus;
    // 2026-11-08 甲方口径：城市道路/快速路/连接道/铁路(合并层)也要进化简和
    // 偏移检查；只给这两位，避免把②③④⑥⑦一并扩到这些图层。
    if (isSimplifyDisplaceOnlyRoad(layerName))
        return focus & (FocusDisplace | FocusSimplify);
    if (nameContainsAny(layerName, mp.riverKeywords))
        return focus & (FocusBreakLong | FocusBreakShort | FocusAttribute | FocusSimplify);
    if (nameContainsAny(layerName, mp.boundaryKeywords))
        return focus & FocusAttribute;
    return 0;
}

// ---- M460 定位辅助 ----
// 坐标文本（米，1位小数），供"前后值"内置入定位信息
static QString locText(const QgsPointXY& p)
{
    return QStringLiteral("(%1, %2)").arg(p.x(), 0, 'f', 1).arg(p.y(), 0, 'f', 1);
}

// 取几何中的最长线部件（多部件线在最长段上做采样定位）
static const QgsAbstractGeometry* mainLinePart(const QgsAbstractGeometry* g)
{
    if (!g) return nullptr;
    if (dynamic_cast<const QgsCurve*>(g)) return g;
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        const QgsAbstractGeometry* best = nullptr;
        double bestLen = -1.0;
        for (int i = 0; i < gc->numGeometries(); ++i) {
            const QgsAbstractGeometry* part = mainLinePart(gc->geometryN(i));
            if (part && part->length() > bestLen) { bestLen = part->length(); best = part; }
        }
        return best;
    }
    return nullptr;
}

// ④⑥定位：沿成果线等弧长采样（固定100段），取距原始线最远的采样点。
// 定位精度约线长/100，代价与线顶点数无关；返回坐标+偏离米数。
static bool maxDeviationPoint(const QgsGeometry& resultGeom, const QgsGeometry& origGeom,
                              QgsPointXY& pt, double& maxDist)
{
    if (resultGeom.isNull() || origGeom.isNull()) return false;
    const QgsCurve* curve = dynamic_cast<const QgsCurve*>(mainLinePart(resultGeom.constGet()));
    if (!curve || curve->numPoints() < 2) return false;
    const double len = curve->length();
    if (len <= 0.0) return false;
    const int samples = 100;
    maxDist = -1.0;
    bool found = false;
    for (int i = 0; i <= samples; ++i) {
        std::unique_ptr<QgsPoint> sp(curve->interpolatePoint(len * i / samples));  // 返回堆对象，需接管
        if (!sp) continue;
        const QgsPointXY p(sp->x(), sp->y());
        const double d = origGeom.distance(QgsGeometry::fromPointXY(p));
        if (d > maxDist) { maxDist = d; pt = p; found = true; }
    }
    return found;
}

// ---- ④ 局部鼓包/突刺检测 ----
// 综合时道路移位可能把局部顶点的“鼓包”带出来：整条线总体偏移不大，但某个
// 顶点相对两侧弦线突然外凸。逐顶点取 1/2/4/8/16 个顶点的弦线，计算垂距
// （弦高）；弦高 > MaxLocalBulge 且该点距原始线 > MaxLocalDeviation 才计入
// （后者排除原始道路本身就有急弯/回头弯的情况）。多部件逐段扫描，返回最严重的一处。
// 距离基准取“同路原始要素集合”的最近距离，而不是只取 bestOrig：综合后一条
// 成果线常合并同一条路的多个原始分段，bestOrig 可能只是一小段，鼓包顶点离它
// 很远、却紧贴同路相邻分段，按 bestOrig 度量会成片误报（2026-10-27 修正）。
struct LocalBulge {
    bool found = false;
    QgsPointXY point;       // 最严重鼓包顶点
    double sagitta = 0.0;   // 最大弦高（米）
    double deviation = 0.0; // 该点到同路原始线的最近距离（米）
    int count = 0;          // 满足条件的顶点数
    int vertexIndex = -1;
};

// 点到原始线集合的最近距离（集合为空返回 -1）
static double nearestRefDistance(const QList<QgsGeometry>& refs, const QgsGeometry& pt)
{
    double best = -1.0;
    for (const QgsGeometry& g : refs) {
        const double d = pt.distance(g);
        if (best < 0.0 || d < best) best = d;
    }
    return best;
}

// 扫描单条线（一段）的局部鼓包
static void scanCurveBulges(const QgsCurve* curve, const QList<QgsGeometry>& refs,
                            const MatchParams& mp, LocalBulge& out)
{
    if (!curve || curve->numPoints() < 3 || refs.isEmpty()) return;
    QgsPointSequence pts;
    curve->points(pts);
    const int n = pts.size();
    if (n < 3) return;
    const int scales[] = {1, 2, 4};   // 2026-10-30 P1-1：只看短波（原 1/2/4/8/16）
    for (int i = 1; i < n - 1; ++i) {
        const QgsPoint& p = pts.at(i);
        double bestSag = 0.0;
        for (const int s : scales) {
            const int left = i - s;
            const int right = i + s;
            if (left < 0 || right >= n) continue;
            const QgsPoint& a = pts.at(left);
            const QgsPoint& b = pts.at(right);
            const double chord = a.distance(b);
            if (chord < 1.0) continue;   // 弦太短，方向不可靠
            // 弦太长的是整段改线，不属于“局部鼓包”，交给⑤缓冲偏移判
            if (chord > 300.0) continue;
            double nx = 0.0, ny = 0.0;
            const double sqr = QgsGeometryUtils::sqrDistToLine(
                p.x(), p.y(), a.x(), a.y(), b.x(), b.y(), nx, ny, 1e-8);
            if (sqr <= 0.0) continue;
            const double sag = std::sqrt(sqr);
            if (sag > bestSag) bestSag = sag;
        }
        if (bestSag <= mp.maxLocalBulge) continue;
        const QgsGeometry pg = QgsGeometry::fromPointXY(QgsPointXY(p.x(), p.y()));
        const double dev = nearestRefDistance(refs, pg);
        if (dev < 0.0 || dev <= mp.maxLocalDeviation) continue;
        ++out.count;
        if (!out.found || bestSag > out.sagitta) {
            out.found = true;
            out.point = QgsPointXY(p.x(), p.y());
            out.sagitta = bestSag;
            out.deviation = dev;
            out.vertexIndex = i;
        }
    }
}

// 递归遍历线的全部部件（综合后道路常被交叉口打断成多段，不能只查最长段）
static void collectCurveBulges(const QgsAbstractGeometry* g, const QList<QgsGeometry>& refs,
                               const MatchParams& mp, LocalBulge& out)
{
    if (!g) return;
    if (const QgsCurve* curve = dynamic_cast<const QgsCurve*>(g)) {
        scanCurveBulges(curve, refs, mp, out);
        return;
    }
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        for (int i = 0; i < gc->numGeometries(); ++i)
            collectCurveBulges(gc->geometryN(i), refs, mp, out);
    }
}

// sameRoad: 同路原始要素；为空时退回只用 bestOrig（无同路标识的兜底）。
// 先用 bestOrig 快速扫一遍，只有确实存在鼓包时才按同路集合复扫，避免全量代价。
static LocalBulge detectLocalBulges(const QgsGeometry& resultGeom,
                                    const QgsGeometry& origGeom,
                                    const QList<QgsGeometry>& sameRoad,
                                    const MatchParams& mp)
{
    LocalBulge out;
    if (resultGeom.isNull() || origGeom.isNull()) return out;
    if (mp.maxLocalBulge <= 0.0) return out;
    QList<QgsGeometry> refs;
    refs.append(origGeom);
    collectCurveBulges(resultGeom.constGet(), refs, mp, out);
    if (out.found && !sameRoad.isEmpty()) {
        QList<QgsGeometry> refs2 = sameRoad;
        refs2.prepend(origGeom);   // bestOrig 必定计入
        LocalBulge out2;
        collectCurveBulges(resultGeom.constGet(), refs2, mp, out2);
        return out2;
    }
    return out;
}

// 取成果线 200 米范围内的同路原始要素（鼓包距离基准）。
// 范围取 200 米是给"道路整体合理偏移 + 局部鼓包"留余量；同路标识仍按
// ELEMID / RN / NAME1 / NAME / TYPE 过滤，避免把邻近另一条路当同路。
static QList<QgsGeometry> collectSameRoadOriginals(
    const QgsVectorLayer& origLayer, const QgsFeature& bestOrig,
    const QVector<QgsFeature>& origFeats, const QHash<QgsFeatureId, int>& origPos,
    const QgsSpatialIndex& origIndex, const QgsGeometry& resultGeom,
    const QString& eid, int origEidIdx, double margin)
{
    QList<QgsGeometry> out;
    if (resultGeom.isNull()) return out;

    // 2026-10-30 P1-2：参考线同时按 ELEMID 和道路标识（RN/NAME1/NAME/TYPE）收，
    // 任一命中即可。原来只用 ELEMID，成果线合并多个原始分段（ELEMID 不同）时
    // 参考线收不全，“缓冲外长度”被系统放大（101 条里 53 条覆盖不足一半）。
    int eidIdx = -1;
    QString eidVal;
    if (!eid.isEmpty() && origEidIdx >= 0) {
        eidIdx = origEidIdx;
        eidVal = eid;
    }
    const RoadIdentity id = identityFromAttrs(origLayer, bestOrig);
    int idIdx = -1;
    QString idVal;
    if (!id.key.isEmpty() && !id.value.isEmpty()) {
        idIdx = (id.key == QStringLiteral("ELEMID"))
            ? origEidIdx : origLayer.fields().lookupField(id.key);
        idVal = id.value;
    }
    if (eidIdx < 0 && idIdx < 0) return out;

    const QgsGeometry around = resultGeom.buffer(margin, 5);
    const QList<QgsFeatureId> ids = origIndex.intersects(around.boundingBox());
    for (const QgsFeatureId oid : ids) {
        const auto it = origPos.constFind(oid);
        if (it == origPos.constEnd()) continue;
        const QgsFeature& of = origFeats.at(it.value());
        if (of.geometry().type() != QgsWkbTypes::LineGeometry) continue;
        bool hit = false;
        if (eidIdx >= 0) {
            const QString v = of.attribute(eidIdx).toString().trimmed();
            if (!v.isEmpty() && v != QStringLiteral("-") && v == eidVal) hit = true;
        }
        if (!hit && idIdx >= 0) {
            const QString v = of.attribute(idIdx).toString().trimmed();
            if (!v.isEmpty() && v != QStringLiteral("-") && v == idVal) hit = true;
        }
        if (!hit) continue;
        out.append(of.geometry());
    }
    return out;
}

// ⑤ 缓冲偏移测量：以"同路原始线"的缓冲并集为基准，计算成果线落在缓冲外的
// 长度及占成果线长的比例。相比质心/端点位移，不受成果线截短、合并、拆分影响：
//   - 同路原始线先合并成多部件线，再一次 buffer（GEOS 会取并集）；
//   - 成果线与缓冲求差，差集长度即"偏离原线超过阈值"的长度；
//   - 差集质心作为定位点。
// tunnelCorridor：隧道线缓冲并集（可空）。原始道路在隧道处被隧道线打断，
//   成果线“道路走隧道”的段落会落在同路原始线缓冲外；这些段落不属于偏移，
//   从差集中剔除（2026-10-30 甲方：中间断了按隧道算，不计偏移）。
// refs 为空（附近找不到同路原始线）时 valid=false，调用方不判⑤。
struct OffsetCheck {
    bool valid = false;
    double outsideLen = 0.0;   // 缓冲外长度（米）
    double ratio = 0.0;        // 缓冲外长度 / 成果线长
    QgsPointXY point;          // 定位点（缓冲外部分质心）
    bool hasPoint = false;
};

static OffsetCheck measureLineOffset(const QgsGeometry& resultGeom,
                                     const QList<QgsGeometry>& refs,
                                     double bufferDist,
                                     const QgsGeometry& tunnelCorridor = QgsGeometry())
{
    OffsetCheck out;
    if (resultGeom.isNull() || refs.isEmpty()) return out;
    const double resultLen = resultGeom.length();
    if (resultLen <= 0.0) return out;

    QgsGeometry refAll;
    for (const QgsGeometry& g : refs) {
        if (g.isNull() || g.isEmpty()) continue;
        refAll = refAll.isNull() ? g : refAll.combine(g);
    }
    if (refAll.isNull() || refAll.isEmpty()) return out;

    // 2026-10-30 P1-2 完整性守卫：参考线总长不到成果线一半，说明同路原始线没
    // 收全（分段标识不同/在范围外），此时“缓冲外长度”不可信，不判⑤。
    double refLen = 0.0;
    for (const QgsGeometry& g : refs)
        refLen += g.length();
    if (refLen < 0.5 * resultLen) return out;   // valid=false

    const QgsGeometry refBuf = refAll.buffer(bufferDist, 8);
    if (refBuf.isNull() || refBuf.isEmpty()) return out;

    QgsGeometry outside = resultGeom.difference(refBuf);
    // 隧道段不计偏移：仅当缓冲外部分与隧道走廊有交集时才做一次求差（省代价）
    if (!outside.isNull() && !outside.isEmpty()
        && !tunnelCorridor.isNull() && !tunnelCorridor.isEmpty()
        && tunnelCorridor.boundingBox().intersects(outside.boundingBox())) {
        const QgsGeometry trimmed = outside.difference(tunnelCorridor);
        if (!trimmed.isNull())
            outside = trimmed;
    }
    out.outsideLen = (outside.isNull() || outside.isEmpty()) ? 0.0 : outside.length();
    out.ratio = qBound(0.0, out.outsideLen / resultLen, 1.0);
    if (!outside.isNull() && !outside.isEmpty()) {
        const QgsGeometry c = outside.centroid();
        if (!c.isNull() && !c.isEmpty()) {
            out.point = c.asPoint();
            out.hasPoint = true;
        }
    }
    out.valid = true;
    return out;
}

// ④定位（面）：对称差=变化区域；返回最大差异块中心+差异面积。
// 顶点总量超限时放弃定位（GEOS 对称差对超大面代价高）。
static bool changeRegionPoint(const QgsGeometry& resultGeom, const QgsGeometry& origGeom,
                              QgsPointXY& pt, double& diffArea)
{
    const int vtx = (resultGeom.constGet() ? resultGeom.constGet()->nCoordinates() : 0)
                  + (origGeom.constGet() ? origGeom.constGet()->nCoordinates() : 0);
    if (vtx > 200000) return false;
    QgsGeometry diff = resultGeom.difference(origGeom);
    const QgsGeometry back = origGeom.difference(resultGeom);
    if (!back.isEmpty())
        diff = diff.isEmpty() ? back : diff.combine(back);
    if (diff.isEmpty() || diff.isNull()) return false;
    diffArea = diff.area();
    QgsGeometry biggest = diff;
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(diff.constGet())) {
        double bestArea = -1.0;
        for (int i = 0; i < gc->numGeometries(); ++i) {
            const QgsGeometry part(gc->geometryN(i)->clone());
            const double a = part.area();
            if (a > bestArea) { bestArea = a; biggest = part; }
        }
    }
    const QgsGeometry c = biggest.centroid();
    if (c.isNull()) return false;
    pt = c.asPoint();
    return true;
}

// ---- 综合前后连通性（断裂）检查数据集索引 ----
// 对数据集中全部线要素建包络框索引（几何不驻留内存，按需取回并限量缓存）；
// 端点距离查询按"小/大两级半径"缓存：小半径（容差）判是否相接，
// 大半径（长断口阈值）量断口有多大。
struct DatasetIndex {
    QgsSpatialIndex index;
    QHash<QgsFeatureId, QPair<QString, QgsFeatureId>> owner; // 内部gid → (图层路径, 要素fid)
    QHash<QString, QHash<QgsFeatureId, QgsFeatureId>> gidOf; // 图层路径 → (要素fid → 内部gid)
    QHash<QString, QgsVectorLayer*> layers;                  // 图层路径 → 已打开图层
    QHash<QgsFeatureId, QgsGeometry> geomCache;              // 内部gid → 要素几何（限量）
    struct Neighbors {
        bool smallDone = false, largeDone = false;
        QList<QgsFeatureId> small, large;   // 小/大半径内候选gid
    };
    QHash<QString, Neighbors> neighborCache;                 // 米级取整坐标 → 候选
    qint64 nextGid = 1;

    ~DatasetIndex() {
        for (QgsVectorLayer* l : qAsConst(layers)) delete l;
    }

    QgsVectorLayer* layerAt(const QString& path)
    {
        auto it = layers.constFind(path);
        if (it != layers.constEnd()) return it.value();
        auto* lyr = new QgsVectorLayer(path, QStringLiteral("m458_idx"), QStringLiteral("ogr"));
        layers.insert(path, lyr);
        return lyr;
    }

    QgsFeatureId gidFor(const QString& path, QgsFeatureId fid) const
    {
        const auto lit = gidOf.constFind(path);
        if (lit == gidOf.constEnd()) return -1;
        return lit.value().value(fid, -1);
    }

    // 几何按需取回（限量缓存，超限整体清空重取，控制内存）
    QgsGeometry geometryOf(QgsFeatureId gid)
    {
        const auto cit = geomCache.constFind(gid);
        if (cit != geomCache.constEnd()) return cit.value();
        const auto oit = owner.constFind(gid);
        if (oit == owner.constEnd()) return QgsGeometry();
        QgsVectorLayer* lyr = layerAt(oit.value().first);
        if (!lyr || !lyr->isValid()) return QgsGeometry();
        const QgsFeature f = lyr->getFeature(oit.value().second);
        if (geomCache.size() > 20000) geomCache.clear();
        geomCache.insert(gid, f.geometry());
        return f.geometry();
    }

    // 半径 radius 内候选（含自身），按点（米级取整）缓存小/大两组
    const QList<QgsFeatureId>& candidatesNear(const QgsPointXY& p, double radius, bool large)
    {
        const QString key = QString::number(qRound64(p.x())) + QLatin1Char('_')
            + QString::number(qRound64(p.y()));
        Neighbors& nb = neighborCache[key];
        bool& done = large ? nb.largeDone : nb.smallDone;
        QList<QgsFeatureId>& vec = large ? nb.large : nb.small;
        if (!done) {
            const QgsRectangle rect(p.x() - radius, p.y() - radius,
                                    p.x() + radius, p.y() + radius);
            vec = index.intersects(rect);
            done = true;
        }
        return vec;
    }

    // 点 p 到"其他线要素"（排除 selfGid，自身要素其他部件不算）的最近距离；
    // 先用 smallR 扫一遍，未命中再扩到 largeR；附近无其他要素返回 -1
    double nearestOtherDistance(const QgsPointXY& p, QgsFeatureId selfGid,
                                double smallR, double largeR)
    {
        const QgsGeometry pg = QgsGeometry::fromPointXY(p);
        double best = -1.0;
        const auto scan = [&](const QList<QgsFeatureId>& cands) {
            for (const QgsFeatureId gid : cands) {
                if (gid == selfGid) continue;
                const QgsGeometry g = geometryOf(gid);
                if (g.isNull()) continue;
                const double d = pg.distance(g);
                if (best < 0.0 || d < best) best = d;
            }
        };
        scan(candidatesNear(p, smallR, false));
        if (best >= 0.0 && best <= smallR) return best;
        if (largeR > smallR) scan(candidatesNear(p, largeR, true));
        return best;
    }
};

// 遍历给定图层路径中的全部线要素建索引（非线图层跳过）
static std::unique_ptr<DatasetIndex> buildDatasetIndex(const QStringList& layerPaths)
{
    auto ds = std::unique_ptr<DatasetIndex>(new DatasetIndex());
    for (const QString& path : layerPaths) {
        QgsVectorLayer* lyr = ds->layerAt(path);
        if (!lyr || !lyr->isValid()) continue;
        if (lyr->geometryType() != QgsWkbTypes::LineGeometry) continue;
        QgsFeatureIterator fit = lyr->getFeatures();
        QgsFeature f;
        while (fit.nextFeature(f)) {
            if (f.geometry().isNull()) continue;
            const QgsFeatureId gid = ds->nextGid++;
            QgsFeature idxFeat;
            idxFeat.setId(gid);
            idxFeat.setGeometry(f.geometry());
            ds->index.addFeature(idxFeat);
            ds->owner.insert(gid, qMakePair(path, f.id()));
            ds->gidOf[path].insert(f.id(), gid);
        }
    }
    return ds;
}

// basename → 路径列表（同名冲突时保留全部，选择时按确定性规则取）
static QHash<QString, QStringList> groupByBase(const QStringList& shps)
{
    QHash<QString, QStringList> byBase;
    for (const QString& p : shps)
        byBase[QFileInfo(p).completeBaseName()].append(p);
    return byBase;
}

// 同名 basename 冲突时选目录最浅者（更接近数据根目录，通常是主数据），
// 深度相同则按路径字典序取首个，保证每次运行结果一致；冲突时经 note 记入日志
static QString pickPath(const QStringList& paths, QString* ambiguousNote = nullptr)
{
    if (paths.isEmpty()) return QString();
    if (paths.size() > 1 && ambiguousNote) {
        QStringList others;
        for (int i = 1; i < paths.size(); ++i) others.append(paths.at(i));
        *ambiguousNote = QStringLiteral("（同名%1个，另见：%2）")
            .arg(paths.size()).arg(others.join(QStringLiteral("; ")));
    }
    QString best = paths.first();
    const auto depthOf = [](const QString& p) {
        return p.count(QLatin1Char('/')) + p.count(QLatin1Char('\\'));
    };
    int bestDepth = depthOf(best);
    for (const QString& p : paths) {
        const int d = depthOf(p);
        if (d < bestDepth || (d == bestDepth && p < best)) {
            bestDepth = d;
            best = p;
        }
    }
    return best;
}

// 成果图层名 → 原始 SHP 路径配对。
// 优先用图层映射表（成果图层名→源图层代码），回退同名/去后缀同名；
// 同名 basename 冲突按"目录最浅优先"确定性规则选取，不再后写覆盖先写。
// origByBase: 原始数据条目（basename 或 GDB 图层代码 → 路径/URI 列表）
static QMap<QString, QString> pairLayers(const QHash<QString, QStringList>& origByBase,
                                         const QStringList& resultShps,
                                         const QHash<QString, QString>* layerMapping,
                                         QStringList* ambiguousNotes)
{
    QStringList resultNames;
    for (const QString& p : resultShps)
        resultNames.append(QFileInfo(p).completeBaseName());

    QMap<QString, QString> pairs; // 成果 basename → 原始路径
    for (const QString& r : resultNames) {
        QString origPath;
        // 1) 图层映射表（支持去后缀回退）
        if (layerMapping) {
            const QString stripped = stripSuffix(r);
            const auto it = layerMapping->constFind(r);
            const auto it2 = layerMapping->constFind(stripped);
            const QString code = (it != layerMapping->constEnd()) ? it.value()
                               : (it2 != layerMapping->constEnd()) ? it2.value() : QString();
            if (!code.isEmpty()) {
                const auto oit = origByBase.constFind(code);
                if (oit != origByBase.constEnd()) {
                    QString note;
                    origPath = pickPath(oit.value(), &note);
                    if (!note.isEmpty() && ambiguousNotes)
                        ambiguousNotes->append(QStringLiteral("[%1] 源图层代码%2存在同名SHP%3")
                                                   .arg(r, code, note));
                }
            }
        }
        // 2) 同名 / 去后缀同名
        if (origPath.isEmpty()) {
            const QStringList& same = origByBase.value(r);
            const QStringList& cand = same.isEmpty()
                ? origByBase.value(stripSuffix(r)) : same;
            if (!cand.isEmpty()) {
                QString note;
                origPath = pickPath(cand, &note);
                if (!note.isEmpty() && ambiguousNotes)
                    ambiguousNotes->append(QStringLiteral("[%1] 原始数据存在同名SHP%2").arg(r, note));
            }
        }
        if (!origPath.isEmpty()) pairs[r] = origPath;
    }
    return pairs;
}

void executeWithMapping(const QString& origDir, const QString& resultDir,
                        int processMode,
                        const QHash<QString, QString>& layerMapping,
                        QList<QPair<QgsFeature, QString>>& allErrors,
                        QStringList& executedChecks,
                        const QString& matchParamPath,
                        int focusMask,
                        QList<FocusError>* focusErrors,
                        const QHash<QString, TrueShiftInfo>* trueShifts)
{
    execute(origDir, resultDir, processMode, allErrors, executedChecks,
            &layerMapping, matchParamPath, focusMask, focusErrors, trueShifts);
}

void execute(const QString& origDir, const QString& resultDir,
             int processMode,
             QList<QPair<QgsFeature, QString>>& allErrors,
             QStringList& executedChecks,
             const QHash<QString, QString>* layerMapping,
             const QString& matchParamPath,
             int focusMask,
             QList<FocusError>* focusErrors,
             const QHash<QString, TrueShiftInfo>* trueShifts)
{
    Q_UNUSED(processMode) // 458 只有模式1（综合前后要素匹配检查），全部执行

    // M460 检查模式：位掩码非0且提供了输出容器时启用（1号位"居民点与道路拓扑"
    // 由对话框用引擎454执行，此处忽略）。启用后：未匹配与未勾选项不再写
    // allErrors（由常规458组输出），M460 项以 FocusError 结构化返回。
    int focus = focusErrors ? (focusMask & ~FocusResidentRoad) : 0;
    // 2026-10-30：按甲方口径，化简检查把水系（河流/渠）也纳入。FocusSimplify 是
    // 内部派生位（不对应界面上任何CheckItem），勾选④变形或⑥拉直任一即自动置位；
    // 河流/渠图层只有携带该位才会执行“线要素化简检查”（见 focusLayerMask）。
    if (focus & (FocusDeform | FocusStraighten)) focus |= FocusSimplify;
    if (focus) {
        QStringList cats;
        if (focus & (FocusBreakLong | FocusBreakShort)) cats << QStringLiteral("断裂/长距离断口");
        if (focus & FocusDeform) cats << QStringLiteral("变形");
        if (focus & FocusDisplace) cats << QStringLiteral("偏移");
        if (focus & FocusStraighten) cats << QStringLiteral("弯曲度");
        if (focus & FocusAttribute) cats << QStringLiteral("属性");
        if (focus & FocusSimplify) cats << QStringLiteral("化简(道路+水系)");
        executedChecks.append(QStringLiteral("综合前后增强检查项：%1").arg(
            cats.isEmpty() ? QStringLiteral("无") : cats.join(QStringLiteral("、"))));
        executedChecks.append(QStringLiteral(
            "M460检查范围：道路/铁路图层参与②~⑦；城市道路/快速路/连接道/铁路(合并层)只参与化简与偏移；"
            "河流/渠图层参与②③断裂、化简检查与⑦线要素属性一致性；"
            "境界图层（省界/市界/县界等）仅参与⑦；其余图层(含注记/代码类、面状地物)不参与，"
            "名单见matchParameter.xml"));
    }

    if (origDir.isEmpty() || resultDir.isEmpty()) {
        executedChecks.append(QStringLiteral("综合前后匹配：需同时提供原始数据与成果数据，本次未执行"));
        return;
    }

    // 匹配参数：优先 matchParameter.xml，缺失时取默认值并在日志中注明
    const MatchParams mp = loadMatchParams(matchParamPath);
    if (!mp.loadedFromFile)
        executedChecks.append(QStringLiteral("综合前后匹配(未找到matchParameter.xml，使用默认参数："
                                             "缓冲%1米/重叠比%2/变化率%3/顶点差%4)")
            .arg(mp.bufferDistance).arg(mp.minOverlapRatio)
            .arg(mp.maxChangeRatio).arg(mp.maxVertexChange));

    // 2026-11-08 ⑤真实移位值：对话框传入「化简后移位前 → 最终成果」逐要素
    // 最大/平均移位；本函数只负责按 resultName#FID 回查，未命中时退回原缓冲法。
    int trueShiftFallback = 0;
    int trueShiftUnmatched = 0;   // 原始数据无来源、但按分阶段数据报出⑤的要素数
    if (trueShifts && !trueShifts->isEmpty()) {
        executedChecks.append(QStringLiteral(
            "⑤真实移位值：已加载%1个成果要素的移位前参照（逐顶点到同路移位前线的最近距离，取最大/平均）")
            .arg(trueShifts->size()));
    }

    // 原始数据支持两种形态：SHP 目录（递归收集）或 FileGDB（枚举图层，层名=图层代码）
    const bool origIsGdb = WujiMissionRunner::isFileGdbSource(origDir);
    const QHash<QString, QStringList> origByBase = origIsGdb
        ? gdbLayersByCode(origDir) : groupByBase(shpPathsIn(origDir));
    const QStringList resultShps = shpPathsIn(resultDir);
    if (origByBase.isEmpty() || resultShps.isEmpty()) {
        executedChecks.append(QStringLiteral("综合前后匹配：无可参与匹配的图层（原始数据%1个图层，成果数据%2个SHP），本次未执行")
                                  .arg(origByBase.size()).arg(resultShps.size()));
        return;
    }

    executedChecks.append((origIsGdb
        ? QStringLiteral("原始数据(FileGDB): %1个图层, 成果数据: %2个图层")
        : QStringLiteral("原始数据: %1个图层, 成果数据: %2个图层"))
        .arg(origByBase.size()).arg(resultShps.size()));

    const QHash<QString, QStringList> resultByBaseGroup = groupByBase(resultShps);
    QMap<QString, QString> resultByBase; // 成果 basename → 路径（同名冲突取目录最浅）
    for (auto git = resultByBaseGroup.constBegin(); git != resultByBaseGroup.constEnd(); ++git)
        resultByBase[git.key()] = pickPath(git.value());

    QStringList ambiguousNotes;
    const QMap<QString, QString> pairs =
        pairLayers(origByBase, resultShps, layerMapping, &ambiguousNotes);
    for (const QString& n : ambiguousNotes)
        executedChecks.append(QStringLiteral("综合前后匹配图层配对提示：%1").arg(n));
    if (pairs.isEmpty()) {
        executedChecks.append(QStringLiteral("综合前后匹配：原始数据与成果数据中未找到可配对图层，本次未执行"));
        return;
    }

    // ---- M460：连通性（断裂）检查数据集索引（全部可配对线图层，综合前后各一份）
    // 只建包络框索引，几何按需取回并限量缓存；②③阈值来自 matchParameter.xml。
    std::unique_ptr<DatasetIndex> origDs, resultDs;
    const bool needConnectivity = (focus & (FocusBreakLong | FocusBreakShort)) != 0;
    if (needConnectivity) {
        // 去重：同一原始图层会被多个成果图层经多头映射重复引用（如 LRDL →
        // 乡道/县道/国道…共 11 个成果图层）。若重复入索引，同一要素会拿到
        // 多个内部 gid，而"排除自身"只能排除其中一个副本，其余副本到自身
        // 端点距离恒为 0 → 这些图层的"综合前间距"恒报 0.0，把原本就不
        // 相接的端点全部误判成"综合前相接、综合后断开"（成片伪报）。
        QStringList origLinePaths, resultLinePaths;
        QSet<QString> seenOrigPaths, seenResultPaths;
        for (auto pit = pairs.constBegin(); pit != pairs.constEnd(); ++pit) {
            const QString rp = resultByBase.value(pit.key());
            if (!rp.isEmpty() && !seenResultPaths.contains(rp)) {
                seenResultPaths.insert(rp);
                resultLinePaths << rp;
            }
            if (!seenOrigPaths.contains(pit.value())) {
                seenOrigPaths.insert(pit.value());
                origLinePaths << pit.value();
            }
        }
        resultDs = buildDatasetIndex(resultLinePaths);
        origDs = buildDatasetIndex(origLinePaths);
    }

    // ---- ②③ 隧道口豁免：隧道线不加入通用连通索引，只把隧道线首尾端点
    //      （隧道口）作为“经隧道连通”的判定参照；道路断口端点落在隧道口
    //      容差内时不报断开。隧道按 CLASID/TYPE 标准字段识别，跨县通用。
    // ---- ⑤ 隧道补充（2026-10-30 甲方口径）：原始道路在隧道处常被隧道线打断
    //      （隧道是独立线要素），成果线经隧道的路段会落在同路原始线缓冲外被
    //      误判为“过度偏移”；把隧道线按同口径缓冲后从缓冲外长度中剔除。
    int tunnelLayers = 0, tunnelFeatures = 0, tunnelExempt = 0;
    QList<QgsPointXY> tunnelPortals;
    QList<QgsGeometry> tunnelLines;
    if (needConnectivity || (focus & FocusDisplace)) {
        tunnelPortals = collectTunnelPortals(resultShps, resultByBase, layerMapping,
                                             tunnelLayers, tunnelFeatures, &executedChecks,
                                             &tunnelLines);
    }
    QgsGeometry tunnelCorridor;   // 隧道线缓冲并集（仅⑤使用，可空）
    if ((focus & FocusDisplace) && !tunnelLines.isEmpty()) {
        QgsGeometry allTunnel;
        for (const QgsGeometry& g : tunnelLines) {
            if (g.isNull() || g.isEmpty()) continue;
            allTunnel = allTunnel.isNull() ? g : allTunnel.combine(g);
        }
        if (!allTunnel.isNull() && !allTunnel.isEmpty())
            tunnelCorridor = allTunnel.buffer(mp.maxDisplacement, 5);
    }

    int totalMatched = 0, totalUnmatched = 0, totalChanged = 0, totalDropped = 0;
    QSet<QString> breakReported;   // 断裂错误去重（综合后端点坐标取整为键）

    int focusPairs = 0;   // M460 模式下实际参与检查的图层数（道路/河流口径）
    for (auto pit = pairs.constBegin(); pit != pairs.constEnd(); ++pit) {
        const QString resultName = pit.key();
        const QString origPath = pit.value();
        const QString resultPath = resultByBase.value(resultName);
        if (resultPath.isEmpty()) continue;

        // M460 范围过滤：道路/铁路查②~⑦；河流/渠查②③+⑦；境界只查⑦；其余图层整对跳过。
        // 名单来自 matchParameter.xml（Road/River/BoundaryLayerKeywords），未配置用内置默认。
        int pairFocus = focus;
        if (focus) {
            pairFocus = focusLayerMask(resultName, mp, focus);
            // 成果同时有「铁路」合并层和「普通铁路/高速铁路」拆分层时，二选一报，
            // 避免同一条线重复出记录。默认“合并层报、拆分层不报”；但若⑤分阶段
            // 数据实际选中的是拆分图层（trueShifts 只有拆分层条目、没有“铁路#”），
            // 说明成果里的 铁路.shp 是未参与移位的原始线，则反过来只让拆分层报。
            const bool railPartLayer = (resultName == QStringLiteral("普通铁路")
                                        || resultName == QStringLiteral("高速铁路"));
            if (resultByBase.contains(QStringLiteral("铁路"))
                && (railPartLayer || resultName == QStringLiteral("铁路"))) {
                bool partsChosen = false;
                if (trueShifts && !trueShifts->isEmpty()) {
                    bool anyPart = false, anyMerged = false;
                    for (auto sit = trueShifts->constBegin();
                         sit != trueShifts->constEnd(); ++sit) {
                        if (sit.key().startsWith(QStringLiteral("普通铁路#"))
                            || sit.key().startsWith(QStringLiteral("高速铁路#")))
                            anyPart = true;
                        else if (sit.key().startsWith(QStringLiteral("铁路#")))
                            anyMerged = true;
                    }
                    partsChosen = anyPart && !anyMerged;
                }
                if (partsChosen) {
                    if (resultName == QStringLiteral("铁路"))
                        pairFocus &= ~(FocusDisplace | FocusSimplify);
                } else if (railPartLayer) {
                    pairFocus &= ~(FocusDisplace | FocusSimplify);
                }
            }
            if (pairFocus == 0) continue;
            focusPairs++;
        }
        // 隧道口豁免只用于道路/铁路断口（河流断口不因邻近道路隧道口而豁免）
        const bool tunnelExemptApplies = nameContainsAny(resultName, mp.roadKeywords);

        QgsVectorLayer origLayer(origPath, QStringLiteral("m458_orig"), QStringLiteral("ogr"));
        QgsVectorLayer resultLayer(resultPath, QStringLiteral("m458_result"), QStringLiteral("ogr"));
        if (!origLayer.isValid() || !resultLayer.isValid()) {
            executedChecks.append(QStringLiteral("[%1] 图层打开失败，本图层未参与匹配").arg(resultName));
            continue;
        }
        // M460 检查②~⑦都基于线要素（道路/河流均为线图层），非线图层不参与
        if (focus && QgsWkbTypes::geometryType(resultLayer.wkbType()) != QgsWkbTypes::LineGeometry)
            continue;

        // 原始要素索引（一次性装入内存做空间查询）
        QgsSpatialIndex origIndex;
        QVector<QgsFeature> origFeats;
        QHash<QgsFeatureId, int> origPos; // fid → origFeats 下标
        {
            QgsFeatureIterator fit = origLayer.getFeatures();
            QgsFeature f;
            while (fit.nextFeature(f)) {
                origIndex.addFeature(f);
                origPos.insert(f.id(), origFeats.size());
                origFeats.append(f);
            }
        }
        if (origFeats.isEmpty()) {
            executedChecks.append(QStringLiteral("[%1] 原始图层无要素，本图层未参与匹配").arg(resultName));
            continue;
        }

        // 成果标识字段（ELEMID 等），用于错误消息定位
        const QString eidField = findField(&resultLayer, QStringList()
            << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
            << QStringLiteral("FormerID"));
        // 原始图层标识字段：ELEMID 一致的候选即为来源要素（优先匹配）
        const QString origEidField = findField(&origLayer, QStringList()
            << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
            << QStringLiteral("FormerID"));
        const int origEidIdx = origEidField.isEmpty() ? -1
            : origLayer.fields().indexOf(origEidField);

        QSet<QgsFeatureId> usedOrigIds;
        int layerMatched = 0, layerUnmatched = 0, layerChanged = 0;
        int layerSimplify = 0;   // ④附加：化简不足条数（单独检查项）

        QgsFeatureIterator rit = resultLayer.getFeatures();
        QgsFeature rf;
        while (rit.nextFeature(rf)) {
            const QgsGeometry rGeom = rf.geometry();
            if (rGeom.isNull()) continue;

            const QString eid = eidField.isEmpty() ? QString()
                : rf.attribute(rf.fields().indexOf(eidField)).toString().trimmed();
            const QString eidText = eid.isEmpty() ? QString() : QStringLiteral(" ELEMID='%1'").arg(eid);

            // 缓冲后按包围盒查询候选原始要素
            const QgsGeometry bufGeom = rGeom.buffer(mp.bufferDistance, 5);
            const QList<QgsFeatureId> candIds =
                origIndex.intersects(bufGeom.boundingBox());

            // 业务标识：成果综合后常按分段输出、ELEMID 只覆盖部分线段（1107
            // 快速路 93 段里仅 3 段能按 OBJECTID/ELEMID 找到同号原始线）。
            // ELEMID 无同号候选时退回 RN/NAME1/NAME，按“同一条路”配对，
            // 避免同一条路的分段被误判成“原始数据中无来源”。
            RoadIdentity resultBizId;
            {
                const QStringList bizKeys = QStringList()
                    << QStringLiteral("RN") << QStringLiteral("NAME1")
                    << QStringLiteral("NAME");
                for (const QString& k : bizKeys) {
                    const QString v = attrText(resultLayer, rf, k);
                    if (!v.isEmpty()) {
                        resultBizId = RoadIdentity{k, v};
                        break;
                    }
                }
            }
            const int resultBizOrigIdx = resultBizId.value.isEmpty() ? -1
                : origLayer.fields().lookupField(resultBizId.key);

            // 候选选取（得分制）：同标识（ELEMID 或 RN/NAME1/NAME）一致者进
            // "同号池"，池内取配对得分最高、得分相同取质心距离最近者；
            // 无同号候选时，在全部候选中同样取最优。
            // 原来"遇到第一个即采用"会抓同一条链上另一段（隔数十公里）、或只是
            // 交叉经过的线当来源——这是位移/断裂/属性误报的根源。
            const QgsPointXY rCentroid = rGeom.centroid().asPoint();
            const double rLen = rGeom.length();
            const QgsWkbTypes::GeometryType rGeomType = rGeom.type();
            const QgsFeature* bestSame = nullptr;
            double bestSameScore = -1.0, bestSameDist = 0.0;
            const QgsFeature* bestAny = nullptr;
            double bestAnyScore = -1.0, bestAnyDist = 0.0;
            QVector<CandRef> candRefs;   // 缓冲内候选（同路合计/化简/鼓包复用）
            for (const QgsFeatureId oid : candIds) {
                const auto posIt = origPos.constFind(oid);
                if (posIt == origPos.constEnd()) continue;
                const QgsFeature& of = origFeats.at(posIt.value());
                const QgsGeometry oGeom = of.geometry();
                // 几何类型守卫：点只配点、线只配线、面只配面。
                // 注记/方向点类成果图层与其父要素共享 ELEMID（方向点↔渠线、
                // 休止符↔铁路线、湖泊点↔湖泊面），同号池优先会让点配到线/面上，
                // 顶点数/质心等指标因量纲不同全部失真（成片伪报的另一根源）。
                if (oGeom.type() != rGeomType) continue;
                double interLen = 0.0;
                const double score = pairScore(bufGeom, rLen, oGeom, &interLen);
                candRefs.append(CandRef{&of, score, interLen});
                const double dist = rCentroid.distance(oGeom.centroid().asPoint());
                bool sameIdentity = (origEidIdx >= 0 && !eid.isEmpty()
                    && of.attribute(origEidIdx).toString().trimmed() == eid);
                // 业务标识兜底要求几何确实擦到缓冲（score>0），避免只落在
                // 缓冲包围盒角落里、实际相距 50 米以上的同路要素被误配。
                if (!sameIdentity && resultBizOrigIdx >= 0 && score > 1e-9) {
                    const QString v = of.attribute(resultBizOrigIdx).toString().trimmed();
                    sameIdentity = (!v.isEmpty() && v != QStringLiteral("-")
                                    && v == resultBizId.value);
                }
                if (sameIdentity) {
                    if (bestSameScore < 0.0 || score > bestSameScore + 1e-9
                        || (score >= bestSameScore - 1e-9 && dist < bestSameDist)) {
                        bestSame = &of;
                        bestSameScore = score;
                        bestSameDist = dist;
                    }
                }
                if (bestAnyScore < 0.0 || score > bestAnyScore + 1e-9
                    || (score >= bestAnyScore - 1e-9 && dist < bestAnyDist)) {
                    bestAny = &of;
                    bestAnyScore = score;
                    bestAnyDist = dist;
                }
            }
            const QgsFeature* bestOrig = bestSame ? bestSame : bestAny;
            const bool eidMatched = (bestSame != nullptr);
            const double bestRatio = eidMatched ? bestSameScore : bestAnyScore;

            if (!bestOrig || (!eidMatched && bestRatio < mp.minOverlapRatio)) {
                // 成果要素在原始数据中找不到来源 → 异常
                // （M460 模式下不重复输出：未匹配由常规「综合前后匹配」组负责）
                totalUnmatched++; layerUnmatched++;
                // 2026-11-08：原始数据中无来源、但「化简后移位前」有参照的成果要素，
                // 仍按真实移位值做⑤判定——不因“未匹配”而漏掉大偏移。
                if (focus && focusErrors && trueShifts
                    && (pairFocus & FocusDisplace)
                    && rGeomType == QgsWkbTypes::LineGeometry) {
                    const TrueShiftInfo shift = trueShifts->value(
                        resultName + QLatin1Char('#') + QString::number(rf.id()));
                    if (shift.valid && shift.maxShift > mp.maxDisplacement) {
                        ++trueShiftUnmatched;
                        QStringList vals;
                        vals << QStringLiteral("真实移位值：最大%1米（平均%2米，阈值%3米）")
                            .arg(shift.maxShift, 0, 'f', 1)
                            .arg(shift.meanShift, 0, 'f', 1)
                            .arg(mp.maxDisplacement, 0, 'f', 1);
                        if (!shift.matchKey.isEmpty())
                            vals << QStringLiteral("匹配依据：%1").arg(shift.matchKey);
                        if (!shift.refLayers.isEmpty())
                            vals << QStringLiteral("移位前参考图层：%1").arg(shift.refLayers);
                        vals << QStringLiteral("逐顶点统计%1个顶点；判据：最大移位>%2米")
                            .arg(shift.vertexCount)
                            .arg(mp.maxDisplacement, 0, 'f', 1);
                        if (shift.fallbackVertices > 0)
                            vals << QStringLiteral("其中%1个顶点标识覆盖不全，按同层就近兜底")
                                .arg(shift.fallbackVertices);
                        vals << QStringLiteral("原始数据中无来源要素，本记录按分阶段数据判定");
                        QgsPointXY locPt;
                        bool hasLoc = false;
                        if (shift.hasPoint) {
                            locPt = shift.point;
                            hasLoc = true;
                            vals << QStringLiteral("最大移位位置（成果侧顶点） %1").arg(locText(locPt));
                        }
                        const QString desc = QStringLiteral(
                            "[%1] 综合前后过度偏移：真实移位最大%2米（阈值%3米）；"
                            "原始数据中无来源要素，按分阶段数据判定%4")
                            .arg(resultName)
                            .arg(shift.maxShift, 0, 'f', 1)
                            .arg(mp.maxDisplacement, 0, 'f', 1)
                            .arg(eidText);
                        focusErrors->append(FocusError{rGeom,
                            QStringLiteral("道路过度偏移"), desc,
                            vals.join(QStringLiteral("; ")), locPt, hasLoc});
                    }
                }
                if (!focus)
                    allErrors.append(qMakePair(rf, QStringLiteral("[%1] 综合前后未匹配：%2缓冲范围内无原始要素来源")
                                                      .arg(resultName, eidText)));
                continue;
            }

            usedOrigIds.insert(bestOrig->id());
            totalMatched++; layerMatched++;

            // ---- M460 检查：综合前后指标（位移/弯曲度/属性/连通性） ----
            const QgsGeometry oGeom = bestOrig->geometry();
            LocalBulge localBulge;   // ④附加：局部鼓包/突刺（仅勾选④时计算）
            OffsetCheck offset;      // ⑤附加：同路原始线缓冲偏移（仅勾选⑤且为线时计算）
            TrueShiftInfo shift;     // ⑤优先：分阶段数据算出的真实移位值（可无效）
            // ⑥ 拉直结果暂存：与④/⑤同要素时并入同一条记录，不再单独输出
            QString straightenItem;
            QString straightenDesc;
            QStringList straightenVals;
            QgsPointXY straightenLoc;
            bool straightenHasLoc = false;
            SameRoadRef sameRoad;    // 同路原始要素合计（化简/顶点变化/鼓包基准）
            if (pairFocus && rGeomType == QgsWkbTypes::LineGeometry
                && (pairFocus & (FocusDeform | FocusStraighten | FocusDisplace | FocusSimplify)))
                sameRoad = collectSameRoadRefs(origLayer, *bestOrig, candRefs,
                                               bufGeom, rLen, eid, origEidIdx);
            if (pairFocus) {
                // 线要素端点（多部件取首尾）及方向翻转对应：位移/断裂检查共用
                QgsPointXY op1, op2, cr1, cr2;
                bool hasPts = (rGeom.type() == QgsWkbTypes::LineGeometry);
                if (hasPts) {
                    QgsPointXY rp1, rp2;
                    hasPts = lineEndpoints(oGeom.constGet(), op1, op2)
                        && lineEndpoints(rGeom.constGet(), rp1, rp2);
                    if (hasPts) {
                        // 综合可能反转线方向：两种配对取总和较小者
                        const bool flip = (op1.distance(rp2) + op2.distance(rp1))
                            < (op1.distance(rp1) + op2.distance(rp2));
                        cr1 = flip ? rp2 : rp1;   // 对应 op1 的成果端点
                        cr2 = flip ? rp1 : rp2;   // 对应 op2 的成果端点
                    }
                }

                // ⑤ 道路过度偏移：以"同路原始线缓冲并集"为基准，成果线落在缓冲外的
                //    长度占比超限即判偏移。不采用质心/端点位移：成果线被截短、
                //    合并或拆分时质心会大幅移动，但线本身并未偏离原线，是此前
                //    偏移误报的主要来源。
                //    附近找不到同路原始线时 valid=false，本要素不判⑤。
                if ((pairFocus & FocusDisplace) && rGeomType == QgsWkbTypes::LineGeometry) {
                    // 2026-11-08 甲方口径：有「化简后移位前 → 最终成果」分阶段数据时，
                    // ⑤只按真实移位值判定，不再退回同路原始线缓冲法；匹配不到移位前
                    // 参照的要素直接不判⑤（由对话框统一记录跳过原因）。
                    // key 与对话框 computeTrueShifts 生成时一致：成果图层基名#FID。
                    if (trueShifts) {
                        shift = trueShifts->value(resultName + QLatin1Char('#')
                                                  + QString::number(rf.id()));
                        if (!shift.valid) ++trueShiftFallback;
                    } else {
                        const double margin = qMax(500.0, mp.maxDisplacement * 2.0);
                        const QList<QgsGeometry> sameRoadNear = collectSameRoadOriginals(
                            origLayer, *bestOrig, origFeats, origPos, origIndex, rGeom,
                            eid, origEidIdx, margin);
                        // 隧道走廊一并传入：成果线经隧道的段落不计入缓冲外长度
                        offset = measureLineOffset(rGeom, sameRoadNear, mp.maxDisplacement,
                                                   tunnelCorridor);
                    }
                }

                // ⑥ 道路拉直异常：弯曲度=线长/首尾直线距离，下降超过阈值报出。
                //    另做“直角拉直（重点）”专项：成果新增近直角拐点（默认70~110°），
                //    而原始同名线在同一位置接近直行（≤30°）时，单独输出、不受
                //    MaxSinuosityDrop 限制（避免被0.5阈值淹没）；同一要素只报专项。
                if (pairFocus & FocusStraighten) {
                    const double s1 = sinuosity(oGeom);
                    const double s2 = sinuosity(rGeom);
                    bool rightAngleReported = false;
                    RightAngleHit ra;
                    if (s1 > 1.0 && s2 > 0.0 && s1 > s2
                        && detectRightAngleStraightening(rGeom, oGeom, mp, ra)) {
                        QStringList vals;
                        vals << QStringLiteral("成果拐点转角 %1°（原始同位置约 %2°），拐点坐标 %3")
                            .arg(ra.resultAngle, 0, 'f', 1)
                            .arg(ra.origAngle, 0, 'f', 1)
                            .arg(locText(ra.corner));
                        vals << QStringLiteral("拐点两侧线段 %1米 / %2米")
                            .arg(ra.seg1, 0, 'f', 1).arg(ra.seg2, 0, 'f', 1);
                        vals << QStringLiteral("弯曲度 %1→%2（下降%3）")
                            .arg(s1, 0, 'f', 2).arg(s2, 0, 'f', 2)
                            .arg(s1 - s2, 0, 'f', 2);
                        straightenItem = QStringLiteral("道路直角拉直（重点）");
                        straightenDesc = QStringLiteral("【直角拉直·重点】综合后新增近直角拐点："
                                                        "成果拐点%1°、原始同位置约%2°，疑被拉直成直角")
                            .arg(ra.resultAngle, 0, 'f', 0)
                            .arg(ra.origAngle, 0, 'f', 0);
                        straightenVals = vals;
                        straightenLoc = ra.corner;
                        straightenHasLoc = true;
                        rightAngleReported = true;
                    }
                    if (!rightAngleReported && s1 > 1.0 && s2 > 0.0
                        && (s1 - s2) > mp.maxSinuosityDrop) {
                        QStringList vals;
                        vals << QStringLiteral("弯曲度 %1→%2（下降%3）")
                            .arg(s1, 0, 'f', 2).arg(s2, 0, 'f', 2)
                            .arg(s1 - s2, 0, 'f', 2);
                        QgsPointXY locPt;
                        bool hasLoc = false;
                        double devDist = 0.0;
                        if (maxDeviationPoint(rGeom, oGeom, locPt, devDist)) {
                            vals << QStringLiteral("最大偏离%1米，位置 %2")
                                .arg(devDist, 0, 'f', 1).arg(locText(locPt));
                            hasLoc = true;
                        }
                        straightenItem = QStringLiteral("道路拉直异常");
                        straightenDesc = QStringLiteral("综合前后弯曲度下降（疑拉直）：%1→%2")
                            .arg(s1, 0, 'f', 2).arg(s2, 0, 'f', 2);
                        straightenVals = vals;
                        straightenLoc = locPt;
                        straightenHasLoc = hasLoc;
                    }
                }

                // ④/⑥ 附加：化简不足（顶点数几乎未减少）。它既属于“变形异常”
                // 也属于“拉直”范畴，勾选④或⑥任一即执行；单独作为“线要素化简检查”
                // 输出，方便按检查项筛选。
                // 2026-10-30 按甲方口径：检查对象扩到水系——河流/渠图层通过派生位
                // FocusSimplify 进入本段（水系不参与④变形/⑤偏移/⑥拉直的其他判据）。
                // 基准口径（2026-10-27 修正）：综合后一个成果要素常合并同一条路的
                // 多个原始分段，按 bestOrig 一对一比较会把“合并后顶点数正常”误报成
                // 化简不足。改为与成果缓冲内“同路原始要素顶点合计”比较；同路原始
                // 要素覆盖成果线不足 50% 时无法判断，跳过不报。
                if (pairFocus & (FocusDeform | FocusStraighten | FocusSimplify)) {
                    const int resultVerts = countVertices(rGeom.constGet());
                    if (rGeomType == QgsWkbTypes::LineGeometry) {
                        const int unionVerts = sameRoad.unionVerts;
                        if (mp.minSimplifyVertexCount > 0 && mp.minVertexReduceRatio > 0.0
                            && sameRoad.coverage >= 0.5
                            && unionVerts >= mp.minSimplifyVertexCount && resultVerts > 0) {
                            const double reduce = double(unionVerts - resultVerts) / double(unionVerts);
                            // 2026-10-30 P0-1：只报“确实减少了、但减少得不够”的；
                            // reduce<0 表示成果顶点反而增加，不属于“化简不足”，不再报。
                            if (reduce >= 0.0 && reduce < mp.minVertexReduceRatio) {
                                ++layerSimplify;
                                const double reducePct = reduce * 100.0;
                                QStringList vals;
                                vals << QStringLiteral("顶点数 同路原始合计%1（成果缓冲内）→成果%2（减少%3%）")
                                    .arg(unionVerts).arg(resultVerts).arg(reducePct, 0, 'f', 1);
                                vals << QStringLiteral("同路原始覆盖成果线%1%，化简要求：顶点减少≥%2%（不足即报）")
                                    .arg(sameRoad.coverage * 100.0, 0, 'f', 0)
                                    .arg(mp.minVertexReduceRatio * 100.0, 0, 'f', 1);
                                focusErrors->append(FocusError{rGeom,
                                    QStringLiteral("线要素化简检查"),
                                    QStringLiteral("[%1] 综合前后化简不足：顶点数 同路原始合计%2（缓冲内）→成果%3（减少%4%，要求减少≥%5%）%6")
                                        .arg(resultName).arg(unionVerts).arg(resultVerts)
                                        .arg(reducePct, 0, 'f', 1)
                                        .arg(mp.minVertexReduceRatio * 100.0, 0, 'f', 1)
                                        .arg(eidText),
                                    vals.join(QStringLiteral("; "))});
                            }
                        }
                    } else {
                        // 非线要素（M460 通常不进入）：沿用原始匹配要素一对一口径
                        const int origVerts = countVertices(oGeom.constGet());
                        if (mp.minSimplifyVertexCount > 0 && mp.minVertexReduceRatio > 0.0
                            && origVerts >= mp.minSimplifyVertexCount && resultVerts > 0) {
                            const double reduce = double(origVerts - resultVerts) / double(origVerts);
                            // 同上：排除成果顶点反而增加（reduce<0）的情况。
                            if (reduce >= 0.0 && reduce < mp.minVertexReduceRatio) {
                                ++layerSimplify;
                                const double reducePct = reduce * 100.0;
                                QStringList vals;
                                vals << QStringLiteral("顶点数 原%1→成果%2（减少%3%）")
                                    .arg(origVerts).arg(resultVerts).arg(reducePct, 0, 'f', 1);
                                vals << QStringLiteral("化简要求：顶点减少≥%1%（不足即报）")
                                    .arg(mp.minVertexReduceRatio * 100.0, 0, 'f', 1);
                                focusErrors->append(FocusError{rGeom,
                                    QStringLiteral("线要素化简检查"),
                                    QStringLiteral("[%1] 综合前后化简不足：顶点数 原%2→成果%3（减少%4%，要求减少≥%5%）%6")
                                        .arg(resultName).arg(origVerts).arg(resultVerts)
                                        .arg(reducePct, 0, 'f', 1)
                                        .arg(mp.minVertexReduceRatio * 100.0, 0, 'f', 1)
                                        .arg(eidText),
                                    vals.join(QStringLiteral("; "))});
                            }
                        }
                    }
                }
                // ④ 附加：局部鼓包/突刺（归“道路变形异常”）
                // 距离基准取同路原始要素（200米范围），避免只与 bestOrig 比较导致误报
                if (pairFocus & FocusDeform) {
                    localBulge = detectLocalBulges(rGeom, oGeom, QList<QgsGeometry>(), mp);
                    if (localBulge.found && rGeomType == QgsWkbTypes::LineGeometry) {
                        const double margin = qMax(200.0, mp.bufferDistance * 2.0);
                        const QList<QgsGeometry> sameRoadNear = collectSameRoadOriginals(
                            origLayer, *bestOrig, origFeats, origPos, origIndex, rGeom,
                            eid, origEidIdx, margin);
                        if (!sameRoadNear.isEmpty())
                            localBulge = detectLocalBulges(rGeom, oGeom, sameRoadNear, mp);
                    }
                }

                // ⑦ 线要素属性一致性：按图层类别只比对约定字段
                //   道路/铁路：NAME、RN、CLASSID(CLASID)、TYPE
                //   境界（省界/市界/县界等）：CLASSID(CLASID)
                //   水系（河流/渠）：TYPE、LEVEL、NAME
                // 字段名单可在 matchParameter.xml 配置；只比对两边图层都存在的字段，
                // 不再比对 ELEMID、OBJECTID、SHAPE_* 等标识/长度字段。
                if (pairFocus & FocusAttribute) {
                    QStringList wanted, catNames;
                    if (nameContainsAny(resultName, mp.roadKeywords)) {
                        wanted = mp.roadAttrFields;
                        catNames << QStringLiteral("道路/铁路");
                    } else if (nameContainsAny(resultName, mp.boundaryKeywords)) {
                        wanted = mp.boundaryAttrFields;
                        catNames << QStringLiteral("境界");
                    } else if (nameContainsAny(resultName, mp.riverKeywords)) {
                        wanted = mp.waterAttrFields;
                        catNames << QStringLiteral("水系");
                    }
                    QStringList diffs, compared;
                    int diffTotal = 0;
                    for (const QString& key : wanted) {
                        const int ri = findFieldIndexCI(resultLayer.fields(), key);
                        const int oi = findFieldIndexCI(origLayer.fields(), key);
                        if (ri < 0 || oi < 0) continue;   // 仅比对两边共有字段
                        const QString fname = resultLayer.fields().at(ri).name();
                        compared << fname;
                        const QString rv = rf.attribute(ri).toString().trimmed();
                        const QString ov = bestOrig->attribute(oi).toString().trimmed();
                        if (rv == ov) continue;
                        diffTotal++;
                        if (diffs.size() < 4)
                            diffs << QStringLiteral("%1: %2→%3").arg(fname,
                                ov.isEmpty() ? QStringLiteral("(空)") : ov,
                                rv.isEmpty() ? QStringLiteral("(空)") : rv);
                    }
                    if (diffTotal > 0) {
                        QString show = diffs.join(QStringLiteral("；"));
                        if (diffTotal > diffs.size())
                            show += QStringLiteral("；…等%1项").arg(diffTotal);
                        const QString scope = compared.isEmpty()
                            ? QString()
                            : QStringLiteral("（%1；比对字段：%2）")
                                  .arg(catNames.join(QStringLiteral("/")),
                                       compared.join(QStringLiteral("/")));
                        focusErrors->append(FocusError{rGeom,
                            QStringLiteral("线要素属性一致性"),
                            QStringLiteral("[%1] 综合前后属性变化%2：%3%4")
                                .arg(resultName, scope, show, eidText),
                            diffs.join(QStringLiteral("; "))});
                    }
                }

                // ②③ 断裂：综合前端点与"其他线"相接（间距≤ConnectTolerance），
                //     综合后断口>MinBreakGap(默认50米)才报；断口>LongGapThreshold 报②，否则报③。
                if (needConnectivity && (pairFocus & (FocusBreakLong | FocusBreakShort))
                    && hasPts && origDs && resultDs) {
                    const QgsFeatureId oGid = origDs->gidFor(origPath, bestOrig->id());
                    const QgsFeatureId rGid = resultDs->gidFor(resultPath, rf.id());
                    const double connTol = mp.connectTolerance;
                    const double longR = qMax(connTol, mp.longGapThreshold);
                    for (int e = 0; e < 2; ++e) {
                        const QgsPointXY& po = (e == 0) ? op1 : op2;
                        const QgsPointXY& pr = (e == 0) ? cr1 : cr2;
                        // 去重：同一断口（综合后端点坐标取整为键）只报一次
                        const QString dkey = QString::number(qRound64(pr.x()))
                            + QLatin1Char('_') + QString::number(qRound64(pr.y()));
                        if (breakReported.contains(dkey)) continue;
                        const double origGap = origDs->nearestOtherDistance(po, oGid, connTol, longR);
                        if (origGap < 0.0 || origGap > connTol) continue;   // 综合前本就未相接
                        const double resGap = resultDs->nearestOtherDistance(pr, rGid, connTol, longR);
                        // 综合后断口≤max(连通容差, MinBreakGap) 视为可接受、不报；
                        // MinBreakGap 默认50米，即断口>50米才报③（>LongGapThreshold归②）。
                        const double resultGapTol = qMax(connTol, mp.minBreakGap);
                        if (resGap >= 0.0 && resGap <= resultGapTol) continue;
                        // 隧道口豁免：成果道路端点落在隧道线端点（隧道口）附近，
                        // 说明道路在此处进入/驶出隧道，属经隧道连通，不报断口。
                        // 只看隧道线首尾端点，不把贴近隧道线中段的端点当作连通。
                        if (tunnelExemptApplies
                            && mp.tunnelPortalTolerance > 0.0 && !tunnelPortals.isEmpty()) {
                            const double portalDist = nearestPortalDistance(tunnelPortals, pr);
                            if (portalDist >= 0.0 && portalDist <= mp.tunnelPortalTolerance) {
                                breakReported.insert(dkey);
                                ++tunnelExempt;
                                continue;
                            }
                        }
                        breakReported.insert(dkey);
                        const bool isLong = (resGap < 0.0) || (resGap > mp.longGapThreshold);
                        const QString itemName = (isLong && (pairFocus & FocusBreakLong))
                            ? QStringLiteral("道路长距离连接")
                            : (!isLong && (pairFocus & FocusBreakShort))
                                ? QStringLiteral("道路·河流断裂") : QString();
                        if (itemName.isEmpty()) continue;   // 只勾选了另一类断口
                        const QString gapText = resGap < 0.0
                            ? QStringLiteral("大于%1").arg(mp.longGapThreshold)
                            : QString::number(resGap, 'f', 1);
                        QStringList vals;
                        vals << QStringLiteral("原间距%1米; 现间距%2米")
                            .arg(origGap, 0, 'f', 1).arg(gapText);
                        vals << QStringLiteral("断口坐标 %1").arg(locText(pr));   // 综合后断开的端点
                        // 断裂按要求输出为“点”要素，点位放在综合后断口处，
                        // 不再把整条成果线写入 综合前后拓扑检查_线.shp。
                        focusErrors->append(FocusError{QgsGeometry::fromPointXY(pr), itemName,
                            QStringLiteral("[%1] 综合前后%2：原相接（端点间距%3米），综合后断开（断口约%4米）%5")
                                .arg(resultName,
                                     isLong ? QStringLiteral("长距离断口") : QStringLiteral("不合理断裂"),
                                     QString::number(origGap, 'f', 1), gapText, eidText),
                            vals.join(QStringLiteral("; ")), pr, true});
                    }
                }
            }

            // 显著变形检测
            double areaChange = 0.0, lengthChange = 0.0;
            int vertexChange = 0;
            compareGeometry(*bestOrig, rf, areaChange, lengthChange, vertexChange);
            // 顶点数变化口径与化简检查一致：成果线合并同路多个原始分段时，
            // 按同路原始合计（缓冲内）作基准；覆盖不足 50% 时退回 bestOrig。
            int vertexChangeForWarn = vertexChange;
            bool unionVertexBaseline = false;
            if (pairFocus && rGeomType == QgsWkbTypes::LineGeometry
                && sameRoad.coverage >= 0.5 && sameRoad.unionVerts > 0) {
                vertexChangeForWarn = countVertices(rGeom.constGet()) - sameRoad.unionVerts;
                unionVertexBaseline = true;
            }
            // ④ 道路变形异常 / ⑤ 道路过度偏移：同一成果要素只输出一条记录，
            // 用“检查项”区分类型，说明中保留顶点数等全部指标。
            //   变形触发：面积/长度变化率超限，或存在局部鼓包；
            //            顶点数单独超限不再触发（同路合并/节点加密误报多），
            //            但顶点数变化仍写入说明与属性值，供甲方核查。
            //   偏移触发：成果线落在“同路原始线缓冲”外的长度占比超限。
            //   两者同时满足时按⑤输出（偏移定位更明确），④指标保留在同一说明中。
            // 2026-10-30：长度变化也按“同路原始合计（合并后）”作基准，
            // 避免成果线合并同路多段时与单条原始段比长度而虚增。
            double lengthChangeForWarn = lengthChange;
            bool unionLengthBaseline = false;
            if (pairFocus && rGeomType == QgsWkbTypes::LineGeometry
                && sameRoad.coverage >= 0.5 && sameRoad.unionLen > 0.0) {
                lengthChangeForWarn = (rLen - sameRoad.unionLen) / sameRoad.unionLen;
                unionLengthBaseline = true;
            }
            const bool areaBad = std::abs(areaChange) > mp.maxChangeRatio;
            // 长度只报“异常变长”：综合化简本身会让线变短，变短不是变形；
            // 变短到丢段属于②③断裂/截断的范畴，不在这里重复报。
            const bool lengthBad = lengthChangeForWarn > mp.maxChangeRatio;
            const bool vertexBad = std::abs(vertexChangeForWarn) > mp.maxVertexChange;
            const bool bulgeBad = localBulge.found;
            const bool deformBad = areaBad || lengthBad || bulgeBad;
            // 有分阶段数据时，⑤只以“最大顶点移位 > MaxDisplacement”判偏移；
            // 没有分阶段数据时才沿用原“缓冲外长度占比 + 最小长度”判据。
            const bool offsetBad = trueShifts
                ? (shift.valid && shift.maxShift > mp.maxDisplacement)
                : (offset.valid
                   && offset.ratio > mp.maxOutsideRatio
                   && offset.outsideLen > qMax(50.0, mp.maxDisplacement * 0.5));
            const bool reportOffset = offsetBad && (pairFocus & FocusDisplace);
            const bool reportDeform = deformBad && (!pairFocus || (pairFocus & FocusDeform));
            // ⑥ 已算出但先不写：若本要素同时命中④/⑤，内容并入同一条记录；
            // 只有未命中④/⑤时才单独输出「道路拉直异常/直角拉直」。
            const bool reportStraighten = !straightenItem.isEmpty()
                && (pairFocus & FocusStraighten);

            QStringList warnings;
            if (areaBad)
                warnings.append(QStringLiteral("面积变化%1%").arg(static_cast<int>(areaChange * 100)));
            if (lengthBad)
                warnings.append(QStringLiteral("长度变化%1%").arg(static_cast<int>(lengthChangeForWarn * 100)));
            if (vertexBad)
                warnings.append(QStringLiteral("顶点数变化%1").arg(vertexChangeForWarn));
            if (bulgeBad)
                warnings.append(QStringLiteral("局部突出%1米（%2处）")
                    .arg(localBulge.sagitta, 0, 'f', 1).arg(localBulge.count));

            if (reportOffset || reportDeform || reportStraighten) {
                totalChanged++; layerChanged++;
                if (pairFocus) {
                    QStringList vals;
                    QStringList messageParts;
                    const bool hasDeformPart = reportOffset || reportDeform;
                    if (reportOffset) {
                        if (shift.valid) {
                            // 真实移位值：直接对比“化简后移位前”和“最终成果”
                            vals << QStringLiteral("真实移位值：最大%1米（平均%2米，阈值%3米）")
                                .arg(shift.maxShift, 0, 'f', 1)
                                .arg(shift.meanShift, 0, 'f', 1)
                                .arg(mp.maxDisplacement, 0, 'f', 1);
                            if (!shift.matchKey.isEmpty())
                                vals << QStringLiteral("匹配依据：%1").arg(shift.matchKey);
                            if (!shift.refLayers.isEmpty())
                                vals << QStringLiteral("移位前参考图层：%1").arg(shift.refLayers);
                            vals << QStringLiteral("逐顶点统计%1个顶点；判据：最大移位>%2米")
                                .arg(shift.vertexCount)
                                .arg(mp.maxDisplacement, 0, 'f', 1);
                            if (shift.fallbackVertices > 0)
                                vals << QStringLiteral("其中%1个顶点标识覆盖不全，按同层就近兜底")
                                    .arg(shift.fallbackVertices);
                            messageParts << QStringLiteral("真实移位最大%1米（阈值%2米）")
                                .arg(shift.maxShift, 0, 'f', 1)
                                .arg(mp.maxDisplacement, 0, 'f', 1);
                        } else {
                            vals << QStringLiteral("缓冲外长度%1米，占成果线长%2%（缓冲%3米，阈值占比>%4%）")
                                .arg(offset.outsideLen, 0, 'f', 1)
                                .arg(offset.ratio * 100.0, 0, 'f', 1)
                                .arg(mp.maxDisplacement, 0, 'f', 0)
                                .arg(mp.maxOutsideRatio * 100.0, 0, 'f', 0);
                            messageParts << QStringLiteral("缓冲外长度%1米（占成果线%2%）")
                                .arg(offset.outsideLen, 0, 'f', 1)
                                .arg(offset.ratio * 100.0, 0, 'f', 1);
                        }
                    }
                    if (hasDeformPart) {
                        messageParts << warnings;
                        vals << warnings;
                        if (unionVertexBaseline)
                            vals << QStringLiteral("顶点数基准：同路原始合计%1（缓冲内），覆盖成果线%2%")
                                .arg(sameRoad.unionVerts)
                                .arg(sameRoad.coverage * 100.0, 0, 'f', 0);
                        if (unionLengthBaseline)
                            vals << QStringLiteral("长度基准：同路原始合计%1米（合并后），覆盖成果线%2%")
                                .arg(sameRoad.unionLen, 0, 'f', 1)
                                .arg(sameRoad.coverage * 100.0, 0, 'f', 0);
                    }
                    if (reportStraighten) {
                        messageParts << straightenDesc;
                        vals.append(straightenVals);
                    }
                    QgsPointXY locPt;
                    bool hasLoc = false;
                    if (reportOffset && shift.valid && shift.hasPoint) {
                        locPt = shift.point;
                        hasLoc = true;
                        vals << QStringLiteral("最大移位位置（成果侧顶点） %1").arg(locText(locPt));
                    } else if (reportOffset && offset.hasPoint) {
                        locPt = offset.point;
                        hasLoc = true;
                        vals << QStringLiteral("偏移位置（缓冲外部分中心） %1").arg(locText(locPt));
                    } else if (reportDeform && bulgeBad) {
                        // 局部鼓包优先定位：鼓包顶点就是异常最直观的位置
                        locPt = localBulge.point;
                        hasLoc = true;
                        vals << QStringLiteral("局部鼓包弦高%1米，距同路原始线%2米，位置 %3")
                            .arg(localBulge.sagitta, 0, 'f', 1)
                            .arg(localBulge.deviation, 0, 'f', 1)
                            .arg(locText(locPt));
                    } else if (reportDeform && rGeom.type() == QgsWkbTypes::LineGeometry) {
                        // 线：最大偏离点（沿成果线采样，距原始线最远处）
                        double devDist = 0.0;
                        if (maxDeviationPoint(rGeom, oGeom, locPt, devDist)) {
                            vals << QStringLiteral("最大偏离%1米，位置 %2")
                                .arg(devDist, 0, 'f', 1).arg(locText(locPt));
                            hasLoc = true;
                        }
                    } else if (reportDeform && rGeom.type() == QgsWkbTypes::PolygonGeometry) {
                        // 面：对称差=变化区域，取最大差异块中心
                        double diffArea = 0.0;
                        if (changeRegionPoint(rGeom, oGeom, locPt, diffArea)) {
                            vals << QStringLiteral("变化区域约%1平方米，最大差异块中心 %2")
                                .arg(diffArea, 0, 'f', 0).arg(locText(locPt));
                            hasLoc = true;
                        }
                    } else if (reportStraighten && straightenHasLoc) {
                        locPt = straightenLoc;
                        hasLoc = true;
                    }
                    // ④/⑤ 定位失败时，用⑥的拐点/最大偏离点兜底
                    if (!hasLoc && reportStraighten && straightenHasLoc) {
                        locPt = straightenLoc;
                        hasLoc = true;
                    }
                    QString itemName, title;
                    if (reportOffset) {
                        itemName = QStringLiteral("道路过度偏移");
                        title = QStringLiteral("综合前后过度偏移");
                    } else if (reportDeform) {
                        itemName = QStringLiteral("道路变形异常");
                        title = QStringLiteral("综合前后变形");
                    } else {
                        itemName = straightenItem;
                    }
                    const QString desc = title.isEmpty()
                        ? QStringLiteral("[%1] %2%3").arg(resultName, messageParts.join(QStringLiteral("; ")), eidText)
                        : QStringLiteral("[%1] %2：%3%4")
                            .arg(resultName, title, messageParts.join(QStringLiteral("; ")), eidText);
                    focusErrors->append(FocusError{rGeom, itemName, desc,
                        vals.join(QStringLiteral("; ")), locPt, hasLoc});
                } else {
                    allErrors.append(qMakePair(rf, QStringLiteral("[%1] 综合前后变形：%2%3")
                                                      .arg(resultName, warnings.join(QStringLiteral("; ")), eidText)));
                }
            }
        }

        // 原始要素被综合掉（无成果要素覆盖）只统计，不报错——综合会按选取指标舍弃要素
        totalDropped += origFeats.size() - usedOrigIds.size();

        executedChecks.append(QStringLiteral("[%1] 匹配%2/未匹配%3/显著变形%4/化简不足%5 (缓冲%6米)")
                                  .arg(resultName)
                                  .arg(layerMatched).arg(layerUnmatched).arg(layerChanged)
                                  .arg(layerSimplify).arg(mp.bufferDistance));
    }

    executedChecks.append(QStringLiteral("综合前后匹配汇总: %1个可比图层%2, %3条匹配, %4条未匹配, %5条显著变形, %6条原始要素被综合")
                              .arg(focus ? focusPairs : pairs.size())
                              .arg(focus ? QStringLiteral("（M460口径：道路/河流，其余图层未参与）") : QString())
                              .arg(totalMatched).arg(totalUnmatched).arg(totalChanged).arg(totalDropped));
    if (needConnectivity && !tunnelPortals.isEmpty())
        executedChecks.append(QStringLiteral(
            "隧道断口豁免：%1个道路端点位于隧道口（容差%2米），已按经隧道连通处理")
            .arg(tunnelExempt).arg(mp.tunnelPortalTolerance, 0, 'f', 1));
    if (trueShifts && !trueShifts->isEmpty() && trueShiftFallback > 0)
        executedChecks.append(QStringLiteral(
            "⑤真实移位值：%1个成果要素无移位前参照，已退回缓冲法判定").arg(trueShiftFallback));
    if (trueShiftUnmatched > 0)
        executedChecks.append(QStringLiteral(
            "⑤道路过度偏移：%1个成果要素综合前后未匹配原始来源，已按真实移位值报出")
            .arg(trueShiftUnmatched));
}

// ================= ① 居民点与道路拓扑（M460） =================
// 2026-10-27 起原“居民点必须压盖道路”规则取消，①改为“村道/乡道端点必须连接
// 道路或被居民点压盖”，实现见下方 buildRoadEndpointFocusErrors。
// 本段保留原始图层定位/标识归一化工具，供左右换边判定使用。

// 原始数据中按图层代码定位图层：FileGDB 用内部图层名（即源代码），
// SHP 目录递归按文件名匹配（均大小写不敏感）；代码未命中时回退用成果图层名再试。
static QStringList origLayerUris(const QString& origDataPath, const QString& code,
                                 const QString& fallbackName)
{
    QStringList out;
    if (origDataPath.isEmpty() || (code.isEmpty() && fallbackName.isEmpty()))
        return out;
    if (QFileInfo(origDataPath).fileName().endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive)) {
        const QHash<QString, QStringList> byCode = gdbLayersByCode(origDataPath);
        const auto lookup = [&byCode](const QString& key) -> QStringList {
            if (key.isEmpty()) return QStringList();
            const auto it = byCode.constFind(key);
            if (it != byCode.constEnd()) return it.value();
            for (auto i = byCode.constBegin(); i != byCode.constEnd(); ++i) {
                if (i.key().compare(key, Qt::CaseInsensitive) == 0) return i.value();
            }
            return QStringList();
        };
        out = lookup(code);
        if (out.isEmpty()) out = lookup(fallbackName);
        return out;
    }
    const QStringList shps = shpPathsIn(origDataPath);
    const auto matchName = [&shps](const QString& name) -> QStringList {
        QStringList m;
        if (name.isEmpty()) return m;
        for (const QString& p : shps) {
            if (QFileInfo(p).completeBaseName().compare(name, Qt::CaseInsensitive) == 0)
                m.append(p);
        }
        return m;
    };
    out = matchName(code);
    if (out.isEmpty()) out = matchName(fallbackName);
    return out;
}

// 标识字段值归一化：DBF 数字型 ELEMID 经 QVariant 转字符串可能出现 "1e+09"，
// 统一按整数文本比较，保证综合前后两侧匹配得上（非数字值原样保留）
static QString normalizeEid(const QVariant& v)
{
    const QString s = v.toString().trimmed();
    bool ok = false;
    const qlonglong n = s.toLongLong(&ok);
    if (ok) return QString::number(n);
    const double d = s.toDouble(&ok);
    if (ok) return QString::number(static_cast<qlonglong>(d));
    return s;
}

// ================= ① 村道/乡道端点连接（M460 专用） =================
// 2026-10-27 按甲方反馈替换原“居民点必须压盖道路”规则：
//   村道/乡道线要素的每个折线端点，必须满足其一：
//     1) 距其他道路（成果侧全部道路图层的任一部件）≤ EndpointBuffer
//        （matchParameter.xml，默认 0.001 米 = 1mm）；
//     2) 距成果居民点任一要素 ≤ EndpointBuffer（端点被居民点压盖）。
//   闭合折线（首尾点重合）不判定；只排除端点所在的那一个部件，
//   同一要素的其他部件仍作为参照道路（多部件道路内部相接不算断头）。
//   另外：端点位于数据外框或县界/市界/省界附近（BoundaryTolerance）时，
//   视为按数据范围裁剪产生的边界端点，不判错（只记日志）。
//   检出结果输出为点（端点位置），检查项沿用“居民点与道路拓扑”。

struct RoadPartRef {
    QString layerName;
    QgsFeatureId featureId = -1;
    int partIndex = 0;
    QgsGeometry geom;
};

// 取线要素的全部折线部件（单部件线也返回一项；非线要素返回空）
static QgsMultiPolylineXY lineParts(const QgsGeometry& g)
{
    if (g.isNull() || g.type() != QgsWkbTypes::LineGeometry)
        return QgsMultiPolylineXY();
    return g.asMultiPolyline();
}

void buildRoadEndpointFocusErrors(
    const QList<QgsVectorLayer*>& subjectLayers,
    const QList<QgsVectorLayer*>& roadLayers,
    const QList<QgsVectorLayer*>& residentLayers,
    const QList<QgsVectorLayer*>& boundaryLayers,
    const QString& matchParamPath,
    QList<FocusError>& out, QStringList* log)
{
    const MatchParams mp = loadMatchParams(matchParamPath);
    const double buf = qMax(0.0, mp.endpointBuffer);
    const double queryBuf = qMax(buf, 1e-6);   // 0 缓冲时仍用极小矩形取候选
    const double bndTol = qMax(0.0, mp.boundaryTolerance);

    // ---- 边界端点参照：数据外框（检查目标图层范围）+ 行政界线 ----
    // 数据按图幅/行政区裁剪时，边界处的道路端点天然接不到东西，属正常截断，
    // 不作为“道路断点”报错（2026-10-27 按反馈增加）。
    QgsRectangle subjectBbox;
    bool hasBbox = false;
    for (QgsVectorLayer* lyr : subjectLayers) {
        if (!lyr) continue;
        const QgsRectangle r = lyr->extent();
        if (r.isNull() || r.isEmpty()) continue;
        if (!hasBbox) { subjectBbox = r; hasBbox = true; }
        else subjectBbox.combineExtentWith(r);
    }

    QgsSpatialIndex boundaryIndex;
    QHash<QgsFeatureId, QgsGeometry> boundaryParts;
    QgsFeatureId nextBndId = 1;
    int boundaryLineCount = 0;
    if (bndTol > 0.0) {
        for (QgsVectorLayer* lyr : boundaryLayers) {
            if (!lyr) continue;
            QgsFeatureIterator it = lyr->getFeatures();
            QgsFeature f;
            while (it.nextFeature(f)) {
                const QgsGeometry g = f.geometry();
                if (g.isNull() || g.type() != QgsWkbTypes::LineGeometry) continue;
                const QgsFeatureId id = nextBndId++;
                boundaryParts.insert(id, g);
                boundaryIndex.addFeature(id, g.boundingBox());
                ++boundaryLineCount;
            }
        }
    }

    // 端点是否被数据范围截断（外框或行政界线附近）
    const auto nearDataBoundary = [&](const QgsPointXY& p) -> bool {
        if (bndTol <= 0.0) return false;
        if (hasBbox) {
            if (p.x() - subjectBbox.xMinimum() <= bndTol
                || subjectBbox.xMaximum() - p.x() <= bndTol
                || p.y() - subjectBbox.yMinimum() <= bndTol
                || subjectBbox.yMaximum() - p.y() <= bndTol)
                return true;
        }
        if (boundaryLineCount == 0) return false;
        const QgsRectangle rect(p.x() - bndTol, p.y() - bndTol,
                                p.x() + bndTol, p.y() + bndTol);
        const QList<QgsFeatureId> cand = boundaryIndex.intersects(rect);
        if (cand.isEmpty()) return false;
        const QgsGeometry pg = QgsGeometry::fromPointXY(p);
        for (const QgsFeatureId id : cand) {
            const auto bit = boundaryParts.constFind(id);
            if (bit == boundaryParts.constEnd()) continue;
            if (pg.distance(bit.value()) <= bndTol) return true;
        }
        return false;
    };

    // ---- 参照道路：按“部件”建空间索引；同一要素的不同部件互为参照 ----
    QgsSpatialIndex roadIndex;
    QHash<QgsFeatureId, RoadPartRef> roadParts;
    QgsFeatureId nextRoadId = 1;
    int roadPartCount = 0;
    for (QgsVectorLayer* lyr : roadLayers) {
        if (!lyr) continue;
        const QString base = QFileInfo(lyr->source()).completeBaseName();
        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsMultiPolylineXY parts = lineParts(f.geometry());
            for (int pi = 0; pi < parts.size(); ++pi) {
                const QgsPolylineXY& part = parts.at(pi);
                if (part.size() < 2) continue;
                const QgsGeometry pg = QgsGeometry::fromPolylineXY(part);
                if (pg.isNull()) continue;
                const QgsFeatureId id = nextRoadId++;
                RoadPartRef ref;
                ref.layerName = base;
                ref.featureId = f.id();
                ref.partIndex = pi;
                ref.geom = pg;
                roadParts.insert(id, ref);
                roadIndex.addFeature(id, pg.boundingBox());
                ++roadPartCount;
            }
        }
    }

    // ---- 参照居民点：按点建空间索引（nearestNeighbor 取最近点） ----
    QgsSpatialIndex resIndex;
    QHash<QgsFeatureId, QPair<QgsPointXY, QString>> resPts;
    QgsFeatureId nextResId = 1;
    for (QgsVectorLayer* lyr : residentLayers) {
        if (!lyr) continue;
        const QString base = QFileInfo(lyr->source()).completeBaseName();
        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsGeometry g = f.geometry();
            if (g.isNull() || g.type() != QgsWkbTypes::PointGeometry) continue;
            const QgsPointXY p = g.asPoint();
            const QgsFeatureId id = nextResId++;
            resPts.insert(id, qMakePair(p, base));
            resIndex.addFeature(id, QgsRectangle(p.x(), p.y(), p.x(), p.y()));
        }
    }

    // 最近参照道路（排除指定部件）；找不到其他道路时返回 false
    const auto nearestRoad = [&](const QgsPointXY& p, const QString& exLayer,
                                 QgsFeatureId exFid, int exPart,
                                 double& dist, QString& label) -> bool {
        const QList<QgsFeatureId> ids = roadIndex.nearestNeighbor(p, 12);
        const QgsGeometry pg = QgsGeometry::fromPointXY(p);
        bool found = false;
        dist = std::numeric_limits<double>::max();
        label.clear();
        for (const QgsFeatureId id : ids) {
            const auto it = roadParts.constFind(id);
            if (it == roadParts.constEnd()) continue;
            const RoadPartRef& ref = it.value();
            if (ref.layerName == exLayer && ref.featureId == exFid
                && ref.partIndex == exPart)
                continue;
            const double d = pg.distance(ref.geom);
            if (d < dist) { dist = d; label = ref.layerName; found = true; }
        }
        if (!found) dist = -1.0;
        return found;
    };

    int endpointCount = 0;
    int errorCount = 0;
    int boundarySkipped = 0;
    QStringList subjectNames;
    for (QgsVectorLayer* lyr : subjectLayers) {
        if (!lyr) continue;
        const QString base = QFileInfo(lyr->source()).completeBaseName();
        if (!subjectNames.contains(base)) subjectNames << base;
        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsMultiPolylineXY parts = lineParts(f.geometry());
            for (int pi = 0; pi < parts.size(); ++pi) {
                const QgsPolylineXY& part = parts.at(pi);
                if (part.size() < 2) continue;
                const QgsPointXY ends[2] = { part.first(), part.last() };
                // 闭合折线首尾重合：是一个闭合环，端点不存在，不判定
                if (ends[0].x() == ends[1].x() && ends[0].y() == ends[1].y())
                    continue;
                for (int e = 0; e < 2; ++e) {
                    const QgsPointXY p = ends[e];
                    ++endpointCount;
                    const QgsGeometry pg = QgsGeometry::fromPointXY(p);

                    // 1) 是否连接其他道路：先用缓冲矩形取候选，再精确比较距离
                    bool connected = false;
                    double roadDist = -1.0;
                    QString roadLabel;
                    const QgsRectangle rect(p.x() - queryBuf, p.y() - queryBuf,
                                            p.x() + queryBuf, p.y() + queryBuf);
                    const QList<QgsFeatureId> cand = roadIndex.intersects(rect);
                    for (const QgsFeatureId id : cand) {
                        const auto rit = roadParts.constFind(id);
                        if (rit == roadParts.constEnd()) continue;
                        const RoadPartRef& ref = rit.value();
                        if (ref.layerName == base && ref.featureId == f.id()
                            && ref.partIndex == pi)
                            continue;   // 端点自身所在部件不作参照
                        const double d = pg.distance(ref.geom);
                        if (d <= buf + 1e-9) { connected = true; break; }
                        if (roadDist < 0.0 || d < roadDist) {
                            roadDist = d;
                            roadLabel = ref.layerName;
                        }
                    }
                    if (!connected && roadDist < 0.0) {
                        double d = -1.0;
                        QString label;
                        if (nearestRoad(p, base, f.id(), pi, d, label)) {
                            roadDist = d;
                            roadLabel = label;
                        }
                    }
                    if (connected) continue;

                    // 2) 是否被居民点压盖
                    bool covered = false;
                    double resDist = -1.0;
                    QString resLabel;
                    const QList<QgsFeatureId> rids = resIndex.nearestNeighbor(p, 1);
                    if (!rids.isEmpty()) {
                        const auto sit = resPts.constFind(rids.first());
                        if (sit != resPts.constEnd()) {
                            const QgsPointXY rp = sit.value().first;
                            const double dx = rp.x() - p.x();
                            const double dy = rp.y() - p.y();
                            resDist = std::sqrt(dx * dx + dy * dy);
                            resLabel = sit.value().second;
                            if (resDist <= buf + 1e-9) covered = true;
                        }
                    }
                    if (covered) continue;

                    // 3) 被数据范围截断的边界端点不报错
                    if (nearDataBoundary(p)) {
                        ++boundarySkipped;
                        continue;
                    }

                    // ---- 未连接也未压盖：输出端点错误 ----
                    ++errorCount;
                    const QString roadText = roadDist >= 0.0
                        ? QStringLiteral("距最近道路%1米（%2）")
                              .arg(roadDist, 0, 'f', 3).arg(roadLabel)
                        : QStringLiteral("无其他道路可参照");
                    const QString resText = resDist >= 0.0
                        ? QStringLiteral("距最近居民点%1米（%2）")
                              .arg(resDist, 0, 'f', 3).arg(resLabel)
                        : QStringLiteral("无居民点可参照");
                    const QString desc = QStringLiteral(
                        "[%1] 道路端点未连接其他道路且未被居民点压盖：%2，%3（缓冲%4米）")
                        .arg(base, roadText, resText,
                             QString::number(buf, 'f', 3));
                    QStringList vals;
                    vals << QStringLiteral("端点：%1").arg(e == 0
                        ? QStringLiteral("起点") : QStringLiteral("终点"));
                    vals << QStringLiteral("要素FID=%1 部件%2").arg(f.id()).arg(pi + 1);
                    vals << (roadDist >= 0.0
                        ? QStringLiteral("距最近道路%1米（%2）")
                              .arg(roadDist, 0, 'f', 3).arg(roadLabel)
                        : QStringLiteral("无其他道路可参照"));
                    vals << (resDist >= 0.0
                        ? QStringLiteral("距最近居民点%1米（%2）")
                              .arg(resDist, 0, 'f', 3).arg(resLabel)
                        : QStringLiteral("无居民点可参照"));
                    vals << QStringLiteral("端点坐标 %1").arg(locText(p));
                    out.append(FocusError{QgsGeometry::fromPointXY(p),
                        QStringLiteral("居民点与道路拓扑"), desc,
                        vals.join(QStringLiteral("; ")), p, true});
                }
            }
        }
    }

    if (log) {
        log->append(QStringLiteral(
            "村道/乡道端点连接：检查村道/乡道图层%1个（%2），道路参照部件%3个、居民点%4个；"
            "端点%5个，检出未连接%6处（缓冲%7米），边界端点跳过%8个"
            "（数据外框/行政界线%9条，容差%10米）")
            .arg(subjectLayers.size())
            .arg(subjectNames.isEmpty() ? QStringLiteral("无") : subjectNames.join(QStringLiteral("/")))
            .arg(roadPartCount).arg(resPts.size())
            .arg(endpointCount).arg(errorCount)
            .arg(QString::number(buf, 'f', 3))
            .arg(boundarySkipped).arg(boundaryLineCount)
            .arg(QString::number(bndTol, 'f', 1)));
        if (residentLayers.isEmpty())
            log->append(QStringLiteral(
                "村道/乡道端点连接：未找到居民点图层，本次仅按道路连接判定"));
        if (roadPartCount == 0)
            log->append(QStringLiteral(
                "村道/乡道端点连接：未找到道路参照图层，全部端点按未连接处理"));
    }
}

// ================= ① 居民点道路左右换边 =================
// 左右侧由 QgsGeometry::closestSegmentWithContext 的 leftOrRight 输出确定
// （<0 左、>0 右、0 失败）。综合可能反转道路方向，比较前按首尾端点对应关系
// 统一成果线方向；只有“同一条道路”上的前后侧别才可比。

// 成果/原始道路的空间索引（要素数可达数万，逐点全量扫描太慢）。
// 除 ELEMID 外还带 RN/NAME1/NAME/TYPE：综合后的国道/省道/县道/乡道成果图层
// 没有 ELEMID，只能靠这些业务属性判断"同一条路"。
struct RoadEntry {
    QString name;
    QString eid;
    QString rn;
    QString name1;
    QString nameAttr;
    QString type;
    QgsGeometry geom;
};

// 道路同路标识：ELEMID 单独处理；属性按 RN > NAME1 > NAME > TYPE 取第一个非空
static void roadIdentity(const RoadEntry& e, QString& key, QString& val)
{
    const auto clean = [](const QString& s) {
        const QString t = s.trimmed();
        return (t.isEmpty() || t == QStringLiteral("-")) ? QString() : t;
    };
    val = clean(e.rn);
    if (!val.isEmpty()) { key = QStringLiteral("RN"); return; }
    val = clean(e.name1);
    if (!val.isEmpty()) { key = QStringLiteral("NAME1"); return; }
    val = clean(e.nameAttr);
    if (!val.isEmpty()) { key = QStringLiteral("NAME"); return; }
    val = clean(e.type);
    if (!val.isEmpty()) { key = QStringLiteral("TYPE"); return; }
    key.clear();
}

struct RoadLookup {
    QList<RoadEntry> entries;
    QHash<QString, int> byEid;   // ELEMID → entries 下标（同一ELEMID取首个）
    QgsSpatialIndex index;

    void add(const QString& name, const QString& eid, const QgsGeometry& geom,
             const QString& rn = QString(), const QString& name1 = QString(),
             const QString& nameAttr = QString(), const QString& type = QString())
    {
        if (geom.isNull()) return;
        const int id = entries.size();
        entries.append(RoadEntry{name, eid, rn, name1, nameAttr, type, geom});
        if (!eid.isEmpty() && !byEid.contains(eid))
            byEid.insert(eid, id);
        QgsFeature f;
        f.setId(id);
        f.setGeometry(geom);
        index.addFeature(f);
    }

    // 按 ELEMID 找同一条道路（找原始侧对应道路用，避免在交叉口误配到另一条路）
    const RoadEntry* findEid(const QString& eid) const
    {
        if (eid.isEmpty()) return nullptr;
        const auto it = byEid.constFind(eid);
        if (it == byEid.constEnd()) return nullptr;
        const int id = it.value();
        if (id < 0 || id >= entries.size()) return nullptr;
        return &entries.at(id);
    }

    // 返回距 p 最近且 ≤maxDist 的道路；best 指向 entries 内部元素（只读）
    bool nearest(const QgsPointXY& p, double maxDist,
                 const RoadEntry*& best, double& dist) const
    {
        best = nullptr;
        dist = -1.0;
        if (maxDist <= 0.0 || entries.isEmpty()) return false;
        const QgsRectangle box(p.x() - maxDist, p.y() - maxDist,
                               p.x() + maxDist, p.y() + maxDist);
        const QList<QgsFeatureId> ids = index.intersects(box);
        if (ids.isEmpty()) return false;
        const QgsGeometry pg = QgsGeometry::fromPointXY(p);
        for (const QgsFeatureId id : ids) {
            if (id < 0 || id >= entries.size()) continue;
            const RoadEntry& e = entries.at(static_cast<int>(id));
            const double d = pg.distance(e.geom);
            if (d <= maxDist && (dist < 0.0 || d < dist)) {
                dist = d;
                best = &e;
            }
        }
        return best != nullptr;
    }

    // 同 nearest，但只考虑同路标识（key=value）的道路：原始侧找对应道路时
    // 不能直接在全部道路里取最近者，否则交叉口/邻近的另一条路会冒充同路。
    bool nearestMatching(const QgsPointXY& p, double maxDist,
                         const QString& key, const QString& val,
                         const RoadEntry*& best, double& dist) const
    {
        best = nullptr;
        dist = -1.0;
        if (maxDist <= 0.0 || entries.isEmpty() || key.isEmpty() || val.isEmpty())
            return false;
        const QgsRectangle box(p.x() - maxDist, p.y() - maxDist,
                               p.x() + maxDist, p.y() + maxDist);
        const QList<QgsFeatureId> ids = index.intersects(box);
        if (ids.isEmpty()) return false;
        const QgsGeometry pg = QgsGeometry::fromPointXY(p);
        for (const QgsFeatureId id : ids) {
            if (id < 0 || id >= entries.size()) continue;
            const RoadEntry& e = entries.at(static_cast<int>(id));
            QString eKey, eVal;
            roadIdentity(e, eKey, eVal);
            if (eKey != key || eVal != val) continue;
            const double d = pg.distance(e.geom);
            if (d <= maxDist && (dist < 0.0 || d < dist)) {
                dist = d;
                best = &e;
            }
        }
        return best != nullptr;
    }
};

// 点在线上：返回 -1 左 / +1 右 / 0 失败；dist 输出点到线的距离（米）
static int pointSideOfLine(const QgsGeometry& line, const QgsPointXY& p, double& dist)
{
    dist = -1.0;
    if (line.isNull()) return 0;
    QgsPointXY nearest;
    int nextVertex = -1;
    int leftOrRight = 0;
    const double d2 = line.closestSegmentWithContext(p, nearest, nextVertex, &leftOrRight);
    if (d2 < 0.0 || leftOrRight == 0) return 0;
    dist = p.distance(nearest);
    return leftOrRight < 0 ? -1 : 1;
}

// 线上离 p 最近的一段（含各部件），用于比较综合前后道路的局部走向
struct SegmentRef {
    bool found = false;
    QgsPointXY a, b;
    double sqr = -1.0;
};

static void scanCurveSegments(const QgsCurve* curve, const QgsPoint& p, SegmentRef& out)
{
    if (!curve || curve->numPoints() < 2) return;
    QgsPointSequence pts;
    curve->points(pts);
    for (int i = 0; i + 1 < pts.size(); ++i) {
        const QgsPoint& a = pts.at(i);
        const QgsPoint& b = pts.at(i + 1);
        double nx = 0.0, ny = 0.0;
        const double sqr = QgsGeometryUtils::sqrDistToLine(
            p.x(), p.y(), a.x(), a.y(), b.x(), b.y(), nx, ny, 1e-8);
        if (sqr < 0.0) continue;
        if (!out.found || sqr < out.sqr) {
            out.found = true;
            out.sqr = sqr;
            out.a = QgsPointXY(a.x(), a.y());
            out.b = QgsPointXY(b.x(), b.y());
        }
    }
}

static void collectSegmentRefs(const QgsAbstractGeometry* g, const QgsPoint& p, SegmentRef& out)
{
    if (!g) return;
    if (const QgsCurve* curve = dynamic_cast<const QgsCurve*>(g)) {
        scanCurveSegments(curve, p, out);
        return;
    }
    if (const QgsGeometryCollection* gc = dynamic_cast<const QgsGeometryCollection*>(g)) {
        for (int i = 0; i < gc->numGeometries(); ++i)
            collectSegmentRefs(gc->geometryN(i), p, out);
    }
}

// 两条线在各自点附近的局部走向夹角余弦（-1~1）。成果道路常被路口打断成
// 多段，用整条线首尾端点判断方向会把分段道路判反，这里只看最近线段。
// 返回 0 表示无法判断（缺线段/零长）。
static double localDirectionCos(const QgsGeometry& origGeom, const QgsPoint& origPt,
                                const QgsGeometry& resultGeom, const QgsPoint& resultPt)
{
    SegmentRef os, rs;
    collectSegmentRefs(origGeom.constGet(), origPt, os);
    collectSegmentRefs(resultGeom.constGet(), resultPt, rs);
    if (!os.found || !rs.found) return 0.0;
    const double ax = os.b.x() - os.a.x(), ay = os.b.y() - os.a.y();
    const double bx = rs.b.x() - rs.a.x(), by = rs.b.y() - rs.a.y();
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (la <= 1e-9 || lb <= 1e-9) return 0.0;
    return (ax * bx + ay * by) / (la * lb);
}

// 两条线在各自点附近的局部走向是否相反
static bool localDirectionsOpposite(const QgsGeometry& origGeom, const QgsPoint& origPt,
                                    const QgsGeometry& resultGeom, const QgsPoint& resultPt)
{
    return localDirectionCos(origGeom, origPt, resultGeom, resultPt) < 0.0;
}

// 成果居民点匹配回原始同名点：ELEMID → 同坐标 → 1米内最近点
static bool findOriginalPoint(const QgsFeature& feat, const QString& eidField,
                              const QHash<QString, QgsPointXY>& byEid,
                              const QList<QgsPointXY>& pts,
                              QgsPointXY& op, QString& eidOut)
{
    eidOut.clear();
    if (!eidField.isEmpty()) {
        const int idx = feat.fields().indexOf(eidField);
        if (idx >= 0) {
            eidOut = normalizeEid(feat.attribute(idx));
            if (!eidOut.isEmpty()) {
                const auto it = byEid.constFind(eidOut);
                if (it != byEid.constEnd()) { op = it.value(); return true; }
            }
        }
    }
    const QgsGeometry g = feat.geometry();
    if (g.isNull() || g.type() != QgsWkbTypes::PointGeometry) return false;
    const QgsPointXY p = g.asPoint();
    for (const QgsPointXY& o : pts) {
        if (qAbs(o.x() - p.x()) < 1e-6 && qAbs(o.y() - p.y()) < 1e-6) {
            op = o;
            return true;
        }
    }
    double best = 1.0;
    bool found = false;
    for (const QgsPointXY& o : pts) {
        const double dx = o.x() - p.x(), dy = o.y() - p.y();
        const double d = std::sqrt(dx * dx + dy * dy);
        if (d <= best) { best = d; op = o; found = true; }
    }
    return found;
}

// 图层显示名：GDB URI 取 layername= 后的内部图层名，SHP 取文件名
static QString layerDisplayName(const QString& uri)
{
    const int idx = uri.indexOf(QStringLiteral("layername="), 0, Qt::CaseInsensitive);
    if (idx >= 0) {
        QString name = uri.mid(idx + 10);
        const int bar = name.indexOf(QLatin1Char('|'));
        if (bar >= 0) name = name.left(bar);
        if (!name.isEmpty()) return name;
    }
    return QFileInfo(uri).completeBaseName();
}

void buildResidentSideFlipFocusErrors(
    const QList<QgsVectorLayer*>& resultResidentLayers,
    const QList<QgsVectorLayer*>& resultRoadLayers,
    const QString& origDataPath,
    const QHash<QString, QString>& layerMapping,
    const QString& matchParamPath,
    QList<FocusError>& out, QStringList* log)
{
    if (resultResidentLayers.isEmpty() || resultRoadLayers.isEmpty()) return;
    if (origDataPath.isEmpty()) {
        if (log) log->append(QStringLiteral("居民点道路左右换边：未提供原始数据，未执行"));
        return;
    }
    const MatchParams mp = loadMatchParams(matchParamPath);
    const double sideTol = mp.pointSideTolerance;

    // ---- 成果侧道路 ----
    RoadLookup resultRoads;
    for (QgsVectorLayer* lyr : resultRoadLayers) {
        if (!lyr) continue;
        const QString base = layerDisplayName(lyr->source());
        const QString eidField = findField(lyr, QStringList()
            << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
            << QStringLiteral("FormerID"));
        const int eidIdx = eidField.isEmpty() ? -1 : lyr->fields().indexOf(eidField);
        QgsFeatureIterator it = lyr->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsGeometry g = f.geometry();
            if (g.isNull() || g.type() != QgsWkbTypes::LineGeometry) continue;
            const QString eid = eidIdx >= 0 ? normalizeEid(f.attribute(eidIdx)) : QString();
            resultRoads.add(base, eid, g,
                            attrText(*lyr, f, QStringLiteral("RN")),
                            attrText(*lyr, f, QStringLiteral("NAME1")),
                            attrText(*lyr, f, QStringLiteral("NAME")),
                            attrText(*lyr, f, QStringLiteral("TYPE")));
        }
    }
    if (resultRoads.entries.isEmpty()) {
        if (log) log->append(QStringLiteral("居民点道路左右换边：成果侧道路图层无有效线要素，未执行"));
        return;
    }

    // ---- 原始侧道路：成果道路图层名经映射得到源图层代码 ----
    QStringList origCodes;
    for (QgsVectorLayer* lyr : resultRoadLayers) {
        if (!lyr) continue;
        const QString base = layerDisplayName(lyr->source());
        // 成果图层可能带“_选取/_名称”等综合后缀，映射表按去掉后缀的名称回退
        const QString code = layerMapping.value(base,
            layerMapping.value(stripSuffix(base), base));
        if (!code.isEmpty() && !origCodes.contains(code)) origCodes.append(code);
    }
    RoadLookup origRoads;
    for (const QString& code : qAsConst(origCodes)) {
        for (const QString& uri : origLayerUris(origDataPath, code, QString())) {
            QgsVectorLayer lyr(uri, QStringLiteral("m460_side_orig"), QStringLiteral("ogr"));
            if (!lyr.isValid() || lyr.geometryType() != QgsWkbTypes::LineGeometry)
                continue;
            const QString eidField = findField(&lyr, QStringList()
                << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
                << QStringLiteral("FormerID"));
            const int eidIdx = eidField.isEmpty() ? -1 : lyr.fields().indexOf(eidField);
            QgsFeatureIterator it = lyr.getFeatures();
            QgsFeature f;
            while (it.nextFeature(f)) {
                const QgsGeometry g = f.geometry();
                if (g.isNull()) continue;
                const QString eid = eidIdx >= 0 ? normalizeEid(f.attribute(eidIdx)) : QString();
                origRoads.add(code, eid, g,
                              attrText(lyr, f, QStringLiteral("RN")),
                              attrText(lyr, f, QStringLiteral("NAME1")),
                              attrText(lyr, f, QStringLiteral("NAME")),
                              attrText(lyr, f, QStringLiteral("TYPE")));
            }
        }
    }
    if (origRoads.entries.isEmpty()) {
        if (log) log->append(QStringLiteral("居民点道路左右换边：原始侧道路图层未找到（%1），未执行")
            .arg(origCodes.isEmpty() ? QStringLiteral("无映射")
                                     : origCodes.join(QStringLiteral("/"))));
        return;
    }

    int checkedPts = 0, flipped = 0, skippedFar = 0, skippedRoad = 0, unmatchedPts = 0;
    QSet<QString> sideReported;   // 同一居民点+同一道路只报一次（跨驻地层级去重）
    for (QgsVectorLayer* resLyr : resultResidentLayers) {
        if (!resLyr) continue;
        const QString resName = layerDisplayName(resLyr->source());
        const QString ptCode = layerMapping.value(resName,
            layerMapping.value(stripSuffix(resName), resName));

        // 原始侧同名居民点
        QList<QgsPointXY> origPts;
        QHash<QString, QgsPointXY> origPtByEid;
        for (const QString& uri : origLayerUris(origDataPath, ptCode, resName)) {
            QgsVectorLayer lyr(uri, QStringLiteral("m460_side_pt"), QStringLiteral("ogr"));
            if (!lyr.isValid()) continue;
            const QString eidField = findField(&lyr, QStringList()
                << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
                << QStringLiteral("FormerID"));
            const int eidIdx = eidField.isEmpty() ? -1 : lyr.fields().indexOf(eidField);
            QgsFeatureIterator it = lyr.getFeatures();
            QgsFeature f;
            while (it.nextFeature(f)) {
                const QgsGeometry g = f.geometry();
                if (g.isNull() || g.type() != QgsWkbTypes::PointGeometry) continue;
                const QgsPointXY p = g.asPoint();
                origPts.append(p);
                if (eidIdx >= 0) {
                    const QString eid = normalizeEid(f.attribute(eidIdx));
                    if (!eid.isEmpty() && !origPtByEid.contains(eid))
                        origPtByEid.insert(eid, p);
                }
            }
        }
        if (origPts.isEmpty()) {
            if (log) log->append(QStringLiteral(
                "居民点道路左右换边：[%1] 原始侧居民点图层未找到，本图层跳过").arg(resName));
            continue;
        }

        const QString resEidField = findField(resLyr, QStringList()
            << QStringLiteral("ELEMID") << QStringLiteral("EntityID")
            << QStringLiteral("FormerID"));
        QgsFeatureIterator it = resLyr->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsGeometry g = f.geometry();
            if (g.isNull() || g.type() != QgsWkbTypes::PointGeometry) continue;
            const QgsPointXY p = g.asPoint();
            QgsPointXY op;
            QString eid;
            if (!findOriginalPoint(f, resEidField, origPtByEid, origPts, op, eid)) {
                ++unmatchedPts;
                continue;
            }
            const RoadEntry* resRoad = nullptr;
            double resDist = -1.0;
            if (!resultRoads.nearest(p, sideTol, resRoad, resDist)) {
                ++skippedFar;
                continue;
            }
            double resSideDist = -1.0;
            const int resSide = pointSideOfLine(resRoad->geom, p, resSideDist);
            if (resSide == 0) { ++skippedRoad; continue; }

            // 原始侧道路：有 ELEMID 时按同号道路找（交叉口不会配到相邻的另一条路）；
            // 无 ELEMID 时按同路业务标识（RN > NAME1 > NAME > TYPE）在原始点附近找最近道路。
            // 旧逻辑直接在全部道路里取最近者，会把交叉口/邻近的另一条路当成同路，
            // 左右侧比较对象不同 → 2026-10-26 永登县 7 条跨侧全部为误报。
            const RoadEntry* origRoad = nullptr;
            double origDist = -1.0;
            bool matchedByEid = false;
            if (!resRoad->eid.isEmpty()) {
                origRoad = origRoads.findEid(resRoad->eid);
                matchedByEid = (origRoad != nullptr);
            }
            QString idKey, idVal;
            if (!matchedByEid) {
                roadIdentity(*resRoad, idKey, idVal);
                if (idKey.isEmpty()
                    || !origRoads.nearestMatching(op, sideTol, idKey, idVal,
                                                  origRoad, origDist)) {
                    ++skippedRoad;   // 原始侧找不到同一条路，不能比较左右侧
                    continue;
                }
            }
            if (!origRoad) { ++skippedRoad; continue; }
            double origSideDist = -1.0;
            const int origSide = pointSideOfLine(origRoad->geom, op, origSideDist);
            if (origSide == 0) { ++skippedRoad; continue; }
            if (origSideDist > sideTol) { ++skippedFar; continue; }

            // 同一条道路确认：同号直接成立；同业务标识时还要求两条线几何邻近
            // （≤缓冲距离），避免同名/同类但不同道路被当成同路。
            bool sameRoad = matchedByEid;
            if (!sameRoad && !idKey.isEmpty()) {
                QString oKey, oVal;
                roadIdentity(*origRoad, oKey, oVal);
                if (oKey == idKey && oVal == idVal
                    && resRoad->geom.distance(origRoad->geom)
                        <= qMax(10.0, mp.bufferDistance))
                    sameRoad = true;
            }
            if (!sameRoad) { ++skippedRoad; continue; }

            // 局部走向需大致平行（|cos|≥0.5），接近垂直时"左右侧"没有可比性
            const double dirCos = localDirectionCos(origRoad->geom,
                    QgsPoint(op.x(), op.y()), resRoad->geom,
                    QgsPoint(p.x(), p.y()));
            if (std::abs(dirCos) < 0.5) { ++skippedRoad; continue; }

            // 居民点确有移动时，移动轨迹必须穿过该道路（原始或成果任一），
            // 否则只是换了最近道路/线段，不是跨侧
            if (op.distance(p) > 0.5) {
                QVector<QgsPointXY> mv;
                mv << op << p;
                const QgsGeometry moveSeg = QgsGeometry::fromPolylineXY(mv);
                if (moveSeg.distance(origRoad->geom) > 0.5
                    && moveSeg.distance(resRoad->geom) > 0.5) {
                    ++skippedRoad;
                    continue;
                }
            }

            // 道路方向反转时，成果侧的左右要取反后再与原始侧比较
            int resSideAligned = resSide;
            if (localDirectionsOpposite(origRoad->geom,
                    QgsPoint(op.x(), op.y()), resRoad->geom,
                    QgsPoint(p.x(), p.y())))
                resSideAligned = -resSideAligned;
            ++checkedPts;
            if (resSideAligned == origSide) continue;

            const QString dkey = QString::number(qRound64(p.x() * 10.0)) + QLatin1Char('_')
                + QString::number(qRound64(p.y() * 10.0)) + QLatin1Char('_') + resRoad->name;
            if (sideReported.contains(dkey)) continue;
            sideReported.insert(dkey);
            ++flipped;
            const QString origSideText = origSide < 0 ? QStringLiteral("左") : QStringLiteral("右");
            const QString resSideText = resSideAligned < 0 ? QStringLiteral("左") : QStringLiteral("右");
            const QString eidText = eid.isEmpty() ? QString()
                : QStringLiteral(" ELEMID='%1'").arg(eid);
            QStringList vals;
            vals << QStringLiteral("原%1侧，距道路%2米")
                .arg(origSideText).arg(origSideDist, 0, 'f', 1);
            vals << QStringLiteral("现%1侧，距道路%2米")
                .arg(resSideText).arg(resSideDist, 0, 'f', 1);
            vals << QStringLiteral("道路图层 %1").arg(resRoad->name);
            vals << QStringLiteral("居民点坐标 %1").arg(locText(p));
            out.append(FocusError{g, QStringLiteral("居民点与道路拓扑"),
                QStringLiteral("[%1] 综合前后居民点跨到道路另一侧：原%2侧（距道路%3米）→ 现%4侧（距道路%5米）%6")
                    .arg(resName, origSideText)
                    .arg(origSideDist, 0, 'f', 1)
                    .arg(resSideText)
                    .arg(resSideDist, 0, 'f', 1)
                    .arg(eidText),
                vals.join(QStringLiteral("; ")), p, true});
        }
    }
    if (log) {
        log->append(QStringLiteral("居民点道路左右换边：检查%1个居民点，跨侧%2处"
                                   "（距道路容差%3米；远离道路跳过%4，道路无法对应跳过%5，原始点未匹配%6）")
            .arg(checkedPts).arg(flipped)
            .arg(sideTol, 0, 'f', 0).arg(skippedFar).arg(skippedRoad).arg(unmatchedPts));
    }
}

// ================= M460 选取比例统计 =================
static qint64 countLayerFeatures(QgsVectorLayer& layer)
{
    const qint64 n = static_cast<qint64>(layer.featureCount());
    if (n >= 0) return n;
    qint64 c = 0;
    QgsFeatureIterator it = layer.getFeatures();
    QgsFeature f;
    while (it.nextFeature(f)) ++c;
    return c;
}

void collectSelectionStats(const QString& origDir, const QString& resultDir,
                           const QHash<QString, QString>* layerMapping,
                           QList<SelectionStat>& stats, QStringList* log)
{
    if (origDir.isEmpty() || resultDir.isEmpty()) {
        if (log) log->append(QStringLiteral(
            "选取比例统计：需同时提供原始数据与成果数据，本次未执行"));
        return;
    }
    const bool origIsGdb = WujiMissionRunner::isFileGdbSource(origDir);
    const QHash<QString, QStringList> origByBase = origIsGdb
        ? gdbLayersByCode(origDir) : groupByBase(shpPathsIn(origDir));
    const QStringList resultShps = shpPathsIn(resultDir);
    if (origByBase.isEmpty() || resultShps.isEmpty()) {
        if (log) log->append(QStringLiteral("选取比例统计：无可统计图层，本次未执行"));
        return;
    }
    QStringList ambiguousNotes;
    const QMap<QString, QString> pairs =
        pairLayers(origByBase, resultShps, layerMapping, &ambiguousNotes);
    if (pairs.isEmpty()) {
        if (log) log->append(QStringLiteral("选取比例统计：原始与成果未找到可配对图层，本次未执行"));
        return;
    }
    const QHash<QString, QStringList> resultByBaseGroup = groupByBase(resultShps);
    int skippedAnnotation = 0, skippedGeom = 0;
    for (auto it = pairs.constBegin(); it != pairs.constEnd(); ++it) {
        const QString base = it.key();
        // 名称/注记/代码/方向点类图层的要素数不是“选取要素”口径，不统计比例。
        // “地沟名”（AANP 地理名称）这类以“名”结尾的纯名称图层同样跳过。
        if (base.contains(QStringLiteral("名称")) || base.contains(QStringLiteral("注记"))
            || base.contains(QStringLiteral("代码")) || base.contains(QStringLiteral("方向点"))
            || base.contains(QStringLiteral("标注")) || base.endsWith(QStringLiteral("名"))) {
            ++skippedAnnotation;
            continue;
        }
        const QString resultPath = pickPath(resultByBaseGroup.value(base));
        if (resultPath.isEmpty()) continue;
        QgsVectorLayer resLayer(resultPath, QStringLiteral("m460_stat_res"), QStringLiteral("ogr"));
        QgsVectorLayer origLayer(it.value(), QStringLiteral("m460_stat_orig"), QStringLiteral("ogr"));
        if (!resLayer.isValid() || !origLayer.isValid()) {
            if (log) log->append(QStringLiteral(
                "选取比例统计：[%1] 图层打开失败，已跳过").arg(it.key()));
            continue;
        }
        // 点/线/面表示转换（如湖泊水库_点 ↔ 湖泊水库面）不是选取口径，跳过
        if (resLayer.geometryType() != origLayer.geometryType()) {
            ++skippedGeom;
            continue;
        }
        SelectionStat s;
        s.resultLayer = it.key();
        s.origLayer = layerDisplayName(it.value());
        s.origCount = countLayerFeatures(origLayer);
        s.resultCount = countLayerFeatures(resLayer);
        stats.append(s);
    }
    if (log) {
        log->append(QStringLiteral("---- 选取比例统计（成果要素数/原始要素数） ----"));
        for (const SelectionStat& s : qAsConst(stats)) {
            const double ratio = s.origCount > 0
                ? 100.0 * double(s.resultCount) / double(s.origCount) : -1.0;
            log->append(QStringLiteral("[%1] 原始%2 → 成果%3，选取比例%4")
                .arg(s.resultLayer).arg(s.origCount).arg(s.resultCount)
                .arg(ratio < 0.0 ? QStringLiteral("—")
                     : QStringLiteral("%1%").arg(ratio, 0, 'f', 2)));
        }
        log->append(QStringLiteral(
            "说明：选取比例=成果要素数/原始要素数；同一原始图层被多个成果图层引用时，按该源图层要素总数计算"));
        // 按源图层汇总：多个成果图层映射到同一源图层（如国道/省道/县道 → LRDL）时，
        // 单层比例之和=该源图层总选取比例，便于核对整体取舍强度。
        QMap<QString, qint64> origCountByLayer, resultSumByLayer;
        for (const SelectionStat& s : qAsConst(stats)) {
            origCountByLayer[s.origLayer] = s.origCount;
            resultSumByLayer[s.origLayer] += s.resultCount;
        }
        log->append(QStringLiteral("---- 按源图层汇总（成果合计/源图层要素数） ----"));
        for (auto oit = origCountByLayer.constBegin(); oit != origCountByLayer.constEnd(); ++oit) {
            const qint64 oc = oit.value();
            const qint64 rc = resultSumByLayer.value(oit.key());
            const double ratio = oc > 0 ? 100.0 * double(rc) / double(oc) : -1.0;
            log->append(QStringLiteral("[%1] 源图层%2 → 成果合计%3，总选取比例%4")
                .arg(oit.key()).arg(oc).arg(rc)
                .arg(ratio < 0.0 ? QStringLiteral("—")
                     : QStringLiteral("%1%").arg(ratio, 0, 'f', 2)));
        }
        if (skippedAnnotation > 0)
            log->append(QStringLiteral(
                "选取比例统计：已跳过%1个名称/注记/代码/方向点类图层（非选取要素口径）")
                .arg(skippedAnnotation));
        if (skippedGeom > 0)
            log->append(QStringLiteral(
                "选取比例统计：已跳过%1个几何类型与源图层不一致的图层（点/线/面表示转换）")
                .arg(skippedGeom));
        if (stats.isEmpty())
            log->append(QStringLiteral("选取比例统计：没有可统计的要素图层"));
        for (const QString& n : qAsConst(ambiguousNotes))
            log->append(QStringLiteral("选取比例统计配对提示：%1").arg(n));
    }
}

// ================= ⑤ 真实移位值（2026-11-08） =================
// 数据口径（甲方 1107 图层对应表）：
//   公路处理2_高速.shp        → 高速公路.shp + 在建高速公路.shp
//   公路处理2_国道3.shp       → 国道.shp
//   公路处理2_省道3.shp       → 省道.shp
//   公路处理2_县道3.shp       → 县道.shp
//   公路处理2_乡道3.shp       → 乡道（专用道）.shp
//   公路处理2_专用公路3.shp   → 乡道（专用道）.shp
//   公路处理2_连接道3.shp     → 连接道.shp
//   公路处理2_快速路3.shp     → 快速路.shp
//   城市道路处理2.shp         → 城市道路.shp
//   村道处理2.shp             → 村道.shp
//   铁路处理2.shp             → 铁路.shp
// 移位前数据可能缺“快速路/专用公路”源文件（它们分别并入省道/乡道），
// 因此匹配不按文件名绑定，而是对全部移位前线要素按道路标识建立索引：
// 成果要素优先用各自最稳定的标识（城市道路 NAME、村道 OBJECTID、铁路 NAME，
// 其余 OBJECTID），找不到再依次退到 RN/NAME1/NAME，最后取 2 公里内最近线。
// 这样省道文件中的“兰秦快速路”（OBJECTID 1372）也能匹配到成果快速路图层。

struct ShiftBeforeRef {
    QString layer;       // 移位前图层基名
    QgsGeometry geom;
    QString objectId;
    QString rn;
    QString name;
    QString name1;
    QString elemid;
};

struct ShiftRefIndex {
    QVector<ShiftBeforeRef> refs;
    QHash<QString, QVector<int>> byObjectId;
    QHash<QString, QVector<int>> byRn;
    QHash<QString, QVector<int>> byName;
    QHash<QString, QVector<int>> byName1;
    QHash<QString, QVector<int>> byElemid;
    QHash<QString, QVector<int>> byLayer;   // 移位前图层基名 → refs 下标
};

// 移位前属性取值：空、"-"、"-9999" 视为无效；OBJECTID=0 也视为占位无效
static QString shiftFieldValue(const QgsFields& fields, const QgsFeature& f,
                               const QString& field)
{
    const int idx = findFieldIndexCI(fields, field);
    if (idx < 0) return QString();
    QString s = f.attribute(idx).toString().trimmed();
    if (s.isEmpty() || s == QStringLiteral("-") || s == QStringLiteral("-9999"))
        return QString();
    if (field.compare(QStringLiteral("OBJECTID"), Qt::CaseInsensitive) == 0
        && s == QStringLiteral("0"))
        return QString();
    return s;
}

static void shiftIndexAppend(QHash<QString, QVector<int>>& index,
                             const QString& value, int refIdx)
{
    if (!value.isEmpty()) index[value].append(refIdx);
}

// 读取一个目录下全部线 SHP，按“道路/铁路”分为两套索引
static void loadShiftBeforeRefs(const QString& dir, ShiftRefIndex& road,
                                ShiftRefIndex& rail, QStringList* log)
{
    const QStringList shps = QDir(dir).entryList(QStringList() << QStringLiteral("*.shp"),
                                                 QDir::Files, QDir::Name);
    int loaded = 0, skipped = 0;
    for (const QString& fn : shps) {
        const QString base = QFileInfo(fn).completeBaseName();
        QgsVectorLayer layer(dir + QLatin1Char('/') + fn, base, QStringLiteral("ogr"));
        if (!layer.isValid() || layer.geometryType() != QgsWkbTypes::LineGeometry) {
            ++skipped;
            continue;
        }
        const bool isRail = base.contains(QStringLiteral("铁路"));
        ShiftRefIndex& idx = isRail ? rail : road;
        const QgsFields fields = layer.fields();
        int n = 0;
        QgsFeatureIterator it = layer.getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsGeometry g = f.geometry();
            if (g.isNull() || g.isEmpty()) continue;
            ShiftBeforeRef r;
            r.layer = base;
            r.geom = g;
            r.objectId = shiftFieldValue(fields, f, QStringLiteral("OBJECTID"));
            r.rn       = shiftFieldValue(fields, f, QStringLiteral("RN"));
            r.name     = shiftFieldValue(fields, f, QStringLiteral("NAME"));
            r.name1    = shiftFieldValue(fields, f, QStringLiteral("NAME1"));
            r.elemid   = shiftFieldValue(fields, f, QStringLiteral("ELEMID"));
            idx.refs.append(r);
            ++n;
        }
        if (n > 0) {
            ++loaded;
            if (log) log->append(QStringLiteral("[移位前] %1：%2个线要素").arg(base).arg(n));
        }
    }
    // 建索引（按 refs 下标）
    for (ShiftRefIndex* idx : {&road, &rail}) {
        for (int i = 0; i < idx->refs.size(); ++i) {
            const ShiftBeforeRef& r = idx->refs.at(i);
            shiftIndexAppend(idx->byObjectId, r.objectId, i);
            shiftIndexAppend(idx->byRn, r.rn, i);
            shiftIndexAppend(idx->byName, r.name, i);
            shiftIndexAppend(idx->byName1, r.name1, i);
            shiftIndexAppend(idx->byElemid, r.elemid, i);
            idx->byLayer[r.layer].append(i);
        }
    }
    if (log)
        log->append(QStringLiteral("真实移位值：移位前线要素共%1个（道路%2 + 铁路%3），跳过非线图层%4个")
            .arg(road.refs.size() + rail.refs.size())
            .arg(road.refs.size()).arg(rail.refs.size()).arg(skipped));
    Q_UNUSED(loaded)
}

static QVector<int> shiftCandidates(const ShiftRefIndex& idx, const QString& key,
                                    const QString& value)
{
    if (key == QStringLiteral("OBJECTID")) return idx.byObjectId.value(value);
    if (key == QStringLiteral("RN"))       return idx.byRn.value(value);
    if (key == QStringLiteral("NAME1"))    return idx.byName1.value(value);
    if (key == QStringLiteral("NAME"))     return idx.byName.value(value);
    if (key == QStringLiteral("ELEMID"))   return idx.byElemid.value(value);
    return QVector<int>();
}

// 各成果图层优先使用的标识字段（按稳定性排序）
static QStringList shiftKeyOrder(const QString& layerBase)
{
    if (layerBase.contains(QStringLiteral("城市道路")))
        return QStringList() << QStringLiteral("NAME") << QStringLiteral("RN")
                             << QStringLiteral("OBJECTID");
    if (layerBase.contains(QStringLiteral("村道")))
        return QStringList() << QStringLiteral("OBJECTID") << QStringLiteral("NAME");
    if (layerBase.contains(QStringLiteral("铁路")))
        return QStringList() << QStringLiteral("NAME") << QStringLiteral("ELEMID")
                             << QStringLiteral("OBJECTID");
    return QStringList() << QStringLiteral("OBJECTID") << QStringLiteral("RN")
                         << QStringLiteral("NAME1") << QStringLiteral("NAME");
}

// 成果图层对应的“同层”移位前图层：标识覆盖不全（综合合并/改名/分段）时，
// 用同层邻近要素兜底，避免把“合并后的另一段”误当成 8 公里大移位。
static QStringList shiftFamilyLayers(const QString& layerBase)
{
    if (layerBase.contains(QStringLiteral("城市道路")))
        return QStringList() << QStringLiteral("城市道路处理2");
    if (layerBase.contains(QStringLiteral("村道")))
        return QStringList() << QStringLiteral("村道处理2");
    if (layerBase.contains(QStringLiteral("铁路")))
        return QStringList() << QStringLiteral("铁路处理2");
    if (layerBase.contains(QStringLiteral("高速公路")))
        return QStringList() << QStringLiteral("公路处理2_高速");
    if (layerBase.contains(QStringLiteral("国道")))
        return QStringList() << QStringLiteral("公路处理2_国道3");
    if (layerBase.contains(QStringLiteral("省道")))
        return QStringList() << QStringLiteral("公路处理2_省道3");
    if (layerBase.contains(QStringLiteral("县道")))
        return QStringList() << QStringLiteral("公路处理2_县道3");
    if (layerBase.contains(QStringLiteral("乡道")))
        return QStringList() << QStringLiteral("公路处理2_乡道3")
                             << QStringLiteral("公路处理2_专用公路3");
    if (layerBase.contains(QStringLiteral("连接道")))
        return QStringList() << QStringLiteral("公路处理2_连接道3");
    if (layerBase.contains(QStringLiteral("快速路")))
        return QStringList() << QStringLiteral("公路处理2_快速路3")
                             << QStringLiteral("公路处理2_省道3");
    return QStringList();
}

static double shiftRectPointDistance(const QgsRectangle& r, double x, double y)
{
    double dx = 0.0, dy = 0.0;
    if (x < r.xMinimum())      dx = r.xMinimum() - x;
    else if (x > r.xMaximum()) dx = x - r.xMaximum();
    if (y < r.yMinimum())      dy = r.yMinimum() - y;
    else if (y > r.yMaximum()) dy = y - r.yMaximum();
    return std::sqrt(dx * dx + dy * dy);
}

static double shiftRectDistance(const QgsRectangle& a, const QgsRectangle& b)
{
    double dx = 0.0, dy = 0.0;
    if (a.xMaximum() < b.xMinimum())      dx = b.xMinimum() - a.xMaximum();
    else if (b.xMaximum() < a.xMinimum()) dx = a.xMinimum() - b.xMaximum();
    if (a.yMaximum() < b.yMinimum())      dy = b.yMinimum() - a.yMaximum();
    else if (b.yMaximum() < a.yMinimum()) dy = a.yMinimum() - b.yMaximum();
    return std::sqrt(dx * dx + dy * dy);
}

// 无任何标识可匹配时：取 2 公里内最近的同类别移位前线（仅作兜底）
static int shiftNearestRef(const ShiftRefIndex& idx, const QgsGeometry& afterGeom)
{
    const QgsRectangle ab = afterGeom.boundingBox();
    int best = -1;
    double bestD = 2000.0;
    for (int i = 0; i < idx.refs.size(); ++i) {
        const QgsGeometry& g = idx.refs.at(i).geom;
        if (shiftRectDistance(ab, g.boundingBox()) > bestD) continue;
        const double d = g.distance(afterGeom);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// 一个顶点到候选线集合的最近距离（包围盒下界剪枝）
static double shiftNearestDistance(const ShiftRefIndex& idx, const QVector<int>& cand,
                                   const QVector<QgsRectangle>& bbs,
                                   const QgsGeometry& pt, double x, double y)
{
    double best = std::numeric_limits<double>::max();
    for (int j = 0; j < cand.size(); ++j) {
        const double lb = shiftRectPointDistance(bbs.at(j), x, y);
        if (lb >= best) continue;
        const double d = idx.refs.at(cand.at(j)).geom.distance(pt);
        if (d < best) best = d;
    }
    return best;
}

// 逐顶点计算成果线到移位前线的最近距离，取最大/平均。
// 标识候选优先（同一道路），但综合后要素常合并多个原始分段或改名，
// 标识候选只覆盖其中一段时，远端顶点会算出几公里的假移位。处理办法：
//   顶点到标识候选距离 ≤ trustRadius → 用标识候选（正常情况，最可信）；
//   否则用同层邻近要素（familyCand，1公里内）兜底；
//   同层也没有时才退回标识距离。
static bool shiftMeasure(const ShiftRefIndex& idx, const QVector<int>& cand,
                         const QVector<int>& familyCand, double trustRadius,
                         const QgsGeometry& afterGeom, TrueShiftInfo& info)
{
    if (cand.isEmpty() && familyCand.isEmpty()) return false;
    QVector<QgsRectangle> idBbs, famBbs;
    idBbs.reserve(cand.size());
    famBbs.reserve(familyCand.size());
    for (int ci : cand) idBbs.append(idx.refs.at(ci).geom.boundingBox());
    for (int ci : familyCand) famBbs.append(idx.refs.at(ci).geom.boundingBox());

    int n = 0, fallbackVerts = 0;
    double maxD = 0.0, sumD = 0.0;
    QgsPointXY maxPt;
    bool hasPt = false;
    for (auto vit = afterGeom.vertices(); vit.hasNext(); ) {
        const QgsPoint p = vit.next();
        const QgsPointXY xy(p.x(), p.y());
        const QgsGeometry pt = QgsGeometry::fromPointXY(xy);
        const double dId = shiftNearestDistance(idx, cand, idBbs, pt, xy.x(), xy.y());
        double chosen = std::numeric_limits<double>::max();
        if (!cand.isEmpty() && dId <= trustRadius) {
            chosen = dId;
        } else {
            const double dFam = shiftNearestDistance(idx, familyCand, famBbs,
                                                     pt, xy.x(), xy.y());
            if (dFam < std::numeric_limits<double>::max()) {
                chosen = dFam;
                ++fallbackVerts;
            } else if (dId < std::numeric_limits<double>::max()) {
                chosen = dId;
            }
        }
        if (!(chosen < std::numeric_limits<double>::max())) continue;
        ++n;
        sumD += chosen;
        if (chosen > maxD) { maxD = chosen; maxPt = xy; hasPt = true; }
    }
    if (n == 0) return false;
    info.valid = true;
    info.maxShift = maxD;
    info.meanShift = sumD / double(n);
    info.vertexCount = n;
    info.fallbackVertices = fallbackVerts;
    info.point = maxPt;
    info.hasPoint = hasPt;
    return true;
}

QHash<QString, TrueShiftInfo> computeTrueShifts(
    const QString& beforeDir, const QString& afterDir,
    const QString& matchParamPath, QStringList* log)
{
    QHash<QString, TrueShiftInfo> out;
    auto note = [log](const QString& s) { if (log) log->append(s); };

    if (beforeDir.isEmpty() || afterDir.isEmpty()) {
        note(QStringLiteral("真实移位值：需同时提供「化简后移位前」与「最终成果」两个目录，本次未计算"));
        return out;
    }
    if (!QFileInfo(beforeDir).isDir() || !QFileInfo(afterDir).isDir()) {
        note(QStringLiteral("真实移位值：输入目录不存在（移位前=%1，移位后=%2），本次未计算")
            .arg(beforeDir, afterDir));
        return out;
    }

    ShiftRefIndex roadIdx, railIdx;
    loadShiftBeforeRefs(beforeDir, roadIdx, railIdx, log);
    if (roadIdx.refs.isEmpty() && railIdx.refs.isEmpty()) {
        note(QStringLiteral("真实移位值：移位前目录内没有可用的线图层，本次未计算"));
        return out;
    }

    const MatchParams mp = loadMatchParams(matchParamPath);
    // 甲方 1107 口径的成果侧图层（顺序与图层对应表一致）
    const QStringList afterLayers = QStringList()
        << QStringLiteral("高速公路") << QStringLiteral("在建高速公路")
        << QStringLiteral("国道") << QStringLiteral("省道")
        << QStringLiteral("县道") << QStringLiteral("乡道（专用道）")
        << QStringLiteral("连接道") << QStringLiteral("快速路")
        << QStringLiteral("城市道路") << QStringLiteral("村道")
        << QStringLiteral("铁路")
        // 铁路辅助成果图层（1107 成果目录中另有按等级拆分的三份铁路数据）
        << QStringLiteral("普通铁路") << QStringLiteral("高速铁路");

    // 铁路图层二选一（2026-11-08 修正）：成果目录常同时有「铁路」和
    // 「普通铁路+高速铁路」。1107 数据里“铁路处理2 → 普通铁路+高速铁路”才是
    // 移位后的最终成果，而 铁路.shp 可能是原样拷进来的原始线（总长与原库一致、
    // 与“化简后移位前”无可比性），拿它去算会把“原始线 vs 化简线”的差异当成
    // 移位值报出一批假偏移。这里用总长度与移位前铁路线总长最接近的一组参与⑤。
    QSet<QString> skipRailLayers;
    {
        double railBeforeLen = 0.0;
        for (const ShiftBeforeRef& r : railIdx.refs) railBeforeLen += r.geom.length();
        auto layerTotalLen = [&afterDir](const QString& name, bool* ok) {
            *ok = false;
            const QString path = afterDir + QLatin1Char('/') + name + QStringLiteral(".shp");
            if (!QFileInfo::exists(path)) return 0.0;
            QgsVectorLayer l(path, name, QStringLiteral("ogr"));
            if (!l.isValid()) return 0.0;
            double total = 0.0;
            QgsFeatureIterator lit = l.getFeatures();
            QgsFeature lf;
            while (lit.nextFeature(lf)) {
                const QgsGeometry lg = lf.geometry();
                if (!lg.isNull() && !lg.isEmpty()) total += lg.length();
            }
            *ok = true;
            return total;
        };
        bool okMerged = false, okPart1 = false, okPart2 = false;
        const double lenMerged = layerTotalLen(QStringLiteral("铁路"), &okMerged);
        const double lenPart1 = layerTotalLen(QStringLiteral("普通铁路"), &okPart1);
        const double lenPart2 = layerTotalLen(QStringLiteral("高速铁路"), &okPart2);
        const double lenParts = lenPart1 + lenPart2;
        if (railBeforeLen > 1.0 && okMerged && (okPart1 || okPart2)) {
            if (qAbs(lenParts - railBeforeLen) + 1.0 < qAbs(lenMerged - railBeforeLen)) {
                skipRailLayers.insert(QStringLiteral("铁路"));
                note(QStringLiteral("[铁路] 移位前铁路总长%1km：普通铁路+高速铁路(%2km)比"
                                    "铁路层(%3km)更接近，⑤按拆分图层计算，铁路层跳过")
                    .arg(railBeforeLen / 1000.0, 0, 'f', 1)
                    .arg(lenParts / 1000.0, 0, 'f', 1)
                    .arg(lenMerged / 1000.0, 0, 'f', 1));
            } else {
                if (okPart1) skipRailLayers.insert(QStringLiteral("普通铁路"));
                if (okPart2) skipRailLayers.insert(QStringLiteral("高速铁路"));
                note(QStringLiteral("[铁路] 移位前铁路总长%1km：铁路层(%2km)比普通铁路+高速铁路"
                                    "(%3km)更接近，⑤按铁路层计算，拆分图层跳过")
                    .arg(railBeforeLen / 1000.0, 0, 'f', 1)
                    .arg(lenMerged / 1000.0, 0, 'f', 1)
                    .arg(lenParts / 1000.0, 0, 'f', 1));
            }
        }
    }

    note(QStringLiteral("---- ⑤真实移位值（化简后移位前 → 最终成果） ----"));
    note(QStringLiteral("匹配规则：城市道路优先NAME、村道优先OBJECTID、铁路优先NAME，"
                        "其余优先OBJECTID；缺失时退RN/NAME1/NAME。逐顶点优先用同标识移位前线；"
                        "标识只覆盖部分线段时，该顶点改用1公里内同层移位前线兜底，"
                        "避免把综合合并段误算成大移位；最大值为真实移位值。"));
    note(QStringLiteral("判定阈值：最大移位 > %1 米（matchParameter.xml MaxDisplacement）")
        .arg(mp.maxDisplacement, 0, 'f', 1));

    int totalAfter = 0, totalMatched = 0, totalOver = 0;
    for (const QString& base : afterLayers) {
        if (skipRailLayers.contains(base)) {
            note(QStringLiteral("[%1] 与移位前铁路总长不匹配（同一条线的另一种表示已在算），跳过")
                .arg(base));
            continue;
        }
        const QString path = afterDir + QLatin1Char('/') + base + QStringLiteral(".shp");
        if (!QFileInfo::exists(path)) {
            note(QStringLiteral("[%1] 最终成果目录中未找到%1.shp，跳过").arg(base));
            continue;
        }
        QgsVectorLayer layer(path, base, QStringLiteral("ogr"));
        if (!layer.isValid()) {
            note(QStringLiteral("[%1] 图层打开失败，跳过").arg(base));
            continue;
        }
        if (layer.geometryType() != QgsWkbTypes::LineGeometry) {
            note(QStringLiteral("[%1] 不是线图层，跳过").arg(base));
            continue;
        }
        if (layer.crs().isGeographic())
            note(QStringLiteral("[%1] 警告：图层为地理坐标，移位值单位为度而非米").arg(base));

        const bool isRail = base.contains(QStringLiteral("铁路"));
        const ShiftRefIndex& refIdx = isRail ? railIdx : roadIdx;
        if (refIdx.refs.isEmpty()) {
            note(QStringLiteral("[%1] 移位前数据中没有%2类线要素，跳过")
                .arg(base, isRail ? QStringLiteral("铁路") : QStringLiteral("道路")));
            continue;
        }
        const QgsFields fields = layer.fields();
        const QStringList keyOrder = shiftKeyOrder(base);
        const QStringList familyNames = shiftFamilyLayers(base);
        // 标识距离 ≤ 该值时信任标识匹配（默认 2×MaxDisplacement，至少150米）
        const double trustRadius = qMax(2.0 * mp.maxDisplacement, 150.0);

        int nLayer = 0, nMatched = 0, nOver = 0;
        double layerMax = 0.0;
        QStringList unmatchedIds;

        QgsFeatureIterator it = layer.getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            const QgsGeometry g = f.geometry();
            if (g.isNull() || g.isEmpty()) continue;
            ++nLayer;
            ++totalAfter;

            // 1) 按标识取候选（同一标识可能对应移位前多个分段）
            QVector<int> cand;
            QString keyUsed;
            for (const QString& key : keyOrder) {
                const QString v = shiftFieldValue(fields, f, key);
                if (v.isEmpty()) continue;
                cand = shiftCandidates(refIdx, key, v);
                if (!cand.isEmpty()) {
                    keyUsed = key + QLatin1Char('=') + v;
                    break;
                }
            }
            // 剔除 50 公里外的同号要素（不同道路复用同一标识的极端情况）
            if (!cand.isEmpty()) {
                const QgsRectangle ab = g.boundingBox();
                QVector<int> near;
                near.reserve(cand.size());
                for (int ci : cand) {
                    if (shiftRectDistance(ab, refIdx.refs.at(ci).geom.boundingBox())
                        <= 50000.0)
                        near.append(ci);
                }
                cand = near;   // 全被剔除时为空，进入位置兜底
            }
            // 2) 同层邻近要素（1公里内）：标识覆盖不全时的兜底参照
            QVector<int> familyCand;
            {
                const QgsRectangle ab = g.boundingBox();
                for (const QString& famLayer : familyNames) {
                    const QVector<int> ids = refIdx.byLayer.value(famLayer);
                    for (int ci : ids) {
                        if (shiftRectDistance(ab,
                                refIdx.refs.at(ci).geom.boundingBox()) <= 1000.0)
                            familyCand.append(ci);
                    }
                }
            }
            // 3) 标识、同层都没有：取 2 公里内最近线
            if (cand.isEmpty() && familyCand.isEmpty()) {
                const int ni = shiftNearestRef(refIdx, g);
                if (ni >= 0) {
                    cand.append(ni);
                    keyUsed = QStringLiteral("按位置最近");
                }
            }
            if (cand.isEmpty() && familyCand.isEmpty()) {
                if (unmatchedIds.size() < 5)
                    unmatchedIds << QString::number(f.id());
                continue;
            }
            if (cand.isEmpty() && !familyCand.isEmpty())
                keyUsed = QStringLiteral("同层就近");

            TrueShiftInfo info;
            info.layer = base;
            info.fid = f.id();
            info.matchKey = keyUsed;
            QStringList refs;
            const QVector<int>& nameRefs = cand.isEmpty() ? familyCand : cand;
            for (int ci : nameRefs) {
                const QString& ln = refIdx.refs.at(ci).layer;
                if (!refs.contains(ln)) {
                    refs << ln;
                    if (refs.size() >= 4) break;
                }
            }
            info.refLayers = refs.join(QStringLiteral("、"));

            if (!shiftMeasure(refIdx, cand, familyCand, trustRadius, g, info)) {
                if (unmatchedIds.size() < 5)
                    unmatchedIds << QString::number(f.id());
                continue;
            }
            if (info.fallbackVertices > 0)
                info.matchKey += QStringLiteral("（%1个顶点同层就近）")
                    .arg(info.fallbackVertices);
            ++nMatched;
            ++totalMatched;
            if (info.maxShift > mp.maxDisplacement) {
                ++nOver;
                ++totalOver;
            }
            if (info.maxShift > layerMax) layerMax = info.maxShift;
            out.insert(base + QLatin1Char('#') + QString::number(f.id()), info);
        }

        QString line = QStringLiteral("[%1] 成果%2个要素，匹配%3个，最大移位%4米，超阈值(>%5米)%6个")
            .arg(base).arg(nLayer).arg(nMatched)
            .arg(layerMax, 0, 'f', 1)
            .arg(mp.maxDisplacement, 0, 'f', 1)
            .arg(nOver);
        if (!unmatchedIds.isEmpty())
            line += QStringLiteral("；未匹配示例FID：%1")
                .arg(unmatchedIds.join(QStringLiteral(",")));
        note(line);
    }

    note(QStringLiteral("真实移位值汇总：成果%1个要素，匹配%2个（未匹配%3个），超阈值%4个")
        .arg(totalAfter).arg(totalMatched).arg(totalAfter - totalMatched).arg(totalOver));
    if (totalMatched == 0)
        note(QStringLiteral("真实移位值：未匹配到任何移位前参照，⑤将退回缓冲法"));
    return out;
}

} // namespace Mission458
