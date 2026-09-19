#ifndef CLIP_EXPORT_ENGINE_H
#define CLIP_EXPORT_ENGINE_H

#include <QList>
#include <QString>
#include <functional>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsgeometry.h>

// ============================================================
// 数据裁剪导出执行引擎（从地图成果模块 map_product_clip_engine 移植）
//
// 与成果模块的区别：本引擎吃的是"数据源"（本地文件 / OGR 子图层 / PostGIS 表 /
// PG 栅格），而不是成果记录（ProductMetadata），因此不依赖 PostgisConnector、
// 不涉及 Large Object 落盘 —— GDAL 可直接读取 PG 栅格。
//
// 行为与成果模块保持一致：
//   - 矢量：逐要素与裁剪多边形求交后写出目标矢量文件（保持源坐标系与属性）；
//   - 栅格：按裁剪范围的外接矩形用 GDAL 切出 GeoTIFF（保持源坐标系与分辨率，
//           不重采样）；
//   - 多源导出：单源时直接写 explicitOutputFile；多源时在 outputDir 下按
//     数据名自动命名（矢量 <名称>.shp/.gpkg，栅格 <名称>.tif）；
//   - 合并模式（mergeLayersIntoOneFile，GPKG/GDB）：所有矢量成果作为独立图层
//     写入 explicitOutputFile 指向的同一个容器文件，图层名取数据名；栅格无法
//     写入矢量容器，仍按 outputDir 输出 <名称>.tif。
// ============================================================
namespace ClipExportEngine
{
    // 一个待裁剪的数据源
    struct Source
    {
        QString uri;              // 数据源 URI：本地路径 / "路径 layername=x" / postgres URI / "PG:db:schema.table"
        QString name;             // 显示名，同时用作输出文件名与容器内图层名
        QString provider;         // 矢量："ogr" / "postgres"；栅格不使用（走 GDAL 直读）
        bool isRaster = false;
    };

    struct Options
    {
        // 裁剪多边形（矩形范围在调用方先转成面）。所在坐标系为 clipCrs；
        // clipCrs 无效时视为与每个源数据的坐标系一致，不进行换算
        QgsGeometry clipGeom;
        QgsCoordinateReferenceSystem clipCrs;

        // 输出目录（多源导出使用）
        QString outputDir;
        // 单源导出时直接写该文件（路径带扩展名，如 .shp/.gpkg/.tif）
        QString explicitOutputFile;

        // 合并写入单一矢量容器文件（GPKG/GDB）：为 true 时所有矢量成果都写入
        // explicitOutputFile 指向的同一个文件，每个成果一个图层（图层名=数据名）
        bool mergeLayersIntoOneFile = false;

        // 矢量输出
        QString vectorDriver = QStringLiteral("ESRI Shapefile"); // "ESRI Shapefile" / "GPKG" / "OpenFileGDB"
        QString fileEncoding = QStringLiteral("UTF-8");          // "UTF-8" / "GBK" / "GB2312"

        bool overwriteExisting = true;    // 已存在同名文件时直接覆盖

        // 可选进度回调（调用方线程内同步调用）：
        //   done/total —— 已完成的源序号（0 起）/ 总数
        //   message    —— 当前阶段文字（可直接展示）
        // 返回 false 表示用户请求取消，引擎中止处理
        std::function<bool(int done, int total, const QString& message)> progress;
    };

    struct Result
    {
        QString name;       // 数据源名
        QString outPath;    // 成功后的实际输出文件
        QString note;       // 补充说明（警告等，可为空）
        QString error;      // 空表示成功
        bool ok = false;
    };

    // 按裁剪多边形执行（矩形范围 / 主区矩形均适用）
    QList<Result> clipSources(const QList<Source>& sources, const Options& opt);

    // 外包矩形 → 面几何（裁剪用）；空矩形返回空几何
    QgsGeometry polygonFromRect(const QgsRectangle& rect);

    // 从数据库栅格的多个 GDAL 连接串写法中挑出第一个能被 GDALOpen 打开的。
    // 全部打不开时返回第一个候选（交给后续流程报出具体错误）；候选为空返回空串
    QString firstOpenableRasterUri(const QStringList& candidates);
}

#endif // CLIP_EXPORT_ENGINE_H
