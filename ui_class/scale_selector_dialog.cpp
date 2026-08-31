#include "scale_selector_dialog.h"
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QMessageBox>
#include <QLabel>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QFileDialog>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QDirIterator>
#include <algorithm>

#include "ui_fit_helper.h"

ScaleSelectorDialog::ScaleSelectorDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);
    DialogFitHelper::install(this);
    this->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

    setCustomRowVisible(false);

    connect(ui.pushButton_save,    &QPushButton::clicked, this, &ScaleSelectorDialog::onSave);
    connect(ui.pushButton_saveAs,  &QPushButton::clicked, this, &ScaleSelectorDialog::onSaveAs);
    connect(ui.comboBox_presetScales, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScaleSelectorDialog::onPresetChanged);
    connect(ui.spinBox_customScale, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ScaleSelectorDialog::onCustomScaleChanged);
}

ScaleSelectorDialog::~ScaleSelectorDialog()
{
}

int ScaleSelectorDialog::selectedScale() const
{
    return ui.spinBox_customScale->value();
}

void ScaleSelectorDialog::setXmlFilePath(const QString& path)
{
    m_xmlFilePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void ScaleSelectorDialog::setScale(int scale)
{
    QString target = QString("1:%1").arg(scale);
    int idx = ui.comboBox_presetScales->findText(target);
    if (idx < 0) {
        static const int scales_wan[]  = {10000, 25000, 50000, 100000, 250000, 500000, 1000000};
        static const char* texts_wan[] = {"1:1万", "1:2.5万", "1:5万", "1:10万", "1:25万", "1:50万", "1:100万"};
        for (int i = 0; i < 7; ++i) {
            if (scale == scales_wan[i]) {
                idx = ui.comboBox_presetScales->findText(QString::fromUtf8(texts_wan[i]));
                break;
            }
        }
    }

    if (idx >= 0) {
        ui.spinBox_customScale->blockSignals(true);
        ui.spinBox_customScale->setValue(scale);
        ui.spinBox_customScale->blockSignals(false);
        ui.comboBox_presetScales->setCurrentIndex(idx);
    } else {
        ui.spinBox_customScale->setValue(scale);
    }
}

void ScaleSelectorDialog::setCustomRowVisible(bool visible)
{
    ui.label_prefix->setVisible(visible);
    ui.spinBox_customScale->setVisible(visible);
}

void ScaleSelectorDialog::onPresetChanged(int index)
{
    if (index < 0) return;

    int lastIdx = ui.comboBox_presetScales->count() - 1;

    if (index == lastIdx) {
        setCustomRowVisible(true);
        ui.spinBox_customScale->setFocus();
        return;
    }

    setCustomRowVisible(false);

    int scale = parsePresetScale(ui.comboBox_presetScales->itemText(index));
    if (scale > 0) {
        ui.spinBox_customScale->blockSignals(true);
        ui.spinBox_customScale->setValue(scale);
        ui.spinBox_customScale->blockSignals(false);
    }
}

int ScaleSelectorDialog::parsePresetScale(const QString& text)
{
    QString s = text.trimmed();
    if (!s.startsWith("1:")) return 0;

    QString numPart = s.mid(2);

    if (numPart.endsWith(QString::fromUtf8("万"))) {
        numPart.chop(1);
        double val = numPart.toDouble();
        return static_cast<int>(val * 10000);
    }

    return numPart.toInt();
}

void ScaleSelectorDialog::onCustomScaleChanged(int value)
{
    QString customText = QString("1:%1").arg(value);
    int lastIdx = ui.comboBox_presetScales->count() - 1;

    int existIdx = ui.comboBox_presetScales->findText(customText);
    if (existIdx < 0) {
        static const int scales_wan[]  = {10000, 25000, 50000, 100000, 250000, 500000, 1000000};
        static const char* texts_wan[] = {"1:1万", "1:2.5万", "1:5万", "1:10万", "1:25万", "1:50万", "1:100万"};
        for (int i = 0; i < 7; ++i) {
            if (value == scales_wan[i]) {
                existIdx = ui.comboBox_presetScales->findText(QString::fromUtf8(texts_wan[i]));
                break;
            }
        }
    }

    ui.comboBox_presetScales->blockSignals(true);

    if (existIdx >= 0 && existIdx != lastIdx) {
        if (existIdx >= 11) m_customItemIndex = existIdx;
        ui.comboBox_presetScales->setCurrentIndex(existIdx);
    } else if (m_customItemIndex >= 0 && m_customItemIndex < lastIdx) {
        ui.comboBox_presetScales->setItemText(m_customItemIndex, customText);
        ui.comboBox_presetScales->setCurrentIndex(m_customItemIndex);
    } else {
        ui.comboBox_presetScales->insertItem(lastIdx, customText);
        m_customItemIndex = lastIdx;
        ui.comboBox_presetScales->setCurrentIndex(m_customItemIndex);
    }

    ui.comboBox_presetScales->blockSignals(false);
}

// ========== XML 处理 ==========

QStringList ScaleSelectorDialog::collectXmlFiles(const QString& mainXmlPath,
                                                  QVector<LinkEntry>& outLinks) const
{
    QStringList files;
    files << mainXmlPath;

    QFile f(mainXmlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return files;

    QXmlStreamReader xml(&f);
    bool isLinkContainer = false;

    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QStringLiteral("MapGeneBatchProcessingLink")) {
                isLinkContainer = true;
            }
            else if (xml.name() == QStringLiteral("Link") && isLinkContainer) {
                LinkEntry e;
                e.run  = (xml.attributes().value("run").toString() == QStringLiteral("true"));
                e.path = xml.readElementText().trimmed();
                if (!e.path.isEmpty())
                    outLinks.append(e);
            }
        }
    }
    f.close();

    QString baseDir = QFileInfo(mainXmlPath).absolutePath();
    for (const auto& link : outLinks) {
        files << QDir::cleanPath(baseDir + "/" + link.path);
    }

    return files;
}

