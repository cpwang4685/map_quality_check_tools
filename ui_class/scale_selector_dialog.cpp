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
#include <QMap>
#include <algorithm>

#include "ui_fit_helper.h"
#include "knowledge_rename_dialog.h"

namespace {

/// UTF-8 BOM 的三个字节。
/// 为什么不用 QString::fromUtf8 / toUtf8 直接往返：那依赖 Qt 内部是否把 BOM 当成
/// U+FEFF 字符保留下来这一实现细节，一旦行为变了，8 个带 BOM 的子 XML 就会被写成
/// 不带 BOM 的——LTZK / NMO 侧的 XML 读取器对无 BOM 的处理不一致，这里不赌。
const QByteArray& utf8Bom()
{
    static const QByteArray bom = QByteArray::fromHex("EFBBBF");
    return bom;
}

/// pos 是否落在某个 [start, end) 区间里
bool indexInRanges(const QVector<QPair<int, int>>& ranges, int pos)
{
    for (const QPair<int, int>& r : ranges) {
        if (pos >= r.first && pos < r.second)
            return true;
    }
    return false;
}

/// 【2026-10-25】把主 XML 里 <Link> 写的相对路径拼成绝对路径。
///
/// 知识库是 Windows 下做的，<Link> 里一律是反斜杠（实测 1 万库 25 条、麒麟 5 万库
/// preknow\数据备份.xml 也都是），而 QDir::cleanPath() 只把「当前平台的原生分隔符」
/// 转成 '/'：Windows 上 '\' 是分隔符，能转；Linux 上 '\' 只是普通文件名字符，
/// 原样保留 → 拼出来的文件不存在 → 子 XML 一个都打不开。
/// 表现就是麒麟上「识别不到 XML 里的比例尺」，保存时也只改到主 XML（主 XML 里
/// 根本没有 <Scale>，于是静默无改动）。
/// 同一工程里 generalization_config_dialog.cpp（10-23 路线 B）和
/// knowledge_rename_dialog.cpp 已有同样的处理，这里补上。
QString resolveLinkPath(const QString& baseDir, const QString& link)
{
    QString relative = link.trimmed();
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (QDir::isAbsolutePath(relative))
        return QDir::cleanPath(relative);
    return QDir::cleanPath(baseDir + QLatin1Char('/') + relative);
}

} // namespace

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
    // 【2026-10-09】空路径要显式清成空。QFileInfo("").absoluteFilePath() 会返回
    // 当前工作目录这种"看着像路径"的东西，一旦漏进来，后面的 KnowledgeRenameDialog
    // 就会把当前工作目录整棵树摆出来给人改名——这个必须堵住。
    if (path.trimmed().isEmpty()) {
        m_xmlFilePath.clear();
        return;
    }
    m_xmlFilePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString ScaleSelectorDialog::xmlFilePath() const
{
    return m_xmlFilePath;
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
        files << resolveLinkPath(baseDir, link.path);
    }

    return files;
}

QStringList ScaleSelectorDialog::collectLinkPaths(const QString& mainXmlPath)
{
    QStringList paths;

    QFile f(mainXmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return paths;

    QXmlStreamReader xml(&f);
    bool inLinkContainer = false;

    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QStringLiteral("MapGeneBatchProcessingLink")) {
                inLinkContainer = true;
            }
            else if (xml.name() == QStringLiteral("Link") && inLinkContainer) {
                const QString p = xml.readElementText().trimmed();
                if (!p.isEmpty())
                    paths << p;
            }
        }
    }
    f.close();

    return paths;
}

