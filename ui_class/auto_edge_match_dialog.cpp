#define _HAS_STD_BYTE 0
#include "auto_edge_match_dialog.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QApplication>
#include <QDateTime>
#include <QColor>
#include <QHash>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QgsSettings.h>

#include "src_select_dialog.h"

#include <algorithm>
#include <cmath>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>

#include "vector/cse_geo_extract_and_process.h"

#include "../ui_task/se_edge_merge_bridge.h"
#include "../ui_task/se_nmo_sdk_bridge.h"

// 1:5万图幅 = 0.25° × 1/6°
static const double kTileLon = 0.25;
static const double kTileLat = 1.0 / 6.0;
// 接边比例尺类型：1 = 1:5万
static const int kScaleType = 1;
// 度/米近似换算（用于插件侧边界扫描）
// SDK 边界带按 1°≈110km 近似（黑盒实测），插件扫描口径与之对齐
static const double kMetersPerDeg = 110000.0;
static const double kDegPerMeter = 1.0 / kMetersPerDeg;
// 匹配字段下拉中的"不按属性"选项（传空字段列表=纯空间匹配）
static const QString kNoFieldOption = QStringLiteral("（不按属性，空间邻近）");
// SDK 邻幅反查安全阈值：blockIdx > 106 的区域邻幅计算失效（实测）
static const int kMaxSafeBlockIdx = 106;
// 接合线检测口径（与统计脚本一致）：断头贴线容差 10m，跨界判定容差 2m
static const double kCutDetectTolDeg = 10.0 / kMetersPerDeg;
static const double kCutSideTolDeg = 2.0 / kMetersPerDeg;
// 插件拉齐判定：两侧贴缝顶点沿线错开上限（米）
static const double kGapAlongM = 150.0;

// 运行时 GDAL 3.x 的 OGRCreateCoordinateTransformation 默认按"权威轴序"输出
// （EPSG:4326 纬度在前），会把几何变换成 (lat,lon) 顺序（实测 gdal308.dll
// 确证）。这里用不带 AXIS 声明的 WKT 构造 WGS84：SRS 无轴序信息时 GDAL
// 回退传统 (lon,lat) 顺序（运行时 DLL 实测生效）。走 C API（新旧版均导出；
// 旧头文件无 ImportFromWkt 方法，且小写废弃符号运行时可能不存在）
static OGRSpatialReference makeWgs84Srs()
{
    OGRSpatialReference srs;
    char* pszWkt = (char*)"GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";
    OSRImportFromWkt(&srs, &pszWkt);
    return srs;
}

// ===================================================================
// 构造与析构
// ===================================================================

AutoEdgeMatchDialog::AutoEdgeMatchDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);

    setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);
    // 关闭即销毁：下次从菜单打开是全新窗口，旧数据/报告不残留
    setAttribute(Qt::WA_DeleteOnClose);

    connect(ui.pushButton_addData,       &QPushButton::clicked, this, &AutoEdgeMatchDialog::addData);
    connect(ui.pushButton_removeDataset, &QPushButton::clicked, this, &AutoEdgeMatchDialog::removeDataset);
    connect(ui.pushButton_browseLog,     &QPushButton::clicked, this, &AutoEdgeMatchDialog::browseLog);
    connect(ui.pushButton_browseResult,  &QPushButton::clicked, this, &AutoEdgeMatchDialog::browseResult);
    connect(ui.pushButton_run,           &QPushButton::clicked, this, &AutoEdgeMatchDialog::onRun);
    connect(ui.pushButton_close,         &QPushButton::clicked, this, &QDialog::close);
    // 手动编辑路径后停止自动跟随（textEdited 仅在用户编辑时触发，程序 setText 不触发）
    connect(ui.lineEdit_logPath,    &QLineEdit::textEdited, this, [this] { m_bLogPathAutoFollow = false; });
    connect(ui.lineEdit_resultPath, &QLineEdit::textEdited, this, [this] { m_bResultPathAutoFollow = false; });
    // 跟踪用户是否改过缓冲距离：1:25万 大图幅输入仅在未改动时自动套 250 米
    connect(ui.doubleSpinBox_distance, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { m_distanceUserSet = true; });

    ui.tableWidget_report->setColumnCount(8);
    ui.tableWidget_report->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("接边情况") << QStringLiteral("图层") << QStringLiteral("图幅(本)")
        << QStringLiteral("FID(本)") << QStringLiteral("图幅(邻)") << QStringLiteral("FID(邻)")
        << QStringLiteral("字段值") << QStringLiteral("说明"));
    ui.tableWidget_report->horizontalHeader()->setStretchLastSection(true);
    ui.tableWidget_report->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui.tableWidget_report->setSelectionBehavior(QAbstractItemView::SelectRows);

    restoreState();
}

AutoEdgeMatchDialog::~AutoEdgeMatchDialog()
{
    saveState();
}

// ===================================================================
// 数据集列表操作
// ===================================================================