QString ScaleSelectorDialog::modifyScaleInMemory(const QString& content, int newScale)
{
    // 匹配 <Scale note="比例尺">数字</Scale>
    // \1=开标签前半, \2=开标签后半, \3=数字, \4=闭标签
    static QRegularExpression re(
        QStringLiteral(R"((<Scale\s+note\s*=\s*["'])比例尺([\"']\s*>)\s*(\d+)\s*(</Scale>))"));

    struct Match { int start; int len; QString openTag; QString closeTag; };
    QVector<Match> matches;

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        auto m = it.next();
        int val = m.captured(3).toInt();
        if (val > 0) {
            Match mt;
            mt.start   = m.capturedStart();
            mt.len     = m.capturedLength();
            mt.openTag  = m.captured(1) + QString::fromUtf8("比例尺") + m.captured(2);
            mt.closeTag = m.captured(4);
            matches.append(mt);
        }
    }

    // 从右往左替换，确保位置偏移不影响前面的匹配
    QString result = content;
    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto& m = matches[i];
        QString replacement = m.openTag + QString::number(newScale) + m.closeTag;
        result.replace(m.start, m.len, replacement);
    }

    return result;
}

bool ScaleSelectorDialog::modifyScaleInFile(const QString& filePath, int newScale)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    QByteArray raw = f.readAll();
    f.close();

    QString content = QString::fromUtf8(raw);
    QString modified = modifyScaleInMemory(content, newScale);

    // 未修改则跳过写入
    if (modified == content)
        return true;

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile out(filePath);
    if (!out.open(QIODevice::WriteOnly))
        return false;

    qint64 written = out.write(modified.toUtf8());
    out.close();
    return (written > 0);
}

// ========== 目录递归拷贝 ==========