int ScaleSelectorDialog::detectCurrentScale(const QString& mainXmlPath)
{
    // 【2026-10-13】以前只读主 XML 自己。可主 XML 通常就是一个 <Link> 容器
    // （本知识库那份只有 1.5KB），<Scale note="比例尺"> 全在被它 Link 的子 XML 里，
    // 于是永远"识别不到当前比例尺"，每次保存 / 另存都得人工填一遍。
    // 现在把主 XML **和它 Link 的每个子 XML** 一起统计，再取出现次数最多的那个值。
    // 非容器（直接格式）的知识库 Link 列表为空，退化成只读主 XML，与以前一致。
    static QRegularExpression re(
        QStringLiteral(R"((<Scale\s+note\s*=\s*["'])比例尺([\"']\s*>)\s*(\d+)\s*(</Scale>))"));

    QStringList files;
    files << QDir::cleanPath(mainXmlPath);
    const QString baseDir = QFileInfo(mainXmlPath).absolutePath();
    for (const QString& rel : collectLinkPaths(mainXmlPath)) {
        const QString p = resolveLinkPath(baseDir, rel);
        if (!files.contains(p, Qt::CaseInsensitive))
            files << p;
    }

    QMap<int, int> counts;                  // 值 -> 出现次数（跨主 XML 与各子 XML 累加）
    for (const QString& filePath : files) {
        bool hasBom = false;
        bool ok = false;
        const QString content = readXmlFile(filePath, &hasBom, &ok);
        if (!ok)
            continue;                       // 某个子 XML 缺失 / 读不了不该拖垮整次识别

        const QVector<QPair<int, int>> comments = commentRanges(content);
        QRegularExpressionMatchIterator it = re.globalMatch(content);
        while (it.hasNext()) {
            auto m = it.next();
            if (indexInRanges(comments, m.capturedStart()))
                continue;
            const int val = m.captured(3).toInt();
            if (val > 0)
                counts[val]++;
        }
    }
    if (counts.isEmpty())
        return -1;

    // 知识库主 XML 一贯以比例尺命名（10000自动综合知识库.xml），文件名里的数字
    // 又恰好是候选之一时优先信它——比"出现次数最多"更能反映这份库自己的定位。
    // 注：文件名开头就是序号时（03图层县图_…_会宁县45w_县图.xml）取到的是序号"03"，
    // 不在候选里，自然落到下面的多数票，不用特殊处理。
    const int fromName = extractScaleFromFileName(mainXmlPath);
    if (fromName > 0 && counts.contains(fromName))
        return fromName;

    // 否则取出现次数最多的；并列时取较大的值（比例尺一般不会小到碰巧并列）
    int best = -1;
    int bestCount = -1;
    for (auto it2 = counts.constBegin(); it2 != counts.constEnd(); ++it2) {
        const int val = it2.key();
        const int cnt = it2.value();
        if (cnt > bestCount || (cnt == bestCount && val > best)) {
            best = val;
            bestCount = cnt;
        }
    }
    return best;
}

QString ScaleSelectorDialog::modifyScaleInMemory(const QString& content, int oldScale, int newScale)
{
    // 匹配 <Scale note="比例尺">数字</Scale>
    // \1=开标签前半, \2=开标签后半, \3=数字, \4=闭标签
    static QRegularExpression re(
        QStringLiteral(R"((<Scale\s+note\s*=\s*["'])比例尺([\"']\s*>)\s*(\d+)\s*(</Scale>))"));

    // 【2026-10-26】注释内部的 <Scale> 不再跳过（以前跳，见 10-09 的说明）。
    // 实测这会让另存出来的知识库"看着没改干净"：45万 → 25万 时，
    // 14_静态注记生成.xml 里 3 处 450000 在注释块里（84~960 行那一整段备用流程），
    // 另存后仍是 450000，甲方核对时一律当成漏改。
    // 而且注释里的**不受"等于 oldScale"的过滤**：注释是说明文字，里面的数字只是描述，
    // 统一换成新比例尺才不会和正文对不上（也不要求用户先猜对注释里原来写的是多少，
    // 于是"已经改了一半的库"再保存一次就能补齐）。
    // 正文里的规则不变：只改等于 oldScale 的那些，防止误伤别的比例尺段。
    const QVector<QPair<int, int>> comments = commentRanges(content);

    struct Match { int start; int len; QString openTag; QString closeTag; };
    QVector<Match> matches;

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        auto m = it.next();

        const int val = m.captured(3).toInt();
        if (val <= 0)
            continue;                                   // 0 与空值表示"不按比例尺约束"（注释里也一样）
        const bool inComment = indexInRanges(comments, m.capturedStart());
        if (!inComment && oldScale > 0 && val != oldScale)
            continue;                                   // 正文只改等于"当前比例尺"的那些

        Match mt;
        mt.start    = m.capturedStart();
        mt.len      = m.capturedLength();
        mt.openTag  = m.captured(1) + QString::fromUtf8("比例尺") + m.captured(2);
        mt.closeTag = m.captured(4);
        matches.append(mt);
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

