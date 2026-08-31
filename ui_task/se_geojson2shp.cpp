#define _HAS_STD_BYTE 0
#include "se_geojson2shp.h"
#include <qdir.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "commontype/se_commondef.h"

/*spdlog日志库头文件*/
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"


/*构造函数*/
SE_Geojson2ShpTask::SE_Geojson2ShpTask(const QString& name,
    const string& strInputPath,
    const string& strOutputPath,
    int iLogLevel,
    const string& strOutputLogPath)
    : QgsTask(name),    
    m_strInputPath(strInputPath), 
    m_strOutputPath(strOutputPath), 
    m_iLogLevel(iLogLevel),
    m_strOutputLogPath(strOutputLogPath),
    mProgress(0)
{
    mCanceled = false;
}


bool SE_Geojson2ShpTask::run()
{
    // 写日志
#pragma region "创建日志器"

#pragma region "日志文件增加日志级别"

// 日志文件名称示例：System_Running_Error_A_point.txt
    string strLogLevel;

    // 错误日志
    if (m_iLogLevel == SE_LOG_LEVEL_ERROR)
    {
        strLogLevel = "Error";
    }

    // 信息
    else if (m_iLogLevel == SE_LOG_LEVEL_INFO)
    {
        strLogLevel = "Info";
    }

    // 调试
    else if (m_iLogLevel == SE_LOG_LEVEL_DEBUG)
    {
        strLogLevel = "Debug";
    }

#pragma endregion



    // 日志器名称
    string strLogFileName = "Geojson2Shp";

    // 日志文件全路径
    string strLogFileFullPath = m_strOutputLogPath + "/System_Running_" + strLogLevel + "_" + strLogFileName + ".txt";

    // 日志器名称
    string strLoggerName = "Geojson2Shp";


    // 创建一个写入普通文件的日志器"
    auto file_logger = spdlog::basic_logger_mt(strLoggerName.c_str(),
        strLogFileFullPath.c_str());

    /*记录日志*/

    // 错误日志
    if (m_iLogLevel == SE_LOG_LEVEL_ERROR)
    {
        // 设置日志级别
        file_logger->set_level(spdlog::level::err);
    }

    // 信息
    else if (m_iLogLevel == SE_LOG_LEVEL_INFO)
    {
        // 设置日志级别
        file_logger->set_level(spdlog::level::info);
    }

    // 调试
    else if (m_iLogLevel == SE_LOG_LEVEL_DEBUG)
    {
        // 设置日志级别
        file_logger->set_level(spdlog::level::debug);
    }

    // 输出一条启动信息
    char szLog[1000] = { 0 };
    sprintf(szLog, "正在执行geojson转shp任务...");
    file_logger->info(szLog);
    file_logger->debug(szLog);
    file_logger->flush();

#pragma endregion


    // 获取当前目录下所有的geojson文件
    QStringList qstrGeojsonFileList = GetFileNames(QString::fromLocal8Bit(m_strInputPath.c_str()));

    memset(szLog, 0, 1000);
    sprintf(szLog, "当前目录:%s下共有%d个geojson文件", m_strInputPath.c_str(), qstrGeojsonFileList.length());
    file_logger->info(szLog);
    file_logger->debug(szLog);
    file_logger->flush();


    for (int i = 0; i < qstrGeojsonFileList.length(); ++i)
    {
        string strGeojsonFileName = qstrGeojsonFileList[i].toLocal8Bit().constData();
        string strGeojsonFullPath = m_strInputPath + "/" + strGeojsonFileName;
        string strBaseName = CPLGetBasename(strGeojsonFullPath.c_str());
        string strShpFullPath = m_strOutputPath + "/" + strBaseName + ".shp";
        string strCPGFullPath = m_strOutputPath + "/" + strBaseName + ".cpg";
        
        // 如果转换成功
        if (ConvertGeoJSONToShapefile(strGeojsonFullPath, strShpFullPath))
        {
            CreateShapefileCPG(strCPGFullPath, "UTF-8");    
            // 每执行一次，计数+1

            memset(szLog, 0, 1000);
            sprintf(szLog, "当前文件：%s转换成功", strGeojsonFullPath.c_str());
            file_logger->info(szLog);
            file_logger->debug(szLog);
            file_logger->flush();

            setProgress((i + 1) * 100.0 / qstrGeojsonFileList.length());
        }
        else
        {
            memset(szLog, 0, 1000);
            sprintf(szLog, "当前文件：%s转换失败！", strGeojsonFullPath.c_str());
            file_logger->error(szLog);
            file_logger->flush();

            // 每执行一次，计数+1
            setProgress((i + 1) * 100.0 / qstrGeojsonFileList.length());
            continue;
        }
    }

    memset(szLog, 0, 1000);
    sprintf(szLog, "geojson转shp任务完毕！");
    file_logger->info(szLog);
    file_logger->debug(szLog);
    file_logger->flush();

    // 卸载日志记录器
    spdlog::drop(strLoggerName.c_str());


    mProgress = 100;
    setProgress(mProgress);
    
    if (isCanceled())
    {
        return false; // 如果任务被取消，返回 false
    }

    // 返回 true 表示任务成功完成
    return true; 
}