bool ScaleSelectorDialog::copyDirectoryRecursive(const QString& srcDir, const QString& dstDir)
{
    QDir src(srcDir);
    if (!src.exists()) return false;

    if (!QDir().mkpath(dstDir)) return false;

    QStringList entries = src.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        QString srcPath = srcDir + "/" + entry;
        QString dstPath = dstDir + "/" + entry;

        QFileInfo fi(srcPath);
        if (fi.isDir()) {
            if (!copyDirectoryRecursive(srcPath, dstPath))
                return false;
        } else {
            // QFile::copy 不会覆盖已存在的文件；目标目录是新选的，不会存在同名文件
            if (!QFile::copy(srcPath, dstPath))
                return false;
        }
    }
    return true;
}

// ========== 路径重命名（跟进比例尺变化） ==========

// ---- 比例尺字符串表示 ----

/// 一对替换 ({旧串, 新串})，按旧串长度降序排列以避免短串误伤长串
using ScaleReplacements = QVector<QPair<QString, QString>>;

/// 生成一个比例尺数值的所有常见字符串表示，并按长度降序
static ScaleReplacements buildScaleReplacements(int oldScale, int newScale)
{
    ScaleReplacements reps;

    // 添加一对替换
    auto add = [&](const QString& oldStr, const QString& newStr) {
        if (!oldStr.isEmpty() && oldStr != newStr)
            reps.append({oldStr, newStr});
    };

    // 1) 纯数字形式 (如 10000 → 50000)
    add(QString::number(oldScale), QString::number(newScale));

    // 2) '万 / w / W' 形式 — 新旧各用各的精度，避免旧串出现 "1.0万" 匹配不到 "1万"
    if (oldScale >= 10000) {
        double oldWan = oldScale / 10000.0;
        double newWan = newScale / 10000.0;
        int oldDec = (oldScale % 10000 == 0) ? 0 : 1;
        int newDec = (newScale % 10000 == 0) ? 0 : 1;

        for (const QString& sfx : {QString::fromUtf8("万"), QStringLiteral("w"), QStringLiteral("W")}) {
            // 旧串：使用旧比例尺自身的自然精度
            QString oldStr = QString::number(oldWan, 'f', oldDec) + sfx;
            // 新串：使用新比例尺自身的自然精度
            QString newStr = QString::number(newWan, 'f', newDec) + sfx;
            add(oldStr, newStr);
        }
    }

    // 3) '千 / k / K' 形式 — 同上
    if (oldScale >= 1000) {
        double oldQian = oldScale / 1000.0;
        double newQian = newScale / 1000.0;
        int oldDec = (oldScale % 1000 == 0) ? 0 : 1;
        int newDec = (newScale % 1000 == 0) ? 0 : 1;

        for (const QString& sfx : {QString::fromUtf8("千"), QStringLiteral("k"), QStringLiteral("K")}) {
            QString oldStr = QString::number(oldQian, 'f', oldDec) + sfx;
            QString newStr = QString::number(newQian, 'f', newDec) + sfx;
            add(oldStr, newStr);
        }
    }

    // 按旧串长度降序 → 长串先替换，避免 "1000" 误伤 "10000"
    std::sort(reps.begin(), reps.end(),
              [](const QPair<QString,QString>& a, const QPair<QString,QString>& b) {
                  return a.first.length() > b.first.length();
              });

    return reps;
}

/// 在字符串中应用所有比例尺替换（替换的是文件名/路径中的比例尺相关子串）
static QString applyScaleReplacements(const QString& str, const ScaleReplacements& reps)
{
    QString result = str;
    for (const auto& rep : reps) {
        // 仅当旧串确实存在时才替换
        if (result.contains(rep.first))
            result.replace(rep.first, rep.second);
    }
    return result;
}

/// 检查字符串是否包含任意旧比例尺表示
static bool containsAnyOldScale(const QString& str, const ScaleReplacements& reps)
{
    for (const auto& rep : reps) {
        if (str.contains(rep.first))
            return true;
    }
    return false;
}

// ---- 实现 ----

int ScaleSelectorDialog::extractScaleFromFileName(const QString& filePath)
{
    QString name = QFileInfo(filePath).completeBaseName();  // 不含后缀的文件名
    QRegularExpression re(QStringLiteral("(\\d+)"));
    auto match = re.match(name);
    if (match.hasMatch())
        return match.captured(1).toInt();
    return -1;
}