bool ScaleSelectorDialog::modifyScaleInFile(const QString& filePath, int oldScale, int newScale)
{
    bool hasBom = false;
    bool ok = false;
    const QString content = readXmlFile(filePath, &hasBom, &ok);
    if (!ok)
        return false;

    const QString modified = modifyScaleInMemory(content, oldScale, newScale);

    // 未修改则跳过写入（也就不用碰磁盘上原文件的时间戳）
    if (modified == content)
        return true;

    return writeXmlFile(filePath, modified, hasBom);
}

// ========== 文件读写（显式处理 UTF-8 BOM） ==========

QString ScaleSelectorDialog::readXmlFile(const QString& filePath, bool* hasBom, bool* ok)
{
    if (hasBom) *hasBom = false;
    if (ok)     *ok = false;

    QFile f(filePath);
    // 刻意不带 QIODevice::Text：带了会把 CRLF 转成 LF，回写时就把整份 XML 的行尾改掉了
    if (!f.open(QIODevice::ReadOnly))
        return QString();

    QByteArray raw = f.readAll();
    f.close();

    const bool bom = raw.startsWith(utf8Bom());
    if (bom)
        raw.remove(0, 3);      // 先剥 BOM 再解码，字符串里就不会残留 U+FEFF

    if (hasBom) *hasBom = bom;
    if (ok)     *ok = true;
    return QString::fromUtf8(raw);
}

bool ScaleSelectorDialog::writeXmlFile(const QString& filePath, const QString& content, bool hasBom)
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());

    QFile out(filePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    QByteArray bytes = content.toUtf8();
    if (hasBom)
        bytes.prepend(utf8Bom());

    const qint64 written = out.write(bytes);
    out.close();
    return (written == bytes.size());
}

QVector<QPair<int, int>> ScaleSelectorDialog::commentRanges(const QString& content)
{
    QVector<QPair<int, int>> ranges;

    int pos = 0;
    for (;;) {
        const int s = content.indexOf(QStringLiteral("<!--"), pos);
        if (s < 0)
            break;

        const int e = content.indexOf(QStringLiteral("-->"), s + 4);
        if (e < 0) {
            // 注释没闭合：保守起见算到结尾，宁可不改也不要把注释尾巴当正文改掉
            ranges.append(qMakePair(s, content.size()));
            break;
        }

        ranges.append(qMakePair(s, e + 3));
        pos = e + 3;
    }

    return ranges;
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

// ========== 【2026-10-09】按改名结果回写主 XML ==========

bool ScaleSelectorDialog::rewriteLinkPathsInXml(const QString& xmlPath,
                                                const KnowledgeRenameDialog& renameDlg)
{
    bool hasBom = false;
    bool ok = false;
    const QString content = readXmlFile(xmlPath, &hasBom, &ok);
    if (!ok)
        return false;

    // 匹配 <Link ...>path</Link>，捕获三部分: 开标签 / 路径文本 / 闭标签
    static QRegularExpression re(QStringLiteral(R"((<Link\b[^>]*>)([^<]*)(</Link>))"));

    struct LinkMatch { int start; int len; QString openTag; QString newPath; QString closeTag; };
    QVector<LinkMatch> matches;

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        auto m = it.next();

        const QString oldPath = m.captured(2);
        const QString newPath = renameDlg.remapLinkPath(oldPath);

        // 只看"含义变了没有"：分隔符（\ 与 /）与首尾空白的差异不算改动，
        // 免得把本来没动过的 <Link> 也重写一遍、把斜杠风格改掉。
        QString a = newPath; a.replace(QLatin1Char('\\'), QLatin1Char('/')); a = a.trimmed();
        QString b = oldPath; b.replace(QLatin1Char('\\'), QLatin1Char('/')); b = b.trimmed();
        if (a == b)
            continue;

        LinkMatch lm;
        lm.start    = m.capturedStart();
        lm.len      = m.capturedLength();
        lm.openTag  = m.captured(1);
        lm.newPath  = newPath;
        lm.closeTag = m.captured(3);
        matches.append(lm);
    }

    if (matches.isEmpty())
        return true;        // 没有需要改的

    // 从右往左替换
    QString result = content;
    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto& lm = matches[i];
        result.replace(lm.start, lm.len, lm.openTag + lm.newPath + lm.closeTag);
    }

    return writeXmlFile(xmlPath, result, hasBom);
}

