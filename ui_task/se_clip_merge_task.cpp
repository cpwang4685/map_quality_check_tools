#define _HAS_STD_BYTE 0
#include "se_clip_merge_task.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_geometry.h>
#include <cpl_string.h>
#include <cstdio>
#include <cstring>
#include <QFile>
#include <sstream>
#include <memory>
#include <vector>
#include <atomic>
#include <map>
#include <set>
#include <algorithm>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/ostream_sink.h"
#include "commontype/se_commondef.h"

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qtextstream.h>
#include <qgscoordinatetransform.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsproject.h>
#include <qgspointxy.h>

// UTF-8 安全截断：按字节数截断，不切断多字节字符
static std::string truncateUtf8(const std::string& s, size_t maxLen)
{
    if (s.size() <= maxLen) return s;
    size_t pos = maxLen;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        pos--;
    return s.substr(0, pos);
}

SeClipMergeTask::SeClipMergeTask(const QString& name,
                                 const vector<string>& vecInputFiles,
                                 const string& strOutputPath,
                                 const string& strClipMode,
                                 const string& strClipFeaturePath,
                                 double dMinX, double dMinY, double dMaxX, double dMaxY,
                                 double dTolerance,
                                 int iLogLevel,
                                 const string& strOutputLogPath,
                                 bool bMergeSingleLayer,
                                 const vector<FieldMappingItem>& fieldMappings)
    : QgsTask(name)
    , m_vecInputFiles(vecInputFiles)
    , m_strOutputPath(strOutputPath)
    , m_strClipMode(strClipMode)
    , m_strClipFeaturePath(strClipFeaturePath)
    , m_dMinX(dMinX), m_dMinY(dMinY), m_dMaxX(dMaxX), m_dMaxY(dMaxY)
    , m_dTolerance(dTolerance)
    , m_iLogLevel(iLogLevel)
    , m_strOutputLogPath(strOutputLogPath)
    , mProgress(0)
    , mCanceled(false)
    , m_bMergeSingleLayer(bMergeSingleLayer)
    , m_fieldMappings(fieldMappings)
{
}

// ---- 裁剪一个要素，返回裁剪后的几何（调用方负责释放）或 nullptr（不相交） ----
static OGRGeometry* ClipGeometry(OGRGeometry* poGeom, OGRGeometry* poClipGeom)
{
    if (!poClipGeom || !poGeom) return poGeom ? poGeom->clone() : nullptr;
    if (!poGeom->Intersects(poClipGeom)) return nullptr;
    OGRGeometry* poResult = poGeom->Intersection(poClipGeom);
    if (poResult && poResult->IsEmpty())
    {
        OGRGeometryFactory::destroyGeometry(poResult);
        return nullptr;
    }
    return poResult;
}