void AutoEdgeMatchDialog::addData()
{
    // 与格式转换同款树形勾选窗口：文件和文件夹可同时勾选、支持多选
    SrcSelectDialog dlg(QStringList() << QStringLiteral("shp")
                                      << QStringLiteral("gpkg")
                                      << QStringLiteral("geojson"),
                        true, true, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QStringList paths = dlg.selectedPaths();
    if (paths.isEmpty()) return;
    m_inputPath = QFileInfo(paths[0]).isDir() ? paths[0]
                                              : QFileInfo(paths[0]).absolutePath();
    addPaths(paths);
}

void AutoEdgeMatchDialog::addPaths(const QStringList& paths)
{
    // 文件夹：递归收集矢量文件；.gdb 是文件夹形式的数据库，整体作为一个数据集加入
    QStringList found;
    for (const QString& p : paths) {
        if (QFileInfo(p).isDir()) {
            QDirIterator fit(p,
                             QStringList() << QStringLiteral("*.shp")
                                           << QStringLiteral("*.gpkg")
                                           << QStringLiteral("*.geojson"),
                             QDir::Files | QDir::NoDotAndDotDot,
                             QDirIterator::Subdirectories);
            while (fit.hasNext()) {
                const QString f = fit.next();
                // 排除插件自身产出的结果库/检测报告，避免上次结果被当输入再次参与接边
                const QString base = QFileInfo(f).completeBaseName();
                if (base.startsWith(QStringLiteral("接边结果_")) ||
                    base.startsWith(QStringLiteral("接边检测报告_")))
                    continue;
                found << f;
            }
            QDirIterator dit(p, QDir::Dirs | QDir::NoDotAndDotDot,
                             QDirIterator::Subdirectories);
            while (dit.hasNext()) {
                const QString d = dit.next();
                if (d.endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
                    found << d;
            }
            if (p.endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
                found << p;
        } else {
            found << p;
        }
    }
    found.sort();

    // 已存在的路径不重复添加，避免同一数据重复参与接边
    QSet<QString> existing;
    for (int i = 0; i < ui.listWidget_datasets->count(); ++i)
        existing.insert(ui.listWidget_datasets->item(i)->text());
    int added = 0;
    for (const QString& p : found) {
        if (existing.contains(p)) continue;
        existing.insert(p);
        ui.listWidget_datasets->addItem(p);
        ++added;
    }
    if (added == 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("所选文件/文件夹下没有新的矢量文件（shp/gpkg/gdb/geojson）"));
        return;
    }
    populateFieldCombo();
    autoUpdateResultPath(); // 内部联动日志路径跟随
}

void AutoEdgeMatchDialog::closeEvent(QCloseEvent* event)
{
    // 执行期间禁止关闭：关闭后任务仍在后台运行，结束时弹出的询问框会"凭空出现"
    if (!ui.pushButton_run->isEnabled()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void AutoEdgeMatchDialog::removeDataset()
{
    int row = ui.listWidget_datasets->currentRow();
    if (row >= 0) {
        delete ui.listWidget_datasets->takeItem(row);
        populateFieldCombo();
    }
}

void AutoEdgeMatchDialog::browseLog()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("请选择日志保存路径"), m_logPath);
    if (!dir.isEmpty()) {
        m_logPath = dir;
        m_bLogPathAutoFollow = false;
        ui.lineEdit_logPath->setText(dir);
    }
}

void AutoEdgeMatchDialog::autoUpdateLogPath()
{
    // 日志默认跟随结果保存路径（结果路径未定/为空时不覆盖，避免清空已有值）
    if (!m_bLogPathAutoFollow) return;
    QString dir = ui.lineEdit_resultPath->text().trimmed();
    if (dir.isEmpty()) return;
    ui.lineEdit_logPath->setText(dir);
    m_logPath = dir;
}

void AutoEdgeMatchDialog::browseResult()
{
    QString startDir = ui.lineEdit_resultPath->text().trimmed();
    if (startDir.isEmpty()) {
        if (ui.listWidget_datasets->count() > 0)
            startDir = QFileInfo(ui.listWidget_datasets->item(0)->text()).absolutePath();
        else if (!m_resultPath.isEmpty())
            startDir = m_resultPath;
    }
    QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("请选择结果保存路径"), startDir);
    if (!dir.isEmpty()) {
        m_resultPath = dir;
        m_bResultPathAutoFollow = false;
        ui.lineEdit_resultPath->setText(dir);
        autoUpdateLogPath(); // 日志默认跟随结果路径
    }
}

void AutoEdgeMatchDialog::autoUpdateResultPath()
{
    if (!m_bResultPathAutoFollow) return;
    if (ui.listWidget_datasets->count() == 0) return;
    QString dir = QFileInfo(ui.listWidget_datasets->item(0)->text()).absolutePath();
    ui.lineEdit_resultPath->setText(dir);
    m_resultPath = dir;
    autoUpdateLogPath(); // 日志默认跟随结果路径
}

// ===================================================================
// 图幅号计算（1:5万，规则为黑盒实测所得）
// ===================================================================

namespace {

// 字段名编码检测（UTF-8 与 GBK），与合并功能口径一致
bool isValidUtf8(const QByteArray& raw, bool* hasMb)
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

QString geomWordFor(int flatType)
{
    switch (flatType) {
        case wkbPoint: case wkbMultiPoint: return QStringLiteral("point");
        case wkbLineString: case wkbMultiLineString: return QStringLiteral("line");
        case wkbPolygon: case wkbMultiPolygon: return QStringLiteral("polygon");
        default: return QString();
    }
}

// 图幅号编码（黑盒实测）：DN + band(2) + sheet(2) + blockIdx(3) + ones(1)
// blockIdx = blockRow*12 + blockCol + 1；ones = 2×2 块内偏移（1..4）
QString makeTileCode(int band, int sheet, int lonIdx, int latIdx)
{
    const int blockRow = latIdx / 2;
    const int blockCol = lonIdx / 2;
    const int blockIdx = blockRow * 12 + blockCol + 1;
    const int ones = (lonIdx % 2) + (latIdx % 2) * 2 + 1;
    return QStringLiteral("DN%1%2%3%4")
               .arg(band, 2, 10, QLatin1Char('0'))
               .arg(sheet, 2, 10, QLatin1Char('0'))
               .arg(blockIdx, 3, 10, QLatin1Char('0'))
               .arg(ones);
}

int blockIdxOf(int lonIdx, int latIdx)
{
    return (latIdx / 2) * 12 + (lonIdx / 2) + 1;
}

// 所有顶点纬度平移 dy（递归处理点/线/环/多边形/多部件）
void shiftLatRec(OGRGeometry* g, double dy)
{
    if (!g) return;
    switch (wkbFlatten(g->getGeometryType())) {
        case wkbPoint: {
            OGRPoint* p = static_cast<OGRPoint*>(g);
            p->setY(p->getY() + dy);
            break;
        }
        case wkbLineString: {
            OGRLineString* l = static_cast<OGRLineString*>(g);
            for (int i = 0; i < l->getNumPoints(); ++i)
                l->setPoint(i, l->getX(i), l->getY(i) + dy, l->getZ(i));
            break;
        }
        default:
            for (int i = 0; i < OGR_G_GetGeometryCount(g); ++i)
                shiftLatRec((OGRGeometry*)OGR_G_GetGeometryRef(g, i), dy);
            break;
    }
}

} // namespace

// 多部件拆单部件（定义见 buildGpkg 前，与 SDK 顶点收集崩溃规避同一函数）
static void collectSingleParts(OGRGeometry* poGeom, QVector<OGRGeometry*>& out);

// 单点归属图幅：口径与图幅公式一致——恰在网格线上的点归属东侧/南侧单元
// （floor+1e-9 与 ceil-1e-9，同图幅公式）
bool AutoEdgeMatchDialog::computeCellOfXY(double lon, double lat, TileInfo* out)
{
    const int sheet = (int)std::floor((lon + 186.0) / 6.0 + 1e-9);
    const double baseLon = sheet * 6.0 - 186.0;
    const int lonIdx = (int)std::floor((lon - baseLon) / kTileLon + 1e-9);
    const int band = (int)std::ceil(lat / 4.0 - 1e-9);
    const int latIdx = (int)std::floor((band * 4.0 - lat) * 6.0 + 1e-9);
    if (band < 1 || sheet < 1 || latIdx < 0 || latIdx > 23 || lonIdx < 0 || lonIdx > 23)
        return false;
    out->realCode = makeTileCode(band, sheet, lonIdx, latIdx);
    out->code = out->realCode;
    out->lonMin = baseLon + lonIdx * kTileLon;
    out->lonMax = out->lonMin + kTileLon;
    out->latMax = band * 4.0 - latIdx / 6.0;
    out->latMin = out->latMax - kTileLat;
    out->latIdx = latIdx;
    out->lonIdx = lonIdx;
    out->shiftLat = 0;
    out->needProj = false;
    return true;
}

// 要素归属图幅：取要素（WGS84）外接矩形中点所在网格单元
bool AutoEdgeMatchDialog::computeCellOf(const OGRGeometry* poGeom, TileInfo* out)
{
    OGREnvelope env;
    poGeom->getEnvelope(&env);
    if (!env.IsInit()) return false;
    return computeCellOfXY((env.MinX + env.MaxX) / 2.0,
                           (env.MinY + env.MaxY) / 2.0, out);
}

namespace {

// 接合线键：经线 k=经度×4 取整、纬线 k=纬度×6 取整（整数键避免浮点比较）
inline QString edgeKey(char dir, int k)
{
    return QString(QLatin1Char(dir)) + QStringLiteral("|") + QString::number(k);
}

// 收集单部件顶点（线：全部点；面：外环；与统计脚本口径一致）
void collectPartPts(const OGRGeometry* g, QVector<QPair<double, double>>* pts)
{
    pts->clear();
    const OGRwkbGeometryType flat = wkbFlatten(g->getGeometryType());
    if (flat == wkbLineString) {
        const OGRLineString* l = static_cast<const OGRLineString*>(g);
        for (int i = 0; i < l->getNumPoints(); ++i)
            pts->append(qMakePair(l->getX(i), l->getY(i)));
    } else if (flat == wkbPolygon) {
        const OGRPolygon* p = static_cast<const OGRPolygon*>(g);
        const OGRLinearRing* r = p->getExteriorRing();
        if (!r) return;
        for (int i = 0; i < r->getNumPoints(); ++i)
            pts->append(qMakePair(r->getX(i), r->getY(i)));
    }
}

// 要素是否跨线：线两侧 2m 外均有顶点
bool crossesLine(const QVector<QPair<double, double>>& pts, char dir, double v)
{
    bool neg = false, pos = false;
    for (const auto& p : pts) {
        const double c = (dir == 'E') ? p.first : p.second;
        if (c > v + kCutSideTolDeg) pos = true;
        else if (c < v - kCutSideTolDeg) neg = true;
        if (neg && pos) return true;
    }
    return false;
}

// 累计接合线统计：断头（线两端点/面外环顶点贴线 10m、且要素不跨该线，侧向取
// 容差外顶点主导侧）+ 跨界（线段严格穿过）
void collectEdgeStats(const OGRGeometry* g, AutoEdgeMatchDialog::EdgeStats* st)
{
    QVector<QPair<double, double>> pts;
    collectPartPts(g, &pts);
    if (pts.size() < 2) return;
    const OGRwkbGeometryType flat = wkbFlatten(g->getGeometryType());
    QVector<int> candIdx;
    if (flat == wkbLineString)
        candIdx << 0 << (pts.size() - 1);
    else
        for (int i = 0; i < pts.size(); ++i) candIdx << i;

    QSet<QString> seen;
    for (int ci : candIdx) {
        const double x = pts[ci].first, y = pts[ci].second;
        const int gx4 = (int)std::lround(x * 4.0);
        const QString kE = edgeKey('E', gx4);
        if (!seen.contains(kE) && std::fabs(x - gx4 / 4.0) <= kCutDetectTolDeg) {
            seen.insert(kE);
            int cnt = 0;
            for (const auto& p : pts) {
                if (p.first > gx4 / 4.0 + kCutSideTolDeg) ++cnt;
                else if (p.first < gx4 / 4.0 - kCutSideTolDeg) --cnt;
            }
            if (cnt > 0) ++st->cutPos[kE];
            else if (cnt < 0) ++st->cutNeg[kE];
        }
        const int gy6 = (int)std::lround(y * 6.0);
        const QString kN = edgeKey('N', gy6);
        if (!seen.contains(kN) && std::fabs(y - gy6 / 6.0) <= kCutDetectTolDeg) {
            seen.insert(kN);
            int cnt = 0;
            for (const auto& p : pts) {
                if (p.second > gy6 / 6.0 + kCutSideTolDeg) ++cnt;
                else if (p.second < gy6 / 6.0 - kCutSideTolDeg) --cnt;
            }
            if (cnt > 0) ++st->cutPos[kN];
            else if (cnt < 0) ++st->cutNeg[kN];
        }
    }

    QSet<QString> cset;
    for (int i = 0; i + 1 < pts.size(); ++i) {
        const double x1 = pts[i].first, y1 = pts[i].second;
        const double x2 = pts[i + 1].first, y2 = pts[i + 1].second;
        const double xa = std::min(x1, x2), xb = std::max(x1, x2);
        const double ya = std::min(y1, y2), yb = std::max(y1, y2);
        for (int k = (int)std::ceil(xa * 4.0 - 1e-9); k <= (int)std::floor(xb * 4.0 + 1e-9); ++k) {
            const double gx = k / 4.0;
            if (xa < gx && gx < xb) cset.insert(edgeKey('E', k));
        }
        for (int k = (int)std::ceil(ya * 6.0 - 1e-9); k <= (int)std::floor(yb * 6.0 + 1e-9); ++k) {
            const double gy = k / 6.0;
            if (ya < gy && gy < yb) cset.insert(edgeKey('N', k));
        }
    }
    for (const QString& k : cset) ++st->cross[k];
}

// 断头要素判定（工作库过滤）：端点/外环顶点在接合线带内（带=接边距离）、
// 且要素不跨该线。命中返回贴线顶点（用于路由到端点所在图幅）与要素相对
// 该顶点最近格网线的主体侧向（+1 东/北，-1 西/南，0 无法判定），供路由纠偏
bool qualifyEdgeFeature(const OGRGeometry* g, const AutoEdgeMatchDialog::EdgeFilterCtx* ctx,
                        double* outLon, double* outLat, int* outSideE, int* outSideN)
{
    QVector<QPair<double, double>> pts;
    collectPartPts(g, &pts);
    if (pts.size() < 2) return false;
    const OGRwkbGeometryType flat = wkbFlatten(g->getGeometryType());
    QVector<int> candIdx;
    if (flat == wkbLineString)
        candIdx << 0 << (pts.size() - 1);
    else
        for (int i = 0; i < pts.size(); ++i) candIdx << i;
    for (int ci : candIdx) {
        const double x = pts[ci].first, y = pts[ci].second;
        const int gx4 = (int)std::lround(x * 4.0);
        const double gx = gx4 / 4.0;
        if (std::fabs(x - gx) <= ctx->bandDeg
            && ctx->activeKeys.contains(edgeKey('E', gx4))
            && !crossesLine(pts, 'E', gx)) {
            const double gy = std::lround(y * 6.0) / 6.0;
            int ce = 0, cn = 0;
            for (const auto& p : pts) {
                if (p.first > gx + kCutSideTolDeg) ++ce;
                else if (p.first < gx - kCutSideTolDeg) --ce;
                if (p.second > gy + kCutSideTolDeg) ++cn;
                else if (p.second < gy - kCutSideTolDeg) --cn;
            }
            *outLon = x; *outLat = y;
            *outSideE = ce > 0 ? 1 : (ce < 0 ? -1 : 0);
            *outSideN = cn > 0 ? 1 : (cn < 0 ? -1 : 0);
            return true;
        }
        const int gy6 = (int)std::lround(y * 6.0);
        const double gy = gy6 / 6.0;
        if (std::fabs(y - gy) <= ctx->bandDeg
            && ctx->activeKeys.contains(edgeKey('N', gy6))
            && !crossesLine(pts, 'N', gy)) {
            const double gxv = std::lround(x * 4.0) / 4.0;
            int ce = 0, cn = 0;
            for (const auto& p : pts) {
                if (p.first > gxv + kCutSideTolDeg) ++ce;
                else if (p.first < gxv - kCutSideTolDeg) --ce;
                if (p.second > gy + kCutSideTolDeg) ++cn;
                else if (p.second < gy - kCutSideTolDeg) --cn;
            }
            *outLon = x; *outLat = y;
            *outSideE = ce > 0 ? 1 : (ce < 0 ? -1 : 0);
            *outSideN = cn > 0 ? 1 : (cn < 0 ? -1 : 0);
            return true;
        }
    }
    return false;
}

// 工作库 FID → 结果库 FID（全量模式两者一致；映射缺失返回 -1）
qint64 mapWorkFid(const AutoEdgeMatchDialog::EdgeFilterCtx* ctx,
                  const QString& workSheet, const QString& gtype, qint64 fid)
{
    if (!ctx) return fid;
    const auto it = ctx->workMap.constFind(
        workSheet + QStringLiteral("_L_") + gtype + QStringLiteral("|") + QString::number(fid));
    return it == ctx->workMap.constEnd() ? -1 : it->second;
}

// 工作库 (图幅号,FID) → (结果图层名, 结果FID)；映射缺失返回空图层名
QPair<QString, qint64> mapWorkToResult(const AutoEdgeMatchDialog::EdgeFilterCtx* ctx,
                                       const QString& workSheet, const QString& gtype,
                                       qint64 fid)
{
    if (!ctx) return qMakePair(QString(), fid);
    const auto it = ctx->workMap.constFind(
        workSheet + QStringLiteral("_L_") + gtype + QStringLiteral("|") + QString::number(fid));
    return it == ctx->workMap.constEnd() ? qMakePair(QString(), qint64(-1)) : it.value();
}

// 接合线描述（日志用）："x=115.5000°（断头120/212 跨界42）"
QString lineDesc(const QString& key, int n, int p, int c)
{
    const char dir = key[0].toLatin1();
    const int k = key.mid(2).toInt();
    const double v = (dir == 'E') ? k / 4.0 : k / 6.0;
    return QStringLiteral("%1=%2°（断头%3/%4 跨界%5）")
        .arg(dir == 'E' ? QStringLiteral("x") : QStringLiteral("y"))
        .arg(v, 0, 'f', 4).arg(n).arg(p).arg(c);
}

} // namespace

// 跨图幅自动拆分：逐要素（多部件拆单部件后）按 WGS84 外接矩形中点归入所在
// 图幅，返回数据集覆盖的全部图幅（按真实图幅号去重）。点要素不产生图幅
// （SDK 不接点；结果库中点要素的归属由 buildGpkg 按同一规则实时判定）
bool AutoEdgeMatchDialog::computeTileInfos(const QString& path, QVector<TileInfo>* out, QString* err,
                                          EdgeStats* stats,
                                          const std::function<void(double)>& onProgress)
{
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
        path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poDS) {
        if (err) *err = QStringLiteral("无法打开数据: %1").arg(path);
        return false;
    }

    OGRSpatialReference wgs84 = makeWgs84Srs();
    bool needProj = false;
    // 全数据集范围（含点要素），用于纯点/退化数据的单图幅兜底
    OGREnvelope totalEnv;
    bool haveEnv = false;
    QHash<QString, TileInfo> byReal;
    for (int li = 0; li < poDS->GetLayerCount(); ++li) {
        OGRLayer* poLayer = poDS->GetLayer(li);
        if (!poLayer || wkbFlatten(poLayer->GetGeomType()) == wkbNone) continue;
        // 接合线统计只取线/面图层（与统计脚本口径一致）
        const bool doStats = stats
            && (wkbFlatten(poLayer->GetGeomType()) == wkbLineString
                || wkbFlatten(poLayer->GetGeomType()) == wkbPolygon);
        OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
        OGRCoordinateTransformation* poCT = nullptr;
        if (poSRS && !poSRS->IsSame(&wgs84)) {
            poCT = OGRCreateCoordinateTransformation(poSRS, &wgs84);
            if (!poCT) {
                GDALClose(poDS);
                if (err) *err = QStringLiteral("无法创建投影转换: %1").arg(path);
                return false;
            }
            needProj = true;
        }
        poLayer->ResetReading();
        const GIntBig featTotal = poLayer->GetFeatureCount();
        GIntBig featDone = 0;
        OGRFeature* poFeat = nullptr;
        while ((poFeat = poLayer->GetNextFeature()) != nullptr) {
            ++featDone;
            if (onProgress && (featDone % 1000) == 0) {
                const double frac = featTotal > 0
                    ? std::min(1.0, (double)featDone / (double)featTotal) : 0.0;
                onProgress((li + frac) / poDS->GetLayerCount());
            }
            OGRGeometry* poGeom = poFeat->GetGeometryRef();
            if (poGeom && !poGeom->IsEmpty()) {
                OGRGeometry* poClone = poGeom->clone();
                if (poClone) {
                    if (!poCT || poClone->transform(poCT) == OGRERR_NONE) {
                        OGREnvelope env;
                        poClone->getEnvelope(&env);
                        if (env.IsInit()) {
                            if (!haveEnv) { totalEnv = env; haveEnv = true; }
                            else totalEnv.Merge(env);
                            if (doStats) {
                                if (!stats->haveEnv) {
                                    stats->envMinX = env.MinX; stats->envMaxX = env.MaxX;
                                    stats->envMinY = env.MinY; stats->envMaxY = env.MaxY;
                                    stats->haveEnv = true;
                                } else {
                                    stats->envMinX = std::min(stats->envMinX, env.MinX);
                                    stats->envMaxX = std::max(stats->envMaxX, env.MaxX);
                                    stats->envMinY = std::min(stats->envMinY, env.MinY);
                                    stats->envMaxY = std::max(stats->envMaxY, env.MaxY);
                                }
                            }
                        }
                        QVector<OGRGeometry*> parts;
                        collectSingleParts(poClone, parts);
                        for (OGRGeometry* part : parts) {
                            TileInfo cell;
                            if (computeCellOf(part, &cell))
                                byReal.insert(cell.realCode, cell);
                            if (doStats)
                                collectEdgeStats(part, stats);
                        }
                    }
                    OGRGeometryFactory::destroyGeometry(poClone);
                }
            }
            OGRFeature::DestroyFeature(poFeat);
        }
        if (poCT) OCTDestroyCoordinateTransformation(poCT);
        if (onProgress)
            onProgress((li + 1.0) / poDS->GetLayerCount());
    }
    GDALClose(poDS);

    // 纯点数据兜底：无任何线/面部件时按全范围中点取单图幅（Round 1 行为）
    if (byReal.isEmpty() && haveEnv) {
        OGRPoint pt;
        pt.setX((totalEnv.MinX + totalEnv.MaxX) / 2.0);
        pt.setY((totalEnv.MinY + totalEnv.MaxY) / 2.0);
        TileInfo cell;
        if (computeCellOf(&pt, &cell))
            byReal.insert(cell.realCode, cell);
    }
    if (byReal.isEmpty()) {
        if (err) *err = QStringLiteral("数据中没有有效几何要素: %1").arg(path);
        return false;
    }
    out->clear();
    for (auto it = byReal.constBegin(); it != byReal.constEnd(); ++it) {
        TileInfo t = it.value();
        t.needProj = needProj;
        out->append(t);
    }
    std::sort(out->begin(), out->end(), [](const TileInfo& a, const TileInfo& b) {
        return a.realCode < b.realCode;
    });
    return true;
}

// SDK 邻幅反查对 blockIdx > 106 的区域失效；需要时全体图幅整体南移
// 1~3 个整度（6 行/度，不改变图幅号个位），取使全部平移后 blockIdx <= 106 的最小值
bool AutoEdgeMatchDialog::chooseShift(QVector<TileInfo>* tiles, QString* err)
{
    bool needShift = false;
    for (const TileInfo& t : *tiles) {
        if (blockIdxOf(t.lonIdx, t.latIdx) > kMaxSafeBlockIdx) {
            needShift = true;
            break;
        }
    }
    if (!needShift) return true;

    for (int s = 1; s <= 3; ++s) {
        bool ok = true;
        for (TileInfo& t : *tiles) {
            const double top = t.latMax - s;
            const int band = (int)std::ceil(top / 4.0 - 1e-9);
            const double latIdxD = std::round((band * 4.0 - top) * 6.0);
            if (band < 1 || latIdxD < 0 || latIdxD > 23) { ok = false; break; }
            if (blockIdxOf(t.lonIdx, (int)latIdxD) > kMaxSafeBlockIdx) { ok = false; break; }
        }
        if (!ok) continue;
        for (TileInfo& t : *tiles) {
            const double top = t.latMax - s;
            const int band = (int)std::ceil(top / 4.0 - 1e-9);
            const int latIdx = (int)std::round((band * 4.0 - top) * 6.0);
            const int sheet = (int)std::floor((t.lonMin + 186.0) / 6.0 + 1e-9);
            t.code = makeTileCode(band, sheet, t.lonIdx, latIdx);
            t.shiftLat = s;
        }
        return true;
    }
    if (err)
        *err = QStringLiteral("数据纬度范围超出接边SDK邻幅计算支持（约2.9°），请按纬度拆分为多组后分别接边");
    return false;
}

// ===================================================================
// 库构建：按图幅号_字母_几何类型建层 + 末尾建记录表
// BuildResult：真实图幅号 + 原始几何/坐标系（用户可见的结果库）
// BuildWork：平移后图幅号 + WGS84 几何（供 SDK 使用的工作库）
// 两块数据同图幅时要素合并入同一层（字母统一 L）
// ===================================================================

// SDK 顶点收集器在多部件/点几何上会调用 gdal308 vtable 中不存在的槽位而崩溃：
// 多部件必须拆成单部件要素，点要素 SDK 不支持（GEO_TYPE 仅 line/polygon）
static void collectSingleParts(OGRGeometry* poGeom, QVector<OGRGeometry*>& out)
{
    if (!poGeom) return;
    OGRwkbGeometryType flat = wkbFlatten(poGeom->getGeometryType());
    if (flat == wkbLineString || flat == wkbPolygon) {
        out.append(poGeom);
        return;
    }
    if (flat == wkbPoint || flat == wkbMultiPoint) return;
    if (OGR_GT_IsSubClassOf(flat, wkbGeometryCollection)) {
        int n = OGR_G_GetGeometryCount((OGRGeometryH)poGeom);
        for (int i = 0; i < n; ++i)
            collectSingleParts((OGRGeometry*)OGR_G_GetGeometryRef((OGRGeometryH)poGeom, i), out);
    }
}

