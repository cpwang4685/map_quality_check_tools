#ifndef SE_FORMAT_CONVERT_TASK_H
#define SE_FORMAT_CONVERT_TASK_H

// 麒麟 QGIS SDK 用小写头（qgstaskmanager.h/qgsmessagelog.h），驼峰仅 Windows SDK 有
#include <qgstaskmanager.h>
#include <qgsmessagelog.h>
#include <string>
#include <qstring.h>
#include <vector>

class GDALDataset;
class OGRLayer;

using namespace std;

class SeFormatConvertTask : public QgsTask
{
    Q_OBJECT

public:
    SeFormatConvertTask(const QString& name,
        const string& strInputPath,
        const string& strOutputPath,
        const string& strSrcDriverName,
        const string& strTgtDriverName,
        const string& strSrcExtension,
        const string& strTgtExtension,
        int iLogLevel,
        const string& strOutputLogPath,
        bool bSingleInputFile = false,
        const string& strLayerName = "",
        const string& strGdbName = "");

    bool run() override;
    bool isCanceled();
    void cancel();
    int progress() const;
    void finished(bool result) override;
    void setSrcFileList(const QStringList& srcFileList) { m_srcFileList = srcFileList; }

signals:
    void taskFinished(bool result);

private:
    string m_strInputPath;
    string m_strOutputPath;
    string m_strSrcDriverName;
    string m_strTgtDriverName;
    string m_strSrcExtension;
    string m_strTgtExtension;
    int m_iLogLevel;
    string m_strOutputLogPath;
    int mProgress;
    bool mCanceled;
    bool m_bSingleInputFile;
    string m_strLayerName;
    string m_strGdbName;
    string m_strCopyError;
    QStringList m_srcFileList;

    QStringList GetFileNames(const QString& path, const QStringList& nameFilters);
    bool ConvertToSHP(const std::string& srcFile, const std::string& tgtDir, const std::string& baseName, const std::string& srcDriver = "");
    bool ConvertToGPKG(const std::string& srcFile, const std::string& outFile, const std::string& srcDriver = "");
    bool ConvertToGDB(const std::string& srcFile, const std::string& gdbPath, const std::string& srcDriver = "");
    bool CopyLayer(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer, const std::string& srcDriver = "");
    bool CreateShapefileCPG(string strCPGFilePath, string strEncoding);
};

#endif
