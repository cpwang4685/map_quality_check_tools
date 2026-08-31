#ifndef CLIP_DIALOG_H
#define CLIP_DIALOG_H

#include <QDialog>
#include <QStringList>
#include <vector>
#include <string>
#include "ui_clip_dialog.h"

class SeClipMergeTask;
class QgisInterface;
class QgsMapCanvas;
class QgsRectangle;
class QgsVectorLayer;
class QgsRubberBand;
class MapExtentTool;

class ClipDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ClipDialog(QgisInterface* iface = nullptr,
                        QWidget* parent = nullptr,
                        Qt::WindowFlags fl = Qt::WindowFlags());
    ~ClipDialog() override;

private slots:
    void browseInput();
    void browseClip();
    void browseOutput();
    void browseLog();
    void onClipModeChanged();
    void onBatchToggled(bool checked);
    void onBatchSubChanged();
    void onBatchOutputModeChanged();
    void onOk();
    void onCancel();
    void onTaskFinished(bool result);
    void onOutputPathEdited(const QString& text);
    void onSelectExtentFromMap();
    void onExtentCaptured(const QgsRectangle& extent);

private:
    void restoreState();
    void saveState();
    void autoUpdateLogPath();
    QStringList scanInputFiles();
    bool isVectorFile(const QString& path);
    QString makeOutputPath(const QString& inputFile);
    void deactivateMapTool();
    void loadInputPreview(const QString& path);
    void loadBatchPreview(const QStringList& files);
    void removeInputPreview();

    Ui::ClipDialog ui;
    QgisInterface* mIface = nullptr;
    QgsMapCanvas* mCanvas = nullptr;
    MapExtentTool* mMapExtentTool = nullptr;
    QList<QgsVectorLayer*> mPreviewLayers;
    QgsRubberBand* mExtentHighlight = nullptr;
    QString m_inputPath;
    QString m_clipPath;
    QString m_outputPath;
    QString m_logPath;
    bool m_bLogPathAutoFollow = true;
    QString m_separateDir;
    QList<SeClipMergeTask*> m_tasks;
    int m_tasksCompleted = 0;
    int m_tasksTotal = 0;
    bool m_allTasksOk = true;
};

#endif // CLIP_DIALOG_H
