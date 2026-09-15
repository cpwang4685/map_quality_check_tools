#ifndef SE_FORMAT_CONVERT_TASK_H
#define SE_FORMAT_CONVERT_TASK_H

// 麒麟 QGIS SDK 用小写头（qgstaskmanager.h/qgsmessagelog.h），驼峰仅 Windows SDK 有
#include <qgstaskmanager.h>
#include <qgsmessagelog.h>
#include <string>
#include <functional>
#include <qstring.h>
#include <qstringlist.h>
#include <vector>

class GDALDataset;
class OGRLayer;
class OGREnvelope;

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
    int successCount() const { return m_successCount; }
    int totalCount() const { return m_totalCount; }
    QStringList failedNames() const { return m_failedNames; }

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
    int m_successCount = 0;
    int m_totalCount = 0;
    QStringList m_failedNames;

    QStringList GetFileNames(const QString& path, const QStringList& nameFilters);
    // baseName：显式层名（单文件模式手填），空串=自动命名（输出名=图层名，不拼源库前缀）；
    // srcBaseName：源库名，输出名被占用时加库名区分；
    // pOutDesc 回填实际输出文件名（日志用），pRenameNote 回填重名改存说明（可空）
    bool ConvertToSHP(const std::string& srcFile, const std::string& tgtDir,
                      const std::string& baseName, const std::string& srcBaseName,
                      const std::string& srcDriver = "",
                      const std::function<void(double)>& progCb = nullptr,
                      std::string* pOutDesc = nullptr, std::string* pRenameNote = nullptr);
    bool ConvertToGPKG(const std::string& srcFile, const std::string& outFile, const std::string& srcDriver = "", const std::function<void(double)>& progCb = nullptr);
    bool ConvertToGDB(const std::string& srcFile, const std::string& gdbPath, const std::string& srcDriver = "", const std::function<void(double)>& progCb = nullptr);
    // 批量模式：源文件的图层作为要素类追加进已打开的 GDB；重名自动 _2/_3 后缀（记录到 renameNote）
    bool CopyFileToGDB(const std::string& srcFile, GDALDataset* poTgtDS, const std::string& srcDriver, std::string& renameNote, const std::function<void(double)>& progCb = nullptr);
    bool CopyLayer(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer, const std::string& srcDriver = "", const std::function<void(double)>& progCb = nullptr, OGREnvelope* pEnv = nullptr);
};

#endif