bool SeClipMergeTask::run()
{
    GDALAllRegister();

    const char* logLevelTag = "Info";
    if (m_iLogLevel == SE_LOG_LEVEL_ERROR)      logLevelTag = "Error";
    else if (m_iLogLevel == SE_LOG_LEVEL_INFO)  logLevelTag = "Info";
    else if (m_iLogLevel == SE_LOG_LEVEL_DEBUG) logLevelTag = "Debug";

    string strLogFileFullPath = m_strOutputLogPath + "/System_Running_"
        + logLevelTag + "_ClipMerge.txt";
    auto logStream = std::make_shared<std::ostringstream>();
    auto logSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*logStream);
    static std::atomic<int> s_loggerId(0);
    string loggerName = "ClipMerge_" + std::to_string(++s_loggerId);
    auto file_logger = std::make_shared<spdlog::logger>(loggerName, logSink);
    spdlog::register_logger(file_logger);
    if (m_iLogLevel == SE_LOG_LEVEL_ERROR)      file_logger->set_level(spdlog::level::err);
    else if (m_iLogLevel == SE_LOG_LEVEL_INFO)  file_logger->set_level(spdlog::level::info);
    else if (m_iLogLevel == SE_LOG_LEVEL_DEBUG) file_logger->set_level(spdlog::level::debug);

    file_logger->info("开始执行要素裁剪任务...");
    file_logger->flush();

    char szLog[1000] = { 0 };
    bool bOk = false;
    OGRGeometry* poClipGeom = nullptr;
    GDALDataset* poOutDS = nullptr;
    OGRwkbGeometryType mergedGeomType = wkbUnknown;
    OGRSpatialReference* mergedSRS = nullptr;
    bool bFirstSRS = true;

    do {
        // ---- 1. 构建裁剪几何 ----
        if (m_strClipMode == "feature")
        {
            GDALDataset* poClipDS = (GDALDataset*)GDALOpenEx(
                m_strClipFeaturePath.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
            if (!poClipDS)
            {
                file_logger->error("无法打开裁剪要素文件: " + m_strClipFeaturePath);
                break;
            }

            for (int i = 0; i < poClipDS->GetLayerCount(); i++)
            {
                OGRLayer* poLayer = poClipDS->GetLayer(i);
                if (!poLayer) continue;
                poLayer->ResetReading();
                OGRFeature* poFeat;
                while ((poFeat = poLayer->GetNextFeature()) != nullptr)
                {
                    OGRGeometry* poGeom = poFeat->GetGeometryRef();
                    if (poGeom)
                    {
                        if (!poClipGeom)
                            poClipGeom = poGeom->clone();
                        else {
                            OGRGeometry* poNew = poClipGeom->Union(poGeom);
                            OGRGeometryFactory::destroyGeometry(poClipGeom);
                            poClipGeom = poNew;
                        }
                    }
                    OGRFeature::DestroyFeature(poFeat);
                }
            }
            GDALClose(poClipDS);

            if (!poClipGeom)
            {
                file_logger->error("裁剪要素文件中无有效几何");
                break;
            }
            snprintf(szLog, sizeof(szLog), "已加载裁剪要素: %s", m_strClipFeaturePath.c_str());
            file_logger->info(szLog);
        }
        else if (m_strClipMode == "coordinate")
        {
            // 用 QGIS 坐标转换（避免 GDAL 2.x + PROJ 9.3 崩溃）
            double tMinX = m_dMinX, tMinY = m_dMinY, tMaxX = m_dMaxX, tMaxY = m_dMaxY;
            bool bTransformed = false;

            if (!m_vecInputFiles.empty())
            {
                GDALDataset* poFirstDS = (GDALDataset*)GDALOpenEx(
                    m_vecInputFiles[0].c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                if (poFirstDS)
                {
                    OGRLayer* poLayer = poFirstDS->GetLayer(0);
                    if (poLayer)
                    {
                        OGRSpatialReference* poDataSRS = poLayer->GetSpatialRef();
                        if (poDataSRS)
                        {
                            char* pszWkt = nullptr;
                            poDataSRS->exportToWkt(&pszWkt);
                            QgsCoordinateReferenceSystem srcCrs(QStringLiteral("EPSG:4326"));
                            QgsCoordinateReferenceSystem tgtCrs(QString::fromUtf8(pszWkt));
                            CPLFree(pszWkt);

                            if (srcCrs.isValid() && tgtCrs.isValid())
                            {
                                QgsCoordinateTransform xform(srcCrs, tgtCrs, QgsProject::instance());
                                QgsPointXY p1 = xform.transform(m_dMinX, m_dMinY);
                                QgsPointXY p2 = xform.transform(m_dMaxX, m_dMinY);
                                QgsPointXY p3 = xform.transform(m_dMaxX, m_dMaxY);
                                QgsPointXY p4 = xform.transform(m_dMinX, m_dMaxY);
                                tMinX = std::min({p1.x(), p2.x(), p3.x(), p4.x()});
                                tMinY = std::min({p1.y(), p2.y(), p3.y(), p4.y()});
                                tMaxX = std::max({p1.x(), p2.x(), p3.x(), p4.x()});
                                tMaxY = std::max({p1.y(), p2.y(), p3.y(), p4.y()});
                                bTransformed = true;

                                snprintf(szLog, sizeof(szLog),
                                    "坐标转换完成: WGS84(%.6f,%.6f ~ %.6f,%.6f) -> (%.6f,%.6f ~ %.6f,%.6f)",
                                    m_dMinX, m_dMinY, m_dMaxX, m_dMaxY,
                                    tMinX, tMinY, tMaxX, tMaxY);
                                file_logger->info(szLog);
                            }
                        }
                    }
                    GDALClose(poFirstDS);
                }
            }

            if (!bTransformed)
            {
                snprintf(szLog, sizeof(szLog), "裁剪范围（WGS84未转换）: X[%.6f, %.6f] Y[%.6f, %.6f]",
                    m_dMinX, m_dMaxX, m_dMinY, m_dMaxY);
                file_logger->info(szLog);
            }

            OGRLinearRing oRing;
            oRing.addPoint(tMinX, tMinY);
            oRing.addPoint(tMaxX, tMinY);
            oRing.addPoint(tMaxX, tMaxY);
            oRing.addPoint(tMinX, tMaxY);
            oRing.closeRings();
            poClipGeom = new OGRPolygon();
            ((OGRPolygon*)poClipGeom)->addRing(&oRing);
        }
        else
        {
            file_logger->info("未指定裁剪模式，将仅执行合并操作");
        }

        file_logger->flush();

        // ---- 2. 输入文件列表 ----
        snprintf(szLog, sizeof(szLog), "共 %d 个输入文件", (int)m_vecInputFiles.size());
        file_logger->info(szLog);
        file_logger->flush();

        if (m_vecInputFiles.empty())
        {
            file_logger->warn("输入文件列表为空");
            break;
        }

        // ---- 3. 确定输出驱动 ----
        QFileInfo fiOutput(QString::fromUtf8(m_strOutputPath.c_str()));
        QString outputExt = fiOutput.suffix().toLower();
        string strDriverName;
        string strOutputFile = m_strOutputPath;

        if (outputExt == "shp")
        {
            strDriverName = "ESRI Shapefile";
        }
        else if (outputExt == "gpkg")
        {
            strDriverName = "GPKG";
        }
        else
        {
            file_logger->error("不支持的输出格式: " + m_strOutputPath + " (仅支持 .shp / .gpkg)");
            break;
        }

        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName(strDriverName.c_str());
        if (!poDriver)
        {
            file_logger->error(string(strDriverName) + " 驱动不可用");
            break;
        }

        QDir().mkpath(fiOutput.absolutePath());

        poOutDS = poDriver->Create(strOutputFile.c_str(), 0, 0, 0, GDT_Unknown, nullptr);

        if (!poOutDS)
        {
            file_logger->error("无法创建输出文件: " + m_strOutputPath);
            break;
        }

        // ---- 4. 合并模式：预扫描所有文件的字段集合 ----
        int totalFiles = (int)m_vecInputFiles.size();
        int processedFiles = 0;
        int totalFeaturesWritten = 0;
        bool bSHPFieldsCreated = false;
        OGRLayer* poOutLayer = nullptr;

        struct MergedFieldDef {
            std::string name;
            OGRFieldType type;
            int width;
            int precision;
        };
        std::vector<MergedFieldDef> mergedFields;

        if (m_bMergeSingleLayer)
        {
            // 几何类型归类函数: 点=1 线=2 面=3 未知=0
            auto geomCategory = [](OGRwkbGeometryType t) -> int {
                switch (wkbFlatten(t)) {
                    case wkbPoint: case wkbMultiPoint: return 1;
                    case wkbLineString: case wkbMultiLineString: return 2;
                    case wkbPolygon: case wkbMultiPolygon: return 3;
                    default: return 0;
                }
            };

            for (int i = 0; i < totalFiles; i++)
            {
                GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
                    m_vecInputFiles[i].c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                if (!poDS) continue;
                OGRLayer* poLayer = poDS->GetLayer(0);
                if (!poLayer) { GDALClose(poDS); continue; }

                if (bFirstSRS && poLayer->GetSpatialRef()) {
                    mergedSRS = poLayer->GetSpatialRef()->Clone();
                    bFirstSRS = false;
                }

                OGRwkbGeometryType geomType = (OGRwkbGeometryType)wkbFlatten(poLayer->GetGeomType());
                if (geomType == wkbUnknown) {
                    poLayer->ResetReading();
                    OGRFeature* poFeat;
                    while ((poFeat = poLayer->GetNextFeature()) != nullptr) {
                        OGRGeometry* poGeom = poFeat->GetGeometryRef();
                        if (poGeom) {
                            geomType = (OGRwkbGeometryType)wkbFlatten(poGeom->getGeometryType());
                            if (geomType != wkbUnknown) {
                                OGRFeature::DestroyFeature(poFeat);
                                break;
                            }
                        }
                        OGRFeature::DestroyFeature(poFeat);
                    }
                }

                if (geomType != wkbUnknown) {
                    int cat = geomCategory(geomType);
                    int existCat = geomCategory(mergedGeomType);
                    if (mergedGeomType == wkbUnknown) {
                        mergedGeomType = geomType;
                    } else if (cat != existCat) {
                        snprintf(szLog, sizeof(szLog),
                            "几何类型冲突: %s 为 %s 类型，与之前的 %s 类型不兼容",
                            m_vecInputFiles[i].c_str(),
                            (cat == 1) ? "点" : (cat == 2) ? "线" : "面",
                            (existCat == 1) ? "点" : (existCat == 2) ? "线" : "面");
                        file_logger->error(szLog);
                    }
                }

                OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
                snprintf(szLog, sizeof(szLog), "预扫描 [%d/%d] %s: %d 个字段",
                    i + 1, totalFiles, m_vecInputFiles[i].c_str(), poDefn->GetFieldCount());
                file_logger->info(szLog);
                for (int f = 0; f < poDefn->GetFieldCount(); f++)
                {
                    OGRFieldDefn* poField = poDefn->GetFieldDefn(f);
                    const char* szName = poField->GetNameRef();
                    if (!szName || strlen(szName) == 0) continue;

                    std::string fieldName(szName);
                    snprintf(szLog, sizeof(szLog), "  字段[%d]: %s (类型=%d)",
                        f, szName, (int)poField->GetType());
                    file_logger->info(szLog);

                    bool bExists = false;
                    for (auto& mf : mergedFields) {
                        if (mf.name == fieldName) { bExists = true; break; }
                    }
                    if (!bExists) {
                        mergedFields.push_back({
                            fieldName,
                            poField->GetType(),
                            poField->GetWidth(),
                            poField->GetPrecision()
                        });
                    }
                }
                GDALClose(poDS);
            }

            // 几何类型冲突则中止
            if (mergedGeomType != wkbUnknown) {
                int existCat = geomCategory(mergedGeomType);
                for (int i = 0; i < totalFiles && existCat != 0; i++) {
                    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
                        m_vecInputFiles[i].c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                    if (!poDS) continue;
                    OGRLayer* poLayer = poDS->GetLayer(0);
                    if (!poLayer) { GDALClose(poDS); continue; }
                    OGRwkbGeometryType gt = (OGRwkbGeometryType)wkbFlatten(poLayer->GetGeomType());
                    if (gt == wkbUnknown) {
                        poLayer->ResetReading();
                        OGRFeature* poFeat;
                        while ((poFeat = poLayer->GetNextFeature()) != nullptr) {
                            OGRGeometry* poGeom = poFeat->GetGeometryRef();
                            if (poGeom) {
                                gt = (OGRwkbGeometryType)wkbFlatten(poGeom->getGeometryType());
                                if (gt != wkbUnknown) { OGRFeature::DestroyFeature(poFeat); break; }
                            }
                            OGRFeature::DestroyFeature(poFeat);
                        }
                    }
                    GDALClose(poDS);
                    if (gt != wkbUnknown && geomCategory(gt) != existCat)
                        existCat = 0; // 冲突标记
                }
                if (existCat == 0) {
                    file_logger->error("输入图层几何类型不一致（点/线/面混合），无法执行合并");
                    break;
                }
            }

            snprintf(szLog, sizeof(szLog), "预扫描完成: 共收集 %d 个不重复字段", (int)mergedFields.size());
            file_logger->info(szLog);
            for (size_t fi = 0; fi < mergedFields.size(); fi++) {
                snprintf(szLog, sizeof(szLog), "  输出字段[%d]: %s (类型=%d, 宽度=%d, 精度=%d)",
                    (int)fi, mergedFields[fi].name.c_str(), (int)mergedFields[fi].type,
                    mergedFields[fi].width, mergedFields[fi].precision);
                file_logger->info(szLog);
            }

            // 如果对话框提供了字段映射，使用映射表覆盖自动扫描结果
            if (!m_fieldMappings.empty())
            {
                mergedFields.clear();
                for (const auto& fm : m_fieldMappings) {
                    mergedFields.push_back({
                        fm.outputName,
                        (OGRFieldType)fm.fieldType,
                        fm.width,
                        fm.precision
                    });
                }
            }

            // 创建输出层（一次性建完所有字段）
            QFileInfo fiFirst(QString::fromUtf8(m_vecInputFiles[0].c_str()));
            char** papszLCO = nullptr;
            if (strDriverName == "ESRI Shapefile")
                papszLCO = CSLSetNameValue(papszLCO, "ENCODING", "UTF-8");
            poOutLayer = poOutDS->CreateLayer(fiFirst.completeBaseName().toStdString().c_str(),
                mergedSRS, (mergedGeomType != wkbUnknown) ? mergedGeomType : wkbUnknown, papszLCO);
            CSLDestroy(papszLCO);
            if (!poOutLayer)
            {
                file_logger->error("无法创建输出图层");
                break;
            }

            // 字段名去重（SHP 限制10字符，截断后可能重名）
            std::map<std::string, std::string> fieldNameMap; // 原名 → 输出名
            {
                std::set<std::string> usedNames;
                for (const auto& mf : mergedFields)
                {
                    std::string outName = mf.name;
                    if (strDriverName == "ESRI Shapefile" && outName.size() > 10)
                        outName = truncateUtf8(outName, 10);

                    std::string candidate = outName;
                    for (int sfx = 1; usedNames.find(candidate) != usedNames.end(); sfx++) {
                        char buf[16];
                        snprintf(buf, sizeof(buf), "_%d", sfx);
                        candidate = outName.substr(0, 10 - strlen(buf)) + buf;
                    }
                    usedNames.insert(candidate);
                    fieldNameMap[mf.name] = candidate;

                    OGRFieldDefn oField(candidate.c_str(), mf.type);
                    int width = mf.width;
                    if ((mf.type == OFTString || mf.type == OFTWideString) && width < 254)
                        width = 254;  // DBF max: 避免中文截断
                    oField.SetWidth(width);
                    oField.SetPrecision(mf.precision);
                    OGRErr err = poOutLayer->CreateField(&oField);
                    if (err != OGRERR_NONE) {
                        snprintf(szLog, sizeof(szLog), "创建字段失败: %s (输出名: %s) err=%d",
                            mf.name.c_str(), candidate.c_str(), (int)err);
                        file_logger->warn(szLog);
                    }
                }
            }
            bSHPFieldsCreated = true;
            snprintf(szLog, sizeof(szLog), "合并模式: 共 %d 个输出字段", (int)fieldNameMap.size());
            file_logger->info(szLog);

            // 逐文件拷贝数据
            for (int i = 0; i < totalFiles; i++)
            {
                if (isCanceled()) { file_logger->warn("任务被用户取消"); break; }

                GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(
                    m_vecInputFiles[i].c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                if (!poSrcDS) {
                    snprintf(szLog, sizeof(szLog), "[%d/%d] 无法打开: %s，跳过",
                        i + 1, totalFiles, m_vecInputFiles[i].c_str());
                    file_logger->warn(szLog);
                    processedFiles++;
                    continue;
                }
                OGRLayer* poSrcLayer = poSrcDS->GetLayer(0);
                if (poSrcLayer)
                {
                    OGRFeatureDefn* poTgtDefn = poOutLayer->GetLayerDefn();
                    OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
                    int nTgtFields = poTgtDefn->GetFieldCount();

                    // 构建 src→tgt 索引映射
                    std::vector<int> srcToTgt(nTgtFields);
                    snprintf(szLog, sizeof(szLog), "数据拷贝 [%d/%d] %s: 字段映射:",
                        i + 1, totalFiles, m_vecInputFiles[i].c_str());
                    file_logger->info(szLog);
                    for (int t = 0; t < nTgtFields; t++) {
                        std::string tgtName = poTgtDefn->GetFieldDefn(t)->GetNameRef();
                        // 还原原始字段名
                        std::string origName = tgtName;
                        for (auto& kv : fieldNameMap) {
                            if (kv.second == tgtName) { origName = kv.first; break; }
                        }
                        // 如果有映射配置，使用源字段名查找
                        std::string srcName = origName;
                        if (!m_fieldMappings.empty()) {
                            for (auto& fm : m_fieldMappings) {
                                if (fm.outputName == origName) { srcName = fm.sourceName; break; }
                            }
                        }
                        srcToTgt[t] = poSrcDefn->GetFieldIndex(srcName.c_str());
                        snprintf(szLog, sizeof(szLog), "  %s → %s (srcIdx=%d)",
                            srcName.c_str(), tgtName.c_str(), srcToTgt[t]);
                        file_logger->info(szLog);
                    }

                    poSrcLayer->ResetReading();
                    OGRFeature* poFeat;
                    while ((poFeat = poSrcLayer->GetNextFeature()) != nullptr)
                    {
                        OGRGeometry* poSrcGeom = poFeat->GetGeometryRef();
                        OGRGeometry* poClipped = ClipGeometry(poSrcGeom, poClipGeom);
                        if (!poClipped) { OGRFeature::DestroyFeature(poFeat); continue; }

                        OGRFeature* poTgtFeat = OGRFeature::CreateFeature(poTgtDefn);
                        poTgtFeat->SetGeometryDirectly(poClipped);
                        for (int t = 0; t < nTgtFields; t++)
                        {
                            int srcIdx = srcToTgt[t];
                            if (srcIdx < 0 || !poFeat->IsFieldSet(srcIdx)) continue;

                            switch (poTgtDefn->GetFieldDefn(t)->GetType()) {
                                case OFTString:
                                case OFTWideString:
                                    poTgtFeat->SetField(t, poFeat->GetFieldAsString(srcIdx));
                                    break;
                                case OFTInteger:
                                    poTgtFeat->SetField(t, poFeat->GetFieldAsInteger(srcIdx));
                                    break;
                                case OFTInteger64:
                                    poTgtFeat->SetField(t, poFeat->GetFieldAsInteger64(srcIdx));
                                    break;
                                case OFTReal:
                                    poTgtFeat->SetField(t, poFeat->GetFieldAsDouble(srcIdx));
                                    break;
                                default:
                                    poTgtFeat->SetField(t, poFeat->GetRawFieldRef(srcIdx));
                                    break;
                            }
                        }
                        if (poOutLayer->CreateFeature(poTgtFeat) == OGRERR_NONE)
                            totalFeaturesWritten++;
                        OGRFeature::DestroyFeature(poTgtFeat);
                        OGRFeature::DestroyFeature(poFeat);
                    }
                }
                GDALClose(poSrcDS);
                processedFiles++;
                snprintf(szLog, sizeof(szLog), "[%d/%d] %s 处理完毕",
                    i + 1, totalFiles, m_vecInputFiles[i].c_str());
                file_logger->info(szLog);
                file_logger->flush();
                setProgress(processedFiles * 100.0 / totalFiles);
            }
        }
        else
        {
            // ---- 非合并模式：遍历输入文件，建字段 + 裁剪 + 写入 ----
            for (int i = 0; i < totalFiles; i++)
            {
                if (isCanceled())
                {
                    file_logger->warn("任务被用户取消");
                    break;
                }

                string strFullPath = m_vecInputFiles[i];
                QFileInfo fiSrc(QString::fromUtf8(strFullPath.c_str()));
                string strBaseName = fiSrc.completeBaseName().toStdString();

                GDALDataset* poSrcDS = (GDALDataset*)GDALOpenEx(
                    strFullPath.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                if (!poSrcDS)
                {
                    snprintf(szLog, sizeof(szLog), "[%d/%d] 无法打开: %s，跳过",
                        i + 1, totalFiles, strBaseName.c_str());
                    file_logger->warn(szLog);
                    processedFiles++;
                    continue;
                }

                for (int iLayer = 0; iLayer < poSrcDS->GetLayerCount(); iLayer++)
                {
                    OGRLayer* poSrcLayer = poSrcDS->GetLayer(iLayer);
                    if (!poSrcLayer) continue;

                    string layerName = strBaseName;
                    if (poSrcDS->GetLayerCount() > 1)
                        layerName += "_" + string(poSrcLayer->GetName());

                    if (strDriverName == "GPKG")
                    {
                        int iExisting = -1;
                        for (int n = 0; n < poOutDS->GetLayerCount(); n++)
                        {
                            if (strcmp(poOutDS->GetLayer(n)->GetName(), layerName.c_str()) == 0)
                            {
                                iExisting = n;
                                break;
                            }
                        }
                        if (iExisting >= 0)
                            poOutDS->DeleteLayer(iExisting);
                        OGRLayer* poTgtLayer = poOutDS->CreateLayer(layerName.c_str(),
                            poSrcLayer->GetSpatialRef(), poSrcLayer->GetGeomType(), nullptr);
                        if (!poTgtLayer) continue;

                        CopyLayer(poSrcLayer, poTgtLayer, poClipGeom, totalFeaturesWritten);
                    }
                    else // SHP: 首文件建层，后续追加
                    {
                        if (!bSHPFieldsCreated)
                        {
                            char** lco = CSLSetNameValue(nullptr, "ENCODING", "UTF-8");
                            poOutLayer = poOutDS->CreateLayer(layerName.c_str(),
                                poSrcLayer->GetSpatialRef(), poSrcLayer->GetGeomType(), lco);
                            CSLDestroy(lco);
                            if (!poOutLayer) continue;
                            CopyLayer(poSrcLayer, poOutLayer, poClipGeom, totalFeaturesWritten);
                            bSHPFieldsCreated = true;
                        }
                        else if (poOutLayer)
                        {
                            CopyData(poSrcLayer, poOutLayer, poClipGeom, totalFeaturesWritten);
                        }
                    }
                }

                GDALClose(poSrcDS);
                processedFiles++;
                snprintf(szLog, sizeof(szLog), "[%d/%d] %s 处理完毕",
                    i + 1, totalFiles, strBaseName.c_str());
                file_logger->info(szLog);
                file_logger->flush();
                setProgress(processedFiles * 100.0 / totalFiles);
            }
        }

        // SHP 输出 CPG 编码文件 + QML 字段别名
        if (strDriverName == "ESRI Shapefile")
        {
            QFileInfo fiCpg(QString::fromUtf8(m_strOutputPath.c_str()));
            QString basePath = fiCpg.absolutePath() + "/" + fiCpg.completeBaseName();

            // CPG
            QFile cpgFile(basePath + ".cpg");
            if (cpgFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                cpgFile.write("UTF-8");
                cpgFile.close();
            }

            // QML: 字段别名（拼音字段 → 中文表头）
            QFile qmlFile(basePath + ".qml");
            if (qmlFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream qml(&qmlFile);
                qml.setCodec("UTF-8");
                qml << "<!DOCTYPE qgis PUBLIC 'http://mrcc.com/qgis.dtd' 'SYSTEM'>\n";
                qml << "<qgis version=\"3.22.0\" styleCategories=\"AllStyleCategories\">\n";
                qml << "  <aliases>\n";

                std::map<std::string, std::string> aliasMap;
                if (!m_fieldMappings.empty()) {
                    for (const auto& fm : m_fieldMappings) {
                        if (fm.outputName != fm.sourceName && !fm.sourceName.empty())
                            aliasMap[fm.outputName] = fm.sourceName;
                    }
                }

                if (poOutLayer) {
                    OGRFeatureDefn* poDefn = poOutLayer->GetLayerDefn();
                    for (int fi = 0; fi < poDefn->GetFieldCount(); fi++) {
                        std::string fldName = poDefn->GetFieldDefn(fi)->GetNameRef();
                        auto it = aliasMap.find(fldName);
                        if (it != aliasMap.end()) {
                            qml << "    <alias field=\"" << fldName.c_str()
                                << "\" index=\"" << fi << "\" name=\""
                                << it->second.c_str() << "\"/>\n";
                        }
                    }
                }

                qml << "  </aliases>\n";
                qml << "</qgis>\n";
                qmlFile.close();
            }
        }

        snprintf(szLog, sizeof(szLog), "裁剪完毕! 处理文件:%d 输出要素:%d",
            processedFiles, totalFeaturesWritten);
        file_logger->info(szLog);
        bOk = (totalFeaturesWritten > 0 && !isCanceled());

    } while (false);

    if (poOutDS) GDALClose(poOutDS);
    if (poClipGeom) OGRGeometryFactory::destroyGeometry(poClipGeom);
    if (mergedSRS) OGRSpatialReference::DestroySpatialReference(mergedSRS);
    file_logger->flush();
    spdlog::drop(loggerName);
    file_logger.reset();

    {
        QFile file(QString::fromStdString(strLogFileFullPath));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::string utf8Content = logStream->str();
            QByteArray gbkBytes = QString::fromUtf8(utf8Content.c_str()).toLocal8Bit();
            file.write(gbkBytes);
            file.close();
        }
    }

    mProgress = 100;
    setProgress(mProgress);
    if (isCanceled()) return false;
    return bOk;
}

// ==========================================================
// CopyLayer: 建字段 + 裁剪 + 全量拷贝要素（GPKG 或 SHP 首层）
// ==========================================================
bool SeClipMergeTask::CopyLayer(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer,
                                OGRGeometry* poClipGeom, int& nWritten)
{
    if (!poSrcLayer || !poTgtLayer) return false;

    OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
    int nSrcFields = poSrcDefn->GetFieldCount();

    std::vector<int> validSrcIdx;
    for (int i = 0; i < nSrcFields; i++)
    {
        OGRFieldDefn* poSrcField = poSrcDefn->GetFieldDefn(i);
        const char* szName = poSrcField->GetNameRef();
        if (szName != nullptr && strlen(szName) > 0)
        {
            poTgtLayer->CreateField(poSrcField);
        }
        else
        {
            OGRFieldDefn oNewField(("field_" + std::to_string(i)).c_str(), poSrcField->GetType());
            oNewField.SetWidth(poSrcField->GetWidth());
            oNewField.SetPrecision(poSrcField->GetPrecision());
            poTgtLayer->CreateField(&oNewField);
        }
        validSrcIdx.push_back(i);
    }

    poSrcLayer->ResetReading();
    OGRFeature* poFeat;
    while ((poFeat = poSrcLayer->GetNextFeature()) != nullptr)
    {
        OGRGeometry* poSrcGeom = poFeat->GetGeometryRef();
        OGRGeometry* poClipped = ClipGeometry(poSrcGeom, poClipGeom);
        if (!poClipped)
        {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }

        OGRFeatureDefn* poCLDefn = poTgtLayer->GetLayerDefn();
        OGRFeature* poTgtFeat = OGRFeature::CreateFeature(poCLDefn);
        poTgtFeat->SetGeometryDirectly(poClipped);
        for (int t = 0; t < (int)validSrcIdx.size(); t++)
        {
            int srcIdx = validSrcIdx[t];
            if (!poFeat->IsFieldSet(srcIdx)) continue;

            switch (poCLDefn->GetFieldDefn(t)->GetType()) {
                case OFTString:
                case OFTWideString:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsString(srcIdx));
                    break;
                case OFTInteger:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsInteger(srcIdx));
                    break;
                case OFTInteger64:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsInteger64(srcIdx));
                    break;
                case OFTReal:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsDouble(srcIdx));
                    break;
                default:
                    poTgtFeat->SetField(t, poFeat->GetRawFieldRef(srcIdx));
                    break;
            }
        }
        if (poTgtLayer->CreateFeature(poTgtFeat) == OGRERR_NONE)
            nWritten++;
        OGRFeature::DestroyFeature(poTgtFeat);
        OGRFeature::DestroyFeature(poFeat);
    }
    return true;
}

