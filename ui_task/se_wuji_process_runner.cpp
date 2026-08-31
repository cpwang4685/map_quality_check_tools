#include "se_wuji_process_runner.h"
#include <QFileInfo>
#include <QDir>

SeWujiProcessRunner::SeWujiProcessRunner(QObject* parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_timeoutMs(600000)  // 10 min default
    , m_cancelled(false)
{
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &SeWujiProcessRunner::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &SeWujiProcessRunner::onReadyReadStderr);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &SeWujiProcessRunner::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &SeWujiProcessRunner::onProcessError);
}

SeWujiProcessRunner::~SeWujiProcessRunner()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

void SeWujiProcessRunner::setExePath(const QString& path) { m_exePath = path; }
void SeWujiProcessRunner::setXmlFilePath(const QString& path) { m_xmlFilePath = path; }
void SeWujiProcessRunner::setDataPath(const QString& path) { m_dataPath = path; }
void SeWujiProcessRunner::setTimeout(int ms) { m_timeoutMs = ms; }
bool SeWujiProcessRunner::isRunning() const { return m_process->state() != QProcess::NotRunning; }
QString SeWujiProcessRunner::outputLog() const { return m_outputLog; }

bool SeWujiProcessRunner::start()
{
    if (m_exePath.isEmpty() || m_xmlFilePath.isEmpty()) {
        emit processError("EXE path or XML file path is empty");
        return false;
    }

    m_cancelled = false;
    m_outputLog.clear();

    QFileInfo exeInfo(m_exePath);
    m_process->setWorkingDirectory(exeInfo.absolutePath());

    QStringList args;
    args << m_xmlFilePath;
    if (!m_dataPath.isEmpty()) {
        args << m_dataPath;
    } else {
        args << QFileInfo(m_xmlFilePath).absolutePath();
    }
    args << "true" << "8";

    m_process->start(m_exePath, args);
    bool started = m_process->waitForStarted(10000);
    if (!started) {
        emit processError("Failed to start MapBatchProcessing.exe: " + m_process->errorString());
        return false;
    }
    return true;
}

void SeWujiProcessRunner::cancel()
{
    m_cancelled = true;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
}

void SeWujiProcessRunner::onReadyReadStdout()
{
    QString text = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    m_outputLog += text;
    emit progressUpdated(-1, text.trimmed());
}

void SeWujiProcessRunner::onReadyReadStderr()
{
    QString text = QString::fromLocal8Bit(m_process->readAllStandardError());
    m_outputLog += text;
}

void SeWujiProcessRunner::onProcessFinished(int exitCode, QProcess::ExitStatus /*status*/)
{
    if (m_cancelled) {
        emit processError("Process cancelled by user");
        return;
    }

    m_outputLog += QString::fromLocal8Bit(m_process->readAllStandardOutput());
    m_outputLog += QString::fromLocal8Bit(m_process->readAllStandardError());

    // MapBatchProcessing.exe may return non-zero exit code even on success;
    // check for success keywords in the output instead
    bool success = exitCode == 0
        || m_outputLog.contains(QStringLiteral("成功"))
        || m_outputLog.contains(QStringLiteral("OK!"));

    emit processFinished(success, m_outputLog);
}

void SeWujiProcessRunner::onProcessError(QProcess::ProcessError error)
{
    if (m_cancelled) return;

    QString msg;
    switch (error) {
    case QProcess::FailedToStart: msg = "Failed to start MapBatchProcessing.exe"; break;
    case QProcess::Crashed:       msg = "MapBatchProcessing.exe crashed"; break;
    case QProcess::Timedout:      msg = "MapBatchProcessing.exe timed out"; break;
    case QProcess::WriteError:    msg = "Write error communicating with MapBatchProcessing.exe"; break;
    case QProcess::ReadError:     msg = "Read error communicating with MapBatchProcessing.exe"; break;
    default:                      msg = "Unknown error running MapBatchProcessing.exe"; break;
    }
    emit processError(msg);
}
