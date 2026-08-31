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
    // 【2026-08-23】以下 DB 相关方法已注释（与地图数据下载 UI 重复）
    // void loadDbConfigSettings();
    // QString pgConnString() const;
    // QString findOgr2ogr() const;
};

#endif // GENERALIZATION_CONFIG_DIALOG_H