bool AutoEdgeMatchDialog::buildGpkg(const QString& gpkgPath, const QVector<TileInfo>& tiles,
                                    BuildMode mode, QString* err,
                                    QStringList* outResultLayers, GDALDataset** outDS,
                                    const std::function<void(double)>& onProgress,
                                    EdgeFilterCtx* ctx)
{
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (!poDrv) {
        if (err) *err = QStringLiteral("GDAL 缺少 GPKG 驱动");
        return false;
    }
    GDALDataset* poDstDS = poDrv->Create(gpkgPath.toUtf8().constData(), 0, 0, 0,
                                         GDT_Unknown, nullptr);
    if (!poDstDS) {
        if (err) *err = QStringLiteral("无法创建数据库: %1").arg(gpkgPath);
        return false;
    }

    OGRSpatialReference wgs84 = makeWgs84Srs();

    // 跨图幅拆分：按真实图幅号查工作库图幅号（可能为平移后）与平移量
    QHash<QString, QString> codeByReal;
    QHash<QString, double> shiftByReal;
    for (const TileInfo& t : tiles) {
        codeByReal.insert(t.realCode, t.code);
        shiftByReal.insert(t.realCode, t.shiftLat);
    }

    bool ok = true;

    struct DstLayerInfo {
        OGRLayer* layer = nullptr;
        QString name;
        QVector<int> srcIdxForDst;  // 目标字段 -> 源字段索引（-1 表示无）
        bool srcIsGbk = false;
        qint64 nextFid = 1;         // 新建层 FID 从 1 起顺序分配（GetFID 兜底用）
    };
    QHash<QString, DstLayerInfo> dstByKey;  // 键：工作库=图幅号+"|"+几何词；结果库=主题层名

    // 逐数据集展平入库：结果库同名同主题跨数据集合为一层（保持原坐标系与整体
    // 几何，不做图幅切层）；工作库按 SDK 约定分幅分层（code_L_几何词，WGS84）
    for (int di = 0; di < ui.listWidget_datasets->count(); ++di) {
        GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(
            ui.listWidget_datasets->item(di)->text().toUtf8().constData(),
            GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poSrcDS) {
            if (err) *err = QStringLiteral("无法打开数据: %1").arg(ui.listWidget_datasets->item(di)->text());
            ok = false;
            break;
        }

        // 结果库落在 exFAT 盘时，逐要素自动提交每次约 10-15ms（j50 11.9 万要素
        // 约半小时）；按数据集包成显式事务，整批落盘一次
        poDstDS->ExecuteSQL("BEGIN", nullptr, "SQLITE");
        for (int li = 0; li < poSrcDS->GetLayerCount(); ++li) {
            OGRLayer* poSrcLayer = poSrcDS->GetLayer(li);
            if (!poSrcLayer) continue;
            if (wkbFlatten(poSrcLayer->GetGeomType()) == wkbNone) continue; // 跳过非空间表

            OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();

            // 源字段名解码（按文件级编码判定）
            QList<QByteArray> rawNames;
            bool fileIsUtf8 = true;
            for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f) {
                QByteArray raw(poSrcDefn->GetFieldDefn(f)->GetNameRef());
                rawNames.append(raw);
                bool hasMb = false;
                bool fieldUtf8 = isValidUtf8(raw, &hasMb);
                if (hasMb && !fieldUtf8) fileIsUtf8 = false;
            }
            QStringList srcNames;
            for (const QByteArray& raw : rawNames) {
                bool hasMb = false;
                isValidUtf8(raw, &hasMb);
                srcNames.append((fileIsUtf8 && hasMb) ? QString::fromUtf8(raw)
                                : hasMb ? QString::fromLocal8Bit(raw)
                                : QString::fromUtf8(raw));
            }

            // 结果库主题层名 = 源图层名净化后直用（工作库按图幅号命名，不用它）
            QByteArray rawLName(poSrcLayer->GetName());
            bool hasMbL = false;
            isValidUtf8(rawLName, &hasMbL);
            QString srcLayerName = (fileIsUtf8 && hasMbL) ? QString::fromUtf8(rawLName)
                                   : hasMbL ? QString::fromLocal8Bit(rawLName)
                                   : QString::fromUtf8(rawLName);
            srcLayerName.replace(QStringLiteral("/"), QStringLiteral("_"));
            srcLayerName.replace(QStringLiteral("\\"), QStringLiteral("_"));
            srcLayerName.replace(QStringLiteral(":"), QStringLiteral("_"));
            if (srcLayerName.isEmpty())
                srcLayerName = QStringLiteral("layer_%1").arg(li);
            if (srcLayerName == QStringLiteral("edge_records"))
                srcLayerName = QStringLiteral("edge_records_data");

            // 图幅归属判定需 WGS84：仅工作库建 CT（结果库保持原坐标系，多源
            // 同名层坐标系不同时在入库前按目标层坐标系统一变换）
            OGRSpatialReference* poSrcSRS = poSrcLayer->GetSpatialRef();
            OGRCoordinateTransformation* poCT = nullptr;
            if (mode == BuildWork && poSrcSRS && !poSrcSRS->IsSame(&wgs84)) {
                poCT = OGRCreateCoordinateTransformation(poSrcSRS, &wgs84);
                if (!poCT) {
                    if (err) *err = QStringLiteral("无法创建投影转换（图层 %1）").arg(QString::fromUtf8(poSrcLayer->GetName()));
                    ok = false;
                    break;
                }
            }

            poSrcLayer->ResetReading();
            const GIntBig featTotal = poSrcLayer->GetFeatureCount();
            GIntBig featDone = 0;
            OGRFeature* poFeat = nullptr;
            while ((poFeat = poSrcLayer->GetNextFeature()) != nullptr) {
                ++featDone;
                // 单图层要素级进度：shp 数据一个文件一层，图层级粒度仍会
                // 长时间停在某个大数据集上；每 1000 要素推进一次进度条
                if (onProgress && (featDone % 1000) == 0) {
                    const double frac = featTotal > 0
                        ? std::min(1.0, (double)featDone / (double)featTotal) : 0.0;
                    onProgress((di + (li + frac) / poSrcDS->GetLayerCount())
                               / ui.listWidget_datasets->count());
                }
                OGRGeometry* poGeom = poFeat->GetGeometryRef();
                // 多部件拆成单部件要素（每部件一个要素）；点要素 SDK 不支持接边，
                // 工作库剔除（见 collectSingleParts），结果库保留（GPKG FID 按层独立，
                // 点层增删不影响线/面层的接边记录对应）
                QVector<OGRGeometry*> parts;
                collectSingleParts(poGeom, parts);
                if (parts.isEmpty() && mode == BuildResult) {
                    int ft = poGeom ? wkbFlatten(poGeom->getGeometryType()) : wkbUnknown;
                    if (ft == wkbPoint || ft == wkbMultiPoint) parts.append(poGeom);
                }
                if (parts.isEmpty()) {
                    OGRFeature::DestroyFeature(poFeat);
                    continue;
                }
                for (int pi = 0; pi < parts.size(); ++pi) {
                    OGRGeometry* poPart = parts[pi];
                    int flatType = wkbFlatten(poPart->getGeometryType());
                    QString word = geomWordFor(flatType);
                    if (word.isEmpty()) continue;

                    // 结果库：整体几何按源图层主题入层，不做图幅路由（1:25万 等
                    // 大图幅输入若按 1:5万 切层，会把 36 个输入图层炸成数百个
                    // 碎层）；工作库：SDK 要求按图幅分层的 WGS84 几何，接合线
                    // 模式只收断头要素，并按贴线顶点路由到端点所在图幅（SDK 只
                    // 认图幅边界带内的端点，中点路由会把长要素端点挪到图幅内部
                    // 而漏配，故按端点路由）
                    OGRGeometry* poGeomOut = poPart->clone();
                    QString code;
                    TileInfo cell;
                    if (mode == BuildWork) {
                        OGRGeometry* poGeomWgs = poGeomOut;
                        if (poCT) poGeomOut->transform(poCT);
                        double touchLon = 0, touchLat = 0;
                        int touchSideE = 0, touchSideN = 0;
                        bool routeByTouch = false;
                        if (ctx && !ctx->activeKeys.isEmpty()) {
                            if (!qualifyEdgeFeature(poGeomWgs, ctx, &touchLon, &touchLat,
                                                    &touchSideE, &touchSideN)) {
                                ++ctx->filteredOut;
                                OGRGeometryFactory::destroyGeometry(poGeomOut);
                                continue;
                            }
                            routeByTouch = true;
                        }
                        if (routeByTouch) {
                            // 贴线顶点恰在接合线上时，投影往返浮点噪声（±1e-8 级，
                            // 超过 computeCellOfXY 的 1e-9 容差）会随机决定其归属
                            // 东/西侧图幅：错侧路由会让 SDK 在同图幅内看到成对要素
                            // 而漏配。按要素主体侧向轻推 1m（远小于缓冲带、远大于
                            // 噪声）保证路由到自身一侧；顶点位于格网角时两轴同时纠偏。
                            const double kTouchEps = 1e-6, kTouchNudge = 1e-5;
                            if (touchSideE != 0 &&
                                std::fabs(touchLon - std::lround(touchLon * 4.0) / 4.0) <= kTouchEps)
                                touchLon = std::lround(touchLon * 4.0) / 4.0 + touchSideE * kTouchNudge;
                            if (touchSideN != 0 &&
                                std::fabs(touchLat - std::lround(touchLat * 6.0) / 6.0) <= kTouchEps)
                                touchLat = std::lround(touchLat * 6.0) / 6.0 + touchSideN * kTouchNudge;
                        }
                        bool cellOk = routeByTouch ? computeCellOfXY(touchLon, touchLat, &cell)
                                                   : computeCellOf(poGeomWgs, &cell);
                        if (!cellOk && routeByTouch)
                            cellOk = computeCellOf(poGeomWgs, &cell); // 贴线顶点越界时退回中点路由
                        if (!cellOk) {
                            OGRGeometryFactory::destroyGeometry(poGeomOut);
                            continue;
                        }
                        code = codeByReal.value(cell.realCode);
                        if (code.isEmpty() && !tiles.isEmpty() && tiles[0].shiftLat > 0) {
                            // 贴线顶点所在图幅不在已识别图幅集内（极少见：该图幅只有长要素
                            // 尾部经过）：按同一纬度平移量补算工作图幅号，避免无平移导致
                            // SDK 读到错误图幅
                            const double s = tiles[0].shiftLat;
                            const double top = cell.latMax - s;
                            const int band = (int)std::ceil(top / 4.0 - 1e-9);
                            const int latIdx = (int)std::round((band * 4.0 - top) * 6.0);
                            const int sheet = (int)std::floor((cell.lonMin + 186.0) / 6.0 + 1e-9);
                            code = makeTileCode(band, sheet, cell.lonIdx, latIdx);
                        }
                        if (code.isEmpty()) code = cell.realCode;
                    }

                    // 工作库"图幅号_L_几何词"（SDK 硬约定，字母统一 L）；结果库
                    // 按源图层主题合并：同名同主题跨数据集合为一层（4 幅 1:25万
                    // 的 lrdl 合成一个 lrdl 层），同名不同几何类型时后缀几何词区分
                    QString layerKey, layerName;
                    if (mode == BuildWork) {
                        layerKey = code + QStringLiteral("|") + word;
                        layerName = QStringLiteral("%1_L_%2").arg(code, word);
                    } else {
                        layerName = srcLayerName;
                        layerKey = layerName;
                        const auto it0 = dstByKey.constFind(layerKey);
                        if (it0 != dstByKey.constEnd() && it0->layer &&
                            wkbFlatten(it0->layer->GetGeomType()) != flatType) {
                            layerName = srcLayerName + QStringLiteral("_") + word;
                            layerKey = layerName;
                        }
                    }
                    DstLayerInfo& dst = dstByKey[layerKey];
                    if (!dst.layer) {
                        dst.name = layerName;
                        OGRSpatialReference* poSrsOut = (mode == BuildWork) ? &wgs84 : poSrcSRS;
                        dst.layer = poDstDS->CreateLayer(dst.name.toUtf8().constData(), poSrsOut,
                                                         (OGRwkbGeometryType)flatType, nullptr);
                        if (!dst.layer) {
                            if (err) *err = QStringLiteral("创建工作图层失败: %1").arg(dst.name);
                            ok = false;
                            break;
                        }
                        // 以首个源图层为准建立字段
                        for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f) {
                            OGRFieldDefn fld(srcNames[f].toUtf8().constData(),
                                             poSrcDefn->GetFieldDefn(f)->GetType());
                            fld.SetWidth(poSrcDefn->GetFieldDefn(f)->GetWidth());
                            fld.SetPrecision(poSrcDefn->GetFieldDefn(f)->GetPrecision());
                            dst.layer->CreateField(&fld);
                        }
                        dst.srcIsGbk = !fileIsUtf8;
                        dst.srcIdxForDst.resize(poSrcDefn->GetFieldCount());
                        for (int f = 0; f < poSrcDefn->GetFieldCount(); ++f)
                            dst.srcIdxForDst[f] = f;
                    } else {
                        // 后续源图层字段可能不同：按名称映射
                        QVector<int> map(dst.layer->GetLayerDefn()->GetFieldCount(), -1);
                        for (int f = 0; f < dst.layer->GetLayerDefn()->GetFieldCount(); ++f) {
                            QString dstName = QString::fromUtf8(
                                dst.layer->GetLayerDefn()->GetFieldDefn(f)->GetNameRef());
                            map[f] = srcNames.indexOf(dstName);
                        }
                        dst.srcIdxForDst = map;
                        dst.srcIsGbk = !fileIsUtf8;
                    }

                    OGRFeature dstFeat(dst.layer->GetLayerDefn());
                    OGRFeatureDefn* poDstDefn = dst.layer->GetLayerDefn();
                    for (int f = 0; f < poDstDefn->GetFieldCount(); ++f) {
                        int si = dst.srcIdxForDst.value(f, -1);
                        if (si < 0 || !poFeat->IsFieldSetAndNotNull(si)) continue;
                        switch (poDstDefn->GetFieldDefn(f)->GetType()) {
                            case OFTInteger:
                                dstFeat.SetField(f, poFeat->GetFieldAsInteger(si));
                                break;
                            case OFTInteger64:
                                dstFeat.SetField(f, poFeat->GetFieldAsInteger64(si));
                                break;
                            case OFTReal:
                                dstFeat.SetField(f, poFeat->GetFieldAsDouble(si));
                                break;
                            case OFTString: {
                                const char* val = poFeat->GetFieldAsString(si);
                                QByteArray bytes(val ? val : "");
                                bool hasMb = false;
                                if (dst.srcIsGbk && !isValidUtf8(bytes, &hasMb))
                                    dstFeat.SetField(f, QString::fromLocal8Bit(bytes).toUtf8().constData());
                                else
                                    dstFeat.SetField(f, bytes.constData());
                                break;
                            }
                            default:
                                break;
                        }
                    }
                    if (mode == BuildWork) {
                        const double shift =
                            shiftByReal.value(cell.realCode, tiles.isEmpty() ? 0.0 : tiles[0].shiftLat);
                        if (shift > 0) shiftLatRec(poGeomOut, -shift);
                    }
                    // 结果库同名主题层跨数据集合并：后续数据集坐标系可能与
                    // 首建层不一致，入库前统一变换到目标层坐标系
                    if (mode == BuildResult && poSrcSRS) {
                        OGRSpatialReference* poDstSRS = dst.layer->GetSpatialRef();
                        if (poDstSRS && !poSrcSRS->IsSame(poDstSRS)) {
                            OGRCoordinateTransformation* poToDst =
                                OGRCreateCoordinateTransformation(poSrcSRS, poDstSRS);
                            if (poToDst) {
                                poGeomOut->transform(poToDst);
                                OCTDestroyCoordinateTransformation(poToDst);
                            }
                        }
                    }
                    dstFeat.SetGeometry(poGeomOut);
                    if (dst.layer->CreateFeature(&dstFeat) != OGRERR_NONE) {
                        if (err) *err = QStringLiteral("写入要素失败（图层 %1）").arg(dst.name);
                        ok = false;
                        break;
                    }
                    // 源键→(结果图层,结果FID) 与 工作(图层,FID)→结果 双向映射：
                    // 结果库按主题合并后 FID 与工作库不再一一对应（全量/接合线
                    // 模式都一样），接边记录回写与报告换算必须经此映射
                    const qint64 fidOut = dstFeat.GetFID() >= 0 ? dstFeat.GetFID() : dst.nextFid;
                    ++dst.nextFid;
                    const QString srcKey = QStringLiteral("%1|%2|%3|%4")
                        .arg(di).arg(li).arg(poFeat->GetFID()).arg(pi);
                    if (mode == BuildResult) {
                        ctx->srcMap.insert(srcKey, qMakePair(dst.name, fidOut));
                    } else if (mode == BuildWork) {
                        const auto it = ctx->srcMap.constFind(srcKey);
                        if (it != ctx->srcMap.constEnd())
                            ctx->workMap.insert(dst.name + QStringLiteral("|")
                                                + QString::number(fidOut), it.value());
                    }
                }
                OGRFeature::DestroyFeature(poFeat);
                if (!ok) break;
            }
            if (poCT) OCTDestroyCoordinateTransformation(poCT);
            // 进度按图层细粒度更新：大数据集单图层遍历可达数十秒，
            // 按数据集更新会让进度条长时间不动，用户误以为卡死
            if (onProgress)
                onProgress((di + (li + 1.0) / poSrcDS->GetLayerCount())
                           / ui.listWidget_datasets->count());
            if (!ok) break;
        }
        poDstDS->ExecuteSQL("COMMIT", nullptr, "SQLITE");
        GDALClose(poSrcDS);
        ui.label_reportSummary->setText((mode == BuildResult
            ? QStringLiteral("正在生成结果库（%1/%2）...")
            : QStringLiteral("正在生成工作库（%1/%2）..."))
            .arg(di + 1).arg(ui.listWidget_datasets->count()));
        QApplication::processEvents();
        if (!ok) break;
    }

    // 末尾建接边记录表（SDK 要求末层为非空间记录表）
    if (ok) {
        OGRLayer* recLayer = poDstDS->CreateLayer("edge_records", nullptr, wkbNone, nullptr);
        if (!recLayer) {
            if (err) *err = QStringLiteral("创建接边记录表失败");
            ok = false;
        } else {
            OGRFieldDefn f1("CUR_FID", OFTInteger64);    recLayer->CreateField(&f1);
            OGRFieldDefn f2("CUR_SHEET", OFTString);     recLayer->CreateField(&f2);
            OGRFieldDefn f3("ADJ_FID", OFTInteger64);    recLayer->CreateField(&f3);
            OGRFieldDefn f4("ADJ_SHEET", OFTString);     recLayer->CreateField(&f4);
            OGRFieldDefn f5("LAYER_TYPE", OFTString);    recLayer->CreateField(&f5);
            OGRFieldDefn f6("GEO_TYPE", OFTString);      recLayer->CreateField(&f6);
            OGRFieldDefn f7("AUTO_MERGE", OFTInteger64); recLayer->CreateField(&f7);
            OGRFieldDefn f8("MERGE_TYPE", OFTInteger64); recLayer->CreateField(&f8);
            OGRFieldDefn f9("CHECKED", OFTInteger64);    recLayer->CreateField(&f9);
            OGRFieldDefn f10("ACCEPTED", OFTInteger64);  recLayer->CreateField(&f10);
            OGRFieldDefn f11("BACKUP1", OFTString);      recLayer->CreateField(&f11);
            OGRFieldDefn f12("BACKUP2", OFTString);      recLayer->CreateField(&f12);
        }
    }

    if (!ok) {
        GDALClose(poDstDS);
        QFile::remove(gpkgPath);
        QFile::remove(gpkgPath + QStringLiteral("-wal"));
        QFile::remove(gpkgPath + QStringLiteral("-shm"));
        return false;
    }

    // BuildResult 句柄保持打开（调用方持有，最终统一关闭），
    // 避免 SDK 运行后重新打开结果库失败
    if (mode == BuildResult && outDS) {
        *outDS = poDstDS;
    } else {
        GDALClose(poDstDS);
        QFileInfo fi(gpkgPath);
        if (!fi.exists() || fi.size() == 0) {
            if (err) *err = QStringLiteral("数据库已关闭但文件未落盘: %1").arg(gpkgPath);
            QFile::remove(gpkgPath);
            QFile::remove(gpkgPath + QStringLiteral("-wal"));
            QFile::remove(gpkgPath + QStringLiteral("-shm"));
            return false;
        }
    }
    if (outResultLayers) {
        for (auto it = dstByKey.constBegin(); it != dstByKey.constEnd(); ++it)
            outResultLayers->append(it.value().name);
    }
    return true;
}

