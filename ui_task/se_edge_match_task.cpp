#define _HAS_STD_BYTE 0
#include "se_edge_match_task.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_geometry.h>
#include <cpl_string.h>
#include <cstdio>
#include <sstream>
#include <memory>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/ostream_sink.h"
#include "commontype/se_commondef.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QString>
#include <QStringList>
#include <map>

SeEdgeMatchTask::SeEdgeMatchTask(const QString& description,
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
                                 const string& logPath)
    : QgsTask(description)
    , m_sourcePath(sourcePath)
    , m_sourceLayer(sourceLayer)
    , m_refPath(refPath)
    , m_refLayer(refLayer)
    , m_tolerance(tolerance)
    , m_vertexMode(vertexMode)
    , m_correctionMode(correctionMode)
    , m_syncNeighbor(syncNeighbor)
    , m_matchLinksStr(matchLinks)
    , m_outputPath(outputPath)
    , m_logLevel(logLevel)
    , m_logPath(logPath)
    , m_progress(0)
    , m_canceled(false)
{
}

vector<SeEdgeMatchTask::MatchLink> SeEdgeMatchTask::parseMatchLinks(const string& serialized)
{
    vector<MatchLink> links;
    QString str = QString::fromUtf8(serialized.c_str());
    QStringList parts = str.split('|', QString::SkipEmptyParts);
    for (const QString& part : parts) {
        QStringList nums = part.split(',');
        if (nums.size() >= 6) {
            MatchLink link;
            link.srcFID = nums[0].toInt();
            link.srcVertexIdx = nums[1].toInt();
            link.tgtFID = nums[2].toInt();
            link.tgtVertexIdx = nums[3].toInt();
            link.tgtX = nums[4].toDouble();
            link.tgtY = nums[5].toDouble();
            links.push_back(link);
        }
    }
    return links;
}

