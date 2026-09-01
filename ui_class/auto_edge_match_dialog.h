#ifndef AUTO_EDGE_MATCH_DIALOG_H
#define AUTO_EDGE_MATCH_DIALOG_H

#include <QDialog>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <string>
#include <vector>

#include "ui_auto_edge_match.h"

struct LayerMergeRecord;
class GDALDataset;
class OGRGeometry;
class QCloseEvent;

class AutoEdgeMatchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AutoEdgeMatchDialog(QWidget* parent = nullptr,
                                 Qt::WindowFlags fl = Qt::WindowFlags());
    ~AutoEdgeMatchDialog() override;

    // 接合线检测统计（每数据集一份；键 "E|k"=经线 k/4°，"N|k"=纬线 k/6°）
    struct EdgeStats {
        QHash<QString, int> cutNeg;  // 西/南侧断头要素数
        QHash<QString, int> cutPos;  // 东/北侧断头要素数
        QHash<QString, int> cross;   // 跨界要素数
        double envMinX = 0, envMaxX = 0, envMinY = 0, envMaxY = 0; // 线/面要素包络
        bool haveEnv = false;
    };

    // 接合线过滤上下文；activeKeys 为空 = 全量模式（不过滤，映射照建）
    struct EdgeFilterCtx {
        QSet<QString> activeKeys;                        // 接合线键集合（同 EdgeStats 键格式）
        QHash<QString, QPair<QString, qint64>> srcMap;   // 源键 → (结果图层名, 结果FID)，BuildResult 填充
        QHash<QString, QPair<QString, qint64>> workMap;  // 工作图层名|工作FID → (结果图层名, 结果FID)，BuildWork 填充
        double bandDeg = 0.0;                            // 断头带（度）= 接边距离
        int skippedRecs = 0;                             // 非接合线/FID映射缺失跳过的接边记录数
        int filteredOut = 0;                             // 工作库剔除的非断头要素数
    };

    // 插件拉齐信息：SDK 只记不合并（AUTO_MERGE=0）的真缝对，插件把两侧贴缝
    // 顶点投影到接合线拉齐后记录于此（图幅号/FID 均为工作库值）
    struct GapFixInfo {
        QString gtype;       // 几何词 line/polygon
        QString curCode;     // 工作库图幅号（本）
        qint64 curFid = 0;   // 工作库 FID（本）
        QString adjCode;     // 工作库图幅号（邻）
        qint64 adjFid = 0;   // 工作库 FID（邻）
        double origDistM = 0; // 拉齐前两侧距缝距离（米，较大侧）
        QString reason;      // 未拉齐原因（跨主题/一对多落选），空=已拉齐
    };

signals:
    void addLayerToMap(const QString& path);

private slots:
    void addData();
    void removeDataset();
    void browseLog();
    void browseResult();
    void onRun();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // 1:5万图幅信息（图幅号编码规则为黑盒实测所得）
    struct TileInfo {
        QString code;      // 工作库图层命名图幅号（可能为纬度平移后）
        QString realCode;  // 真实图幅号（报告与结果库用）
        double lonMin = 0, lonMax = 0, latMin = 0, latMax = 0; // 真实 WGS84 范围
        double shiftLat = 0; // 纬度南移量（度，0=未平移）
        int latIdx = 0;    // 幅内纬度序 0-23（真实）
        int lonIdx = 0;    // 幅内经度序 0-23
        bool needProj = false; // 源数据坐标系非 WGS84，需投影
    };

    // 结果库：真实图幅号 + 原始几何/坐标系；工作库：平移后图幅号 + WGS84 几何
    enum BuildMode { BuildResult = 0, BuildWork = 1 };

    // 要素归属图幅：按 WGS84 外接矩形中点所在网格单元（恰在网格线上的点归东/南侧单元）
    static bool computeCellOf(const OGRGeometry* poGeom, TileInfo* out);
    // 单点归属图幅（断头要素按贴线顶点路由到端点所在图幅，保证 SDK 在真实边界处看到端点）
    static bool computeCellOfXY(double lon, double lat, TileInfo* out);
    bool computeTileInfos(const QString& path, QVector<TileInfo>* out, QString* err,
                          EdgeStats* stats = nullptr,
                          const std::function<void(double)>& onProgress = {});
    bool chooseShift(QVector<TileInfo>* tiles, QString* err);
    bool buildGpkg(const QString& gpkgPath, const QVector<TileInfo>& tiles,
                   BuildMode mode, QString* err, QStringList* outResultLayers,
                   GDALDataset** outDS = nullptr,
                   const std::function<void(double)>& onProgress = {},
                   EdgeFilterCtx* ctx = nullptr);
    bool copyEdgeRecords(const QString& srcPath, const QString& dstPath,
                         const QVector<TileInfo>& tiles, QString* err,
                         GDALDataset* poDstIn = nullptr,
                         EdgeFilterCtx* ctx = nullptr,
                         const QVector<GapFixInfo>& fixed = QVector<GapFixInfo>());
    // SDK 只记不合并（AUTO_MERGE=0）的真缝对拉齐：两侧贴缝顶点距缝在缓冲带内
    // 且沿线错开 ≤150 米时，投影到接合线上（沿线坐标取平均）写回结果库。
    // 同主题、按沿线距离贪心 1:1 配对；跨主题对与一对多落选对写入 rejected
    bool fixGapPairs(const QString& workPath, const QVector<TileInfo>& tiles,
                     GDALDataset* poDst, EdgeFilterCtx* ctx,
                     QVector<GapFixInfo>* out, QVector<GapFixInfo>* rejected,
                     QString* err);
    void detectUnmatched(const QString& gpkgPath, const QString& field,
                         const QVector<TileInfo>& tiles, double distDeg,
                         const std::vector<LayerMergeRecord>& records,
                         int* outUnmatched, int* outDiffName,
                         EdgeFilterCtx* ctx = nullptr,
                         const QVector<GapFixInfo>& fixed = QVector<GapFixInfo>(),
                         const QVector<GapFixInfo>& rejected = QVector<GapFixInfo>());
    // 两条工作图幅号（可能平移后）的共享边是否为接合线（按真实坐标键判定）
    static bool sharedEdgeActive(const QHash<QString, TileInfo>& byCode,
                                 const QString& curCode, const QString& adjCode,
                                 const QSet<QString>& activeKeys);
    void addPaths(const QStringList& paths);
    void clearReport();
    void appendReportRow(const QStringList& cols);
    void writeLog(const QStringList& lines);
    void saveState();
    void restoreState();
    void autoUpdateLogPath();
    void autoUpdateResultPath();
    void populateFieldCombo();
    void autoSelectBestField();

    Ui::AutoEdgeMatchDialog ui;
    QString m_inputPath;
    QString m_logPath;
    QString m_resultPath;
    bool m_bLogPathAutoFollow = true;
    bool m_bResultPathAutoFollow = true;
    bool m_distanceUserSet = false;  // 用户改过缓冲距离（1:25万大图幅不再自动套 250 米）
};

#endif // AUTO_EDGE_MATCH_DIALOG_H
