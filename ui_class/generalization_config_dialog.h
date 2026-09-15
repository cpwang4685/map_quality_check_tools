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

    QString shpDirectory() const;
    QString gdbDirectory() const;   // 【2026-09-09】GDB 数据页：GDB 目录选择
    QString convXmlPath() const;    // 【2026-09-09】GDB 数据页：gdb→shp 转换知识库 XML 路径
    QString configXmlPath() const;
    QString outputDirectory() const;
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
    // void onTestDbConnection();
    // void onFetchData();
    // void onSelectLayerTypes();
    void onSelectFsLayerTypes();
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
    QPushButton*  m_btnExecute          = nullptr;

    // ---- 子线程 Worker ----
    QThread*              m_workerThread = nullptr;
    GeneralizationWorker* m_worker       = nullptr;
    QProgressDialog*      m_execProgress = nullptr;

    // ---- 内部方法 ----
    void connectSignals();
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
