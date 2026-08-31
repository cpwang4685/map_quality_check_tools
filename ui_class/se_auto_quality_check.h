#ifndef SE_AUTO_QUALITY_CHECK_H
#define SE_AUTO_QUALITY_CHECK_H

#include <QDialog>
#include "ui_auto_quality_check.h"
#include <QString>
#include <QProcess>
#include <QCheckBox>
#include <QGroupBox>
#include <QScrollArea>
#include <QPushButton>
#include "qgisinterface.h"
#include <vector>
#include <string>
#include <QHash>
#include <QDomDocument>
#include "se_layer_mapping_dialog.h"
using namespace std;

// ====== 单个检查项 运行时结构 ======
struct MissionCheckItem {
    QString name;          // 检查项名称（如"字段名称检查"）
    int missionId;         // 所属Mission ID (453-459)
    int mode;               // ProcessMode 位掩码
    bool enabled;           // 是否默认启用
    bool implemented;       // 插件侧是否已实现
    QString applyTo;        // 适用几何类型 "point"/"line"/"polygon"/"all"
    QString thresholdKey;   // 关联阈值key（空=无）
    QString note;           // 说明
    QCheckBox* checkbox = nullptr; // UI控件指针
};

// ====== Mission 组 运行时结构 ======
struct MissionGroup {
    int id;
    QString name;
    QString category;
    QString note;
    bool implemented;           // 是否有实现
    QWidget* groupBox = nullptr;  // 外层容器（箭头按钮+折叠内容）
    QPushButton* arrowBtn = nullptr;  // 展开/折叠箭头按钮
    QList<MissionCheckItem> items;
};

class CSE_AutoQualityCheckDialog : public QDialog
{
    Q_OBJECT
public:
    CSE_AutoQualityCheckDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~CSE_AutoQualityCheckDialog() override;

private:
    Ui_SeAutoQualityCheckDialog ui;

    // ---- 数据路径 ----
    QString m_qstrOrigDataPath;     // 原始数据目录（综合前1w）
    QString m_qstrResultDataPath;   // 成果数据目录（综合后5w）
    QString m_qstrMissionXmlPath;   // mission_config.xml 路径
    QString m_qstrOutputDir;        // 质检结果输出目录
    QString m_qstrLayerMappingPath; // 图层映射表路径（CSV）

    // ---- Mission 配置数据 ----
    QList<MissionGroup> m_missionGroups;  // 全部Mission组
    QHash<QString, double> m_thresholds;  // 阈值参数
    QList<LayerMappingItem> m_layerMappingItems; // 图层映射：标准图层→实际SHP

    // ---- 引擎 ----
    QProcess* m_chkProcess = nullptr;

    // ---- 初始化 ----
    void restoreState();
    bool loadMissionConfig(const QString& xmlPath = QString());
    void loadDefaultMissionConfig();
    void buildMissionUI();
    void applyThresholdsToSpinBoxes();
    void collectThresholdsFromSpinBoxes();

    // ---- 路径工具 ----
    bool CheckFileOrDirExist(const QString& path);
    QString getDefaultMissionXmlPath();

    // ---- 图层映射 ----
    void loadLayerMappingCsv(const QString& csvPath);
    QList<LayerMappingItem> getDefaultStandardLayers() const;
    QString getDefaultLayerMappingCsvPath() const;

    // ---- XML生成 ----
    QString generateMissionXml(const QString& dataDir, const QString& dataLabel);

    // ---- 写入日志 ----
    void writeJsonLog(const QString& logPath, const QString& dataType,
        int total, const QStringList& errors, const QStringList& checks,
        const QStringList& outputFiles = QStringList());

    // ---- 按映射的类型取SHP列表 ----
    QStringList getMappedShpByType(const QString& geomType) const;

    // ---- 图层扫描 ----
    static QStringList scanShpFiles(const QString& dirPath);

private slots:
    // 路径浏览
    void onBrowseOrigData();
    void onBrowseResultData();
    void onBrowseMissionXml();
    void onBrowseOutputDir();
    void onBrowseLayerMappingConfig();
    void onConfigureLayerMapping();

    // 质检
    void onStartCheck();
    void onChkProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onSelectAll();
    void onDeselectAll();

    void onClose();
};

#endif
