#ifndef MERGE_DIALOG_H
#define MERGE_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QComboBox>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include "ui_merge_dialog.h"

class MergeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MergeDialog(QWidget* parent = nullptr,
                         Qt::WindowFlags fl = Qt::WindowFlags());
    ~MergeDialog() override;

private slots:
    void browseInput();
    void browseOutput();
    void browseLog();
    void addDataset();
    void removeDataset();
    void moveDatasetUp();
    void moveDatasetDown();
    void onOk();
    void onOutputPathEdited(const QString& text);
    void refreshMode();

signals:
    void addLayerToMap(const QString& path);

private:
    // 按输入几何类型分流：面→溶解(Mission 327)，线→连接(Mission 175)
    enum Mode { ModeEmpty, ModePolygon, ModeLine, ModeMixed, ModePoint };
    enum GeomCat { CatUnknown = 0, CatPoint = 1, CatLine = 2, CatPolygon = 3 };

    void restoreState();
    void saveState();
    void autoUpdateLogPath();
    Mode detectMode();
    void applyMode(Mode mode);
    void populateFieldCombo();
    void autoSelectBestField();
    void autoAdjustParameters();
    void setDefaultOutput();
    void resizeToContent();
    static GeomCat geomCategory(int type);

    Ui::MergeDialog ui;
    QString m_inputPath;
    QString m_outputPath;
    QString m_logPath;
    bool m_bLogPathAutoFollow = true;
    Mode m_mode = ModeEmpty;

    // 字段下拉（面=分组字段/线=连接字段，同一个控件按模式改文案）
    QLabel* m_labelField = nullptr;
    QComboBox* m_comboField = nullptr;

    // 面（溶解）高级参数组
    QGroupBox* m_groupPolygon = nullptr;
    QDoubleSpinBox* m_spinBufferDistance = nullptr;
    QDoubleSpinBox* m_spinAngleEpsilon = nullptr;
    QComboBox* m_comboNeighborStyle = nullptr;
    QLabel* m_labelBufferMetric = nullptr;

    // 线（连接）高级参数组
    QGroupBox* m_groupLine = nullptr;
    QDoubleSpinBox* m_spinFuzzyTolerance = nullptr;
    QComboBox* m_comboLinkMode = nullptr;
    QLabel* m_labelFuzzyMetric = nullptr;
};

#endif // MERGE_DIALOG_H