bool ScaleSelectorDialog::setNoticeInXml(const QString& xmlPath, const QString& newBaseName)
{
    if (newBaseName.isEmpty())
        return true;

    bool hasBom = false;
    bool ok = false;
    const QString content = readXmlFile(xmlPath, &hasBom, &ok);
    if (!ok)
        return false;

    // 只动根元素 <MapGeneBatchProcessingLink ... notice="..."> 上的那个属性
    static QRegularExpression re(QStringLiteral(
        R"(<MapGeneBatchProcessingLink\b[^>]*\bnotice\s*=\s*(["'])([^"']*)\1)"));

    const QRegularExpressionMatch m = re.match(content);
    if (!m.hasMatch())
        return true;                    // 没有 notice 属性：不动

    const QString escaped = newBaseName.toHtmlEscaped();
    if (m.captured(2) == escaped)
        return true;                    // 已经对了，不用写盘

    QString result = content;
    result.replace(m.capturedStart(2), m.capturedLength(2), escaped);
    return writeXmlFile(xmlPath, result, hasBom);
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
//
// 【2026-10-09 改】两条流程现在是同一个样子：
//   ① 识别主 XML 的当前比例尺
//   ② 弹 KnowledgeRenameDialog 让用户定名字（点「取消」= 整个保存操作终止）
//   ③ 执行改名
//   ④ 把改名结果回写进主 XML：<Link> 路径 + 根元素 notice
//   ⑤ 按"旧值 → 新值"改所有 XML 里的 <Scale note="比例尺">
//
// 第 ③④ 步以前是"从文件名猜一个旧比例尺数字，再把整棵子树里含该数字的名字做全局
// 字符串替换"，在真实知识库上会改错（详见 scale_selector_dialog.h 里那段说明）。
// 现在改名完全由用户在对话框里决定，程序只负责执行 + 同步 <Link>。

namespace {

/// 按"识别到 / 识别不到"给出统一的提示语。返回实际用于过滤的旧比例尺。
int askOldScaleIfUnknown(QWidget* parent, int detected)
{
    if (detected > 0)
        return detected;
    QMessageBox::information(parent, QString::fromUtf8("提示"),
        QString::fromUtf8("没能从主 XML 及其 Link 的子 XML 里识别出当前比例尺。\n"
                          "请在接下来的对话框里手动填写「原比例尺」——\n"
                          "填什么值就只改哪些 <Scale>（值为 0 或空的始终不动）。"));
    return 1;   // 交给对话框：用户可以改成别的值
}

} // namespace

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

    const int newScale  = selectedScale();
    const int oldScale  = detectCurrentScale(m_xmlFilePath);
    const QString oldBase = QFileInfo(m_xmlFilePath).completeBaseName();

    // ===== 1. 改名：用户名下这棵树怎么叫就怎么叫（取消 → 整个保存终止）=====
    KnowledgeRenameDialog renameDlg(m_xmlFilePath, collectLinkPaths(m_xmlFilePath), this);
    renameDlg.setOldScale(askOldScaleIfUnknown(this, oldScale));
    renameDlg.setNewScale(newScale);
    if (renameDlg.exec() != QDialog::Accepted)
        return;

    const int useOldScale = renameDlg.oldScale();
    const QList<KnowledgeRenameDialog::RenameOp> ops = renameDlg.renameOps();
    int renamedCount = 0;

    // ===== 2. 执行改名 =====
    if (!ops.isEmpty()) {
        const QStringList errs = renameDlg.validationErrors();
        if (!errs.isEmpty()) {
            QMessageBox::warning(this, QString::fromUtf8("命名检查未通过"),
                QString::fromUtf8("下面这些名字要先改好，本次保存已中止：\n\n%1")
                    .arg(errs.join(QStringLiteral("\n"))));
            return;
        }

        const QStringList fails = KnowledgeRenameDialog::executeRenames(ops);
        if (!fails.isEmpty()) {
            QMessageBox::warning(this, QString::fromUtf8("改名失败"),
                QString::fromUtf8("以下条目没能改成，本次保存已中止：\n\n%1")
                    .arg(fails.join(QStringLiteral("\n"))));
            return;
        }
        renamedCount = ops.size();

        // 主 XML 自己、乃至它所在的整个目录，都可能被改名 —— 后面所有操作都要走
        // 新路径，否则拿老文件名去 collectXmlFiles 会一个文件都找不到。
        // 【2026-10-09 之二】改问对话框要"新绝对路径"：根目录也改名时，
        // 自己拼"老目录 + 新文件名"就指到老地方去了。
        const QString newMainXml = renameDlg.mainXmlNewAbsPath();
        if (!QFile::exists(newMainXml)) {
            // 正常走不到这里：改名失败上面已经拦下来了。真到这儿说明"算出来的新位置"
            // 和"磁盘上的实际位置"对不上——宁可中止，也不要拿老路径继续往下改 XML。
            QMessageBox::warning(this, QString::fromUtf8("保存失败"),
                QString::fromUtf8("改名后按新名字找不到主 XML，本次保存已中止。\n"
                                  "磁盘上的名字可能已经变了，请先核对：\n%1")
                    .arg(QDir::toNativeSeparators(newMainXml)));
            return;
        }
        m_xmlFilePath = newMainXml;

        // ===== 3. 把改名结果回写进主 XML =====
        rewriteLinkPathsInXml(m_xmlFilePath, renameDlg);

        const QString newBase = QFileInfo(m_xmlFilePath).completeBaseName();
        if (newBase != oldBase)
            setNoticeInXml(m_xmlFilePath, newBase);           // notice 跟着文件名走
    }

    // ===== 4. 改 Scale（主 XML + 所有 Link 子 XML）=====
    QVector<LinkEntry> links;
    const QStringList allFiles = collectXmlFiles(m_xmlFilePath, links);

    int success = 0;
    QStringList failedFiles;
    for (const QString& fp : allFiles) {
        if (!QFile::exists(fp))
            continue;
        if (modifyScaleInFile(fp, useOldScale, newScale))
            ++success;
        else
            failedFiles << fp;
    }

    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("部分失败"),
            QString::fromUtf8("Scale 修改 — 成功: %1 / 失败: %2\n\n失败文件:\n%3")
                .arg(success).arg(failedFiles.size())
                .arg(failedFiles.join(QStringLiteral("\n"))));
        return;
    }

    QMessageBox::information(this, QString::fromUtf8("保存成功"),
        QString::fromUtf8("已将缩编比例尺修改为：1:%1\n\n"
                          "原比例尺: 1:%2\n"
                          "Scale 标签更新: %3 个文件\n"
                          "重命名: %4 个条目")
            .arg(newScale).arg(useOldScale).arg(success).arg(renamedCount));
    accept();
}

