#ifndef SE_CLIP_MERGE_TASK_H
#define SE_CLIP_MERGE_TASK_H

#include <qgstaskmanager.h>
#include <qgsmessagelog.h>
#include <string>
#include <vector>
#include <map>
#include <set>

using namespace std;

class OGRLayer;
class OGRGeometry;

struct FieldMappingItem {
    string outputName;    // 输出字段名
    string sourceName;    // 源文件中查找的字段名
    int fieldType;        // OGRFieldType
    int width;
    int precision;
};

class SeClipMergeTask : public QgsTask
{
    Q_OBJECT

public:
    // clipMode: "feature"/"coordinate"/""
    // mergeSingleLayer: true = 多文件合并为单个图层, false = 各自独立
    // fieldMappings: 空 = 自动收集全部字段; 非空 = 按映射表输出
    SeClipMergeTask(const QString& name,
                    const vector<string>& vecInputFiles,
                    const string& strOutputPath,
                    const string& strClipMode,
                    const string& strClipFeaturePath,
                    double dMinX, double dMinY, double dMaxX, double dMaxY,
                    double dTolerance,
                    int iLogLevel,
                    const string& strOutputLogPath,
                    bool bMergeSingleLayer = false,
                    const vector<FieldMappingItem>& fieldMappings = {});

    bool run() override;
    bool isCanceled();
    void cancel();
    void finished(bool result) override;

signals:
    void taskFinished(bool result);

private:
    vector<string> m_vecInputFiles;
    string m_strOutputPath;
    string m_strClipMode;
    string m_strClipFeaturePath;
    double m_dMinX, m_dMinY, m_dMaxX, m_dMaxY;
    double m_dTolerance;
    int m_iLogLevel;
    string m_strOutputLogPath;
    int mProgress;
    bool mCanceled;
    bool m_bMergeSingleLayer;
    vector<FieldMappingItem> m_fieldMappings;

    bool CopyLayer(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer,
                   OGRGeometry* poClipGeom, int& nWritten);
    void CopyData(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer,
                  OGRGeometry* poClipGeom, int& nWritten);
};

#endif
