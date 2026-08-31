#ifndef SE_NMO_SDK_BRIDGE_H
#define SE_NMO_SDK_BRIDGE_H

#include <QString>
#include <QStringList>

namespace SeNmoSdkBridge {

// Call Nmo::MapBatchProcessing::FunctionsProcessing::DoXMLFile() directly.
// xmlPath: absolute path to the mission XML file
// dataPath: base directory for resolving relative paths in the XML
// Returns true on success.
bool executeMission(const QString& xmlPath, const QString& dataPath);

// Ensure input shapefiles have GBK-encoded DBF field names (SDK requirement).
// Creates GBK temp copies in dataPath. Returns updated path list.
// The caller should use these paths for XML generation and pass the list
// to cleanupGbkTempFiles() after SDK execution.
QStringList ensureGbkShapefiles(const QStringList& inputPaths, const QString& dataPath);

// Delete GBK temp files created by ensureGbkShapefiles.
void cleanupGbkTempFiles(const QStringList& tempPaths);

// Normalize an SDK-produced output shapefile to canonical GBK (CP936).
// The SDK's DBF writer is not deterministic: the first DoXMLFile call in a
// session may emit GBK (system ANSI) while later calls emit UTF-8. This
// detects the actual byte encoding of the .dbf (field names + string values)
// and re-encodes the file to GBK via GDAL, which sets a correct LDID and a
// matching .cpg. GBK is the one encoding both QGIS/LTZK and ArcGIS read
// correctly on a Chinese Windows system (ArcGIS has poor UTF-8 shapefile
// support and otherwise garbles UTF-8 bytes). Returns true on success.
bool normalizeOutputToGbk(const QString& shpPath);

// Merge multiple Shapefiles (same schema) into a single output.
// All inputs must share identical field definitions, SRS and geometry type.
// Uses GDAL ESRI Shapefile driver with ENCODING=GBK.
// Returns true on success.
bool mergeShapefiles(const QStringList& inputPaths, const QString& outputPath);

// Delete a Shapefile and all its sidecar files (.shp, .shx, .dbf, .prj, .cpg, etc.)
void deleteShapefile(const QString& shpPath);

// If inputs use a geographic CRS (e.g. WGS84), reproject them to a projected
// CRS (EPSG:3857) for more accurate SDK distance calculations.
// Returns updated path list. outOriginalSrsWkt receives the WKT of the original
// CRS (empty string if no projection was needed — nothing to reverse later).
QStringList ensureProjectedInputs(const QStringList& inputPaths, const QString& dataPath,
                                   QString& outOriginalSrsWkt);

// Reproject SDK output files from the projected CRS back to their original
// geographic CRS. Does nothing if originalSrsWkt is empty.
void reprojectOutputsToOriginal(const QStringList& outputPaths, const QString& originalSrsWkt);

// Delete temp files created by ensureProjectedInputs.
void cleanupProjectedTempFiles(const QStringList& tempPaths);

// Write a run log file to logDir/System_Running_<levelTag>_<funcTag>.txt.
// Content is GBK-encoded (readable in notepad). Truncates any previous file.
void writeRunLog(const QString& logDir, const QString& levelTag,
                 const QString& funcTag, const QStringList& lines);

} // namespace SeNmoSdkBridge

#endif