bool ScaleSelectorDialog::updateLinkPathsInXml(const QString& xmlPath, int oldScale, int newScale)
{
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    QByteArray raw = f.readAll();
    f.close();

    QString content = QString::fromUtf8(raw);
    ScaleReplacements reps = buildScaleReplacements(oldScale, newScale);
    if (reps.isEmpty())
        return true;

    // 匹配 <Link ...>path</Link>，捕获三部分: 开标签 / 路径文本 / 闭标签
    static QRegularExpression re(QStringLiteral(R"((<Link\b[^>]*>)([^<]*)(</Link>))"));

    struct LinkMatch { int start; int end; QString openTag; QString path; QString closeTag; };
    QVector<LinkMatch> matches;

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        auto m = it.next();
        QString linkPath = m.captured(2);
        if (containsAnyOldScale(linkPath, reps)) {
            LinkMatch lm;
            lm.start    = m.capturedStart();
            lm.end      = m.capturedEnd();
            lm.openTag  = m.captured(1);
            lm.path     = linkPath;
            lm.closeTag = m.captured(3);
            matches.append(lm);
        }
    }

    if (matches.isEmpty())
        return true;

    // 从右往左替换
    QString result = content;
    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto& lm = matches[i];
        QString newPath = applyScaleReplacements(lm.path, reps);
        result.replace(lm.start, lm.end - lm.start, lm.openTag + newPath + lm.closeTag);
    }

    QDir().mkpath(QFileInfo(xmlPath).absolutePath());
    QFile out(xmlPath);
    if (!out.open(QIODevice::WriteOnly))
        return false;

    out.write(result.toUtf8());
    out.close();
    return true;
}

int ScaleSelectorDialog::renameScaleRelatedPaths(const QString& rootDir, int oldScale, int newScale)
{
    ScaleReplacements reps = buildScaleReplacements(oldScale, newScale);
    if (reps.isEmpty())
        return 0;

    struct RenameEntry {
        QString oldPath;
        QString newPath;
        int     depth;
    };
    QVector<RenameEntry> entries;

    // 递归遍历，收集所有文件名/目录名中含旧比例尺表示法的条目
    QDirIterator it(rootDir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QString name = it.fileName();
        if (containsAnyOldScale(name, reps)) {
            RenameEntry e;
            e.oldPath = it.filePath();
            e.newPath = QFileInfo(e.oldPath).absolutePath() + "/"
                        + applyScaleReplacements(name, reps);
            e.depth = e.oldPath.count(QLatin1Char('/'));
            entries.append(e);
        }
    }

    // 深度从深到浅排序（文件在目录之前，子目录在父目录之前）
    std::sort(entries.begin(), entries.end(),
              [](const RenameEntry& a, const RenameEntry& b) { return a.depth > b.depth; });

    int renamed = 0;
    for (const auto& e : entries) {
        if (QFile::exists(e.newPath))
            continue;
        if (QFile::rename(e.oldPath, e.newPath))
            renamed++;
    }

    return renamed;
}

// ========== 保存 / 另存为 ==========

