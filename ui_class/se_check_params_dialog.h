#ifndef SE_CHECK_PARAMS_DIALOG_H
#define SE_CHECK_PARAMS_DIALOG_H

#include <QDialog>
#include <QHash>
#include <QString>

class QDoubleSpinBox;
class QFormLayout;
class QLineEdit;

// ================================================================
//  检查项参数设置对话框
//  只暴露面向最终用户的参数，文本型配置（⑦比对字段、检查图层范围）和
//  GlobalParams 不在界面中编辑，保存时保持配置文件原值不动。
//    M459(9)：AcuteAngle、SliverArea、NarrowWidth、MinArea、MinNodeDistance、
//             AvgNodeDensityUpper/Lower、NodeDensityUpper/Lower
//    M460：按七项关注点分组（综合前后配对参数移到本页）
//      配对 BufferDistance、MinOverlapRatio
//      ① EndpointBuffer、BoundaryTolerance、PointSideTolerance
//      ②③ LongGapThreshold、ConnectTolerance、MinBreakGap、TunnelPortalTolerance
//      ④ MaxChangeRatio、MaxVertexChange
//      ⑤ MaxDisplacement
//      ⑥ MaxSinuosityDrop
//      高级（默认隐藏）：MaxLocalBulge、MaxLocalDeviation、
//         MinSimplifyVertexCount、MinVertexReduceRatio、
//         RightAngleMinAngle/MaxAngle/OrigMaxTurn/MinSegment/SearchRadius
//  保存时写回 mission_config.xml 与同目录 matchParameter.xml（自动备份），
//  不修改检查项开关等非参数配置。
// ================================================================
class CSE_CheckParamsDialog : public QDialog
{
    Q_OBJECT
public:
    CSE_CheckParamsDialog(const QString& missionXmlPath,
                          const QHash<QString, double>& overrides,
                          QWidget* parent = nullptr);

    // 保存成功后的数值型参数值（文本参数直接写回 matchParameter.xml）
    QHash<QString, double> values() const { return m_values; }
    QString matchParamPath() const { return m_matchParamPath; }

    // 把一组参数写回 mission_config.xml（GlobalParams/Thresholds）与同目录
    // matchParameter.xml（Mission 458 Parameter）；只有内容变化才写，写前自动备份。
    // 主窗口的“开始质检时自动保存”直接调用本函数，不再依赖对话框。
    static bool saveValuesToFiles(const QString& missionXmlPath,
                                  const QHash<QString, double>& values,
                                  const QHash<QString, int>& decimals,
                                  const QHash<QString, QString>& textValues,
                                  QString* error);

private slots:
    void onSave();

private:
    struct Field {
        QDoubleSpinBox* editor = nullptr;
        int decimals = 2;
    };

    QString m_missionXmlPath;
    QString m_matchParamPath;
    QHash<QString, Field> m_fields;
    QHash<QString, QLineEdit*> m_textFields;
    QHash<QString, double> m_values;

    void buildUi();
    void loadValues(const QHash<QString, double>& overrides);
    void addField(QFormLayout* form, const QString& key, const QString& label,
                  double minV, double maxV, int decimals, double step,
                  double defaultV, const QString& hint);
    void addTextField(QFormLayout* form, const QString& key, const QString& label,
                      const QString& defaultV, const QString& hint);
    bool saveToFiles(QString* error);
    static QString buildMissionConfigText(const QString& source,
                                          const QHash<QString, double>& values,
                                          const QHash<QString, int>& decimals,
                                          QString* error);
    static QString buildMatchParamText(const QString& source, bool fileExists,
                                       const QHash<QString, double>& values,
                                       const QHash<QString, int>& decimals,
                                       const QHash<QString, QString>& textValues,
                                       QString* error);

    static QString formatNumber(double v, int decimals);
    static bool backupFile(const QString& path, QString* error);
    static bool writeFileAtomic(const QString& path, const QString& text, QString* error);
};

#endif
