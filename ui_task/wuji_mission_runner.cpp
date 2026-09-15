#include "wuji_mission_runner.h"

#include "wuji_engine_bridge.h"

#include <qgsvectorlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsvectordataprovider.h>
#include <qgswkbtypes.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTextStream>

#include <memory>

namespace WujiMissionRunner {

// ---- 数据准备 ----

QString exportLayerShp(QgsVectorLayer* layer, const QString& dir,
                       const QString& baseName, QString* errOut)
{
    if (!layer || !layer->isValid()) {
        if (errOut) *errOut = QStringLiteral("图层无效");
        return QString();
    }
    const QString path = QDir(dir).filePath(baseName + QStringLiteral(".shp"));
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = QStringLiteral("ESRI Shapefile");
    options.fileEncoding = QStringLiteral("GBK");
    QString errorMsg;
    QgsVectorFileWriter::WriterError err = QgsVectorFileWriter::writeAsVectorFormatV3(
        layer, path, QgsCoordinateTransformContext(), options, &errorMsg);
    if (err != QgsVectorFileWriter::NoError) {
        if (errOut) *errOut = QStringLiteral("导出失败: %1").arg(errorMsg);
        return QString();
    }
    // 引擎不读 .cpg，删除避免其干扰
    QFile::remove(QDir(dir).filePath(baseName + QStringLiteral(".cpg")));
    return path;
}

namespace {

QStringList shpSidecarExts()
{
    return { QStringLiteral(".shp"), QStringLiteral(".shx"),
             QStringLiteral(".dbf"), QStringLiteral(".prj"),
             QStringLiteral(".cpg"), QStringLiteral(".qix"),
             QStringLiteral(".sbn"), QStringLiteral(".sbx") };
}

// 判断 shp 是否已是 GBK 编码：
// .cpg 声明 UTF-8 或 QGIS 探测为 UTF 系列时返回 false（需要转换）。
bool isGbkShapefile(const QString& shpPath)
{
    QFileInfo fi(shpPath);
    const QString base = fi.completeBaseName();
    const QString cpgPath = fi.absolutePath() + "/" + base + ".cpg";
    QFile cpgFile(cpgPath);
    if (cpgFile.open(QIODevice::ReadOnly)) {
        const QByteArray cpg = cpgFile.readAll().trimmed().toUpper();
        cpgFile.close();
        if (cpg == "UTF-8" || cpg == "UTF8")
            return false;
    }
    QgsVectorLayer probe(shpPath, base, QStringLiteral("ogr"));
    if (!probe.isValid() || !probe.dataProvider())
        return true; // 打不开则保持原样，交给引擎处理
    const QString detected = probe.dataProvider()->encoding().toUpper();
    if (detected.contains(QStringLiteral("UTF")) ||
        detected.contains(QStringLiteral("UNICODE")))
        return false;
    return true; // GBK / CP936 / System（中文系统默认 GBK）
}

} // namespace

QStringList stageShapefiles(const QStringList& srcPaths, const QString& dir)
{
    QStringList copied;
    for (const QString& p : srcPaths) {
        QFileInfo fi(p);
        if (!fi.exists()) continue;
        const QString base = fi.completeBaseName();
        // 复制 shp 及全部附属文件到工作目录
        for (const QString& e : shpSidecarExts()) {
            const QString s = fi.absolutePath() + "/" + base + e;
            if (QFileInfo::exists(s))
                QFile::copy(s, QDir(dir).filePath(base + e));
        }
        copied.append(QDir(dir).filePath(base + QStringLiteral(".shp")));
    }

    // GBK 校正：UTF-8 源转出 *_gbk.shp 副本
    QStringList result;
    for (const QString& p : copied) {
        if (isGbkShapefile(p)) {
            result.append(p);
            continue;
        }
        QFileInfo fi(p);
        const QString base = fi.completeBaseName();
        const QString tmpPath = QDir(dir).filePath(base + QStringLiteral("_gbk.shp"));
        // 清理残留临时文件
        const QFileInfoList stale = QDir(dir).entryInfoList(
            QStringList() << (base + QStringLiteral("_gbk.*")), QDir::Files);
        for (const QFileInfo& s : stale)
            QFile::remove(s.absoluteFilePath());

        QString errOut;
        std::unique_ptr<QgsVectorLayer> probeLayer(
            new QgsVectorLayer(p, base, QStringLiteral("ogr")));
        const QString out = exportLayerShp(
            probeLayer.get(), dir, base + QStringLiteral("_gbk"), &errOut);
        if (out.isEmpty()) {
            result.append(p); // 转换失败退回原文件
            continue;
        }
        result.append(out);
    }
    return result;
}

QString mergeLayersShp(const QList<QgsVectorLayer*>& layers, const QString& dir,
                       const QString& baseName, QString* errOut)
{
    if (layers.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("无图层");
        return QString();
    }
    if (layers.size() == 1)
        return exportLayerShp(layers.first(), dir, baseName, errOut);

    QgsVectorLayer* first = layers.first();
    QString geomSpec;
    switch (first->geometryType()) {
    case QgsWkbTypes::PointGeometry:   geomSpec = QStringLiteral("Point"); break;
    case QgsWkbTypes::LineGeometry:    geomSpec = QStringLiteral("LineString"); break;
    case QgsWkbTypes::PolygonGeometry: geomSpec = QStringLiteral("Polygon"); break;
    default:
        if (errOut) *errOut = QStringLiteral("不支持的几何类型");
        return QString();
    }
    QString spec = geomSpec;
    const QString authid = first->crs().authid();
    if (!authid.isEmpty())
        spec += QStringLiteral("?crs=") + authid;

    QgsVectorLayer memLayer(spec, baseName, QStringLiteral("memory"));
    if (!memLayer.isValid()) {
        if (errOut) *errOut = QStringLiteral("无法创建内存合并图层");
        return QString();
    }
    QList<QgsFeature> feats;
    for (QgsVectorLayer* l : layers) {
        QgsFeature f;
        QgsFeatureIterator it = l->getFeatures();
        while (it.nextFeature(f)) {
            QgsFeature nf;
            nf.setGeometry(f.geometry());
            feats.append(nf);
        }
    }
    memLayer.dataProvider()->addFeatures(feats);
    return exportLayerShp(&memLayer, dir, baseName, errOut);
}

// ---- 执行 ----

bool runMissionXml(const QString& xml, const QString& dataPath,
                   const QString& label, int processMode,
                   QStringList& executedChecks)
{
    const QString xmlPath = QDir(dataPath).filePath(QStringLiteral("_mission.xml"));
    {
        QFile f(xmlPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            executedChecks.append(QStringLiteral("%1：无法写入任务XML，本次未执行").arg(label));
            return false;
        }
        QTextStream ts(&f);
        ts.setCodec("UTF-8");
        ts << xml;
        f.close();
    }

    // 引擎以独立子进程执行（官方入口：MapBatchProcessing.exe <xml> <dataPath> true 8）。
    // 不用进程内 DoXMLFile 的原因：引擎遇到异常数据（如线图层喂给面检查）
    // 会段错误/挂死，子进程崩溃只报失败，不会带崩 QGIS。
    QString errOut;
    bool ok = false;
    const QString engineDir = WujiEngineBridge::engineDir();
    const QString exePath = QDir(engineDir).filePath(QStringLiteral("MapBatchProcessing.exe"));
    if (!QFileInfo::exists(exePath)) {
        errOut = QStringLiteral("未找到引擎程序 %1").arg(exePath);
    } else {
        QProcess proc;
        proc.setWorkingDirectory(engineDir);
        // 引擎会在任务中途读 stdin（写第一个结果层后、写第二个之前，疑似
        // "按任意键"式等待）：QProcess 默认给它一个永不关闭的空管道，引擎
        // 永远等不到输入 → 0 CPU 永久挂起（2026-08-23 实测复现：命令行
        // stdin=EOF 0.076 秒完成，stdin 挂空管道即永久挂起）。
        // 改为 NUL 设备让读取立即返回 EOF。
        proc.setStandardInputFile(QProcess::nullDevice());
        proc.start(exePath, QStringList() << xmlPath << dataPath
                   << QStringLiteral("true") << QStringLiteral("8"));
        if (!proc.waitForStarted(15000)) {
            errOut = QStringLiteral("引擎进程启动失败：%1").arg(proc.errorString());
        } else {
            // 等待期间持续读走子进程输出：Windows 匿名管道缓冲仅 4KB，
            // 引擎输出一多就会把自己憋死在写管道上（Qt 文档警告的经典死锁），
            // 此前所有"超时(300秒)"挂起大概率源于此。
            QByteArray outBuf;
            QByteArray errBuf;
            const int kTimeoutMs = 300000;
            QElapsedTimer timer;
            timer.start();
            bool finished = false;
            while (proc.state() != QProcess::NotRunning) {
                outBuf.append(proc.readAllStandardOutput());
                errBuf.append(proc.readAllStandardError());
                const int remaining = kTimeoutMs - int(timer.elapsed());
                if (remaining <= 0)
                    break;
                if (proc.waitForFinished(qMin(remaining, 50))) {
                    finished = true;
                    break;
                }
            }
            if (!finished && proc.state() != QProcess::NotRunning) {
                // 超时：强制终止，并带回控制台输出尾部定位挂起点
                proc.kill();
                const bool killed = proc.waitForFinished(5000);
                outBuf.append(proc.readAllStandardOutput());
                errBuf.append(proc.readAllStandardError());
                const QString console = QString::fromLocal8Bit(outBuf)
                    .append(QString::fromLocal8Bit(errBuf)).simplified();
                const QString tail = console.size() > 2048
                    ? console.right(2048) : console;
                errOut = killed
                    ? QStringLiteral("引擎执行超时(300秒)已强制终止")
                    : QStringLiteral("引擎执行超时(300秒)且强制终止无效");
                errOut += tail.isEmpty()
                    ? QStringLiteral("(引擎无控制台输出)")
                    : QStringLiteral("，控制台输出尾部：%1").arg(tail);
            } else {
                outBuf.append(proc.readAllStandardOutput());
                errBuf.append(proc.readAllStandardError());
                const int exitCode = proc.exitCode();
                // 引擎控制台输出为 GBK 编码
                const QString out = QString::fromLocal8Bit(outBuf);
                const QString err = QString::fromLocal8Bit(errBuf);
                const QString console = QString(out).append(err).simplified();
                if (exitCode != 0) {
                    errOut = QStringLiteral("引擎进程异常退出(代码%1)%2")
                        .arg(exitCode)
                        .arg(console.isEmpty() ? QString() : QStringLiteral("：%1").arg(console));
                } else if (!out.contains(QStringLiteral("任务执行成功")) ||
                           out.contains(QStringLiteral("任务执行失败"))) {
                    // 引擎"任务执行失败"同样是退出码 0，必须按控制台文本判定
                    errOut = console;
                } else {
                    ok = true;
                }
            }
        }
    }
    if (!ok) {
        executedChecks.append(QStringLiteral("%1执行失败%3")
            .arg(label)
            .arg(errOut.isEmpty() ? QString() : QStringLiteral("：%1").arg(errOut)));
    }
    return ok;
}

// ---- 结果读取 ----

namespace {

// 从 srcLayer 取回结果要素对应的原始要素。
// 引擎结果层的 FID 与源图层不一致时按 FID 取回会失败，此时回退用标识字段
// （ELEMID/EntityID/FormerID，纯数字不受编码影响）在源图层中查找——
// 否则错误要素会带着引擎结果层的乱码属性输出。
QgsFeature mapFeature(const QgsFeature& resFeat, QgsVectorLayer* srcLayer)
{
    if (srcLayer) {
        const QgsFeature srcFeat = srcLayer->getFeature(resFeat.id());
        if (srcFeat.isValid()) return srcFeat;
        const QStringList cand = { QStringLiteral("ELEMID"), QStringLiteral("EntityID"),
                                   QStringLiteral("FormerID") };
        const QgsFields& flds = srcLayer->fields();
        for (const QString& c : cand) {
            const int srcIdx = flds.indexOf(c);
            if (srcIdx < 0) continue;
            const int resIdx = resFeat.fields().indexOf(c);
            if (resIdx < 0) continue;
            const QString eid = resFeat.attribute(resIdx).toString().trimmed();
            if (eid.isEmpty()) continue;
            QgsFeatureIterator it = srcLayer->getFeatures(
                QgsFeatureRequest().setFilterExpression(
                    QStringLiteral("\"%1\" = '%2'").arg(c, eid)).setLimit(1));
            QgsFeature srcFeatByEid;
            if (it.nextFeature(srcFeatByEid))
                return srcFeatByEid;
        }
    }
    return resFeat;
}

} // namespace

void readResultErrors(const QString& resultShpPath, QgsVectorLayer* srcLayer,
                      const QString& label, ResultReadMode mode,
                      const QString& fieldName,
                      QList<QPair<QgsFeature, QString>>& allErrors)
{
    if (!QFileInfo::exists(resultShpPath)) return;
    QgsVectorLayer res(resultShpPath,
                       QFileInfo(resultShpPath).completeBaseName(),
                       QStringLiteral("ogr"));
    if (!res.isValid()) return;

    const int infoIdx = res.fields().indexOf(QStringLiteral("info_NM"));
    const int fieldIdx = res.fields().indexOf(fieldName);

    QgsFeature f;
    QgsFeatureIterator it = res.getFeatures();
    while (it.nextFeature(f)) {
        const QString info = infoIdx >= 0
            ? f.attribute(infoIdx).toString().trimmed() : QString();

        bool isError = false;
        switch (mode) {
        case ReadInfoField:
            // 标志字段缺失时全部要素视为错误（与旧桥接层语义一致）
            isError = (fieldIdx < 0)
                ? true
                : !f.attribute(fieldIdx).toString().trimmed().isEmpty();
            break;
        case ReadFlagInt:
            isError = (fieldIdx < 0) ? true : (f.attribute(fieldIdx).toInt() != 0);
            break;
        case ReadMatchString:
            isError = (fieldIdx < 0) ? true : (f.attribute(fieldIdx).toString() != QStringLiteral("1"));
            break;
        case ReadAll:
            isError = true;
            break;
        }
        if (!isError) continue;

        QString msg = label;
        if (!info.isEmpty())
            msg += QStringLiteral(": %1").arg(info);
        allErrors.append(qMakePair(mapFeature(f, srcLayer), msg));
    }
}

QStringList shpNamesIn(const QString& dir)
{
    QStringList names;
    const QFileInfoList list = QDir(dir).entryInfoList(
        QStringList() << QStringLiteral("*.shp"), QDir::Files, QDir::Name);
    for (const QFileInfo& fi : list)
        names.append(fi.fileName());
    return names;
}

void readNewResultShps(const QString& dir, const QStringList& beforeFiles,
                       QgsVectorLayer* srcLayer, const QString& label,
                       QList<QPair<QgsFeature, QString>>& allErrors)
{
    const QSet<QString> before = QSet<QString>(beforeFiles.begin(), beforeFiles.end());
    for (const QString& name : shpNamesIn(dir)) {
        if (before.contains(name)) continue;
        readResultErrors(QDir(dir).filePath(name), srcLayer, label,
                         ReadInfoField, QStringLiteral("info_NM"), allErrors);
    }
}

bool isFileGdbSource(const QString& path)
{
    if (path.isEmpty()) return false;
    // "xxx.gdb" 目录（或文件）：QFileInfo 会自动去掉末尾分隔符
    return QFileInfo(path).fileName().endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive);
}

} // namespace WujiMissionRunner