// SDK 在工作库追加接边记录后，复制到结果库（CUR_SHEET/ADJ_SHEET 映射回真实图幅号）
// 同时把 SDK 在工作库内调整好的几何写回结果库（工作库 FID 与结果库一一对应）

namespace {

// 把 SDK 调整后的单个要素几何从工作库（WGS84）转回原坐标系写入结果库。
// fid=工作库 FID，dstLayerName/dstFid=结果库主题层名与 FID（经 FID 映射换算）
bool applyMergedGeometry(GDALDataset* poSrc, GDALDataset* poDst,
                         const QHash<QString, double>& shiftByCode,
                         const QString& shiftedSheet, qint64 fid,
                         const QString& dstLayerName, qint64 dstFid,
                         const QString& gtype, QString* err)
{
    OGRLayer* srcLyr = poSrc->GetLayerByName(
        (shiftedSheet + QStringLiteral("_L_") + gtype).toUtf8().constData());
    if (!srcLyr) {
        if (err) *err = QStringLiteral("工作库缺少图层: %1_L_%2").arg(shiftedSheet, gtype);
        return false;
    }
    OGRFeature* srcFeat = srcLyr->GetFeature(fid);
    if (!srcFeat) {
        if (err) *err = QStringLiteral("工作库缺少要素: %1_L_%2 FID=%3")
                           .arg(shiftedSheet, gtype).arg(fid);
        return false;
    }
    OGRGeometry* poGeom = srcFeat->GetGeometryRef();
    if (!poGeom) {
        OGRFeature::DestroyFeature(srcFeat);
        if (err) *err = QStringLiteral("工作库要素几何为空: %1_L_%2 FID=%3")
                           .arg(shiftedSheet, gtype).arg(fid);
        return false;
    }
    OGRLayer* dstLyr = poDst->GetLayerByName(dstLayerName.toUtf8().constData());
    if (!dstLyr) {
        OGRFeature::DestroyFeature(srcFeat);
        if (err) *err = QStringLiteral("结果库缺少图层: %1").arg(dstLayerName);
        return false;
    }

    OGRSpatialReference wgs84 = makeWgs84Srs();
    OGRSpatialReference* poDstSrs = dstLyr->GetSpatialRef();
    OGRGeometry* poOut = poGeom->clone();
    // 工作库几何按纬度平移法整体南移了 shiftLat 度，回写前必须恢复，
    // 否则已接边要素会整体偏移（1°≈111km）落到错误位置
    const double shiftLat = shiftByCode.value(shiftedSheet, 0.0);
    if (shiftLat != 0.0)
        shiftLatRec(poOut, shiftLat);
    if (poDstSrs && !poDstSrs->IsSame(&wgs84)) {
        OGRCoordinateTransformation* ct =
            OGRCreateCoordinateTransformation(&wgs84, poDstSrs);
        if (!ct || poOut->transform(ct) != OGRERR_NONE) {
            if (ct) OCTDestroyCoordinateTransformation(ct);
            OGRGeometryFactory::destroyGeometry(poOut);
            OGRFeature::DestroyFeature(srcFeat);
            if (err) *err = QStringLiteral("接边几何转回原坐标系失败: %1 FID=%2")
                               .arg(dstLayerName).arg(fid);
            return false;
        }
        OCTDestroyCoordinateTransformation(ct);
    }

    OGRFeature* dstFeat = dstLyr->GetFeature(dstFid);
    if (!dstFeat) {
        OGRGeometryFactory::destroyGeometry(poOut);
        OGRFeature::DestroyFeature(srcFeat);
        if (err) *err = QStringLiteral("结果库缺少要素: %1 FID=%2")
                           .arg(dstLayerName).arg(dstFid);
        return false;
    }
    bool okSet = dstFeat->SetGeometry(poOut) == OGRERR_NONE;
    if (okSet)
        okSet = dstLyr->SetFeature(dstFeat) == OGRERR_NONE;
    OGRFeature::DestroyFeature(dstFeat);
    OGRGeometryFactory::destroyGeometry(poOut);
    OGRFeature::DestroyFeature(srcFeat);
    if (!okSet) {
        if (err) *err = QStringLiteral("接边几何写回结果库失败: %1 FID=%2")
                           .arg(dstLayerName).arg(dstFid);
        return false;
    }
    return true;
}

// 插件拉齐对查找键（工作图幅号|工作FID|几何词|邻图幅号|邻FID）
QString gapPairKey(const QString& c1, qint64 f1, const QString& g,
                   const QString& c2, qint64 f2)
{
    return QStringLiteral("%1|%2|%3|%4|%5").arg(c1).arg(f1).arg(g).arg(c2).arg(f2);
}

// 要素距缝最近顶点（线：两端点；面：外环全部顶点）。返回索引、垂直距离（度）
// 与沿线坐标（E 缝为 y，N 缝为 x；缝坐标为工作库 WGS84）
bool nearestSeamVertex(const OGRGeometry* g, char dir, double seamVal,
                       int* outIdx, double* outDist, double* outAlong)
{
    *outIdx = -1;
    *outDist = 1e30;
    *outAlong = 0;
    if (!g) return false;
    const OGRwkbGeometryType flat = wkbFlatten(g->getGeometryType());
    const OGRLineString* ls = nullptr;
    if (flat == wkbLineString)
        ls = static_cast<const OGRLineString*>(g);
    else if (flat == wkbPolygon)
        ls = static_cast<const OGRPolygon*>(g)->getExteriorRing();
    if (!ls || ls->getNumPoints() < 2) return false;
    QVector<int> idxs;
    if (flat == wkbLineString)
        idxs << 0 << (ls->getNumPoints() - 1);
    else
        for (int i = 0; i < ls->getNumPoints(); ++i) idxs << i;
    for (int i : idxs) {
        const double x = ls->getX(i);
        const double y = ls->getY(i);
        const double perp = (dir == 'E') ? std::fabs(x - seamVal) : std::fabs(y - seamVal);
        if (perp < *outDist) {
            *outDist = perp;
            *outIdx = i;
            *outAlong = (dir == 'E') ? y : x;
        }
    }
    return *outIdx >= 0;
}

// 把克隆几何的第 idx 个顶点移到缝上（沿线坐标取两侧平均）；闭合环首尾
// 同一点需同步修改，否则环不闭合
void moveVertexToSeam(OGRGeometry* g, int idx, char dir, double seamVal, double alongVal)
{
    if (!g) return;
    const OGRwkbGeometryType flat = wkbFlatten(g->getGeometryType());
    OGRLineString* ls = nullptr;
    bool isRing = false;
    if (flat == wkbLineString)
        ls = static_cast<OGRLineString*>(g);
    else if (flat == wkbPolygon) {
        ls = static_cast<OGRPolygon*>(g)->getExteriorRing();
        isRing = true;
    }
    if (!ls || idx < 0 || idx >= ls->getNumPoints()) return;
    auto setPt = [&](int i) {
        if (dir == 'E') ls->setPoint(i, seamVal, alongVal);
        else ls->setPoint(i, alongVal, seamVal);
    };
    setPt(idx);
    if (isRing) {
        if (idx == 0) setPt(ls->getNumPoints() - 1);
        else if (idx == ls->getNumPoints() - 1) setPt(0);
    }
}

} // namespace

// 两条工作图幅号（可能平移后）的共享边是否为接合线（按真实坐标键判定）
bool AutoEdgeMatchDialog::sharedEdgeActive(const QHash<QString, TileInfo>& byCode,
                                           const QString& curCode, const QString& adjCode,
                                           const QSet<QString>& activeKeys)
{
    const auto itA = byCode.constFind(curCode);
    const auto itB = byCode.constFind(adjCode);
    if (itA == byCode.constEnd() || itB == byCode.constEnd()) return false;
    const TileInfo& a = itA.value();
    const TileInfo& b = itB.value();
    if (std::fabs(b.lonMin - a.lonMax) < 1e-9 && std::fabs(b.latMin - a.latMin) < 1e-9)
        return activeKeys.contains(edgeKey('E', (int)std::lround(a.lonMax * 4.0)));
    if (std::fabs(a.lonMin - b.lonMax) < 1e-9 && std::fabs(a.latMin - b.latMin) < 1e-9)
        return activeKeys.contains(edgeKey('E', (int)std::lround(b.lonMax * 4.0)));
    if (std::fabs(b.latMin - a.latMax) < 1e-9 && std::fabs(b.lonMin - a.lonMin) < 1e-9)
        return activeKeys.contains(edgeKey('N', (int)std::lround(a.latMax * 6.0)));
    if (std::fabs(a.latMin - b.latMax) < 1e-9 && std::fabs(a.lonMin - b.lonMin) < 1e-9)
        return activeKeys.contains(edgeKey('N', (int)std::lround(a.latMin * 6.0)));
    return false;
}

bool AutoEdgeMatchDialog::copyEdgeRecords(const QString& srcPath, const QString& dstPath,
                                          const QVector<TileInfo>& tiles, QString* err,
                                          GDALDataset* poDstIn, EdgeFilterCtx* ctx,
                                          const QVector<GapFixInfo>& fixed)
{
    QHash<QString, QString> codeMap;
    QHash<QString, double> shiftByCode;
    QHash<QString, TileInfo> byCode;
    for (const TileInfo& t : tiles) {
        codeMap.insert(t.code, t.realCode);
        shiftByCode.insert(t.code, t.shiftLat);
        byCode.insert(t.code, t);
    }
    // 插件拉齐对查找表（双向键）：复制时把对应 SDK 记录改写为
    // AUTO_MERGE=1/MERGE_TYPE=4（几何已由 fixGapPairs 直接写回结果库）
    QHash<QString, double> fixDist;
    for (const GapFixInfo& g : fixed) {
        fixDist.insert(gapPairKey(g.curCode, g.curFid, g.gtype, g.adjCode, g.adjFid), g.origDistM);
        fixDist.insert(gapPairKey(g.adjCode, g.adjFid, g.gtype, g.curCode, g.curFid), g.origDistM);
    }

    GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
        srcPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poSrc) {
        if (err) *err = QStringLiteral("无法打开工作库: %1").arg(srcPath);
        return false;
    }
    // 优先用 buildGpkg 保持打开的句柄；仅在无句柄时重新打开
    GDALDataset* poDst = poDstIn;
    if (!poDst) {
        poDst = (GDALDataset*)GDALOpenEx(
            dstPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr);
        if (!poDst) {
            // CPL 错误必须先于 GDALClose 读取，关闭操作可能覆盖错误队列
            const QByteArray cplMsg = CPLGetLastErrorMsg() ? CPLGetLastErrorMsg() : "";
            GDALClose(poSrc);
            if (err) {
                QFileInfo fi(dstPath);
                const QString existsStr = fi.exists() ? QStringLiteral("是（%1 字节）").arg(fi.size())
                                                      : QStringLiteral("否");
                *err = QStringLiteral("无法打开结果库: %1\n文件存在: %2\nGDAL: %3")
                           .arg(dstPath, existsStr, QString::fromUtf8(cplMsg));
            }
            return false;
        }
    }
    OGRLayer* poSrcLyr = poSrc->GetLayerByName("edge_records");
    OGRLayer* poDstLyr = poDst->GetLayerByName("edge_records");
    if (!poSrcLyr || !poDstLyr) {
        GDALClose(poSrc);
        if (!poDstIn) GDALClose(poDst);
        if (err) *err = QStringLiteral("接边记录表缺失");
        return false;
    }

    poDst->ExecuteSQL("BEGIN", nullptr, "SQLITE");
    poSrcLyr->ResetReading();
    OGRFeature* poFeat = nullptr;
    while ((poFeat = poSrcLyr->GetNextFeature()) != nullptr) {
        const QString gtype = QString::fromUtf8(poFeat->GetFieldAsString("GEO_TYPE"));
        const qint64 curFid = poFeat->GetFieldAsInteger64("CUR_FID");
        const qint64 adjFid = poFeat->GetFieldAsInteger64("ADJ_FID");
        const QString curSheet = QString::fromUtf8(poFeat->GetFieldAsString("CUR_SHEET"));
        const QString adjSheet = QString::fromUtf8(poFeat->GetFieldAsString("ADJ_SHEET"));
        const double pluginDist = fixDist.value(
            gapPairKey(curSheet, curFid, gtype, adjSheet, adjFid), -1.0);

        QString curDstLayer, adjDstLayer;
        qint64 curDstFid = curFid, adjDstFid = adjFid;
        if (ctx) {
            // 接合线模式：共享边非接合线的记录整体剔除（不复制、不回写几何）
            if (!ctx->activeKeys.isEmpty() &&
                !sharedEdgeActive(byCode, curSheet, adjSheet, ctx->activeKeys)) {
                ++ctx->skippedRecs;
                OGRFeature::DestroyFeature(poFeat);
                continue;
            }
            // 结果库按主题合并后 FID 不一一对应，经映射换算为结果图层名+FID
            const auto curMap = mapWorkToResult(ctx, curSheet, gtype, curFid);
            const auto adjMap = mapWorkToResult(ctx, adjSheet, gtype, adjFid);
            if (curMap.first.isEmpty() || adjMap.first.isEmpty()) {
                ++ctx->skippedRecs;
                OGRFeature::DestroyFeature(poFeat);
                continue;
            }
            curDstLayer = curMap.first;
            curDstFid = curMap.second;
            adjDstLayer = adjMap.first;
            adjDstFid = adjMap.second;
        }

        OGRFeature dstFeat(poDstLyr->GetLayerDefn());
        OGRFeatureDefn* poDstDefn = poDstLyr->GetLayerDefn();
        for (int i = 0; i < poDstDefn->GetFieldCount(); ++i) {
            const char* nm = poDstDefn->GetFieldDefn(i)->GetNameRef();
            if (strcmp(nm, "CUR_SHEET") == 0 || strcmp(nm, "ADJ_SHEET") == 0) {
                QString v = QString::fromUtf8(poFeat->GetFieldAsString(i));
                if (codeMap.contains(v)) v = codeMap.value(v);
                dstFeat.SetField(i, v.toUtf8().constData());
            } else if (strcmp(nm, "CUR_FID") == 0) {
                dstFeat.SetField(i, curDstFid);
            } else if (strcmp(nm, "ADJ_FID") == 0) {
                dstFeat.SetField(i, adjDstFid);
            } else if (pluginDist >= 0 && strcmp(nm, "AUTO_MERGE") == 0) {
                dstFeat.SetField(i, (GIntBig)1);
            } else if (pluginDist >= 0 && strcmp(nm, "MERGE_TYPE") == 0) {
                dstFeat.SetField(i, (GIntBig)4);
            } else if (pluginDist >= 0 && strcmp(nm, "BACKUP1") == 0) {
                dstFeat.SetField(i, QStringLiteral("插件拉齐:%1米")
                    .arg(QString::number(pluginDist, 'f', 1)).toUtf8().constData());
            } else {
                switch (poDstDefn->GetFieldDefn(i)->GetType()) {
                    case OFTInteger:
                        dstFeat.SetField(i, poFeat->GetFieldAsInteger(i));
                        break;
                    case OFTInteger64:
                        dstFeat.SetField(i, poFeat->GetFieldAsInteger64(i));
                        break;
                    case OFTReal:
                        dstFeat.SetField(i, poFeat->GetFieldAsDouble(i));
                        break;
                    case OFTString:
                        dstFeat.SetField(i, poFeat->GetFieldAsString(i));
                        break;
                    default:
                        break;
                }
            }
        }
        if (poDstLyr->CreateFeature(&dstFeat) != OGRERR_NONE) {
            if (err) *err = QStringLiteral("写入接边记录失败");
            GDALClose(poSrc);
            if (!poDstIn) GDALClose(poDst);
            return false;
        }
        // 已接边的记录：把 SDK 在工作库内调整好的几何写回结果库（本幅与邻幅两侧都写，
        // SDK 可能只动一侧（强制法）或两侧（平均法））
        if (poFeat->GetFieldAsInteger64("AUTO_MERGE") == 1) {
            if (!applyMergedGeometry(poSrc, poDst, shiftByCode, curSheet, curFid, curDstLayer, curDstFid, gtype, err)
                || !applyMergedGeometry(poSrc, poDst, shiftByCode, adjSheet, adjFid, adjDstLayer, adjDstFid, gtype, err)) {
                GDALClose(poSrc);
                if (!poDstIn) GDALClose(poDst);
                return false;
            }
        }
        OGRFeature::DestroyFeature(poFeat);
    }
    poDst->ExecuteSQL("COMMIT", nullptr, "SQLITE");
    GDALClose(poSrc);
    if (!poDstIn) GDALClose(poDst);
    return true;
}

