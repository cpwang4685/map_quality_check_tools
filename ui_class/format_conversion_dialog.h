#ifndef FORMAT_CONVERSION_DIALOG_H
#define FORMAT_CONVERSION_DIALOG_H

#include <QDialog>
#include <QStringList>
#include "ui_format_conversion.h"

class QPushButton;

class FormatConversionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FormatConversionDialog(QWidget* parent = nullptr,
                                    Qt::WindowFlags fl = Qt::WindowFlags());
    ~FormatConversionDialog() override;

private:
    void restoreState();
    bool CheckFileOrDirExist(const QString& path);
    void resizeToContent();
    void autoFillNames();
    void autoUpdateLogPath();
    QString cleanFileName(const QString& fileName);
    bool isShpTarget() const;
    bool isGdbTarget() const;
    bool isGpkgTarget() const;
    bool isMdbSource() const;
    static bool isAccessEngineInstalled();
    QString accessEngineInstallerPath();
    void currentSourceFilter(QStringList& extensions, bool& allowFiles, bool& showGdbDirs) const;

    Ui::FormatConversionDialog ui;
    QString m_qstrInputDataPath;
    QString m_qstrSavePath;
    QString m_qstrOutputLogPath;
    QStringList m_srcFileList;
    bool m_bLogPathAutoFollow = true;
    bool m_bLayerNameManual = false;
    bool m_bGdbNameManual = false;

private slots:
    void Button_Open_clicked();
    void Button_SelectData_clicked();
    void Button_RemoveSelected_clicked();
    void Button_Save_clicked();
    void Button_OK_accepted();
    void Button_Cancel_rejected();
    void pushButton_SaveLog_clicked();
    void onConversionTypeChanged();
    void onBatchToggled(bool checked);
    void onTaskFinished(bool result);
    void CalculateTotalProgress();
    void onInputPathEdited(const QString& text);
    void onOutputPathEdited(const QString& text);
    void resetAllNames();

private:
    QPushButton* mBtnResetName = nullptr;
};

#endif
