#pragma once
#ifndef GENERALIZATION_CONFIG_DIALOG_H
#define GENERALIZATION_CONFIG_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QDateTime>
#include <QApplication>
#include <QProgressDialog>
#include <QStringList>
#include <QThread>
#include <QXmlStreamReader>

class QgisInterface;

// ============================================================================
// GeneralizationWorker — 在子线程中调用 DoXMLFile，逐 Link 发送进度信号
// ============================================================================
class GeneralizationWorker : public QObject
{
    Q_OBJECT
public:
    explicit GeneralizationWorker(QObject* parent = nullptr) : QObject(parent) {}

    struct LinkEntry {
        bool    run;
        QString path;   // 相对于容器 XML 的路径
    };

    void setXmlDir(const QString& dir)         { m_xmlDir = dir; }
    void setDataDir(const QString& dir)        { m_dataDir = dir; }
    void setDllDir(const QString& dir)         { m_dllDir = dir; }
    void setLinks(const QVector<LinkEntry>& v) { m_links = v; }

public slots:
    void process();

signals:
    void progressChanged(int current, int total, const QString& stepName);
    void finished(int success, int total);
    void logMessage(const QString& msg, int level);  // 0=info 1=warning 2=critical

private:
    QString            m_xmlDir;
    QString            m_dataDir;
    QString            m_dllDir;
    QString            m_savedCwd;   // Linux: 保存原 CWD，DoXMLFile 结束后恢复（仅非 Windows 使用）
    QVector<LinkEntry> m_links;
};

// ============================================================================
// 综合缩编配置对话框
// ============================================================================
class GeneralizationConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GeneralizationConfigDialog(QWidget* parent = nullptr);
    ~GeneralizationConfigDialog() override;

    // 【2026-10-18】「从数据库获取数据」弹出的数据源对话框需要地图画布
    // （按范围导出要手动绘制/选择要素），由插件入口注入。
    void setQgisInterface(QgisInterface* iface);

    QString shpDirectory() const;
    QString gdbDirectory() const;   // 【2026-09-09】GDB 数据页：GDB 目录选择
    QString convXmlPath() const;    // 【2026-09-09】GDB 数据页：gdb→shp 转换知识库 XML 路径
    QString configXmlPath() const;
    QString outputDirectory() const;
    // 【2026-10-10】注记工程目录：里面放甲方提供的 .2dm 地图工程文件（可多个）。
    // 综合执行前会把该目录下所有 *.2dm 拷进结果目录——知识库里 <ProjectFile> 只写
    // 文件名，引擎按 DoXMLFile 的 relativePath（= 结果目录）去解析，所以两者必须同处一地。
    // 【2026-10-13】原来配套的「复制到结果目录」复选框已按需求去掉：目录非空就一律拷，
    // 留空则什么都不做（视为该知识库没有 405 注记步骤）。
    QString labelProjectDir() const;
    bool    isFileSystemSource() const;   // 仅文件系统数据源，恒 true
    // bool  isDatabaseSource() const;     // 【2026-08-23】PostGIS 数据库源已注释（与地图数据下载 UI 重复）

signals:
    void executeRequested();

private slots:
    // 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复）
    // void onSourceTypeChanged();
    void onBrowseShpDir();
    void onBrowseGdbDir();    // 【2026-09-09】GDB 数据页：GDB 目录浏览
    void onBrowseConvXml();   // 【2026-09-09】GDB 数据页：转换知识库 XML 浏览
    void onRunGdb2Shp();      // 【2026-09-09】GDB 数据页：执行 gdb→shp 转换
    void onBrowseConfigXml();
    void onBrowseOutputDir();
    void onBrowseLabelProjectDir();   // 【2026-10-10】注记工程目录浏览
    // 【2026-11-08】打开无极地图工作系统，用于制作 .2dm 注记工程文件
    void onOpenLabelProjectMaker();
    // 【2026-10-18】打开「从数据库获取数据源」对话框
    void onOpenProductSource();
    // void onTestDbConnection();
    // void onFetchData();
    // void onSelectLayerTypes();
    void onSelectFsLayerTypes();
    // 【2026-11-02】按当前数据重新识别「制图主区名称」
    void onDetectMainRegion();
    void onExecute();

    // ---- Worker 信号回调 (主线程) ----
    void onWorkerProgress(int current, int total, const QString& stepName);
    void onWorkerFinished(int success, int total);

