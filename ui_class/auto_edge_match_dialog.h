#ifndef AUTO_EDGE_MATCH_DIALOG_H
#define AUTO_EDGE_MATCH_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QComboBox>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include "ui_auto_edge_match.h"

class AutoEdgeMatchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AutoEdgeMatchDialog(QWidget* parent = nullptr,
                                 Qt::WindowFlags fl = Qt::WindowFlags());
    ~AutoEdgeMatchDialog() override;

private slots:
    void browseInput();
    void addDataset();
    void removeDataset();
    void moveDatasetUp();
    void moveDatasetDown();
    void browseOutput();
    void browseLog();
    void onOutputPathEdited(const QString& text);
    void onOk();

signals:
    void addLayerToMap(const QString& path);

private:
    void restoreState();
    void saveState();
    void autoUpdateLogPath();
    void populateFieldCombo();
    void autoSelectBestField();
    void autoAdjustParameters();

    Ui::AutoEdgeMatchDialog ui;
    QString m_inputPath;
    QString m_outputPath;
    QString m_logPath;
    bool m_bLogPathAutoFollow = true;

    QComboBox* m_comboField = nullptr;

    // Advanced parameter panel
    QGroupBox* m_groupAdvanced = nullptr;
    QDoubleSpinBox* m_spinFuzzyTolerance = nullptr;
    QComboBox* m_comboLinkMode = nullptr;
    QLabel* m_labelFuzzyMetric = nullptr;
};

#endif // AUTO_EDGE_MATCH_DIALOG_H
