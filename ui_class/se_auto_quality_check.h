#ifndef SE_AUTO_QUALITY_CHECK_H
#define SE_AUTO_QUALITY_CHECK_H

#include <QDialog>
#include "ui_auto_quality_check.h"
#include <QString>
#include <QShowEvent>
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

class QDoubleSpinBox;
class QFormLayout;

using namespace std;

// ====== 单个检查项 运行时结构 ======
struct MissionCheckItem {
    QString name;          // 检查项名称（如"字段名称检查"）
    int missionId;         // 所属Mission ID (453-459)
    int mode;               // ProcessMode 位掩码
    bool enabled;           // 是否默认启用
    bool visible = true;    // 是否在界面显示（visible="false" 时隐藏，配置保留）
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

protected:
    // 首次显示时按屏幕可用区域定尺寸（低分辨率/高DPI下不越出屏幕）
    void showEvent(QShowEvent* event) override;

private:
    Ui_SeAutoQualityCheckDialog ui;
    bool m_uiFitOnce = false;   // showEvent 只在首次显示时做一次尺寸适配

    // ---- 数据路径 ----
    QString m_qstrOrigDataPath;     // 原始数据目录（综合前1w）
    QString m_qstrResultDataPath;   // 成果数据目录（综合后5w）
    QString m_qstrMissionXmlPath;   // mission_config.xml 路径
    QString m_qstrOutputDir;        // 质检结果输出目录
    QString m_qstrLayerMappingPath; // 图层映射表路径（CSV）
    // M460 ⑤真实移位值：分阶段数据（2026-11-08）
    QString m_qstrShiftBeforePath;  // 化简后移位前数据目录
    QString m_qstrShiftAfterPath;   // 最终成果（移位后）数据目录

    // ---- Mission 配置数据 ----
    QList<MissionGroup> m_missionGroups;  // 全部Mission组
    QHash<QString, double> m_thresholds;  // 阈值参数
    QList<LayerMappingItem> m_layerMappingItems; // 图层映射：标准图层→实际SHP

    // ---- 参数控件（检查项页签内） ----
    QHash<QString, QDoubleSpinBox*> m_paramSpins;  // 参数key → 数值框
    QHash<QString, int> m_paramDecimals;           // 参数key → 小数位

    // ---- 初始化 ----
    void restoreState();
    bool loadMissionConfig(const QString& xmlPath = QString());
    void loadDefaultMissionConfig();
    void buildMissionUI();
    void applyThresholdsToSpinBoxes();
    void collectThresholdsFromSpinBoxes();

    // ---- 检查项页签内的参数控件 ----
    QWidget* buildParamsPanel(int missionId, QWidget* parent);
    void addParamField(QFormLayout* form, const QString& key, const QString& label,
                       double minV, double maxV, int decimals, double step,
                       double defaultV, const QString& hint);
    void loadMatchParams(const QString& matchParamPath);
    void applyParamsToWidgets();
    void collectParamsFromWidgets();
    bool saveParamsToXml(QString* error);

    // ---- 路径工具 ----
    bool CheckFileOrDirExist(const QString& path);
    QString getDefaultMissionXmlPath();

    // ---- 图层映射 ----
    void loadLayerMappingCsv(const QString& csvPath);
    QList<LayerMappingItem> getDefaultStandardLayers() const;
    QString getDefaultLayerMappingCsvPath() const;

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
    void onBrowseShiftBefore();
    void onBrowseShiftAfter();
    void onBrowseMissionXml();
    void onBrowseOutputDir();
    void onBrowseLayerMappingConfig();
    void onConfigureLayerMapping();
    void onOpenCheckParams();

    // 质检
    void onStartCheck();
    void onSelectAll();
    void onDeselectAll();

    void onClose();
};

#endif