bool SeEdgeMatchTask::run()
{
    GDALAllRegister();

    string strLogLevel;
    if (m_logLevel == SE_LOG_LEVEL_ERROR)      strLogLevel = "Error";
    else if (m_logLevel == SE_LOG_LEVEL_INFO)  strLogLevel = "Info";
    else if (m_logLevel == SE_LOG_LEVEL_DEBUG) strLogLevel = "Debug";

    string strLogFileFullPath = m_logPath + "/System_Running_"
        + strLogLevel + "_EdgeMatch.txt";
    auto logStream = make_shared<ostringstream>();
    auto logSink = make_shared<spdlog::sinks::ostream_sink_mt>(*logStream);
    auto file_logger = make_shared<spdlog::logger>("EdgeMatch", logSink);
    spdlog::register_logger(file_logger);
    if (m_logLevel == SE_LOG_LEVEL_ERROR)      file_logger->set_level(spdlog::level::err);
    else if (m_logLevel == SE_LOG_LEVEL_INFO)  file_logger->set_level(spdlog::level::info);
    else if (m_logLevel == SE_LOG_LEVEL_DEBUG) file_logger->set_level(spdlog::level::debug);

    file_logger->info("开始执行要素自动接边任务...");
    file_logger->flush();

    bool bOk = false;

    do {
        // Parse match links
        vector<MatchLink> links = parseMatchLinks(m_matchLinksStr);
        if (links.empty()) {
            file_logger->info("没有匹配链接，任务结束");
            bOk = true;
            break;
        }

        char szLog[1000] = {0};
        snprintf(szLog, sizeof(szLog), "共 %d 对匹配链接，容差=%.6f，校正模式=%d",
            (int)links.size(), m_tolerance, m_correctionMode);
        file_logger->info(szLog);
        file_logger->flush();

        // Open source dataset
        GDALDataset* srcDS = (GDALDataset*)GDALOpenEx(
            m_sourcePath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr);
        if (!srcDS) {
            file_logger->info("无法打开待校正文件（尝试只读模式）");
            srcDS = (GDALDataset*)GDALOpenEx(
                m_sourcePath.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        }
        if (!srcDS) {
            file_logger->error("无法打开待校正文件");
            break;
        }

        OGRLayer* srcLayer = srcDS->GetLayerByName(m_sourceLayer.c_str());
        if (!srcLayer) {
            // Try first layer
            srcLayer = srcDS->GetLayer(0);
        }
        if (!srcLayer) {
            file_logger->error("找不到待校正图层");
            GDALClose(srcDS);
            break;
        }

        // Open reference dataset (read-only)
        GDALDataset* refDS = (GDALDataset*)GDALOpenEx(
            m_refPath.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!refDS) {
            file_logger->error("无法打开基准参考文件");
            GDALClose(srcDS);
            break;
        }
        OGRLayer* refLayer = refDS->GetLayerByName(m_refLayer.c_str());
        if (!refLayer) {
            refLayer = refDS->GetLayer(0);
        }
        if (!refLayer) {
            file_logger->error("找不到基准参考图层");
            GDALClose(srcDS);
            GDALClose(refDS);
            break;
        }

        // Build reference vertex lookup: FID -> geometry
        map<int, OGRGeometry*> refGeomMap;
        refLayer->ResetReading();
        OGRFeature* refFeat;
        while ((refFeat = refLayer->GetNextFeature()) != nullptr) {
            int fid = (int)refFeat->GetFID();
            OGRGeometry* geom = refFeat->GetGeometryRef();
            if (geom)
                refGeomMap[fid] = geom->clone();
            OGRFeature::DestroyFeature(refFeat);
        }

        // Create output directory
        VSIMkdir(m_outputPath.c_str(), 0777);

        // Prepare output file (copy of source, then modify in-place)
        string srcBaseName = CPLGetBasename(m_sourcePath.c_str());
        string srcExt = CPLGetExtension(m_sourcePath.c_str());
        string outFile = m_outputPath + "/edge_matched_" + srcBaseName;
        if (!srcExt.empty()) outFile += "." + srcExt;

        // For shapefile, need to copy all sidecar files
        if (srcExt == "shp" || srcExt == "SHP") {
            string sidecars[] = {".shx", ".dbf", ".prj", ".cpg", ".qix", ".sbn", ".sbx"};
            for (const string& ext : sidecars) {
                string srcSidecar = m_sourcePath.substr(0, m_sourcePath.length() - 4) + ext;
                string tgtSidecar = outFile.substr(0, outFile.length() - 4) + ext;
                // Check if source sidecar exists
                VSIStatBufL sStat;
                if (VSIStatL(srcSidecar.c_str(), &sStat) == 0) {
                    // Copy via GDAL
                    GDALDataset* srcSideDS = (GDALDataset*)GDALOpenEx(
                        srcSidecar.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                    if (srcSideDS) {
                        GDALClose(srcSideDS);
                    }
                    // Simple file copy
                    VSILFILE* fSrc = VSIFOpenL(srcSidecar.c_str(), "rb");
                    VSILFILE* fTgt = VSIFOpenL(tgtSidecar.c_str(), "wb");
                    if (fSrc && fTgt) {
                        char buf[8192];
                        size_t n;
                        while ((n = VSIFReadL(buf, 1, sizeof(buf), fSrc)) > 0)
                            VSIFWriteL(buf, 1, n, fTgt);
                    }
                    if (fSrc) VSIFCloseL(fSrc);
                    if (fTgt) VSIFCloseL(fTgt);
                }
            }
        }

        // Open output dataset (copy from source via GDAL)
        GDALDriver* poDriver = srcDS->GetDriver();
        if (!poDriver) {
            file_logger->error("无法获取驱动");
            GDALClose(srcDS);
            GDALClose(refDS);
            break;
        }

        // For shapefile: delete existing output then copy
        if (srcExt == "shp" || srcExt == "SHP") {
            VSIUnlink(outFile.c_str());
        }

        GDALDataset* outDS = poDriver->CreateCopy(outFile.c_str(), srcDS, FALSE, nullptr, nullptr, nullptr);
        if (!outDS) {
            // Fallback: create from scratch
            snprintf(szLog, sizeof(szLog), "CreateCopy 失败，尝试直接创建: %s", outFile.c_str());
            file_logger->info(szLog);
            outDS = poDriver->Create(outFile.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        }
        if (!outDS) {
            file_logger->error("无法创建输出文件");
            GDALClose(srcDS);
            GDALClose(refDS);
            break;
        }

        OGRLayer* outLayer = outDS->GetLayer(0);
        if (!outLayer) {
            OGRSpatialReference* srs = srcLayer->GetSpatialRef();
            char** lco = nullptr;
            lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
            outLayer = outDS->CreateLayer(srcBaseName.c_str(), srs,
                srcLayer->GetGeomType(), lco);
            CSLDestroy(lco);
            if (outLayer && srcLayer->GetLayerDefn()) {
                OGRFeatureDefn* defn = srcLayer->GetLayerDefn();
                for (int f = 0; f < defn->GetFieldCount(); f++)
                    outLayer->CreateField(defn->GetFieldDefn(f), true);
            }
        }

        if (!outLayer) {
            file_logger->error("无法创建输出图层");
            GDALClose(outDS);
            GDALClose(srcDS);
            GDALClose(refDS);
            break;
        }

        // Process each link
        int snapCount = 0;
        int totalLinks = (int)links.size();

        // Group links by source FID for efficiency
        map<int, vector<MatchLink>> linksBySrcFID;
        for (const MatchLink& link : links)
            linksBySrcFID[link.srcFID].push_back(link);

        // Process features
        srcLayer->ResetReading();
        OGRFeature* srcFeat;
        int featIdx = 0;

        while ((srcFeat = srcLayer->GetNextFeature()) != nullptr) {
            if (m_canceled) break;

            int fid = (int)srcFeat->GetFID();
            auto it = linksBySrcFID.find(fid);

            if (it != linksBySrcFID.end()) {
                OGRGeometry* geom = srcFeat->GetGeometryRef();
                if (geom) {
                    const vector<MatchLink>& featLinks = it->second;

                    for (const MatchLink& link : featLinks) {
                        // Use stored target coordinates (works for both vertex and segment matches)
                        double tgtX = link.tgtX;
                        double tgtY = link.tgtY;

                        auto refIt = refGeomMap.find(link.tgtFID);
                        if (refIt == refGeomMap.end()) continue;
                        OGRGeometry* refGeom = refIt->second;

                        double midX = 0, midY = 0;
                        bool foundSrcVertex = false;

                        // Snap source vertex
                        if (wkbFlatten(geom->getGeometryType()) == wkbLineString) {
                            OGRLineString* ls = (OGRLineString*)geom;
                            if (link.srcVertexIdx < ls->getNumPoints()) {
                                double srcX = ls->getX(link.srcVertexIdx);
                                double srcY = ls->getY(link.srcVertexIdx);
                                midX = (srcX + tgtX) / 2.0;
                                midY = (srcY + tgtY) / 2.0;
                                foundSrcVertex = true;
                            }
                        } else if (wkbFlatten(geom->getGeometryType()) == wkbPolygon) {
                            OGRPolygon* poly = (OGRPolygon*)geom;
                            OGRLinearRing* ring = poly->getExteriorRing();
                            if (ring && link.srcVertexIdx < ring->getNumPoints()) {
                                double srcX = ring->getX(link.srcVertexIdx);
                                double srcY = ring->getY(link.srcVertexIdx);
                                midX = (srcX + tgtX) / 2.0;
                                midY = (srcY + tgtY) / 2.0;
                                foundSrcVertex = true;
                            }
                        }

                        if (!foundSrcVertex) continue;

                        // Apply correction
                        if (wkbFlatten(geom->getGeometryType()) == wkbLineString) {
                            OGRLineString* ls = (OGRLineString*)geom;
                            if (m_correctionMode == 0) {
                                // Only move endpoint
                                ls->setPoint(link.srcVertexIdx, midX, midY);
                            } else {
                                // Reshape: move vertex + adjust adjacent
                                ls->setPoint(link.srcVertexIdx, midX, midY);
                                if (ls->getNumPoints() >= 2) {
                                    int adj = (link.srcVertexIdx == 0) ? 1 :
                                              (link.srcVertexIdx == ls->getNumPoints() - 1) ?
                                              ls->getNumPoints() - 2 : -1;
                                    if (adj >= 0) {
                                        double ax = ls->getX(adj);
                                        double ay = ls->getY(adj);
                                        ls->setPoint(adj,
                                            (ax + midX) / 2.0,
                                            (ay + midY) / 2.0);
                                    }
                                }
                            }
                        } else if (wkbFlatten(geom->getGeometryType()) == wkbPolygon) {
                            OGRPolygon* poly = (OGRPolygon*)geom;
                            OGRLinearRing* ring = poly->getExteriorRing();
                            if (ring) {
                                int nPts = ring->getNumPoints();
                                if (m_correctionMode == 0) {
                                    ring->setPoint(link.srcVertexIdx, midX, midY);
                                    // Sync closing vertex
                                    if (link.srcVertexIdx == 0)
                                        ring->setPoint(nPts - 1, midX, midY);
                                    else if (link.srcVertexIdx == nPts - 1)
                                        ring->setPoint(0, midX, midY);
                                } else {
                                    ring->setPoint(link.srcVertexIdx, midX, midY);
                                    // Sync closing vertex
                                    if (link.srcVertexIdx == 0)
                                        ring->setPoint(nPts - 1, midX, midY);
                                    else if (link.srcVertexIdx == nPts - 1)
                                        ring->setPoint(0, midX, midY);

                                    if (nPts >= 2) {
                                        int adj = (link.srcVertexIdx == 0 || link.srcVertexIdx == nPts - 1) ? 1 : -1;
                                        if (adj >= 0) {
                                            double ax = ring->getX(adj);
                                            double ay = ring->getY(adj);
                                            ring->setPoint(adj,
                                                (ax + midX) / 2.0,
                                                (ay + midY) / 2.0);
                                        }
                                    }
                                }
                            }
                        }

                        snapCount++;

                        // Sync neighbor: also snap reference feature (vertex matches only)
                        if (m_syncNeighbor && link.tgtVertexIdx >= 0 && refGeom) {
                            if (wkbFlatten(refGeom->getGeometryType()) == wkbLineString) {
                                OGRLineString* ls = (OGRLineString*)refGeom;
                                if (link.tgtVertexIdx < ls->getNumPoints())
                                    ls->setPoint(link.tgtVertexIdx, midX, midY);
                            } else if (wkbFlatten(refGeom->getGeometryType()) == wkbPolygon) {
                                OGRPolygon* poly = (OGRPolygon*)refGeom;
                                OGRLinearRing* ring = poly->getExteriorRing();
                                if (ring && link.tgtVertexIdx < ring->getNumPoints()) {
                                    ring->setPoint(link.tgtVertexIdx, midX, midY);
                                    int nPts = ring->getNumPoints();
                                    if (link.tgtVertexIdx == 0)
                                        ring->setPoint(nPts - 1, midX, midY);
                                    else if (link.tgtVertexIdx == nPts - 1)
                                        ring->setPoint(0, midX, midY);
                                }
                            }
                        }
                    }

                    // Update feature geometry
                    srcFeat->SetGeometry(geom);
                }
            }

            // Write to output
            if (outLayer->CreateFeature(srcFeat) != OGRERR_NONE) {
                snprintf(szLog, sizeof(szLog), "写入要素 FID=%d 失败", fid);
                file_logger->warn(szLog);
            }

            OGRFeature::DestroyFeature(srcFeat);

            featIdx++;
            if (featIdx % 100 == 0) {
                GIntBig fc = srcLayer->GetFeatureCount();
                if (fc > 0) setProgress(static_cast<int>(featIdx * 50.0 / fc));
                if (m_canceled) break;
            }
        }

        // Also write reference features if sync is enabled
        if (m_syncNeighbor && !m_canceled) {
            string refBaseName = CPLGetBasename(m_refPath.c_str());
            string refExt = CPLGetExtension(m_refPath.c_str());
            string refOutFile = m_outputPath + "/edge_matched_" + refBaseName;
            if (!refExt.empty()) refOutFile += "." + refExt;

            GDALDataset* refOutDS = poDriver->CreateCopy(refOutFile.c_str(), refDS, FALSE, nullptr, nullptr, nullptr);
            if (refOutDS) {
                OGRLayer* refOutLayer = refOutDS->GetLayer(0);
                if (!refOutLayer) {
                    OGRSpatialReference* refSRS = refLayer->GetSpatialRef();
                    char** lco = nullptr;
                    lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
                    refOutLayer = refOutDS->CreateLayer(refBaseName.c_str(), refSRS,
                        refLayer->GetGeomType(), lco);
                    CSLDestroy(lco);
                    if (refOutLayer && refLayer->GetLayerDefn()) {
                        OGRFeatureDefn* defn = refLayer->GetLayerDefn();
                        for (int f = 0; f < defn->GetFieldCount(); f++)
                            refOutLayer->CreateField(defn->GetFieldDefn(f), true);
                    }
                }
                if (refOutLayer) {
                    refLayer->ResetReading();
                    while ((refFeat = refLayer->GetNextFeature()) != nullptr) {
                        int fid = (int)refFeat->GetFID();
                        auto it = refGeomMap.find(fid);
                        if (it != refGeomMap.end()) {
                            refFeat->SetGeometry(it->second);
                        }
                        refOutLayer->CreateFeature(refFeat);
                        OGRFeature::DestroyFeature(refFeat);
                    }
                }
                GDALClose(refOutDS);
            }
        }

        // Write CPG for shapefile
        if (srcExt == "shp" || srcExt == "SHP") {
            string cpgPath = outFile.substr(0, outFile.length() - 4) + ".cpg";
            VSILFILE* fp = VSIFOpenL(cpgPath.c_str(), "w");
            if (fp) {
                VSIFWriteL("UTF-8", 1, 5, fp);
                VSIFCloseL(fp);
            }
            // Also write CPG for ref output when syncNeighbor is enabled
            if (m_syncNeighbor) {
                string refExt = CPLGetExtension(m_refPath.c_str());
                if (refExt == "shp" || refExt == "SHP") {
                    string refBaseName = CPLGetBasename(m_refPath.c_str());
                    string refCpgPath = m_outputPath + "/edge_matched_" + refBaseName + ".cpg";
                    VSILFILE* fp2 = VSIFOpenL(refCpgPath.c_str(), "w");
                    if (fp2) {
                        VSIFWriteL("UTF-8", 1, 5, fp2);
                        VSIFCloseL(fp2);
                    }
                }
            }
        }

        // Cleanup
        for (auto& kv : refGeomMap)
            OGRGeometryFactory::destroyGeometry(kv.second);
        GDALClose(outDS);
        GDALClose(srcDS);
        GDALClose(refDS);

        snprintf(szLog, sizeof(szLog), "接边完毕! 匹配链接:%d  成功校正:%d",
            totalLinks, snapCount);
        file_logger->info(szLog);
        bOk = true;

    } while (false);

    file_logger->flush();
    spdlog::drop("EdgeMatch");
    file_logger.reset();

    // Write log
    {
        QFile file(QString::fromStdString(strLogFileFullPath));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            string utf8Content = logStream->str();
            QByteArray gbkBytes = QString::fromUtf8(utf8Content.c_str()).toLocal8Bit();
            file.write(gbkBytes);
            file.close();
        }
    }

    m_progress = 100;
    setProgress(m_progress);
    if (m_canceled) return false;
    return bOk;
}

bool SeEdgeMatchTask::isCanceled() { return m_canceled; }
void SeEdgeMatchTask::cancel()     { m_canceled = true; }
void SeEdgeMatchTask::finished(bool result) { emit taskFinished(result); }