bool SE_Geojson2ShpTask::isCanceled()
{
    // 返回当前取消状态
    return mCanceled;           
}

void SE_Geojson2ShpTask::cancel()
{
    // 设置取消状态为 true
    mCanceled = true;           
}

int SE_Geojson2ShpTask::progress() const
{
    // 返回当前进度
    return mProgress;           
}

void SE_Geojson2ShpTask::finished(bool result)
{
    emit taskFinished(result);
}

QStringList SE_Geojson2ShpTask::GetFileNames(const QString& path)
{
    QDir dir(path);
    QStringList nameFilters;
    nameFilters << "*.geojson" << "*.GEOJSON";
    QStringList files = dir.entryList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
    return files;
}


bool SE_Geojson2ShpTask::ConvertGeoJSONToShapefile(const std::string& geojsonFile, const std::string& shpFile)
{
    // 注册所有的格式
    GDALAllRegister();

    // 打开GeoJSON文件
    GDALDataset* poGeoJSONDataset = (GDALDataset*)GDALOpenEx(geojsonFile.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (poGeoJSONDataset == nullptr) {
        //std::cerr << "Failed to open GeoJSON file: " << geojsonFile << std::endl;
        return false;
    }

    // 创建Shapefile数据集
    GDALDriver* poShapefileDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (poShapefileDriver == nullptr) {
        //std::cerr << "Shapefile driver is not available." << std::endl;
        GDALClose(poGeoJSONDataset);
        return false;
    }

    GDALDataset* poShapefileDataset = poShapefileDriver->Create(shpFile.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (poShapefileDataset == nullptr) {
        //std::cerr << "Failed to create Shapefile: " << shpFile << std::endl;
        GDALClose(poGeoJSONDataset);
        return false;
    }

    // 获取GeoJSON数据集的图层
    OGRLayer* poGeoJSONLayer = poGeoJSONDataset->GetLayer(0);
    if (poGeoJSONLayer == nullptr) 
    {
        //std::cerr << "Failed to get layer from GeoJSON dataset." << std::endl;
        GDALClose(poGeoJSONDataset);
        GDALClose(poShapefileDataset);
        return false;
    }

    // 获取GeoJSON图层的几何类型
    OGRwkbGeometryType oGeoJSONType = poGeoJSONLayer->GetGeomType();


    // 创建Shapefile图层，图层几何类型与输入图层保持一致
    string strShpFileName = CPLGetBasename(shpFile.c_str());
    OGRLayer* poShapefileLayer = poShapefileDataset->CreateLayer(strShpFileName.c_str(), poGeoJSONLayer->GetSpatialRef(), oGeoJSONType, nullptr);
    if (poShapefileLayer == nullptr) 
    {
        //std::cerr << "Failed to create layer in Shapefile." << std::endl;
        GDALClose(poGeoJSONDataset);
        GDALClose(poShapefileDataset);
        return false;
    }

    // 复制字段
    OGRFeatureDefn* poGeoJSONLayerDefn = poGeoJSONLayer->GetLayerDefn();
    for (int i = 0; i < poGeoJSONLayerDefn->GetFieldCount(); i++) 
    {
        OGRFieldDefn* poFieldDefn = poGeoJSONLayerDefn->GetFieldDefn(i);
        poShapefileLayer->CreateField(poFieldDefn);
    }

    // 复制要素
    OGRFeature* poFeature;
    poGeoJSONLayer->ResetReading();
    while ((poFeature = poGeoJSONLayer->GetNextFeature()) != nullptr) 
    {
        poShapefileLayer->CreateFeature(poFeature);
        OGRFeature::DestroyFeature(poFeature);
    }

    // 清理
    GDALClose(poGeoJSONDataset);
    GDALClose(poShapefileDataset);

    return true;
    //std::cout << "Conversion completed: " << geojsonFile << " to " << shpFile << std::endl;
}


// 创建shp数据的cpg编码文件
bool SE_Geojson2ShpTask::CreateShapefileCPG(string strCPGFilePath, string strEncoding)
{
    FILE* fp = fopen(strCPGFilePath.c_str(), "w");
    if (!fp)
    {
        return false;
    }

    fprintf(fp, "%s", strEncoding.c_str());

    fclose(fp);

    return true;
}