// SDK 对带内但端点距离超内部阈值（实测约 50-60 米，与缓冲距离无关）的缝对
// 只记不合并（AUTO_MERGE=0）。插件拉齐：两侧贴缝顶点距缝 ≤ 缓冲带且沿线
// 错开 ≤150 米的真缝对，把两侧贴缝顶点投影到接合线上（沿线坐标取平均），
// 几何写回结果库；接边记录由 copyEdgeRecords 改写为 AUTO_MERGE=1/方式4
bool AutoEdgeMatchDialog::fixGapPairs(const QString& workPath,
                                      const QVector<TileInfo>& tiles,
                                      GDALDataset* poDst, EdgeFilterCtx* ctx,
                                      QVector<GapFixInfo>* out,
                                      QVector<GapFixInfo>* rejected, QString* err)
{
    if (!out) return false;
    out->clear();
    if (rejected) rejected->clear();
    if (!ctx || ctx->bandDeg <= 0 || !poDst) return true;

    QHash<QString, TileInfo> byCode;
    QHash<QString, double> shiftByCode;
    for (const TileInfo& t : tiles) {
        byCode.insert(t.code, t);
        shiftByCode.insert(t.code, t.shiftLat);
    }
    GDALDataset* poSrc = (GDALDataset*)GDALOpenEx(
        workPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr);
    if (!poSrc) {
        if (err) *err = QStringLiteral("插件拉齐：无法打开工作库");
        return false;
    }
    OGRLayer* recLyr = poSrc->GetLayerByName("edge_records");
    if (!recLyr) {
        GDALClose(poSrc);
        if (err) *err = QStringLiteral("插件拉齐：工作库缺少接边记录表");
        return false;
    }
    const double bandDeg = ctx->bandDeg;
    bool ok = true;

    // 第一遍：收集候选对（真缝、贴缝顶点在带内、沿线错开 ≤150 米、同主题）。
    // 跨主题对（SDK 按几何词混层，如河流配道路）不拉齐，记入 rejected 如实报未接边
    struct Cand {
        QString gtype, curCode, adjCode;
        qint64 curFid = 0, adjFid = 0;
        char seamDir = 0;
        double seamVal = 0;
        int ci = -1, ai = -1;
        double cAlong = 0, aAlong = 0;
        double origM = 0;
        QString curLayer, adjLayer;
        qint64 curDst = -1, adjDst = -1;
        double alongGapM = 0;
    };
    QVector<Cand> cands;
    recLyr->ResetReading();
    OGRFeature* poFeat = nullptr;
    while ((poFeat = recLyr->GetNextFeature()) != nullptr) {
        if (poFeat->GetFieldAsInteger64("AUTO_MERGE") != 0) {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        const QString gtype = QString::fromUtf8(poFeat->GetFieldAsString("GEO_TYPE"));
        const qint64 curFid = poFeat->GetFieldAsInteger64("CUR_FID");
        const qint64 adjFid = poFeat->GetFieldAsInteger64("ADJ_FID");
        const QString curCode = QString::fromUtf8(poFeat->GetFieldAsString("CUR_SHEET"));
        const QString adjCode = QString::fromUtf8(poFeat->GetFieldAsString("ADJ_SHEET"));
        if (gtype.isEmpty() || curCode.isEmpty() || adjCode.isEmpty()) {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        if (!ctx->activeKeys.isEmpty() &&
            !sharedEdgeActive(byCode, curCode, adjCode, ctx->activeKeys)) {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        const auto itA = byCode.constFind(curCode);
        const auto itB = byCode.constFind(adjCode);
        if (itA == byCode.constEnd() || itB == byCode.constEnd()) {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        const TileInfo& a = itA.value();
        const TileInfo& b = itB.value();
        // 共享边方向与位置（工作坐标：经度不受纬度平移影响，纬度需减去平移量）
        char seamDir = 0;
        double seamVal = 0;
        if (std::fabs(b.lonMin - a.lonMax) < 1e-9)      { seamDir = 'E'; seamVal = a.lonMax; }
        else if (std::fabs(a.lonMin - b.lonMax) < 1e-9) { seamDir = 'E'; seamVal = b.lonMax; }
        else if (std::fabs(b.latMin - a.latMax) < 1e-9) { seamDir = 'N'; seamVal = a.latMax - a.shiftLat; }
        else if (std::fabs(a.latMin - b.latMax) < 1e-9) { seamDir = 'N'; seamVal = b.latMax - b.shiftLat; }
        else {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        const auto curMap = mapWorkToResult(ctx, curCode, gtype, curFid);
        const auto adjMap = mapWorkToResult(ctx, adjCode, gtype, adjFid);
        if (curMap.first.isEmpty() || adjMap.first.isEmpty()) {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        OGRLayer* curLyr = poSrc->GetLayerByName(
            (curCode + QStringLiteral("_L_") + gtype).toUtf8().constData());
        OGRLayer* adjLyr = poSrc->GetLayerByName(
            (adjCode + QStringLiteral("_L_") + gtype).toUtf8().constData());
        if (!curLyr || !adjLyr) {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        OGRFeature* cf = curLyr->GetFeature(curFid);
        OGRFeature* af = adjLyr->GetFeature(adjFid);
        if (!cf || !af) {
            if (cf) OGRFeature::DestroyFeature(cf);
            if (af) OGRFeature::DestroyFeature(af);
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        OGRGeometry* cg = cf->GetGeometryRef();
        OGRGeometry* ag = af->GetGeometryRef();
        int ci = -1, ai = -1;
        double cDist = 1e30, aDist = 1e30, cAlong = 0, aAlong = 0;
        const bool cOk = nearestSeamVertex(cg, seamDir, seamVal, &ci, &cDist, &cAlong);
        const bool aOk = nearestSeamVertex(ag, seamDir, seamVal, &ai, &aDist, &aAlong);
        const double alongM = std::fabs(cAlong - aAlong) * kMetersPerDeg;
        const double origM = std::max(cDist, aDist) * kMetersPerDeg;
        if (!cOk || !aOk || std::max(cDist, aDist) > bandDeg || alongM > kGapAlongM) {
            OGRFeature::DestroyFeature(cf);
            OGRFeature::DestroyFeature(af);
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        // 跨主题配对不拉齐，如实报未接边（异主题）
        if (curMap.first != adjMap.first) {
            if (rejected) {
                GapFixInfo r;
                r.gtype = gtype;
                r.curCode = curCode; r.curFid = curFid;
                r.adjCode = adjCode; r.adjFid = adjFid;
                r.origDistM = origM;
                r.reason = QStringLiteral("跨主题未配对（两侧属不同主题图层，不拉齐）");
                rejected->append(r);
            }
            OGRFeature::DestroyFeature(cf);
            OGRFeature::DestroyFeature(af);
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }
        Cand c;
        c.gtype = gtype;
        c.curCode = curCode; c.curFid = curFid;
        c.adjCode = adjCode; c.adjFid = adjFid;
        c.seamDir = seamDir; c.seamVal = seamVal;
        c.ci = ci; c.ai = ai;
        c.cAlong = cAlong; c.aAlong = aAlong;
        c.origM = origM;
        c.curLayer = curMap.first; c.curDst = curMap.second;
        c.adjLayer = adjMap.first; c.adjDst = adjMap.second;
        c.alongGapM = alongM;
        cands.append(c);
        OGRFeature::DestroyFeature(cf);
        OGRFeature::DestroyFeature(af);
        OGRFeature::DestroyFeature(poFeat);
    }

    // 第二遍：按沿线距离贪心 1:1 配对——每个端点只与最近的邻居拉齐，
    // 落选对记入 rejected 报"未接边（一对多落选）"
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        if (x.alongGapM != y.alongGapM) return x.alongGapM < y.alongGapM;
        return x.origM < y.origM;
    });
    poSrc->ExecuteSQL("BEGIN", nullptr, "SQLITE");
    poDst->ExecuteSQL("BEGIN", nullptr, "SQLITE");
    QSet<QString> usedCur, usedAdj;
    for (const Cand& c : cands) {
        const QString keyCur = c.curCode + QStringLiteral("|") + QString::number(c.curFid);
        const QString keyAdj = c.adjCode + QStringLiteral("|") + QString::number(c.adjFid);
        if (usedCur.contains(keyCur) || usedAdj.contains(keyAdj)) {
            if (rejected) {
                GapFixInfo r;
                r.gtype = c.gtype;
                r.curCode = c.curCode; r.curFid = c.curFid;
                r.adjCode = c.adjCode; r.adjFid = c.adjFid;
                r.origDistM = c.origM;
                r.reason = QStringLiteral("一对多落选（端点已与更近的邻居配对）");
                rejected->append(r);
            }
            continue;
        }
        usedCur.insert(keyCur);
        usedAdj.insert(keyAdj);
        OGRLayer* curLyr = poSrc->GetLayerByName(
            (c.curCode + QStringLiteral("_L_") + c.gtype).toUtf8().constData());
        OGRLayer* adjLyr = poSrc->GetLayerByName(
            (c.adjCode + QStringLiteral("_L_") + c.gtype).toUtf8().constData());
        if (!curLyr || !adjLyr) {
            ok = false;
            if (err) *err = QStringLiteral("插件拉齐：工作库缺少图层");
            break;
        }
        OGRFeature* cf = curLyr->GetFeature(c.curFid);
        OGRFeature* af = adjLyr->GetFeature(c.adjFid);
        if (!cf || !af) {
            if (cf) OGRFeature::DestroyFeature(cf);
            if (af) OGRFeature::DestroyFeature(af);
            ok = false;
            if (err) *err = QStringLiteral("插件拉齐：工作库缺少要素");
            break;
        }
        OGRGeometry* cg = cf->GetGeometryRef();
        OGRGeometry* ag = af->GetGeometryRef();
        // 拉齐：垂直坐标=缝值，沿线坐标取两侧平均；先改工作库几何，
        // 再经 applyMergedGeometry 平移/投影写回结果库
        const double alongAvg = (c.cAlong + c.aAlong) / 2.0;
        OGRGeometry* cNew = cg->clone();
        OGRGeometry* aNew = ag->clone();
        if (!cNew || !aNew) {
            if (cNew) OGRGeometryFactory::destroyGeometry(cNew);
            if (aNew) OGRGeometryFactory::destroyGeometry(aNew);
            OGRFeature::DestroyFeature(cf);
            OGRFeature::DestroyFeature(af);
            ok = false;
            if (err) *err = QStringLiteral("插件拉齐：几何克隆失败");
            break;
        }
        moveVertexToSeam(cNew, c.ci, c.seamDir, c.seamVal, alongAvg);
        moveVertexToSeam(aNew, c.ai, c.seamDir, c.seamVal, alongAvg);
        cf->SetGeometry(cNew);
        af->SetGeometry(aNew);
        OGRGeometryFactory::destroyGeometry(cNew);
        OGRGeometryFactory::destroyGeometry(aNew);
        if (curLyr->SetFeature(cf) != OGRERR_NONE ||
            adjLyr->SetFeature(af) != OGRERR_NONE) {
            OGRFeature::DestroyFeature(cf);
            OGRFeature::DestroyFeature(af);
            ok = false;
            if (err) *err = QStringLiteral("插件拉齐：工作库几何写回失败");
            break;
        }
        OGRFeature::DestroyFeature(cf);
        OGRFeature::DestroyFeature(af);
        if (!applyMergedGeometry(poSrc, poDst, shiftByCode, c.curCode, c.curFid,
                                 c.curLayer, c.curDst, c.gtype, err) ||
            !applyMergedGeometry(poSrc, poDst, shiftByCode, c.adjCode, c.adjFid,
                                 c.adjLayer, c.adjDst, c.gtype, err)) {
            ok = false;
            break;
        }
        GapFixInfo info;
        info.gtype = c.gtype;
        info.curCode = c.curCode;
        info.curFid = c.curFid;
        info.adjCode = c.adjCode;
        info.adjFid = c.adjFid;
        info.origDistM = c.origM;
        out->append(info);
    }
    poSrc->ExecuteSQL("COMMIT", nullptr, "SQLITE");
    poDst->ExecuteSQL("COMMIT", nullptr, "SQLITE");
    GDALClose(poSrc);
    return ok;
}

// ===================================================================
// 未接要素检测（异名/孤立，SDK 静默跳过，插件自行检测）
// 在 SDK 运行后的工作库上扫描（WGS84、可能平移后），报告用真实图幅号
// ===================================================================

void AutoEdgeMatchDialog::detectUnmatched(const QString& gpkgPath, const QString& field,
                                          const QVector<TileInfo>& tiles, double distDeg,
                                          const std::vector<LayerMergeRecord>& records,
                                          int* outUnmatched, int* outDiffName,
                                          EdgeFilterCtx* ctx,
                                          const QVector<GapFixInfo>& fixed,
                                          const QVector<GapFixInfo>& rejected)
{
    int unmatched = 0, diffName = 0;
    static const QStringList kGeomWords = { QStringLiteral("line"),
                                            QStringLiteral("polygon"),
                                            QStringLiteral("point") };

    // 同一图幅可能对应多个数据集（要素已合并进同一层），按图幅号去重避免重复扫描
    QVector<TileInfo> uniq;
    QSet<QString> seen;
    for (const TileInfo& t : tiles) {
        if (seen.contains(t.code)) continue;
        seen.insert(t.code);
        uniq.append(t);
    }

    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
        gpkgPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poDS) return;

    const int count = uniq.size();
    for (int i = 0; i < count; ++i) {
        for (int j = 0; j < count; ++j) {
            if (i == j) continue;
            // 邻接判定用真实边界比对（覆盖跨4°纬度带边界的相邻图幅，
            // 其 latIdx 差值不连续，用下标算术会漏判）
            const bool isEw = std::fabs(uniq[j].lonMin - uniq[i].lonMax) < 1e-9
                           && std::fabs(uniq[j].latMin - uniq[i].latMin) < 1e-9;
            const bool isNs = std::fabs(uniq[j].latMax - uniq[i].latMin) < 1e-9
                           && std::fabs(uniq[j].lonMin - uniq[i].lonMin) < 1e-9;
            if (!isEw && !isNs) continue;

            const TileInfo& ti = uniq[i];
            const TileInfo& tj = uniq[j];
            const QString realA = ti.realCode;
            const QString realB = tj.realCode;

            // 接合线模式：只扫描共享边为接合线的图幅对
            if (ctx && !ctx->activeKeys.isEmpty()) {
                const QString key = isEw ? edgeKey('E', (int)std::lround(ti.lonMax * 4.0))
                                         : edgeKey('N', (int)std::lround(ti.latMin * 6.0));
                if (!ctx->activeKeys.contains(key)) continue;
            }

            // 公共边界线（工作库为平移后几何）
            OGRLineString boundLine;
            if (isEw) {
                const double x = ti.lonMax; // 经度不受平移影响
                boundLine.addPoint(x, ti.latMin - ti.shiftLat - 1.0);
                boundLine.addPoint(x, ti.latMax - ti.shiftLat + 1.0);
            } else {
                const double y = ti.latMin - ti.shiftLat; // 北图幅南边（平移后）
                const double x0 = std::min(ti.lonMin, tj.lonMin) - 1.0;
                const double x1 = std::max(ti.lonMax, tj.lonMax) + 1.0;
                boundLine.addPoint(x0, y);
                boundLine.addPoint(x1, y);
            }

            // 距离提示换算：边界方向决定主要偏移分量（东西边界≈经度差，南北≈纬度差）
            const double cosLat = std::cos((ti.latMin + ti.latMax) * 0.5
                                           * 3.14159265358979323846 / 180.0);
            const double mPerDeg = kMetersPerDeg * (isEw ? cosLat : 1.0);

            for (const QString& word : kGeomWords) {
                const QString nameA = ti.code + QStringLiteral("_L_") + word;
                const QString nameB = tj.code + QStringLiteral("_L_") + word;
                OGRLayer* poA = poDS->GetLayerByName(nameA.toUtf8().constData());
                OGRLayer* poB = poDS->GetLayerByName(nameB.toUtf8().constData());
                if (!poA || !poB) continue;

                // 已接 FID 集合（SDK 记录图幅号为工作库号；记录方向与扫描
                // 循环方向无关，两个方向都要比对，否则反向迭代会漏排除）。
                // 只吸收真合并（AUTO_MERGE=1）的记录：SDK 对带内但距离超
                // 内部阈值的对只记不合并（AUTO_MERGE=0），这些要素仍要
                // 按未接边如实报出，否则"既不算已接也不报未接"两边漏报
                QSet<qint64> aMatched, bMatched;
                for (const LayerMergeRecord& rec : records) {
                    if (rec.iAutoMergeType == 0 || rec.iAutoMergeType == 4) continue;
                    if (QString::fromStdString(rec.strMergeGeoType) != word) continue;
                    const QString tc = QString::fromStdString(rec.strTileCode);
                    const QString ac = QString::fromStdString(rec.strAdjacentTileCode);
                    if (tc == ti.code && ac == tj.code) {
                        aMatched.insert((qint64)rec.iFID);
                        bMatched.insert((qint64)rec.iAdjacentFID);
                    } else if (tc == tj.code && ac == ti.code) {
                        aMatched.insert((qint64)rec.iAdjacentFID);
                        bMatched.insert((qint64)rec.iFID);
                    }
                }
                // 插件拉齐的缝对同样吸收：两侧要素已拉到接合线上，不再报未接边
                for (const GapFixInfo& g : fixed) {
                    if (g.gtype != word) continue;
                    if (g.curCode == ti.code && g.adjCode == tj.code) {
                        aMatched.insert(g.curFid);
                        bMatched.insert(g.adjFid);
                    } else if (g.curCode == tj.code && g.adjCode == ti.code) {
                        aMatched.insert(g.adjFid);
                        bMatched.insert(g.curFid);
                    }
                }
                // 跨主题/一对多落选缝对同样吸收：报告按对整体输出未接边行，
                // 扫描不再单侧重复报
                for (const GapFixInfo& g : rejected) {
                    if (g.gtype != word) continue;
                    if (g.curCode == ti.code && g.adjCode == tj.code) {
                        aMatched.insert(g.curFid);
                        bMatched.insert(g.adjFid);
                    } else if (g.curCode == tj.code && g.adjCode == ti.code) {
                        aMatched.insert(g.adjFid);
                        bMatched.insert(g.curFid);
                    }
                }

                int fieldIdxA = poA->GetLayerDefn()->GetFieldIndex(field.toUtf8().constData());
                int fieldIdxB = poB->GetLayerDefn()->GetFieldIndex(field.toUtf8().constData());
                const bool byField = !field.isEmpty();

                struct Cand { qint64 fid; QString value; OGRGeometry* geom; };
                auto collectCands = [&](OGRLayer* poLayer, int fieldIdx, QVector<Cand>* out) {
                    poLayer->ResetReading();
                    OGRFeature* fe = nullptr;
                    while ((fe = poLayer->GetNextFeature()) != nullptr) {
                        OGRGeometry* g = fe->GetGeometryRef();
                        if (g && g->Distance(&boundLine) <= distDeg) {
                            Cand c;
                            c.fid = fe->GetFID();
                            c.value = (fieldIdx >= 0 && fe->IsFieldSetAndNotNull(fieldIdx))
                                          ? QString::fromUtf8(fe->GetFieldAsString(fieldIdx)) : QString();
                            c.geom = g->clone();
                            out->append(c);
                        }
                        OGRFeature::DestroyFeature(fe);
                    }
                };
                QVector<Cand> candA, candB;
                collectCands(poA, fieldIdxA, &candA);
                collectCands(poB, fieldIdxB, &candB);

                // 同名判定用全量候选（对方已接边也算同名存在，如多部件拆出的
                // 未接部分）；附近提示用未接候选（已接要素不应再被提示为异名）
                QVector<int> nearIdxA, nearIdxB;
                for (int k = 0; k < candA.size(); ++k)
                    if (!aMatched.contains(candA[k].fid)) nearIdxA.append(k);
                for (int k = 0; k < candB.size(); ++k)
                    if (!bMatched.contains(candB[k].fid)) nearIdxB.append(k);

                auto scan = [&](OGRLayer* poLayer, int fieldIdx, bool isA,
                                const QVector<Cand>& sameCands,
                                const QVector<int>& nearIdx) {
                    poLayer->ResetReading();
                    OGRFeature* f = nullptr;
                    while ((f = poLayer->GetNextFeature()) != nullptr) {
                        OGRGeometry* g = f->GetGeometryRef();
                        if (!g || g->Distance(&boundLine) > distDeg) {
                            OGRFeature::DestroyFeature(f);
                            continue;
                        }
                        const QSet<qint64>& matched = isA ? aMatched : bMatched;
                        if (matched.contains(f->GetFID())) {
                            OGRFeature::DestroyFeature(f);
                            continue;
                        }
                        QString value = (fieldIdx >= 0 && f->IsFieldSetAndNotNull(fieldIdx))
                                            ? QString::fromUtf8(f->GetFieldAsString(fieldIdx)) : QString();
                        bool hasSame = false;
                        if (byField) {
                            for (const Cand& c : sameCands) {
                                if (c.value == value && g->Distance(c.geom) <= distDeg * 2.0) {
                                    hasSame = true;
                                    break;
                                }
                            }
                        }
                        QString nearList;
                        int nearCount = 0;
                        if (!hasSame) {
                            for (int k : nearIdx) {
                                const Cand& c = sameCands[k];
                                double d = g->Distance(c.geom);
                                if (d > distDeg * 2.0) continue;
                                if (nearCount < 3) {
                                    const qint64 nearDisp = mapWorkFid(ctx, isA ? tj.code : ti.code,
                                                                       word, c.fid);
                                    nearList += (nearCount ? QStringLiteral("；") : QString())
                                                + QStringLiteral("%1(FID%2,约%3米)")
                                                      .arg(c.value)
                                                      .arg(nearDisp >= 0 ? QString::number(nearDisp)
                                                                         : QString::number(c.fid))
                                                      .arg(QString::number(d * mPerDeg, 'f', 1));
                                }
                                ++nearCount;
                            }
                        }
                        ++unmatched;
                        QString note;
                        if (byField && hasSame)
                            note = QStringLiteral("同名未接边（可能超出缓冲距离）");
                        else if (!nearList.isEmpty()) {
                            if (byField) ++diffName;
                            note = byField ? QStringLiteral("疑似异名：") + nearList
                                           : QStringLiteral("附近有要素但SDK未接边（距离或条件不满足）：") + nearList;
                        } else {
                            note = QStringLiteral("边界附近无对应要素（孤立）");
                        }
                        // 报告图层列显示真实结果图层名（按主题合并后的层），
                        // FID 显示结果库 FID
                        const auto dispMap = mapWorkToResult(ctx, isA ? ti.code : tj.code,
                                                            word, f->GetFID());
                        const qint64 dispFid = dispMap.second >= 0 ? dispMap.second
                                                                   : f->GetFID();
                        const QString dispLayer = dispMap.first.isEmpty() ? word : dispMap.first;
                        const QString fidStr = QString::number(dispFid);
                        appendReportRow(QStringList()
                            << QStringLiteral("未接边")
                            << dispLayer
                            << (isA ? realA : realB)
                            << (isA ? fidStr : QStringLiteral("-"))
                            << (isA ? realB : realA)
                            << (isA ? QStringLiteral("-") : fidStr)
                            << value << note);
                        OGRFeature::DestroyFeature(f);
                    }
                };
                scan(poA, fieldIdxA, true, candB, nearIdxB);
                scan(poB, fieldIdxB, false, candA, nearIdxA);

                for (Cand& c : candA) delete c.geom;
                for (Cand& c : candB) delete c.geom;
            }
        }
    }
    GDALClose(poDS);

    if (outUnmatched) *outUnmatched = unmatched;
    if (outDiffName) *outDiffName = diffName;
}

// ===================================================================
// 报告表格
// ===================================================================

void AutoEdgeMatchDialog::clearReport()
{
    ui.tableWidget_report->setRowCount(0);
}

void AutoEdgeMatchDialog::appendReportRow(const QStringList& cols)
{
    // 整行按接边情况着色：已接边绿 / 疑似异名橙 / 同名未接边黄 / 孤立灰
    QColor rowColor(255, 255, 255);
    const QString& status = cols.value(0);
    const QString& note = cols.value(7);
    if (status == QStringLiteral("已接边")) {
        rowColor = QColor(220, 245, 220);
    } else if (note.startsWith(QStringLiteral("疑似异名"))) {
        rowColor = QColor(255, 224, 178);
    } else if (note.contains(QStringLiteral("同名未接边"))) {
        rowColor = QColor(255, 243, 170);
    } else if (note.contains(QStringLiteral("孤立"))) {
        rowColor = QColor(232, 232, 232);
    }
    int row = ui.tableWidget_report->rowCount();
    ui.tableWidget_report->insertRow(row);
    for (int c = 0; c < cols.size() && c < ui.tableWidget_report->columnCount(); ++c) {
        QTableWidgetItem* item = new QTableWidgetItem(cols[c]);
        item->setBackground(rowColor);
        ui.tableWidget_report->setItem(row, c, item);
    }
}

// ===================================================================
// 执行
// ===================================================================

void AutoEdgeMatchDialog::onRun()
{
    // 防重入：执行期间按钮已禁用，双击/连点第二次进入时直接返回
    // （此前按钮在扫描阶段后才禁用，连点会并发跑两次、结尾弹两个询问框）
    if (!ui.pushButton_run->isEnabled()) return;
    const int count = ui.listWidget_datasets->count();
    if (count < 2) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("请至少添加两块数据"));
        return;
    }
    QString field = ui.comboBox_field->currentText().trimmed();
    if (field == kNoFieldOption) {
        field.clear(); // 不按属性：空字段列表 = 纯空间邻近匹配
    } else {
        int parenIdx = field.indexOf(QStringLiteral(" ("));
        if (parenIdx > 0) field = field.left(parenIdx);
        if (field.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("请选择匹配字段，或选择\"不按属性\"按空间邻近接边"));
            return;
        }
    }
    double distance = ui.doubleSpinBox_distance->value();
    if (distance <= 0) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("缓冲距离必须大于0"));
        return;
    }
    // 结果保存目录提前校验：路径错误不应让用户白等一次整县扫描
    QString outDir = ui.lineEdit_resultPath->text().trimmed();
    if (outDir.isEmpty())
        outDir = QFileInfo(ui.listWidget_datasets->item(0)->text()).absolutePath();
    else if (!QFileInfo::exists(outDir) && !QDir().mkpath(outDir)) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("结果保存路径不存在且无法创建: %1").arg(outDir));
        return;
    }

    // 日志路径以界面当前值为准（含手动编辑的情况），空则跟随结果目录
    m_logPath = ui.lineEdit_logPath->text().trimmed();
    if (m_logPath.isEmpty()) m_logPath = outDir;

    // 进入执行状态（校验全部通过后立即禁用按钮，防止扫描期间连点并发重入）
    ui.pushButton_run->setEnabled(false);
    ui.pushButton_close->setEnabled(false);
    ui.progressBar->setRange(0, 100);
    ui.progressBar->setValue(0);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto setProgress = [this](int v) {
        ui.progressBar->setValue(v);
        QApplication::processEvents();
    };

    // 1. 计算各数据集覆盖的全部图幅（跨图幅自动拆分：逐要素按 WGS84 外接矩形
    //    中点归入所在图幅，一个数据集可覆盖多个图幅）；同时累计接合线统计
    QVector<QVector<TileInfo>> tileGroups(count);
    QVector<EdgeStats> statsByDs(count);
    QStringList errs;
    for (int i = 0; i < count; ++i) {
        ui.label_reportSummary->setText(QStringLiteral("正在分析图幅覆盖（%1/%2）...")
                                            .arg(i + 1).arg(count));
        QString err;
        if (!computeTileInfos(ui.listWidget_datasets->item(i)->text(), &tileGroups[i], &err,
                              &statsByDs[i],
                              [&](double f) { setProgress((int)((i + f) * 15.0 / count)); }))
            errs << err;
    }
    if (!errs.isEmpty()) {
        QApplication::restoreOverrideCursor();
        ui.pushButton_run->setEnabled(true);
        ui.pushButton_close->setEnabled(true);
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("图幅号识别失败:\n%1").arg(errs.join(QStringLiteral("\n"))));
        return;
    }

    // 1:25万 及更大图幅输入（单数据集包络同时跨越多个 1:5万 行列）接边
    // 缓冲标准为 250 米：用户未改动默认值（50 米）时自动套用，显式设置
    // 过的值不干预
    bool largeTileInput = false;
    for (const QVector<TileInfo>& g : tileGroups) {
        double w0 = 1e30, w1 = -1e30, h0 = 1e30, h1 = -1e30;
        for (const TileInfo& t : g) {
            w0 = std::min(w0, t.lonMin); w1 = std::max(w1, t.lonMax);
            h0 = std::min(h0, t.latMin); h1 = std::max(h1, t.latMax);
        }
        if (w1 - w0 > 0.25 + 1e-9 && h1 - h0 > 1.0 / 6.0 + 1e-9) {
            largeTileInput = true;
            break;
        }
    }
    if (largeTileInput && !m_distanceUserSet)
        distance = 250.0;

    // 2. 汇总去重为唯一图幅列表
    QVector<TileInfo> tiles;
    QHash<QString, int> idxByReal;
    for (int di = 0; di < count; ++di) {
        for (const TileInfo& t : tileGroups[di]) {
            const int idx = idxByReal.value(t.realCode, -1);
            if (idx < 0) {
                idxByReal.insert(t.realCode, tiles.size());
                tiles.append(t);
            }
        }
    }

    // 2.5 接合线判定：
    //   ① 数据集邻接边界：格网线两侧均有数据集包络贴线（10m 容差）
    //   ② 检测裁切线：两侧断头要素各 ≥2 且跨界要素 ≤ 两侧断头总数
    //   两者皆无 → 全量模式（与旧版行为完全一致）
    QHash<QString, int> aggNeg, aggPos, aggCross;
    double envMinX = 0, envMaxX = 0, envMinY = 0, envMaxY = 0;
    bool haveEnv = false;
    for (const EdgeStats& s : statsByDs) {
        if (!s.haveEnv) continue;
        if (!haveEnv) {
            envMinX = s.envMinX; envMaxX = s.envMaxX;
            envMinY = s.envMinY; envMaxY = s.envMaxY;
            haveEnv = true;
        } else {
            envMinX = std::min(envMinX, s.envMinX);
            envMaxX = std::max(envMaxX, s.envMaxX);
            envMinY = std::min(envMinY, s.envMinY);
            envMaxY = std::max(envMaxY, s.envMaxY);
        }
        for (auto it = s.cutNeg.constBegin(); it != s.cutNeg.constEnd(); ++it)
            aggNeg[it.key()] += it.value();
        for (auto it = s.cutPos.constBegin(); it != s.cutPos.constEnd(); ++it)
            aggPos[it.key()] += it.value();
        for (auto it = s.cross.constBegin(); it != s.cross.constEnd(); ++it)
            aggCross[it.key()] += it.value();
    }
    QSet<QString> activeKeys;
    QStringList activeLineDescs;
    if (haveEnv) {
        QSet<QString> allKeys;
        for (auto it = aggNeg.constBegin(); it != aggNeg.constEnd(); ++it)
            allKeys.insert(it.key());
        for (auto it = aggPos.constBegin(); it != aggPos.constEnd(); ++it)
            allKeys.insert(it.key());
        for (auto it = aggCross.constBegin(); it != aggCross.constEnd(); ++it)
            allKeys.insert(it.key());
        for (const QString& k : allKeys) {
            const int n = aggNeg.value(k), p = aggPos.value(k), c = aggCross.value(k);
            if (n >= 2 && p >= 2 && c <= n + p) {
                activeKeys.insert(k);
                activeLineDescs << QStringLiteral("②") + lineDesc(k, n, p, c);
            }
        }
        for (int k = (int)std::ceil((envMinX - kCutDetectTolDeg) * 4.0 - 1e-9);
             k <= (int)std::floor((envMaxX + kCutDetectTolDeg) * 4.0 + 1e-9); ++k) {
            const double v = k / 4.0;
            bool west = false, east = false;
            for (const EdgeStats& s : statsByDs) {
                if (!s.haveEnv) continue;
                if (std::fabs(s.envMaxX - v) <= kCutDetectTolDeg) west = true;
                if (std::fabs(s.envMinX - v) <= kCutDetectTolDeg) east = true;
            }
            const QString key = edgeKey('E', k);
            if (west && east && !activeKeys.contains(key)) {
                activeKeys.insert(key);
                activeLineDescs << QStringLiteral("①") + lineDesc(key, aggNeg.value(key),
                                                                  aggPos.value(key), aggCross.value(key));
            }
        }
        for (int k = (int)std::ceil((envMinY - kCutDetectTolDeg) * 6.0 - 1e-9);
             k <= (int)std::floor((envMaxY + kCutDetectTolDeg) * 6.0 + 1e-9); ++k) {
            const double v = k / 6.0;
            bool south = false, north = false;
            for (const EdgeStats& s : statsByDs) {
                if (!s.haveEnv) continue;
                if (std::fabs(s.envMaxY - v) <= kCutDetectTolDeg) south = true;
                if (std::fabs(s.envMinY - v) <= kCutDetectTolDeg) north = true;
            }
            const QString key = edgeKey('N', k);
            if (south && north && !activeKeys.contains(key)) {
                activeKeys.insert(key);
                activeLineDescs << QStringLiteral("①") + lineDesc(key, aggNeg.value(key),
                                                                  aggPos.value(key), aggCross.value(key));
            }
        }
    }
    // ctx 始终有效：activeKeys 为空 = 全量模式（不过滤、映射照建，
    // 结果库按主题合并后 FID 换算在任何模式都需要）
    EdgeFilterCtx ctxStorage;
    ctxStorage.activeKeys = activeKeys;
    ctxStorage.bandDeg = distance * kDegPerMeter;
    EdgeFilterCtx* ctx = &ctxStorage;

    // 3. SDK 邻幅计算范围限制：blockIdx > 106 时整体纬度南移
    QString err;
    if (!chooseShift(&tiles, &err)) {
        QApplication::restoreOverrideCursor();
        ui.pushButton_run->setEnabled(true);
        ui.pushButton_close->setEnabled(true);
        QMessageBox::warning(this, QStringLiteral("提示"), err);
        return;
    }
    const bool shifted = !tiles.isEmpty() && tiles[0].shiftLat > 0;
    bool anyProj = false;
    for (const QVector<TileInfo>& g : tileGroups)
        for (const TileInfo& t : g)
            anyProj = anyProj || t.needProj;

    // 4. 孤立数据提示（四向配对由 SDK 自动完成，无相邻图幅的数据不产生记录）
    auto isAdjacent = [](const TileInfo& a, const TileInfo& b) -> bool {
        const bool ew = (std::fabs(b.lonMin - a.lonMax) < 1e-9 ||
                         std::fabs(a.lonMin - b.lonMax) < 1e-9) &&
                        std::fabs(b.latMin - a.latMin) < 1e-9;
        const bool ns = (std::fabs(b.latMin - a.latMax) < 1e-9 ||
                         std::fabs(a.latMin - b.latMax) < 1e-9) &&
                        std::fabs(b.lonMin - a.lonMin) < 1e-9;
        return ew || ns;
    };
    QStringList isolated;
    for (int di = 0; di < count; ++di) {
        bool hasAdj = false;
        for (const TileInfo& ta : tileGroups[di]) {
            for (int dj = 0; dj < count; ++dj) {
                if (dj == di) continue;
                for (const TileInfo& tb : tileGroups[dj]) {
                    if (isAdjacent(ta, tb)) { hasAdj = true; break; }
                }
                if (hasAdj) break;
            }
            if (hasAdj) break;
        }
        if (!hasAdj)
            isolated << ui.listWidget_datasets->item(di)->text();
    }

    saveState();

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
    const QString resultPath = outDir + QStringLiteral("/接边结果_") + ts + QStringLiteral(".gpkg");
    // SDK 内部以窄字符方式打开工作库路径，中文路径会打开失败（probe 实测 ret=5），
    // 工作库必须放在纯 ASCII 路径（系统临时目录）
    const QString workPath = QDir::tempPath() + QStringLiteral("/edge_work_") + ts + QStringLiteral(".gpkg");
    const QString csvPath = outDir + QStringLiteral("/接边检测报告_") + ts + QStringLiteral(".csv");
    // 结果库句柄保持打开：SDK 运行后重开结果库会失败（六测报错），全程用同一句柄写入
    GDALDataset* resultDS = nullptr;

    auto failCleanup = [&](const QString& msg) {
        if (resultDS) {
            GDALClose(resultDS);
            resultDS = nullptr;
        }
        QFile::remove(workPath);
        QFile::remove(workPath + QStringLiteral("-wal"));
        QFile::remove(workPath + QStringLiteral("-shm"));
        QFile::remove(resultPath);
        QFile::remove(resultPath + QStringLiteral("-wal"));
        QFile::remove(resultPath + QStringLiteral("-shm"));
        QApplication::restoreOverrideCursor();
        ui.progressBar->setValue(0);
        ui.pushButton_run->setEnabled(true);
        ui.pushButton_close->setEnabled(true);
        QMessageBox::warning(this, QStringLiteral("接边"), msg);
    };

    // 5. 结果库：按源图层主题合并分层 + 原坐标系整体几何（句柄保持打开，供第 9 步直接写入）
    QStringList resultLayers;
    if (!buildGpkg(resultPath, tiles, BuildResult, &err, &resultLayers, &resultDS,
                   [&](double f) { setProgress(15 + (int)(f * 30.0)); }, ctx)) {
        failCleanup(err);
        return;
    }

    // 6. 工作库：平移后图幅号 + WGS84 几何（接合线模式仅收断头要素）
    QStringList workLayers;
    if (!buildGpkg(workPath, tiles, BuildWork, &err, &workLayers, nullptr,
                   [&](double f) { setProgress(45 + (int)(f * 30.0)); }, ctx)) {
        failCleanup(err);
        return;
    }

    // 7. 调用 SDK（单字母 L；四向邻接由 SDK 自动配对）
    LayerMatchParam p;
    p.strLayerName = "L";
    p.bPointChecked = true;
    p.bLineStringChecked = true;
    p.bPolygonChecked = true;
    if (!field.isEmpty()) p.vFields.push_back(field.toUtf8().constData());
    std::vector<LayerMatchParam> params;
    params.push_back(p);

    std::vector<LayerMergeRecord> records;
    setProgress(78);
    int ret = SeEdgeMergeBridge::opAutoMerge(params, kScaleType, distance,
                                             workPath.toStdString(), records, &err);
    if (ret != 0) {
        QString reason;
        if (ret == -1) reason = err;
        else if (ret == 1) reason = QStringLiteral("比例尺不合法");
        else if (ret == 2) reason = QStringLiteral("接边距离不合法");
        else if (ret == 3) reason = QStringLiteral("图层匹配参数设置不合法");
        else if (ret == 4) reason = QStringLiteral("gpkg数据库路径不合法");
        else reason = QStringLiteral("其它错误（返回码 %1）").arg(ret);
        QStringList log;
        log << QStringLiteral("功能: 接边");
        log << QStringLiteral("执行时间: ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        log << QStringLiteral("SDK执行结果: 失败（") + reason + QStringLiteral("）");
        const QStringList sdkMsgs = SeEdgeMergeBridge::takeSdkMessages();
        if (!sdkMsgs.isEmpty()) {
            log << QStringLiteral("SDK消息:");
            log << sdkMsgs;
        }
        writeLog(log);
        failCleanup(QStringLiteral("接边失败：%1").arg(reason));
        return;
    }

    // 7.5 插件拉齐：SDK 只记不合并（AUTO_MERGE=0）的真缝对——两侧贴缝顶点
    // 距缝在缓冲带内且沿线错开 ≤150 米——直接拉到接合线上写回结果库
    // （SDK 内部合并阈值约 50-60 米，与缓冲距离无关，超阈的缝对由插件补齐）
    QVector<GapFixInfo> fixed;
    QVector<GapFixInfo> rejectedFixes;
    setProgress(80);
    if (!fixGapPairs(workPath, tiles, resultDS, ctx, &fixed, &rejectedFixes, &err)) {
        failCleanup(err);
        return;
    }

    // 8. 未接（异名/孤立）检测：在工作库上扫描（删除工作库之前）
    clearReport();
    int unmatched = 0, diffName = 0;
    setProgress(82);
    detectUnmatched(workPath, field, tiles, distance * kDegPerMeter,
                    records, &unmatched, &diffName, ctx, fixed, rejectedFixes);
    setProgress(90);

    // 9. 接边记录复制回结果库（图幅号映射回真实图幅号，直接写第 5 步保持的句柄）
    if (!copyEdgeRecords(workPath, resultPath, tiles, &err, resultDS, ctx, fixed)) {
        failCleanup(err);
        return;
    }

    // 10. 关闭结果库落盘，删除工作库
    GDALClose(resultDS);
    resultDS = nullptr;
    QFile::remove(workPath);
    QFile::remove(workPath + QStringLiteral("-wal"));
    QFile::remove(workPath + QStringLiteral("-shm"));

    QApplication::restoreOverrideCursor();
    setProgress(97);

    // 11. 报告：已接边明细（显示真实图幅号与结果库图层名/FID；接合线模式剔除非接合线记录）
    QHash<QString, QString> shiftToReal;
    QHash<QString, TileInfo> byCode;
    for (const TileInfo& t : tiles) {
        shiftToReal.insert(t.code, t.realCode);
        byCode.insert(t.code, t);
    }
    int autoAvg = 0, autoForce = 0, autoOpt = 0;
    int pluginFixed = 0;
    int keptCount = 0;
    int rejectedShown = 0;
    QHash<QString, double> fixLookup;
    for (const GapFixInfo& g : fixed)
        fixLookup.insert(gapPairKey(g.curCode, g.curFid, g.gtype, g.adjCode, g.adjFid),
                         g.origDistM);
    QHash<QString, QString> rejectLookup;
    for (const GapFixInfo& g : rejectedFixes)
        rejectLookup.insert(gapPairKey(g.curCode, g.curFid, g.gtype, g.adjCode, g.adjFid),
                            g.reason);
    for (const LayerMergeRecord& rec : records) {
        const QString workCur = QString::fromStdString(rec.strTileCode);
        const QString workAdj = QString::fromStdString(rec.strAdjacentTileCode);
        if (!ctx->activeKeys.isEmpty() &&
            !sharedEdgeActive(byCode, workCur, workAdj, ctx->activeKeys))
            continue;
        const QString word = QString::fromStdString(rec.strMergeGeoType);
        double fixDist = fixLookup.value(
            gapPairKey(workCur, (qint64)rec.iFID, word, workAdj, (qint64)rec.iAdjacentFID), -1.0);
        if (fixDist < 0)
            fixDist = fixLookup.value(
                gapPairKey(workAdj, (qint64)rec.iAdjacentFID, word, workCur, (qint64)rec.iFID), -1.0);
        // SDK 对带内但端点距离超内部阈值（实测约 50-60 米）的对只记不合并
        // （表 AUTO_MERGE=0，records 向量中 iAutoMergeType=4）：不算"已接边"；
        // 其中插件拉齐过的缝对（fixDist>=0）显示"已接边（插件拉齐）"，
        // 其余仍由 detectUnmatched 如实报出
        if (fixDist < 0 && (rec.iAutoMergeType == 0 || rec.iAutoMergeType == 4)) {
            QString why = rejectLookup.value(
                gapPairKey(workCur, (qint64)rec.iFID, word, workAdj, (qint64)rec.iAdjacentFID));
            if (why.isEmpty())
                why = rejectLookup.value(
                    gapPairKey(workAdj, (qint64)rec.iAdjacentFID, word, workCur, (qint64)rec.iFID));
            if (!why.isEmpty()) {
                ++rejectedShown;
                const auto rCurMap = mapWorkToResult(ctx, workCur, word, (qint64)rec.iFID);
                const auto rAdjMap = mapWorkToResult(ctx, workAdj, word, (qint64)rec.iAdjacentFID);
                QString rLayer = !rCurMap.first.isEmpty() ? rCurMap.first
                                 : !rAdjMap.first.isEmpty() ? rAdjMap.first : word;
                if (!rCurMap.first.isEmpty() && !rAdjMap.first.isEmpty()
                    && rCurMap.first != rAdjMap.first)
                    rLayer = rCurMap.first + QStringLiteral("/") + rAdjMap.first;
                const qint64 rCurDisp = rCurMap.second >= 0 ? rCurMap.second : (qint64)rec.iFID;
                const qint64 rAdjDisp = rAdjMap.second >= 0 ? rAdjMap.second : (qint64)rec.iAdjacentFID;
                appendReportRow(QStringList()
                    << QStringLiteral("未接边") << rLayer
                    << shiftToReal.value(workCur, workCur) << QString::number(rCurDisp)
                    << shiftToReal.value(workAdj, workAdj) << QString::number(rAdjDisp)
                    << QStringLiteral("-") << why);
            }
            continue;
        }
        ++keptCount;
        QString mode;
        if (fixDist >= 0) {
            ++pluginFixed;
            mode = QStringLiteral("插件拉齐");
        } else {
            if (rec.iAutoMergeType == 2) ++autoAvg;
            else if (rec.iAutoMergeType == 1) ++autoForce;
            else if (rec.iAutoMergeType == 3) ++autoOpt;
            mode = rec.iAutoMergeType == 2 ? QStringLiteral("平均法")
                 : rec.iAutoMergeType == 1 ? QStringLiteral("强制法")
                 : rec.iAutoMergeType == 3 ? QStringLiteral("优化法")
                 : QStringLiteral("方式%1").arg(rec.iAutoMergeType);
        }
        // 图层列显示结果库真实主题层名（如 lrdl），映射缺失时退回几何词
        QString curCode = shiftToReal.value(workCur, workCur);
        QString adjCode = shiftToReal.value(workAdj, workAdj);
        const auto curMap = mapWorkToResult(ctx, workCur, word, (qint64)rec.iFID);
        const auto adjMap = mapWorkToResult(ctx, workAdj, word, (qint64)rec.iAdjacentFID);
        QString dispLayer = !curMap.first.isEmpty() ? curMap.first
                            : !adjMap.first.isEmpty() ? adjMap.first : word;
        if (!curMap.first.isEmpty() && !adjMap.first.isEmpty()
            && curMap.first != adjMap.first)
            dispLayer = curMap.first + QStringLiteral("/") + adjMap.first;
        const qint64 curDisp = curMap.second >= 0 ? curMap.second : (qint64)rec.iFID;
        const qint64 adjDisp = adjMap.second >= 0 ? adjMap.second : (qint64)rec.iAdjacentFID;
        appendReportRow(QStringList()
            << QStringLiteral("已接边") << dispLayer
            << curCode << QString::number(curDisp)
            << adjCode << QString::number(adjDisp)
            << QStringLiteral("-")
            << (fixDist >= 0
                    ? QStringLiteral("插件拉齐（原距 %1 米）").arg(QString::number(fixDist, 'f', 1))
                    : QStringLiteral("已按%1接边").arg(mode)));
    }

    // 12. 报告 CSV
    {
        QFile f(csvPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts2(&f);
            ts2.setCodec("UTF-8");
            ts2.setGenerateByteOrderMark(true);
            ts2 << QStringLiteral("序号,接边情况,图层,图幅(本),FID(本),图幅(邻),FID(邻),字段值,说明\n");
            for (int r = 0; r < ui.tableWidget_report->rowCount(); ++r) {
                ts2 << (r + 1);
                for (int c = 0; c < ui.tableWidget_report->columnCount(); ++c) {
                    QString v = ui.tableWidget_report->item(r, c)
                                    ? ui.tableWidget_report->item(r, c)->text() : QString();
                    v.replace(QStringLiteral(","), QStringLiteral("，"));
                    ts2 << QStringLiteral(",") << v;
                }
                ts2 << QStringLiteral("\n");
            }
            f.close();
        }
    }

    ui.progressBar->setValue(100);
    QString summary = QStringLiteral("接边完成：接上 %1 对，边界未接要素 %2 处（其中疑似异名 %3 处）。\n结果库: %4\n报告: %5")
                          .arg(keptCount).arg(unmatched + rejectedShown).arg(diffName)
                          .arg(resultPath, csvPath);
    if (!activeKeys.isEmpty())
        summary += QStringLiteral("\n接合线 %1 条（工作库仅保留接合线断头要素，剔除 %2 个）")
                       .arg(activeKeys.size()).arg(ctx->filteredOut);
    ui.label_reportSummary->setText(summary);

    QStringList log;
    log << QStringLiteral("功能: 接边");
    log << QStringLiteral("执行时间: ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    log << QStringLiteral("参与图幅（共%1个）:").arg(tiles.size());
    for (int di = 0; di < count; ++di) {
        QStringList codes;
        for (const TileInfo& t : tileGroups[di])
            codes << t.realCode;
        log << QStringLiteral("  %1（%2个图幅）: %3")
                   .arg(QFileInfo(ui.listWidget_datasets->item(di)->text()).fileName())
                   .arg(codes.size())
                   .arg(codes.join(QStringLiteral("、")));
    }
    log << QStringLiteral("匹配口径: ") + (field.isEmpty()
        ? QStringLiteral("不按属性（纯空间邻近）") : QStringLiteral("按字段 %1").arg(field));
    log << QStringLiteral("缓冲距离: ") + QString::number(distance) + QStringLiteral(" 米");
    log << QStringLiteral("SDK 接边: %1 对（强制法 %2，平均法 %3，优化法 %4）")
               .arg(keptCount - pluginFixed).arg(autoForce).arg(autoAvg).arg(autoOpt);
    if (!fixed.isEmpty())
        log << QStringLiteral("插件拉齐: SDK 因端点距离超内部合并阈值未合并的缝对 %1 对，"
                              "已由插件拉齐到接合线（接边记录方式4）").arg(fixed.size());
    if (!rejectedFixes.isEmpty())
        log << QStringLiteral("插件拉齐过滤: %1 对未拉齐（跨主题或一对多落选），已如实报未接边")
               .arg(rejectedFixes.size());
    if (largeTileInput && !m_distanceUserSet)
        log << QStringLiteral("处理说明: 检测到 1:25万 及以上大图幅输入，缓冲距离自动采用 250 米");
    if (!activeKeys.isEmpty()) {
        log << QStringLiteral("接合线模式: 检测到 %1 条接合线（①数据集邻接边界 ②检测裁切线）")
                   .arg(activeKeys.size());
        for (const QString& d : activeLineDescs)
            log << QStringLiteral("  ") + d;
        log << QStringLiteral("处理说明: 工作库仅保留接合线断头要素（剔除 %1 个跨界或无关要素），"
                              "接边记录仅保留接合线图幅对（跳过 %2 条）")
                   .arg(ctx->filteredOut).arg(ctx->skippedRecs);
    } else {
        log << QStringLiteral("接合线模式: 未检测到接合线，按全量模式执行（与旧版行为一致）");
    }
    if (shifted)
        log << QStringLiteral("处理说明: 图幅号超出SDK邻幅计算范围，数据整体南移 %1° 参与接边（结果已映射回真实图幅号）")
                   .arg(tiles[0].shiftLat);
    if (anyProj)
        log << QStringLiteral("处理说明: 输入为投影坐标系，已自动转换 WGS84 参与接边（结果库保持原坐标系）");
    if (!isolated.isEmpty())
        log << QStringLiteral("未参与接边的数据（无相邻图幅）: ") + isolated.join(QStringLiteral("、"));
    log << QStringLiteral("结果库: ") + resultPath;
    log << QStringLiteral("检测报告: ") + csvPath;
    log << QStringLiteral("接边结果: ") + summary;
    const QStringList sdkMsgs = SeEdgeMergeBridge::takeSdkMessages();
    if (!sdkMsgs.isEmpty()) {
        log << QStringLiteral("SDK消息:");
        log << sdkMsgs;
    }
    writeLog(log);

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("接边"));
    // 弹窗只给结论，明细在对话框报告区/检测报告/运行日志里
    box.setText(QStringLiteral("接边完成：已接 %1 对，边界未接 %2 处。\n\n是否将结果加载到地图？")
                    .arg(keptCount).arg(unmatched + rejectedShown));
    QPushButton* btnYes = box.addButton(QStringLiteral("是"), QMessageBox::YesRole);
    box.addButton(QStringLiteral("否"), QMessageBox::NoRole);
    box.setDefaultButton(btnYes);
    box.exec();
    if (box.clickedButton() == btnYes) {
        for (const QString& layer : resultLayers)
            emit addLayerToMap(resultPath + QStringLiteral("|layername=") + layer);
    }
    // 按钮在弹框与结果加载全部结束之后才恢复：避免加载期间再次触发执行
    ui.pushButton_run->setEnabled(true);
    ui.pushButton_close->setEnabled(true);
}