private:
    // ---- 数据源模式 ----
    // 【2026-08-25】文件系统 radio 源选择已去除（仅文件系统一种来源），数据源分组框移除
    // 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复）
    // QRadioButton* m_radioDatabase  = nullptr;
    // QButtonGroup* m_btnGroupSource = nullptr;

    // ---- 文件系统面板 ----
    // 【2026-10-18】QGIS 接口（由插件入口注入），供「从数据库获取数据源」使用
    QgisInterface* m_pQgisIface = nullptr;
    QWidget*      m_widgetFileInput = nullptr;
    QLineEdit*    m_lineEditShpDir  = nullptr;
    QPushButton*  m_btnSelectFsLayerTypes = nullptr;
    QLabel*       m_labelFsSelectedTypes  = nullptr;
    QStringList   m_selectedFsLayerTypes;

    // ---- GDB 数据页（已废弃） ----
    // 【2026-09-09】测试 gdb→shp：GDB 目录 + 转换知识库 XML + 执行转换按钮。
    // 【2026-09-10】该页签的 UI 已整体移除（矢量数据目录现在同时接受 SHP / FileGDB，
    // 走两阶段流程，不再需要独立的 GDB 测试入口）。下面两个指针不再被创建，
    // onBrowseGdbDir/onBrowseConvXml/onRunGdb2Shp 也因控件消失而不再被 connect，
    // 属不可达代码，保留仅为减少改动面；切勿让它们重新接上。
    QLineEdit*    m_lineEditGdbDir   = nullptr;
    QLineEdit*    m_lineEditConvXml  = nullptr;

    // ---- 运行期文案（综合 / gdb→shp 转换共用同一 worker 回调） ----
    QString       m_taskLabel;            // 结果弹窗/进度标题（"综合缩编" / "GDB→SHP 转换"）
    QString       m_progressLabelPrefix;  // 进度条前缀，startWorkerRun 里按 taskLabel 生成

    // ---- 【2026-09-10】GDB 输入两阶段执行状态 ----
    // 输入是 FileGDB 时：阶段1 = gdb→shp 转换（临时 XML，输出到结果输出目录），
    // 阶段1 的 finished 回调里再启动阶段2 = 综合（综合知识库容器）。
    // 单一对话框、串行执行，故用普通成员保存阶段2参数即可。
    bool                                     m_pendingGdb2ShpRun = false;
    QString                                  m_stage2XmlDir;   // 综合容器：XML 所在目录（直接格式则为 XML 全路径）
    QString                                  m_stage2DataDir;  // 综合的数据基准目录 = 结果输出目录
    QString                                  m_stage2DllDir;   // NMO SDK / 插件目录
    QVector<GeneralizationWorker::LinkEntry> m_stage2Links;    // 已剔除 gdb2shp 转换步骤的综合 Link

    // ---- 数据库面板 ----
    // 【2026-08-23】地图综合 PostGIS 数据库源已注释（与地图数据下载 UI 重复）
    // QWidget*      m_widgetDbInput      = nullptr;
    // QLineEdit*    m_lineEditDbHost     = nullptr;
    // QSpinBox*     m_spinDbPort         = nullptr;
    // QLineEdit*    m_lineEditDbName     = nullptr;
    // QLineEdit*    m_lineEditDbSchema   = nullptr;
    // QLineEdit*    m_lineEditDbUser     = nullptr;
    // QLineEdit*    m_lineEditDbPassword = nullptr;
    // QPushButton*  m_btnTestDbConn      = nullptr;
    // QPushButton*  m_btnFetchData       = nullptr;
    // QLabel*       m_labelDbStatus      = nullptr;
    // QPushButton*  m_btnSelectLayerTypes = nullptr;
    // QLabel*       m_labelSelectedTypes  = nullptr;
    // QStringList   m_selectedDbLayerTypes;

    // ---- 公共配置 ----
    QLineEdit*    m_lineEditConfigXml   = nullptr;
    QLineEdit*    m_lineEditOutputDir   = nullptr;
    // 【2026-10-10】注记工程目录（.2dm 所在文件夹）
    // 【2026-10-13】原「复制到结果目录」复选框（m_chkCopyLabelProject）已去掉
    QLineEdit*    m_lineEditLabelProjectDir = nullptr;
    // 【2026-11-02】制图主区名称：放在「自定义综合比例尺」按钮右侧同一行。
    // 从矢量数据里的 BOUA5（县级行政区划面）自动识别，用户可手改；
    // 执行前据此校验知识库中是否存在对应县的预处理/后处理 XML，并把主 XML 的
    // Link 切到该县（只改 Link，不改县 XML 内容）。
    QLineEdit*    m_lineEditMainRegion  = nullptr;
    QPushButton*  m_btnExecute          = nullptr;

    // ---- 【2026-10-23】GDB 输入（路线B）----
    // 知识库没有 GDB→SHP 转换步骤时，插件自己把 GDB 转成 SHP 工作副本再跑整条链；
    // 知识库里那条 EsriGDB→SHP 的成果导出步骤不在链里跑，改由整链结束后按它的
    // <LayerNameList> 平铺导出。m_gdbExportXml 非空 = 本次需要做导出。
    QString       m_gdbExportXml;
    QString       m_exportSummary;   // 导出结果文案（拼到完成弹窗里）
    void runGdbExportStep(const QString& xmlPath);

    // ---- 子线程 Worker ----
    QThread*              m_workerThread = nullptr;
    GeneralizationWorker* m_worker       = nullptr;
    QProgressDialog*      m_execProgress = nullptr;

    // ---- 内部方法 ----
    void connectSignals();
    // 【2026-11-02】从数据源（FileGDB / SHP 目录）里的 BOUA5 识别主区名称。
    // 规则：跳过 NAME 以「属」开头的裁剪碎片，只认 AreaCode=620000（甘肃），
    // 按要素几何面积取最大；识别不到返回空串。
    QString detectMainRegionName(const QString& dataPath) const;
    // 【2026-11-07】主区名称改为「改 SQL、不切 Link」：
    // 顺着主 XML 里 1数据预处理_区县\<县名>.xml 这条 Link 找到被指向的那个县 XML
    //（跳过 预处理 / 预处理新增 / 分层改名 这些公共文件），只把它里面
    // <Mission id="11" note="属性查询:县级行政区划面"> 的
    // <SQL>NAME LIKE '%<县名>'</SQL> 中的县名换成 region。
    // Link 本身、县 XML 的其它内容一律不动，也不做 .bak 备份。
    // 返回改动的 SQL 条数（0 = 没找到 Link / 没找到该 Mission / 已经是目标县名）。
    int applyMainRegionToXml(const QString& xmlPath, const QString& region);
    // 【2026-11-07】给结果目录（含子目录）里每个 .dbf 补一个 .cpg 编码声明。
    // NMO 引擎按知识库的 Encoding_UTF8 落盘，但 DBF 头部的 LDID 固定写成 0x57(CP936)，
    // 也不生成 .cpg；GDAL/QGIS 读的时候没有 .cpg 可参考就按 Latin-1 解，属性表中文全乱。
    // 这里读第一条记录的字符字段判断实际编码（UTF-8 / CP936），写进 .cpg；
    // 已有 .cpg 的不覆盖。返回新写的个数。
    int writeDbfCpgFiles(const QString& rootDir);
    // 【2026-10-10】把注记工程目录下的 *.2dm 拷进结果目录（保留原文件名）。
    // 返回 false 表示已弹错/用户选择中止，调用方直接 return。
    // 目录为空（未填）时视为"本知识库无注记步骤"，不拷不报错，返回 true。
    bool copyLabelProjectFiles(const QString& outDir);
    // 【2026-09-09】通用启动一次 DoXMLFile 任务（onExecute / onRunGdb2Shp 共用）
    void startWorkerRun(const QString& xmlDir, const QString& dataDir,
                        const QVector<GeneralizationWorker::LinkEntry>& links,
                        const QString& dllDir, const QString& taskLabel);
    // 【2026-09-10】GDB 输入准备：在知识库 Link 里找 gdb2shp 转换步骤 → 生成临时
    // 转换 XML（把实际 GDB 目录 / 输出目录注入进去）→ 把该 Link 从综合步骤里剔除。
    // 返回 false 表示已弹错，调用方直接 return。
    bool prepareGdb2ShpStage(const QString& gdbDir, const QString& outDir,
                             bool isLinkContainer,
                             QVector<GeneralizationWorker::LinkEntry>& links,
                             QString& tempXmlPath);
    // 【2026-08-23】以下 DB 相关方法已注释（与地图数据下载 UI 重复）
    // void loadDbConfigSettings();
    // QString pgConnString() const;
    // QString findOgr2ogr() const;
};

#endif // GENERALIZATION_CONFIG_DIALOG_H