// ==========================================================
// CopyData: 仅拷贝要素数据（SHP 追加层），按字段名匹配
// ==========================================================
void SeClipMergeTask::CopyData(OGRLayer* poSrcLayer, OGRLayer* poTgtLayer,
                               OGRGeometry* poClipGeom, int& nWritten)
{
    if (!poSrcLayer || !poTgtLayer) return;

    OGRFeatureDefn* poSrcDefn = poSrcLayer->GetLayerDefn();
    OGRFeatureDefn* poTgtDefn = poTgtLayer->GetLayerDefn();
    int nTgtFields = poTgtDefn->GetFieldCount();

    // 构建 srcIdx → tgtIdx 映射表
    std::vector<int> srcToTgt(nTgtFields);
    for (int t = 0; t < nTgtFields; t++)
        srcToTgt[t] = poSrcDefn->GetFieldIndex(poTgtDefn->GetFieldDefn(t)->GetNameRef());

    // 检查源文件有但目标没有的字段，追加入目标
    for (int s = 0; s < poSrcDefn->GetFieldCount(); s++)
    {
        const char* szName = poSrcDefn->GetFieldDefn(s)->GetNameRef();
        if (szName && strlen(szName) > 0 && poTgtDefn->GetFieldIndex(szName) < 0)
        {
            poTgtLayer->CreateField(poSrcDefn->GetFieldDefn(s));
        }
    }
    // 重建映射（可能有新字段加入）
    nTgtFields = poTgtDefn->GetFieldCount();
    srcToTgt.resize(nTgtFields);
    for (int t = 0; t < nTgtFields; t++)
        srcToTgt[t] = poSrcDefn->GetFieldIndex(poTgtDefn->GetFieldDefn(t)->GetNameRef());

    poSrcLayer->ResetReading();
    OGRFeature* poFeat;
    while ((poFeat = poSrcLayer->GetNextFeature()) != nullptr)
    {
        OGRGeometry* poSrcGeom = poFeat->GetGeometryRef();
        OGRGeometry* poClipped = ClipGeometry(poSrcGeom, poClipGeom);
        if (!poClipped)
        {
            OGRFeature::DestroyFeature(poFeat);
            continue;
        }

        OGRFeature* poTgtFeat = OGRFeature::CreateFeature(poTgtDefn);
        poTgtFeat->SetGeometryDirectly(poClipped);
        for (int t = 0; t < nTgtFields; t++)
        {
            int srcIdx = srcToTgt[t];
            if (srcIdx < 0 || !poFeat->IsFieldSet(srcIdx)) continue;

            switch (poTgtDefn->GetFieldDefn(t)->GetType()) {
                case OFTString:
                case OFTWideString:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsString(srcIdx));
                    break;
                case OFTInteger:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsInteger(srcIdx));
                    break;
                case OFTInteger64:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsInteger64(srcIdx));
                    break;
                case OFTReal:
                    poTgtFeat->SetField(t, poFeat->GetFieldAsDouble(srcIdx));
                    break;
                default:
                    poTgtFeat->SetField(t, poFeat->GetRawFieldRef(srcIdx));
                    break;
            }
        }
        if (poTgtLayer->CreateFeature(poTgtFeat) == OGRERR_NONE)
            nWritten++;
        OGRFeature::DestroyFeature(poTgtFeat);
        OGRFeature::DestroyFeature(poFeat);
    }
}

bool SeClipMergeTask::isCanceled() { return mCanceled; }
void SeClipMergeTask::cancel()     { mCanceled = true; }
void SeClipMergeTask::finished(bool result) { emit taskFinished(result); }