// ===================================================================
// 日志 / 持久化
// ===================================================================

void AutoEdgeMatchDialog::writeLog(const QStringList& lines)
{
    SeNmoSdkBridge::writeRunLog(m_logPath, QStringLiteral("Info"),
                                QStringLiteral("EdgeMatch"), lines);
}

void AutoEdgeMatchDialog::restoreState()
{
    const QgsSettings settings;
    m_inputPath = settings.value(QStringLiteral("EdgeMatch/InputPath"), QDir::homePath()).toString();
    m_logPath   = settings.value(QStringLiteral("EdgeMatch/LogPath"), QString()).toString();
    m_resultPath = settings.value(QStringLiteral("EdgeMatch/ResultPath"), QString()).toString();
    if (!m_logPath.isEmpty()) {
        // 上次显式保存过日志路径 → 保持，不再跟随结果路径
        m_bLogPathAutoFollow = false;
        ui.lineEdit_logPath->setText(m_logPath);
    }
    if (!m_resultPath.isEmpty()) {
        m_bResultPathAutoFollow = false;
        ui.lineEdit_resultPath->setText(m_resultPath);
    }
}

void AutoEdgeMatchDialog::saveState()
{
    m_inputPath = QFileInfo(ui.listWidget_datasets->count() > 0
                                ? ui.listWidget_datasets->item(0)->text()
                                : m_inputPath).absolutePath();
    m_logPath = ui.lineEdit_logPath->text().trimmed();
    if (m_logPath.isEmpty()) m_logPath = m_inputPath;
    m_resultPath = ui.lineEdit_resultPath->text().trimmed();

    QgsSettings settings;
    settings.setValue(QStringLiteral("EdgeMatch/InputPath"), m_inputPath);
    // 仅持久化用户显式设置的日志目录；自动跟随值不持久化，保证下次仍默认跟随结果路径
    if (!m_bLogPathAutoFollow)
        settings.setValue(QStringLiteral("EdgeMatch/LogPath"), m_logPath);
    // 仅持久化用户显式选择的结果目录；自动跟随值不持久化，保证下次仍默认跟随数据目录
    if (!m_bResultPathAutoFollow && !m_resultPath.isEmpty())
        settings.setValue(QStringLiteral("EdgeMatch/ResultPath"), m_resultPath);
}