void ScaleSelectorDialog::onSave()
{
    if (m_xmlFilePath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("保存失败"),
            QString::fromUtf8("未关联 XML 配置文件。\n请先在地图综合中选择配置文件。"));
        return;
    }
    if (!QFile::exists(m_xmlFilePath)) {
        QMessageBox::warning(this, QString::fromUtf8("保存失败"),
            QString::fromUtf8("XML 配置文件不存在:\n%1").arg(m_xmlFilePath));
        return;
    }

    int newScale = selectedScale();

    // ===== 1. 修改 Scale 标签 =====
    QVector<LinkEntry> links;
    QStringList allFiles = collectXmlFiles(m_xmlFilePath, links);

    int success = 0;
    QStringList failedFiles;

    for (const QString& fp : allFiles) {
        if (QFile::exists(fp)) {
            if (modifyScaleInFile(fp, newScale))
                success++;
            else
                failedFiles << fp;
        }
    }

    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("部分失败"),
            QString::fromUtf8("Scale 修改 — 成功: %1 / 失败: %2\n\n失败文件:\n%3")
                .arg(success).arg(failedFiles.size())
                .arg(failedFiles.join("\n")));
        return;
    }

    // ===== 2. 跟进比例尺：更新 Link 路径 + 重命名文件/目录 =====
    int oldScale = extractScaleFromFileName(m_xmlFilePath);
    int renamedCount = 0;

    if (oldScale > 0 && oldScale != newScale) {
        // 2a. 更新主 XML 中的 Link 路径
        updateLinkPathsInXml(m_xmlFilePath, oldScale, newScale);

        // 2b. 重命名含旧比例尺数字的文件和目录
        QString rootDir = QFileInfo(m_xmlFilePath).absolutePath();
        renamedCount = renameScaleRelatedPaths(rootDir, oldScale, newScale);
    }

    QMessageBox::information(this, QString::fromUtf8("保存成功"),
        QString::fromUtf8("已将缩编比例尺修改为：1:%1\n\n"
                          "Scale 标签更新: %2 个文件\n"
                          "路径重命名: %3 个条目")
            .arg(newScale).arg(success).arg(renamedCount));
    accept();
}

void ScaleSelectorDialog::onSaveAs()
{
    if (m_xmlFilePath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("另存失败"),
            QString::fromUtf8("未关联 XML 配置文件。\n请先在地图综合中选择配置文件。"));
        return;
    }

    QString srcDir = QFileInfo(m_xmlFilePath).absolutePath();
    if (!QDir(srcDir).exists()) {
        QMessageBox::warning(this, QString::fromUtf8("另存失败"),
            QString::fromUtf8("源目录不存在:\n%1").arg(srcDir));
        return;
    }

    // 1. 选择目标目录
    QString targetDir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择另存目录"), QDir::homePath());
    if (targetDir.isEmpty()) return;

    // 2. 拷贝源目录内容到目标目录（不额外创建源目录名的子文件夹）
    QString dstSubDir = targetDir;

    // 3. 递归拷贝整个目录树
    if (!copyDirectoryRecursive(srcDir, dstSubDir)) {
        QMessageBox::warning(this, QString::fromUtf8("另存失败"),
            QString::fromUtf8("拷贝目录失败！\n\n源: %1\n目标: %2").arg(srcDir).arg(dstSubDir));
        return;
    }

    // 4. 在拷贝后的目录中执行比例尺修改
    QString copiedXmlPath = dstSubDir + "/" + QFileInfo(m_xmlFilePath).fileName();

    int newScale = selectedScale();
    QVector<LinkEntry> links;
    QStringList allFiles = collectXmlFiles(copiedXmlPath, links);

    int success = 0;
    QStringList failedFiles;

    for (const QString& fp : allFiles) {
        if (QFile::exists(fp)) {
            if (modifyScaleInFile(fp, newScale))
                success++;
            else
                failedFiles << fp;
        }
    }

    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("部分失败"),
            QString::fromUtf8("Scale 修改 — 成功: %1 / 失败: %2\n\n失败文件:\n%3")
                .arg(success).arg(failedFiles.size())
                .arg(failedFiles.join("\n")));
        return;
    }

    // ===== 5. 跟进比例尺：更新 Link 路径 + 重命名文件/目录 =====
    int oldScale = extractScaleFromFileName(copiedXmlPath);
    int renamedCount = 0;

    if (oldScale > 0 && oldScale != newScale) {
        updateLinkPathsInXml(copiedXmlPath, oldScale, newScale);
        renamedCount = renameScaleRelatedPaths(dstSubDir, oldScale, newScale);
    }

    QMessageBox::information(this, QString::fromUtf8("另存成功"),
        QString::fromUtf8("另存成功！\n\n"
                          "已将缩编比例尺修改为：1:%1\n"
                          "Scale 标签更新: %2 个文件\n"
                          "路径重命名: %3 个条目\n"
                          "另存路径：%4")
            .arg(newScale).arg(success).arg(renamedCount).arg(dstSubDir));
    accept();
}
