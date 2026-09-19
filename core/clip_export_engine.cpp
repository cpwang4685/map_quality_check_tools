// 数据裁剪导出执行引擎（从地图成果模块 core/map_product_clip_engine.cpp 移植）
// 只保留真正干活的 writeVectorClip / rasterCropByRect 及其辅助函数，
// 去掉成果记录（ProductMetadata）、PostGIS 连接器与 Large Object 落盘那套依赖。

/*--------------本模块---------------*/
#include "clip_export_engine.h"

/*--------------QT---------------*/
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QVector>
#include <QDebug>

/*--------------QGIS---------------*/
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgsfield.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsexception.h>
#include <qgswkbtypes.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsmessagelog.h>

/*--------------GDAL---------------*/
#include <gdal.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_string.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>

using namespace ClipExportEngine;

namespace ClipExportEngine
{
namespace
{
// 本文件的编译时间戳：写在日志与失败信息里，用于确认 QGIS 当前加载的插件 DLL
// 到底是哪一次编译的产物（QGIS 不会卸载已加载的插件 DLL，重新生成后必须
// 完全退出并重启 QGIS 才会生效）。
const QString kEngineBuildStamp = QStringLiteral(__DATE__ " " __TIME__);

const QString kLogTag = QStringLiteral("数据裁剪");

// ------------------------------------------------------------------
// 通用小工具
// ------------------------------------------------------------------
QString sanitizeBaseName(const QString& name)
{
    QString s = name;
    s.replace('\\', '_').replace('/', '_').replace(':', '_')
        .replace('*', '_').replace('?', '_').replace('"', '_')
        .replace('<', '_').replace('>', '_').replace('|', '_');
    if (s.trimmed().isEmpty())
        s = QStringLiteral("data");
    return s.trimmed();
}

QString vectorSuffixForDriver(const QString& driver)
{
    if (driver.compare(QStringLiteral("GPKG"), Qt::CaseInsensitive) == 0)
        return QStringLiteral(".gpkg");
    // 文件型地理数据库（GDAL 3.6+ 的 OpenFileGDB 支持写入）
    if (driver.compare(QStringLiteral("OpenFileGDB"), Qt::CaseInsensitive) == 0)
        return QStringLiteral(".gdb");
    return QStringLiteral(".shp");
}

// 目标矢量格式中 FID 列的名字。Shapefile / GeoJSON 等没有独立 FID 列的格式
// 返回空串。
//
// QGIS 的 OGR provider 在写要素时（qgsogrprovider.cpp 的 addFeaturePrivate，
// 其中唯一的 OGR_F_SetFID 调用），会把"与目标 FID 列同名"的那个属性值直接
// 当作 OGR FID 使用，该属性也不会再作为普通字段写出。FileGDB 的 FID 列名为
// OBJECTID、GPKG 为 fid，因此源数据里同名的 OBJECTID / fid 字段会被当成 FID。
QString targetFidColumnName(const QString& driver)
{
    if (driver.compare(QStringLiteral("OpenFileGDB"), Qt::CaseInsensitive) == 0
        || driver.compare(QStringLiteral("FileGDB"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("OBJECTID");
    if (driver.compare(QStringLiteral("GPKG"), Qt::CaseInsensitive) == 0
        || driver.compare(QStringLiteral("SQLite"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("fid");
    return QString();
}

struct TransformResult
{
    QgsGeometry geom;
    bool ok = true;
    QString err;
};

// 把几何从 fromCrs 换算到 toCrs；任一坐标系无效/相同时不换算直接返回
TransformResult transformGeom(const QgsGeometry& g,
                              const QgsCoordinateReferenceSystem& fromCrs,
                              const QgsCoordinateReferenceSystem& toCrs)
{
    TransformResult ret;
    if (g.isNull())
    {
        ret.ok = false;
        ret.err = QStringLiteral("裁剪几何无效");
        return ret;
    }
    if (!fromCrs.isValid() || !toCrs.isValid() || fromCrs == toCrs)
    {
        ret.geom = g;
        return ret;
    }
    QgsCoordinateTransform ct(fromCrs, toCrs, QgsProject::instance());
    if (!ct.isValid())
    {
        ret.ok = false;
        ret.err = QStringLiteral("坐标系换算不可用：%1 → %2")
                      .arg(fromCrs.authid(), toCrs.authid());
        return ret;
    }
    try
    {
        QgsGeometry t = g; // transform() 为就地修改，需先拷贝
        if (t.transform(ct) == Qgis::GeometryOperationResult::Success)
            ret.geom = t;
        else
        {
            ret.ok = false;
            ret.err = QStringLiteral("坐标系换算失败：%1 → %2")
                          .arg(fromCrs.authid(), toCrs.authid());
        }
    }
    catch (const QgsCsException&)
    {
        ret.ok = false;
        ret.err = QStringLiteral("坐标系换算失败：%1 → %2")
                      .arg(fromCrs.authid(), toCrs.authid());
    }
    return ret;
}

// 把矩形外包范围换算到目标坐标系，供栅格像素窗口计算
bool transformBBox(const QgsRectangle& rect,
                   const QgsCoordinateReferenceSystem& fromCrs,
                   const QgsCoordinateReferenceSystem& toCrs,
                   QgsRectangle& outRect)
{
    if (rect.isNull() || rect.isEmpty()) return false;
    if (!fromCrs.isValid() || !toCrs.isValid() || fromCrs == toCrs)
    {
        outRect = rect;
        return true;
    }
    QgsCoordinateTransform ct(fromCrs, toCrs, QgsProject::instance());
    if (!ct.isValid()) return false;
    try
    {
        outRect = ct.transformBoundingBox(rect);
        return !outRect.isNull() && !outRect.isEmpty();
    }
    catch (const QgsCsException&)
    {
        return false;
    }
}

QgsGeometry rectToPolygon(const QgsRectangle& rect)
{
    return QgsGeometry::fromWkt(rect.asWktPolygon());
}

// 目标文件路径：合并模式写单一容器文件；单源用显式路径；多源在输出目录下按数据名自动命名
bool composeOutPath(const Options& opt, const QString& sourceName,
                    bool isRaster, QString& outPath, QString& err)
{
    // ① 合并写入单一矢量容器文件（GPKG/GDB）：所有矢量成果共用同一目标文件，
    //    图层名由写出阶段按数据名区分（栅格无法写入，仍走下面的目录模式）
    if (!isRaster && opt.mergeLayersIntoOneFile && !opt.explicitOutputFile.isEmpty())
    {
        QString p = opt.explicitOutputFile;
        const QString wantSuffix = vectorSuffixForDriver(opt.vectorDriver);
        if (!p.endsWith(wantSuffix, Qt::CaseInsensitive))
        {
            QFileInfo fi(p);
            p = fi.absolutePath() + "/" + fi.completeBaseName() + wantSuffix;
        }
        outPath = p;
        return true;
    }

    // ② 单源显式输出文件（非合并模式）
    if (!opt.explicitOutputFile.isEmpty() && !opt.mergeLayersIntoOneFile)
    {
        QString p = opt.explicitOutputFile;
        if (isRaster)
        {
            // 栅格统一产出 GeoTIFF：非 .tif/.tiff 尾缀统一纠正为 .tif
            if (p.endsWith(QStringLiteral(".tiff"), Qt::CaseInsensitive))
            {
                p.chop(5);
                p += QStringLiteral(".tif");
            }
            else if (!p.endsWith(QStringLiteral(".tif"), Qt::CaseInsensitive))
            {
                p += QStringLiteral(".tif");
            }
        }
        else
        {
            // 矢量输出扩展名与驱动保持一致
            const QString wantSuffix = vectorSuffixForDriver(opt.vectorDriver);
            if (!p.endsWith(wantSuffix, Qt::CaseInsensitive))
            {
                QFileInfo fi(p);
                if (fi.suffix().isEmpty())
                    p += wantSuffix;
                else
                    p = fi.absolutePath() + "/" + fi.completeBaseName() + wantSuffix;
            }
        }
        outPath = p;
        return true;
    }

    if (opt.outputDir.isEmpty())
    {
        err = QStringLiteral("未指定输出目录");
        return false;
    }
    QDir dir(opt.outputDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        err = QStringLiteral("输出目录不可用：%1").arg(opt.outputDir);
        return false;
    }
    QString base = dir.filePath(sanitizeBaseName(sourceName));
    if (isRaster)
        outPath = base + QStringLiteral(".tif");
    else
        outPath = base + vectorSuffixForDriver(opt.vectorDriver);
    return true;
}

bool removeIfExists(const QString& path)
{
    return QFile::remove(path) || !QFile::exists(path);
}

// 删除已存在的数据集：普通文件直接删；目录型数据集（如 .gdb）递归删除
bool removeDatasetIfExists(const QString& path)
{
    if (!QFileInfo::exists(path)) return true;
    if (QFileInfo(path).isDir())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

// 读取栅格的坐标系（GDAL 直读，本地文件与 PG: 栅格一致）
QgsCoordinateReferenceSystem probeRasterCrs(const QString& file)
{
    QgsCoordinateReferenceSystem crs;
    GDALDatasetH hProbe = GDALOpen(file.toUtf8().constData(), GA_ReadOnly);
    if (hProbe)
    {
        const char* proj = GDALGetProjectionRef(hProbe);
        if (proj && proj[0])
            crs = QgsCoordinateReferenceSystem::fromWkt(QString::fromUtf8(proj));
        GDALClose(hProbe);
    }
    return crs;
}

// ------------------------------------------------------------------
// 矢量裁剪（与成果模块 map_product_clip_engine.cpp 的 writeVectorClip 一致）
// ------------------------------------------------------------------
bool writeVectorClip(QgsVectorLayer* src, const QgsGeometry& clipPoly,
                     const Options& opt, const QString& outPath,
                     bool appendLayer, const QString& layerNameOverride,
                     QString& realOutPath, QString& note, QString& err,
                     const std::function<bool(const QString&)>& report)
{
    if (!src || !src->isValid())
    {
        err = QStringLiteral("源图层无效");
        return false;
    }
    if (clipPoly.isNull() || clipPoly.isEmpty())
    {
        err = QStringLiteral("裁剪范围无效");
        return false;
    }

    // 输出文件（Shapefile 系列文件 / GPKG 由驱动追加扩展名）
    QString suffix = vectorSuffixForDriver(opt.vectorDriver);
    QString noExt = outPath;
    if (noExt.endsWith(suffix, Qt::CaseInsensitive))
        noExt.chop(suffix.size());
    else if (noExt.endsWith(QStringLiteral(".tif"), Qt::CaseInsensitive)
             || noExt.endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
        noExt = QFileInfo(noExt).path() + "/" + QFileInfo(noExt).completeBaseName();

    // 合并写入单一容器文件（GPKG/GDB）时直接使用完整目标路径，
    // 后续图层以 CreateOrOverwriteLayer 追加到同一文件
    const bool mergeIntoFile = opt.mergeLayersIntoOneFile;
    const QString datasetName = mergeIntoFile ? outPath : noExt;

    // 仅首个图层需要做"是否已存在/清理旧文件"处理，追加图层不能动容器文件
    if (!appendLayer)
    {
        if (!opt.overwriteExisting && QFile::exists(outPath))
        {
            err = QStringLiteral("输出文件已存在：%1").arg(outPath);
            return false;
        }
        if (mergeIntoFile)
        {
            // 容器文件（.gpkg 文件 / .gdb 目录）先整体清掉，保证从头写入
            removeDatasetIfExists(outPath);
        }
        // 清理可能残留的同名附属文件（shp 系列）
        else if (opt.vectorDriver.compare(QStringLiteral("GPKG"), Qt::CaseInsensitive) != 0)
        {
            const QStringList suffixes =
                { ".shp", ".shx", ".dbf", ".prj", ".cpg", ".qpj" };
            for (const QString& s : suffixes)
                removeIfExists(noExt + s);
        }
        else
        {
            removeIfExists(outPath);
        }
    }

    // 目标几何类型（统一提升为 Multi 以便容纳求交结果）
    const QgsWkbTypes::Type srcType = src->wkbType();
    const QgsWkbTypes::Type outType =
        QgsWkbTypes::multiType(QgsWkbTypes::flatType(srcType));
    const QString typeStr = QgsWkbTypes::displayString(outType);
    if (typeStr.isEmpty())
    {
        err = QStringLiteral("无法识别源图层几何类型");
        return false;
    }

    QString crsToken;
    if (src->crs().isValid())
    {
        crsToken = src->crs().authid();
        if (crsToken.isEmpty()) crsToken = src->crs().toWkt();
    }
    QString uri = typeStr;
    if (!crsToken.isEmpty())
        uri += QString("?crs=%1").arg(crsToken);

    QgsVectorLayer* mem = new QgsVectorLayer(uri,
        QFileInfo(noExt).completeBaseName(), QStringLiteral("memory"));
    if (!mem || !mem->isValid())
    {
        delete mem;
        err = QStringLiteral("创建输出图层失败");
        return false;
    }
    mem->dataProvider()->addAttributes(src->fields().toList());
    mem->updateFields();

    if (report && !report(QStringLiteral("正在读取源数据并与裁剪范围求交…")))
    {
        delete mem;
        err = QStringLiteral("已取消");
        return false;
    }

    // 内存图层声明为“目标基类 + Multi 部件”；求交结果须规整到同样式，
    // 否则提交时会被拒绝（几何图形类型与当前图层不相容）
    const QgsWkbTypes::GeometryType memGeomType = QgsWkbTypes::geometryType(outType);
    QgsFeatureIterator it = src->getFeatures();
    QgsFeature f;
    QgsFeatureList feats;
    qint64 clipped = 0;
    qint64 scanned = 0;
    qint64 skippedGeom = 0;
    // 输出要素重新编号（1 起自增）：源数据的 FID 可能超出目标格式的表示范围。
    // 例如 FileGDB 仅支持 32 位正整数 FID，而源为 GeoPackage / PostGIS 等
    // 64 位 FID 数据时，直接沿用会出现"Only 32 bit positive integers FID
    // supported by FileGDB"导致整个成果写出失败。裁剪成果为新数据集，
    // 重新编号不影响业务属性。
    QgsFeatureId outFid = 1;
    while (it.nextFeature(f))
    {
        ++scanned;
        // 周期上报：导出大图层时让界面及时刷新并可取消
        if (scanned % 2000 == 0)
        {
            if (report && !report(QStringLiteral("正在与裁剪范围求交，已处理 %1 个要素…")
                                      .arg(scanned)))
            {
                delete mem;
                err = QStringLiteral("已取消");
                return false;
            }
        }
        QgsGeometry g = f.geometry();
        if (g.isNull() || g.isEmpty()) continue;
        QgsGeometry inter = g.intersection(clipPoly);
        if (inter.isNull() || inter.isEmpty()) continue;
        // 统一转成目标基类的 Multi 部件：顺带丢弃曲线与 Z/M 维度，
        // 让单部件、带 Z/M、曲线等结果都能被图层接受
        const QgsGeometry norm = inter.convertToType(memGeomType, true);
        if (norm.isNull() || norm.isEmpty()
            || QgsWkbTypes::flatType(norm.wkbType()) != outType)
        {
            // 极端情况下仍无法规整的要素跳过，避免整个成果提交失败
            ++skippedGeom;
            continue;
        }
        f.setGeometry(norm);
        // 顺序编号仅用于排查：内存图层本身会重新分配 FID，最终写出的 FID
        // 取决于目标格式（见下方"与目标 FID 列同名的属性字段处理"）
        f.setId(outFid++);
        feats << f;
        ++clipped;
    }

    if (report && !report(QStringLiteral("求交完成，共裁剪出 %1 个要素，正在写出文件…")
                              .arg(clipped)))
    {
        delete mem;
        err = QStringLiteral("已取消");
        return false;
    }
    // ------------------------------------------------------------------
    // 与目标 FID 列同名的属性字段处理
    //
    // QGIS 写要素时，OGR 要素的 FID 取自"与目标 FID 列同名"的那个属性
    // （qgsogrprovider.cpp 的 addFeaturePrivate，其中唯一的 OGR_F_SetFID 调用），
    // 该属性也不会再作为普通字段写出。FileGDB 的 FID 列名是 OBJECTID、
    // GPKG 是 fid，因此源数据里同名的 OBJECTID / fid 字段会被当成 FID 使用。
    // 只要其中某个值不是 32 位正整数（0、负值、超出 2^31-1，或与其它要素重复），
    // OpenFileGDB 就会整层写出失败：
    //   Only 32 bit positive integers FID supported by FileGDB
    // 这里先收集全部合法值，再把非法值改成"本图层内未被占用的正整数"，
    // 既保证唯一也保证落在 32 位范围内；合法值保持原样不变。
    // ------------------------------------------------------------------
    const QString fidFieldName = targetFidColumnName(opt.vectorDriver);
    int fidAttrIdx = -1;
    if (!fidFieldName.isEmpty() && !feats.isEmpty())
    {
        const QgsFields srcFields = src->fields();
        for (int i = 0; i < srcFields.count(); ++i)
        {
            if (srcFields.at(i).name().compare(fidFieldName, Qt::CaseInsensitive) == 0)
            {
                fidAttrIdx = i;
                break;
            }
        }
    }
    if (fidAttrIdx >= 0)
    {
        // 第一遍：收集全部合法取值（用于保留原值与判重）
        QSet<qlonglong> usedFids;
        for (const QgsFeature& cf : feats)
        {
            bool ok = false;
            const qlonglong fidValue = cf.attribute(fidAttrIdx).toLongLong(&ok);
            if (ok && fidValue > 0 && fidValue <= 2147483647LL)
                usedFids.insert(fidValue);
        }
        // 第二遍：合法值只保留给第一个使用它的要素，其余（0/负值/超范围/重复）
        // 改成图层内尚未占用的正整数
        qlonglong nextFid = 1;
        qint64 invalidFids = 0;
        for (QgsFeature& cf : feats)
        {
            bool ok = false;
            const qlonglong fidValue = cf.attribute(fidAttrIdx).toLongLong(&ok);
            if (ok && fidValue > 0 && fidValue <= 2147483647LL && usedFids.remove(fidValue))
                continue;
            while (nextFid <= 2147483647LL && usedFids.contains(nextFid))
                ++nextFid;
            if (nextFid > 2147483647LL)
            {
                // 极端情况：32 位内已无可用值，置空交由驱动分配
                cf.setAttribute(fidAttrIdx, QVariant());
            }
            else
            {
                usedFids.insert(nextFid);
                cf.setAttribute(fidAttrIdx, QVariant(nextFid));
                ++nextFid;
            }
            ++invalidFids;
        }
        QgsMessageLog::logMessage(
            QStringLiteral("裁剪写出：%1 将使用字段 %2 作为 FID（该字段不作为普通字段写出）%3")
                .arg(opt.vectorDriver, fidFieldName)
                .arg(invalidFids > 0
                         ? QStringLiteral("；其中 %1 个值非法（0/负值/超 32 位/重复），"
                                          "已改为图层内未占用的正整数")
                               .arg(invalidFids)
                         : QString()),
            kLogTag,
            invalidFids > 0 ? Qgis::Warning : Qgis::Info);
    }

    if (!feats.isEmpty())
    {
        mem->startEditing();
        if (!mem->addFeatures(feats))
        {
            mem->rollBack();
            delete mem;
            err = QStringLiteral("生成裁剪要素失败");
            return false;
        }
        if (!mem->commitChanges())
        {
            const QStringList commitErrs = mem->commitErrors();
            mem->rollBack();
            delete mem;
            err = QStringLiteral("提交裁剪要素失败");
            if (!commitErrs.isEmpty())
                err += QStringLiteral("：%1").arg(commitErrs.join(QStringLiteral("；")));
            return false;
        }
    }

    // 诊断日志：记录本次写出使用的引擎构建、驱动与要素数，
    // 便于确认运行的插件 DLL 版本（QGIS 需完全重启才会加载新 DLL）
    QgsMessageLog::logMessage(
        QStringLiteral("裁剪写出［引擎构建 %1］：目标=%2，驱动=%3，要素 %4 个")
            .arg(kEngineBuildStamp, datasetName, opt.vectorDriver).arg(clipped),
        kLogTag, Qgis::Info);

    QgsVectorFileWriter::SaveVectorOptions saveOpt;
    saveOpt.driverName = opt.vectorDriver;
    // 合并模式下图层名取数据名（由调用方传入），否则沿用输出文件名
    saveOpt.layerName = layerNameOverride.isEmpty()
        ? QFileInfo(noExt).completeBaseName() : layerNameOverride;
    saveOpt.fileEncoding = opt.fileEncoding;
    // 追加图层：在已存在的容器文件中新建/覆盖同名图层；否则整体新建文件
    saveOpt.actionOnExistingFile = appendLayer
        ? QgsVectorFileWriter::CreateOrOverwriteLayer
        : QgsVectorFileWriter::CreateOrOverwriteFile;

    QString errMsg;
    QString newFilename;
    QgsVectorFileWriter::WriterError wErr = QgsVectorFileWriter::writeAsVectorFormatV3(
        mem, datasetName, mem->transformContext(), saveOpt, &errMsg, &newFilename);
    delete mem;

    if (wErr != QgsVectorFileWriter::NoError)
    {
        err = QStringLiteral("写出矢量文件失败：%1").arg(
            errMsg.isEmpty() ? QString::number(int(wErr)) : errMsg);
        // 附带引擎构建时间：若报错中看不到该标记，说明运行的仍是旧的插件 DLL
        err += QStringLiteral("［裁剪引擎构建：%1］").arg(kEngineBuildStamp);
        return false;
    }
    if (skippedGeom > 0)
        note = QStringLiteral("有 %1 个要素因几何类型无法规整而跳过").arg(skippedGeom);
    if (clipped == 0)
    {
        if (!note.isEmpty())
            note += QStringLiteral("；");
        note += QStringLiteral("裁剪范围内没有要素，已生成空文件");
    }
    realOutPath = newFilename.isEmpty() ? outPath : newFilename;
    if (report) report(QStringLiteral("写出完成"));
    return true;
}

// ------------------------------------------------------------------
// 栅格裁剪（GDAL，按范围外接矩形切出 GeoTIFF，不重采样）
// ------------------------------------------------------------------
bool rasterCropByRect(const QString& srcFile, const QString& outFile,
                      const QgsRectangle& rectInRasterCrs, QString& err)
{
    GDALDatasetH hSrc = GDALOpen(srcFile.toUtf8().constData(), GA_ReadOnly);
    if (!hSrc)
    {
        err = QStringLiteral("GDAL 无法打开源栅格：%1").arg(srcFile);
        return false;
    }
    if (GDALGetRasterCount(hSrc) < 1)
    {
        GDALClose(hSrc);
        err = QStringLiteral("源栅格没有波段");
        return false;
    }
    int nXSize = GDALGetRasterXSize(hSrc);
    int nYSize = GDALGetRasterYSize(hSrc);
    if (nXSize <= 0 || nYSize <= 0)
    {
        GDALClose(hSrc);
        err = QStringLiteral("源栅格尺寸无效");
        return false;
    }

    double gt[6] = { 0, 1, 0, 0, 0, -1 };
    if (GDALGetGeoTransform(hSrc, gt) != CE_None)
    {
        GDALClose(hSrc);
        err = QStringLiteral("源栅格没有地理变换信息，无法按范围裁剪");
        return false;
    }
    const double px = gt[1];
    const double py = gt[5];
    if (std::fabs(px) < 1e-30 || std::fabs(py) < 1e-30)
    {
        GDALClose(hSrc);
        err = QStringLiteral("源栅格像素尺寸异常");
        return false;
    }

    // 外包矩形 → 像素窗口（兼容负像素高度 gt[5]<0）
    auto xToCol = [&](double x) { return (x - gt[0]) / px; };
    auto yToRow = [&](double y) { return (y - gt[3]) / py; };
    double colA = xToCol(rectInRasterCrs.xMinimum());
    double colB = xToCol(rectInRasterCrs.xMaximum());
    double rowA = yToRow(rectInRasterCrs.yMaximum());
    double rowB = yToRow(rectInRasterCrs.yMinimum());

    int xOff = static_cast<int>(std::floor(std::min(colA, colB)));
    int yOff = static_cast<int>(std::floor(std::min(rowA, rowB)));
    int nCols = static_cast<int>(std::ceil(std::fabs(colB - colA)));
    int nRows = static_cast<int>(std::ceil(std::fabs(rowB - rowA)));
    if (nCols < 1) nCols = 1;
    if (nRows < 1) nRows = 1;

    if (xOff >= nXSize || yOff >= nYSize || xOff + nCols <= 0 || yOff + nRows <= 0)
    {
        GDALClose(hSrc);
        err = QStringLiteral("裁剪范围与源栅格没有交集");
        return false;
    }
    if (xOff < 0) { nCols += xOff; xOff = 0; }
    if (yOff < 0) { nRows += yOff; yOff = 0; }
    if (xOff + nCols > nXSize) nCols = nXSize - xOff;
    if (yOff + nRows > nYSize) nRows = nYSize - yOff;

    if (QFile::exists(outFile)) QFile::remove(outFile);

    QVector<QString> args;
    args << QStringLiteral("-of") << QStringLiteral("GTiff")
         << QStringLiteral("-srcwin")
         << QString::number(xOff) << QString::number(yOff)
         << QString::number(nCols) << QString::number(nRows);
    QVector<QByteArray> argUtf8;
    argUtf8.reserve(args.size());
    QVector<char*> argv;
    for (const QString& s : args)
    {
        argUtf8 << s.toUtf8();
        argv << argUtf8.last().data();
    }
    argv << nullptr;

    GDALTranslateOptions* trOpt = GDALTranslateOptionsNew(argv.data(), nullptr);
    GDALDatasetH hDst = GDALTranslate(outFile.toUtf8().constData(), hSrc, trOpt, nullptr);
    GDALTranslateOptionsFree(trOpt);
    GDALClose(hSrc);
    if (hDst)
    {
        GDALClose(hDst);
        return QFile::exists(outFile);
    }
    err = QStringLiteral("GDAL 按范围裁切失败");
    return false;
}

// ------------------------------------------------------------------
// 单个数据源的裁剪
// ------------------------------------------------------------------
Result clipOneSource(const Source& src, const Options& opt, bool appendToContainer,
                     const std::function<bool(const QString&)>& report)
{
    Result r;
    r.name = src.name;
    r.ok = false;

    const auto reportStep = [&report](const QString& msg) -> bool
    {
        return !report || report(msg);
    };

    if (opt.clipGeom.isNull() || opt.clipGeom.isEmpty())
    {
        r.error = QStringLiteral("裁剪范围为空");
        return r;
    }

    // ---------- 栅格 ----------
    if (src.isRaster)
    {
        QString outPath;
        QString err;
        if (!composeOutPath(opt, src.name, true, outPath, err))
        {
            r.error = err;
            return r;
        }
        if (!opt.overwriteExisting && QFile::exists(outPath))
        {
            r.error = QStringLiteral("输出文件已存在：%1").arg(outPath);
            return r;
        }

        if (!reportStep(QStringLiteral("正在按所选范围裁切栅格…")))
        {
            r.error = QStringLiteral("已取消");
            return r;
        }

        // 栅格按裁剪范围（多边形/矩形）的外接矩形裁切，保持源坐标系与分辨率
        const QgsCoordinateReferenceSystem rasterCrs = probeRasterCrs(src.uri);
        QgsRectangle targetRect;
        bool hasRect = transformBBox(opt.clipGeom.boundingBox(), opt.clipCrs,
                                     rasterCrs, targetRect);
        if (!hasRect)
        {
            // 无坐标系信息时按同一坐标直接计算
            targetRect = opt.clipGeom.boundingBox();
        }
        if (targetRect.isNull() || targetRect.isEmpty())
        {
            r.error = QStringLiteral("裁剪范围换算到栅格坐标系失败");
            return r;
        }

        if (!rasterCropByRect(src.uri, outPath, targetRect, err))
        {
            r.error = err;
            return r;
        }
        r.outPath = outPath;
        r.note = QStringLiteral("栅格按所选范围的外接矩形裁切（保持原分辨率）");
        r.ok = true;
        return r;
    }

    // ---------- 矢量 ----------
    QString outPath;
    QString err;
    if (!composeOutPath(opt, src.name, false, outPath, err))
    {
        r.error = err;
        return r;
    }

    if (!reportStep(QStringLiteral("正在加载矢量数据…")))
    {
        r.error = QStringLiteral("已取消");
        return r;
    }
    QgsVectorLayer* layer = new QgsVectorLayer(src.uri, src.name, src.provider);
    if (!layer || !layer->isValid())
    {
        delete layer;
        r.error = QStringLiteral("无法打开数据源：%1").arg(src.uri);
        return r;
    }

    // 裁剪几何换算到源图层坐标系
    TransformResult tr = transformGeom(opt.clipGeom, opt.clipCrs, layer->crs());
    if (!tr.ok)
    {
        r.error = tr.err;
        delete layer;
        return r;
    }

    QString realOut;
    QString note;
    // 合并写入单一容器文件时：图层名取数据名，追加到已建好的容器中
    const QString layerName = opt.mergeLayersIntoOneFile
        ? sanitizeBaseName(src.name) : QString();
    bool okWrite = writeVectorClip(layer, tr.geom, opt, outPath,
                                   appendToContainer, layerName,
                                   realOut, note, err, report);
    delete layer;
    if (!okWrite)
    {
        r.error = err;
        return r;
    }
    r.outPath = realOut;
    r.note = note;
    r.ok = true;
    return r;
}
} // namespace
} // namespace

// ============================================================
// 对外入口
// ============================================================
QgsGeometry ClipExportEngine::polygonFromRect(const QgsRectangle& rect)
{
    if (rect.isNull() || rect.isEmpty())
        return QgsGeometry();
    return rectToPolygon(rect);
}

QString ClipExportEngine::firstOpenableRasterUri(const QStringList& candidates)
{
    if (candidates.isEmpty())
    {
        return QString();
    }
    for (const QString& candidate : candidates)
    {
        const QString uri = candidate.trimmed();
        if (uri.isEmpty())
        {
            continue;
        }
        GDALAllRegister();
        GDALDatasetH hProbe = GDALOpen(uri.toUtf8().constData(), GA_ReadOnly);
        if (hProbe)
        {
            GDALClose(hProbe);
            return uri;
        }
    }
    // 全都打不开：返回第一个非空候选，由后续流程给出"打开失败"的具体报错
    for (const QString& candidate : candidates)
    {
        if (!candidate.trimmed().isEmpty())
        {
            return candidate.trimmed();
        }
    }
    return QString();
}

QList<ClipExportEngine::Result> ClipExportEngine::clipSources(
    const QList<Source>& sources, const Options& opt)
{
    QList<Result> results;
    const int total = sources.size();
    // 合并写入单一容器文件时，标记容器是否已由前面的矢量成果建立：
    // 首个矢量成果整体新建文件，其余以"追加图层"方式写入同一文件
    bool containerWritten = false;
    for (int i = 0; i < total; ++i)
    {
        const Source& src = sources.at(i);
        // 组装带"第 x/y 个数据：名称"前缀的阶段文字，转发给外部进度回调
        const std::function<bool(const QString&)> report =
            [&opt, &src, i, total](const QString& stage) -> bool
            {
                if (!opt.progress) return true;
                const QString msg = QStringLiteral("正在导出第 %1/%2 个数据：%3\n%4")
                    .arg(i + 1).arg(total).arg(src.name).arg(stage);
                return opt.progress(i, total, msg);
            };

        Result r = clipOneSource(src, opt, containerWritten, report);
        results << r;
        // 合并模式下，矢量成果成功写出后容器文件即已建立
        if (r.ok && opt.mergeLayersIntoOneFile && !src.isRaster)
            containerWritten = true;
        // 用户取消（中途或当前数据刚取消）：立即中止后续导出
        if (r.error == QStringLiteral("已取消"))
            break;
    }
    return results;
}