// ===================================================================
// 字段下拉（交集 + 自动选择；首项为"不按属性"）
// ===================================================================

void AutoEdgeMatchDialog::populateFieldCombo()
{
    QString current = ui.comboBox_field->currentText().trimmed();
    int parenIdx = current.indexOf(QStringLiteral(" ("));
    if (parenIdx > 0) current = current.left(parenIdx);
    ui.comboBox_field->clear();

    int count = ui.listWidget_datasets->count();
    if (count == 0) return;
    ui.comboBox_field->addItem(kNoFieldOption);

    QSet<QString> fields;
    bool firstFile = true;
    for (int di = 0; di < count; ++di) {
        QString path = ui.listWidget_datasets->item(di)->text();
        GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
            path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!poDS) continue;
        OGRLayer* poLayer = poDS->GetLayer(0);
        if (!poLayer) { GDALClose(poDS); continue; }
        OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
        QList<QByteArray> rawNames;
        bool fileIsUtf8 = true;
        for (int f = 0; f < poDefn->GetFieldCount(); ++f) {
            QByteArray raw(poDefn->GetFieldDefn(f)->GetNameRef());
            rawNames.append(raw);
            bool hasMb = false;
            bool fieldUtf8 = isValidUtf8(raw, &hasMb);
            if (hasMb && !fieldUtf8) fileIsUtf8 = false;
        }
        QSet<QString> fileFields;
        for (const QByteArray& raw : rawNames) {
            bool hasMb = false;
            isValidUtf8(raw, &hasMb);
            QString name = (fileIsUtf8 && hasMb) ? QString::fromUtf8(raw)
                         : hasMb ? QString::fromLocal8Bit(raw)
                         : QString::fromUtf8(raw);
            if (!name.isEmpty()) fileFields.insert(name);
        }
        GDALClose(poDS);
        if (firstFile) { fields = fileFields; firstFile = false; }
        else fields.intersect(fileFields);
    }

    ui.comboBox_field->addItems(fields.values());
    if (current == kNoFieldOption || current.isEmpty()) {
        ui.comboBox_field->setCurrentIndex(0);
    } else if (fields.contains(current)) {
        ui.comboBox_field->setCurrentText(current);
    } else {
        ui.comboBox_field->setCurrentIndex(0);
    }
    autoSelectBestField();
}

