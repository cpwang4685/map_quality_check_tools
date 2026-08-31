#ifndef SE_WUJI_PROCESS_RUNNER_H
#define SE_WUJI_PROCESS_RUNNER_H

#include <QObject>
#include <QProcess>
#include <QString>

class SeWujiProcessRunner : public QObject
{
    Q_OBJECT

public:
    explicit SeWujiProcessRunner(QObject* parent = nullptr);
    ~SeWujiProcessRunner();

    void setExePath(const QString& path);
    void setXmlFilePath(const QString& path);
    void setDataPath(const QString& path);
    void setTimeout(int milliseconds);
    bool start();
    void cancel();
    bool isRunning() const;
    QString outputLog() const;

signals:
    void progressUpdated(int percent, const QString& message);
    void processFinished(bool success, const QString& outputDir);
    void processError(const QString& errorMessage);

private slots:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    QProcess* m_process;
    QString m_exePath;
    QString m_xmlFilePath;
    QString m_dataPath;
    int m_timeoutMs;
    QString m_outputLog;
    bool m_cancelled;
};

#endif
