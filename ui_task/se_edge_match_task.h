#ifndef SE_EDGE_MATCH_TASK_H
#define SE_EDGE_MATCH_TASK_H

#include <qgstaskmanager.h>
#include <qgsmessagelog.h>
#include <QString>
#include <string>
#include <vector>

using namespace std;

class SeEdgeMatchTask : public QgsTask
{
    Q_OBJECT

public:
    SeEdgeMatchTask(const QString& description,
                    const string& sourcePath,
                    const string& sourceLayer,
                    const string& refPath,
                    const string& refLayer,
                    double tolerance,
                    int vertexMode,
                    int correctionMode,
                    bool syncNeighbor,
                    const string& matchLinks,
                    const string& outputPath,
                    int logLevel,
                    const string& logPath);

    bool run() override;
    bool isCanceled();
    void cancel();
    void finished(bool result) override;

signals:
    void taskFinished(bool result);

private:
    struct MatchLink {
        int srcFID, srcVertexIdx;
        int tgtFID, tgtVertexIdx;
        double tgtX, tgtY;
    };
    vector<MatchLink> parseMatchLinks(const string& serialized);

    string m_sourcePath, m_sourceLayer;
    string m_refPath, m_refLayer;
    double m_tolerance;
    int m_vertexMode;
    int m_correctionMode;
    bool m_syncNeighbor;
    string m_matchLinksStr;
    string m_outputPath;
    int m_logLevel;
    string m_logPath;
    int m_progress;
    bool m_canceled;
};

#endif