void AutoEdgeMatchDialog::autoSelectBestField()
{
    int datasetCount = ui.listWidget_datasets->count();
    int comboCount = ui.comboBox_field->count();
    if (datasetCount == 0 || comboCount == 0) return;

    // 默认选择"不按属性"时不自动改选其它字段
    const bool keepDefault = (ui.comboBox_field->currentIndex() == 0);

    int curIdx = ui.comboBox_field->currentIndex();
    QString curField;
    if (curIdx > 0) {
        curField = ui.comboBox_field->itemText(curIdx);
        int p = curField.indexOf(QStringLiteral(" ("));
        if (p > 0) curField = curField.left(p);
    }
    bool hadSelection = !curField.isEmpty();

    auto isPerFragmentField = [](const QString& name) -> bool {
        QString u = name.toUpper();
        return u == QStringLiteral("SHAPE_AREA")
            || u == QStringLiteral("SHAPE_LENG")
            || u == QStringLiteral("SHAPE_LE_1")
            || u.startsWith(QStringLiteral("FID"))
            || u == QStringLiteral("OGC_FID");
    };

    struct Candidate { QString name; int distinct; bool perFragment; };
    QList<Candidate> candidates;

    for (int ci = 1; ci < comboCount; ++ci) { // 跳过首项"不按属性"
        QString fieldName = ui.comboBox_field->itemText(ci);
        bool perFragment = isPerFragmentField(fieldName);
        QSet<QString> values;
        for (int di = 0; di < datasetCount; ++di) {
            QString path = ui.listWidget_datasets->item(di)->text();
            GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
                path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
            if (!poDS) continue;
            OGRLayer* poLayer = poDS->GetLayer(0);
            if (!poLayer) { GDALClose(poDS); continue; }
            int idx = poLayer->GetLayerDefn()->GetFieldIndex(fieldName.toUtf8().constData());
            if (idx < 0)
                idx = poLayer->GetLayerDefn()->GetFieldIndex(fieldName.toLocal8Bit().constData());
            if (idx < 0) { GDALClose(poDS); continue; }
            poLayer->ResetReading();
            OGRFeature* feat;
            int sampled = 0;
            const int kMaxSample = 5000;
            while ((feat = poLayer->GetNextFeature()) != nullptr && sampled < kMaxSample) {
                if (feat->IsFieldSetAndNotNull(idx))
                    values.insert(QString::fromUtf8(feat->GetFieldAsString(idx)));
                OGRFeature::DestroyFeature(feat);
                ++sampled;
            }
            while ((feat = poLayer->GetNextFeature()) != nullptr)
                OGRFeature::DestroyFeature(feat);
            GDALClose(poDS);
        }
        candidates.append({fieldName, values.count(), perFragment});
    }
    if (candidates.isEmpty()) return;

    if (!keepDefault) {
        // 接边需要区分度高的字段（如名称/编码），取非碎段字段中 distinct 最大者
        const Candidate* best = nullptr;
        for (const auto& c : candidates)
            if (!c.perFragment && c.distinct >= 1)
                if (!best || c.distinct > best->distinct)
                    best = &c;

        int curDistinct = 0;
        if (hadSelection) {
            for (const auto& c : candidates) {
                if (c.name == curField) { curDistinct = c.distinct; break; }
            }
        }
        if (best && (!hadSelection || (curDistinct <= 1 && best->distinct > 1))) {
            int idx = ui.comboBox_field->findText(best->name);
            if (idx >= 0) ui.comboBox_field->setCurrentIndex(idx);
        }
    }

    for (int ci = 1; ci < comboCount; ++ci) {
        QString name = ui.comboBox_field->itemText(ci);
        for (const auto& c : candidates) {
            if (c.name == name) {
                ui.comboBox_field->setItemText(ci, QStringLiteral("%1 (%2种)").arg(name).arg(c.distinct));
                break;
            }
        }
    }
}