void ScaleSelectorDialog::onSaveAs()
{
    if (m_xmlFilePath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("另存失败"),
            QString::fromUtf8("未关联 XML 配置文件。\n请先在地图综合中选择配置文件。"));
        return;
    }

    const QString srcDir = QFileInfo(m_xmlFilePath).absolutePath();
    if (!QDir(srcDir).exists()) {
        QMessageBox::warning(this, QString::fromUtf8("另存失败"),
            QString::fromUtf8("源目录不存在:\n%1").arg(srcDir));
        return;
    }

    // 1. 选择目标目录
    const QString targetDir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择另存目录"), QDir::homePath());
    if (targetDir.isEmpty())
        return;

    const int newScale  = selectedScale();
    const int oldScale  = detectCurrentScale(m_xmlFilePath);
    const QString oldBase = QFileInfo(m_xmlFilePath).completeBaseName();

    // 2. 改名 —— 在**源**树上预览，结果只应用到副本，源目录一个字节都不动
    KnowledgeRenameDialog renameDlg(m_xmlFilePath, collectLinkPaths(m_xmlFilePath), this);
    renameDlg.setOldScale(askOldScaleIfUnknown(this, oldScale));
    renameDlg.setNewScale(newScale);
    // 【2026-10-09 之二】副本的"根"就是用户上一步选的目标目录，名字在那边定。
    // 这里再允许改一次，等于去改用户刚指定的文件夹，属于意料之外的副作用。
    renameDlg.setRootDirNameEditable(false);
    if (renameDlg.exec() != QDialog::Accepted)
        return;

    const int useOldScale = renameDlg.oldScale();
    const QList<KnowledgeRenameDialog::RenameOp> ops = renameDlg.renameOps();

    if (!ops.isEmpty()) {
        const QStringList errs = renameDlg.validationErrors();
        if (!errs.isEmpty()) {
            QMessageBox::warning(this, QString::fromUtf8("命名检查未通过"),
                QString::fromUtf8("下面这些名字要先改好，本次另存已中止：\n\n%1")
                    .arg(errs.join(QStringLiteral("\n"))));
            return;
        }
    }

    // 3. 先按原名整棵拷贝（不额外创建源目录名的子文件夹）
    if (!copyDirectoryRecursive(srcDir, targetDir)) {
        QMessageBox::warning(this, QString::fromUtf8("另存失败"),
            QString::fromUtf8("拷贝目录失败！\n\n源: %1\n目标: %2").arg(srcDir).arg(targetDir));
        return;
    }

    // 4. 在副本里执行改名：把每条 op 的路径从"源目录前缀"换成"目标目录前缀"。
    //    op 已按深度从深到浅排好，父目录搬走时子树上的条目都已经改完了。
    //    （上面把根目录锁住了，所以每条 op 都落在源目录之下，前缀替换一定成立。）
    int renamedCount = 0;
    if (!ops.isEmpty()) {
        const QString srcPrefix = QDir::cleanPath(srcDir);
        const QString dstPrefix = QDir::cleanPath(targetDir);

        QList<KnowledgeRenameDialog::RenameOp> dstOps;
        for (const KnowledgeRenameDialog::RenameOp& op : ops) {
            KnowledgeRenameDialog::RenameOp o = op;
            o.oldAbsPath = dstPrefix + op.oldAbsPath.mid(srcPrefix.length());
            o.newAbsPath = dstPrefix + op.newAbsPath.mid(srcPrefix.length());
            dstOps.append(o);
        }

        const QStringList fails = KnowledgeRenameDialog::executeRenames(dstOps);
        if (!fails.isEmpty()) {
            QMessageBox::warning(this, QString::fromUtf8("改名失败"),
                QString::fromUtf8("副本里以下条目没能改成（源目录未改动）：\n\n%1")
                    .arg(fails.join(QStringLiteral("\n"))));
            return;
        }
        renamedCount = dstOps.size();
    }

    // 5. 副本里的主 XML 路径。根目录在对话框里被锁住不改名，所以副本的根就是
    //    targetDir，用主 XML 的新文件名拼一下即可（它一定是根目录的直接子项）。
    QString rel = renameDlg.mainXmlNewRelPath();          // `\` 分隔
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString mainXmlAbs = QDir::cleanPath(targetDir + QLatin1Char('/') + rel);
    if (!QFile::exists(mainXmlAbs)) {
        // 兜底：真没找到就退回拷贝时的原名，别把后面所有步骤带偏
        mainXmlAbs = QDir::cleanPath(
            targetDir + QLatin1Char('/') + QFileInfo(m_xmlFilePath).fileName());
    }

    // 6. 回写副本主 XML 的 <Link> 与 notice
    if (!ops.isEmpty()) {
        rewriteLinkPathsInXml(mainXmlAbs, renameDlg);
        const QString newBase = QFileInfo(mainXmlAbs).completeBaseName();
        if (newBase != oldBase)
            setNoticeInXml(mainXmlAbs, newBase);
    }

    // 7. 改副本里的 Scale（主 XML + 所有 Link 子 XML）
    QVector<LinkEntry> links;
    const QStringList allFiles = collectXmlFiles(mainXmlAbs, links);

    int success = 0;
    QStringList failedFiles;
    for (const QString& fp : allFiles) {
        if (!QFile::exists(fp))
            continue;
        if (modifyScaleInFile(fp, useOldScale, newScale))
            ++success;
        else
            failedFiles << fp;
    }

    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("部分失败"),
            QString::fromUtf8("Scale 修改 — 成功: %1 / 失败: %2\n\n失败文件:\n%3")
                .arg(success).arg(failedFiles.size())
                .arg(failedFiles.join(QStringLiteral("\n"))));
        return;
    }

    // 8. 【2026-10-09 之七】把 m_xmlFilePath 指向副本：另存为之后，当前正在编辑的就是
    //    刚存出来的这一份，调用方（地图综合对话框）据此把配置文件栏同步成新路径。
    //    改造前这里刻意不改，结果是另存出来的 XML 不会被地图综合对话框引用。
    m_xmlFilePath = mainXmlAbs;

    QMessageBox::information(this, QString::fromUtf8("另存成功"),
        QString::fromUtf8("另存成功！\n\n"
                          "已将缩编比例尺修改为：1:%1\n"
                          "原比例尺: 1:%2\n"
                          "Scale 标签更新: %3 个文件\n"
                          "重命名: %4 个条目\n"
                          "另存路径：%5")
            .arg(newScale).arg(useOldScale).arg(success).arg(renamedCount).arg(targetDir));
    accept();
}
