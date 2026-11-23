#include "knowledge_rename_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTreeView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QPushButton>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QHeaderView>
#include <algorithm>

#include "ui_fit_helper.h"

namespace {

// 节点自定义数据槽（挂在名称列那一项上）
constexpr int kRoleAbsPath = Qt::UserRole + 1;   // 原绝对路径
constexpr int kRoleOldName = Qt::UserRole + 2;   // 原名
constexpr int kRoleIsDir   = Qt::UserRole + 3;   // 是否目录
constexpr int kRoleLinked  = Qt::UserRole + 4;   // 是否被主 XML 的 <Link> 引用
constexpr int kRoleOldRel  = Qt::UserRole + 5;   // 原相对路径（`/` 分隔）

/// 归一化一条相对路径：去空白、`\`→`/`、去掉开头的 "./"、合并 "//"、转小写。
/// 用来做"路径是不是同一条"的比较（Windows 路径本来就大小写不敏感）。
QString normalizeRel(const QString& p)
{
    QString s = p.trimmed();
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (s.startsWith(QLatin1String("./")))
        s.remove(0, 2);
    s.replace(QLatin1String("//"), QLatin1String("/"));
    return s.toLower();
}

/// `\` → `/`（路径里除分隔符外不会有反斜杠）
QString toSlash(QString s) { return s.replace(QLatin1Char('\\'), QLatin1Char('/')); }

/// `/` → `\`（XML 里的路径一律用反斜杠，麒麟上也一样，所以不能靠 QDir::toNativeSeparators）
QString toBackslash(QString s) { return s.replace(QLatin1Char('/'), QLatin1Char('\\')); }

/// 目录深度（按 `/` 数）
int depthOf(const QString& path) { return path.count(QLatin1Char('/')); }

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

KnowledgeRenameDialog::KnowledgeRenameDialog(const QString& mainXmlPath,
                                             const QStringList& linkedRelPaths,
                                             QWidget* parent)
    : QDialog(parent)
{
    m_mainXmlAbs    = QDir::cleanPath(QFileInfo(mainXmlPath).absoluteFilePath());
    m_rootDir       = QDir::cleanPath(QFileInfo(mainXmlPath).absolutePath());
    m_rootDirParent = QDir::cleanPath(QFileInfo(m_rootDir).absolutePath());
    m_mainXmlName   = QFileInfo(mainXmlPath).fileName();

    // 主 XML 直接躺在盘根（D:/ 或 /）时，这个"目录"没有名字可改
    if (m_rootDirParent.isEmpty()
        || m_rootDirParent.compare(m_rootDir, Qt::CaseInsensitive) == 0) {
        m_rootEditable   = false;
        m_rootLockReason = QString::fromUtf8("系统根目录，不能改名");
    }

    for (const QString& p : linkedRelPaths) {
        const QString n = normalizeRel(p);
        if (!n.isEmpty())
            m_linkedRel << n;
    }

    setWindowTitle(QString::fromUtf8("知识库重命名"));
    this->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    resize(760, 580);
    setMinimumSize(560, 400);
    DialogFitHelper::install(this);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    // ---- 顶部：目录 + 两个比例尺 ----
    auto* infoLabel = new QLabel(
        QString::fromUtf8("知识库目录：%1").arg(QDir::toNativeSeparators(m_rootDir)), this);
    infoLabel->setWordWrap(true);
    mainLayout->addWidget(infoLabel);

    auto* scaleRow = new QHBoxLayout();
    scaleRow->addWidget(new QLabel(QString::fromUtf8("识别到原比例尺为：1:"), this));
    m_spinOldScale = new QSpinBox(this);
    m_spinOldScale->setRange(1, 100000000);
    m_spinOldScale->setSingleStep(500);
    m_spinOldScale->setValue(10000);
    m_spinOldScale->setKeyboardTracking(false);
    m_spinOldScale->setToolTip(QString::fromUtf8(
        "按所有子 XML 里 <Scale note=\"比例尺\"> 出现次数最多的那个值自动识别，\n"
        "识别错了可以在这里直接改。改比例尺时只会把等于这个值的 <Scale> 换成目标值\n"
        "（值为 0 或空的表示\"不按比例尺约束\"，保持不动）。"));
    scaleRow->addWidget(m_spinOldScale);

    m_labelTarget = new QLabel(this);
    scaleRow->addSpacing(16);
    scaleRow->addWidget(m_labelTarget);
    scaleRow->addStretch();
    mainLayout->addLayout(scaleRow);

    m_labelHint = new QLabel(
        QString::fromUtf8("双击下面的名称即可修改（文件和文件夹都可以改）——第一行就是主 XML 所在的那个文件夹。\n"
                          "标记为「被主 XML 引用」的条目改名后，主 XML 里的 <Link> 会自动同步成新路径；"
                          "标为「其它文件」的不动。"), this);
    m_labelHint->setWordWrap(true);
    mainLayout->addWidget(m_labelHint);

    // ---- 中间：目录树 ----
    m_model = new QStandardItemModel(this);
    m_tree  = new QTreeView(this);
    m_tree->setModel(m_model);
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setStretchLastSection(false);
    mainLayout->addWidget(m_tree, 1);

    // ---- 底部：按钮 ----
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto* btnCancel = new QPushButton(QString::fromUtf8("取消"), this);
    auto* btnOk     = new QPushButton(QString::fromUtf8("确认"), this);
    btnOk->setDefault(true);
    btnRow->addWidget(btnOk);
    btnRow->addWidget(btnCancel);
    mainLayout->addLayout(btnRow);

    connect(btnOk,     &QPushButton::clicked, this, &KnowledgeRenameDialog::onAccepted);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    // 名称列被编辑完（双击改完回车/点别处）后判一下有没有变化
    connect(m_model, &QStandardItemModel::itemChanged, this, &KnowledgeRenameDialog::onItemChanged);

    buildTree();
    m_tree->resizeColumnToContents(0);
}

KnowledgeRenameDialog::~KnowledgeRenameDialog() = default;

// ============================================================================
// 比例尺
// ============================================================================

int KnowledgeRenameDialog::oldScale() const { return m_spinOldScale->value(); }
void KnowledgeRenameDialog::setOldScale(int scale) { m_spinOldScale->setValue(scale); }

void KnowledgeRenameDialog::setNewScale(int scale)
{
    m_labelTarget->setText(QString::fromUtf8("目标比例尺：1:%1").arg(scale));
}

void KnowledgeRenameDialog::setRootDirNameEditable(bool editable)
{
    // 主 XML 躺在盘根时根目录本来就没有名字，谁调都不给改
    if (m_rootDirParent.isEmpty()
        || m_rootDirParent.compare(m_rootDir, Qt::CaseInsensitive) == 0)
        return;

    m_rootEditable   = editable;
    m_rootLockReason = editable
        ? QString()
        : QString::fromUtf8("名字由另存的目标目录决定");
    applyRootEditability();
}

// ============================================================================
// 建树
// ============================================================================

void KnowledgeRenameDialog::buildTree()
{
    m_loading = true;
    m_model->clear();
    m_model->setHorizontalHeaderLabels({ QString::fromUtf8("名称"), QString::fromUtf8("说明") });

    // 【2026-10-09 之二】根 = 主 XML 所在的目录**本身**（以前只列它的内容，
    // 结果这个目录自己反而改不了名）。appendEntry 会自己往下递归。
    appendEntry(nullptr, m_rootDir);
    m_loading = false;

    applyRootEditability();
    m_tree->resizeColumnToContents(0);
    m_tree->resizeColumnToContents(1);
    m_tree->expandToDepth(1);
}

void KnowledgeRenameDialog::applyRootEditability()
{
    QStandardItem* rootName = m_model->item(0, 0);
    if (!rootName)
        return;

    rootName->setEditable(m_rootEditable);

    if (QStandardItem* noteItem = m_model->item(0, 1)) {
        noteItem->setText(m_rootEditable
            ? QString::fromUtf8("主 XML 所在目录")
            : QString::fromUtf8("主 XML 所在目录（不可改名）"));
    }
    rootName->setToolTip(m_rootEditable
        ? QString::fromUtf8("双击改名后，主 XML 的路径会跟着变")
        : QString::fromUtf8("不可改名：%1").arg(m_rootLockReason));
}

void KnowledgeRenameDialog::appendEntry(QStandardItem* parentItem, const QString& absPath)
{
    const QFileInfo fi(absPath);
    const bool isDir  = fi.isDir();
    const bool isRoot = (absPath.compare(m_rootDir, Qt::CaseInsensitive) == 0);

    QString rel = absPath.mid(m_rootDir.length());
    if (rel.startsWith(QLatin1Char('/')))
        rel.remove(0, 1);
    const bool linked = m_linkedRel.contains(rel.toLower());

    auto* nameItem = new QStandardItem(fi.fileName());
    nameItem->setEditable(true);
    nameItem->setData(absPath,          kRoleAbsPath);
    nameItem->setData(fi.fileName(),    kRoleOldName);
    nameItem->setData(isDir,            kRoleIsDir);
    nameItem->setData(linked,           kRoleLinked);
    nameItem->setData(rel,              kRoleOldRel);

    QString note;
    if (isRoot)
        note = QString::fromUtf8("主 XML 所在目录");
    else if (isDir)
        note = QString::fromUtf8("目录");
    else if (fi.suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
        note = QString::fromUtf8("XML");
    else
        note = QString::fromUtf8("其它文件");
    if (linked) {
        note = QString::fromUtf8("被主 XML 引用 · ") + note;
        QFont f = nameItem->font();
        f.setBold(true);
        nameItem->setFont(f);
    }
    if (absPath.compare(m_mainXmlAbs, Qt::CaseInsensitive) == 0)
        note = QString::fromUtf8("主 XML · ") + note;

    auto* noteItem = new QStandardItem(note);
    noteItem->setEditable(false);

    if (isDir) {
        QDir d(absPath);
        const QFileInfoList kids = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
        for (const QFileInfo& k : kids) {
            // 【2026-10-13】跳过 SVN 工作副本的内部目录。.svn 里自带 prop-base / props /
            // text-base / tmp 一整套子目录，和别处的 .svn 内部目录同名；它们不是知识库
            // 内容，列出来只会搅进重名预检，还可能被误改名把工作副本弄坏。
            if (k.isDir() && k.fileName().compare(QLatin1String(".svn"), Qt::CaseInsensitive) == 0)
                continue;
            appendEntry(nameItem, QDir::cleanPath(k.absoluteFilePath()));
        }
    }

    if (parentItem)
        parentItem->appendRow({ nameItem, noteItem });
    else
        m_model->invisibleRootItem()->appendRow({ nameItem, noteItem });
}

void KnowledgeRenameDialog::onItemChanged(QStandardItem* item)
{
    if (m_loading || !item || item->column() != 0)
        return;
    // 这里只做个标记，合法性留给点「确认」时统一预检——边改边弹错太吵，
    // 而且用户很可能改到一半才把一个名字改完整。
    Q_UNUSED(item);
}

// ============================================================================
// 从当前编辑状态算出所有节点
// ============================================================================

void KnowledgeRenameDialog::collectNodes(QStandardItem* parentItem,
                                         const QString& parentNewRel,
                                         const QString& parentNewAbs,
                                         QList<Node>& out) const
{
    for (int r = 0; r < parentItem->rowCount(); ++r) {
        QStandardItem* nameItem = parentItem->child(r, 0);
        if (!nameItem)
            continue;

        Node n;
        n.absPath   = nameItem->data(kRoleAbsPath).toString();
        n.oldName   = nameItem->data(kRoleOldName).toString();
        n.newName   = nameItem->text().trimmed();
        n.isDir     = nameItem->data(kRoleIsDir).toBool();
        n.oldRel    = nameItem->data(kRoleOldRel).toString();

        const bool isRoot = (n.absPath.compare(m_rootDir, Qt::CaseInsensitive) == 0);
        if (isRoot) {
            // 根的相对路径恒为空：它自己在 <Link> 的基准之外。主 XML 跟着它所在的
            // 目录一起搬走，里面的相对链接还是原样，所以改根名不该动任何 <Link>；
            // parentRel 给个不可能重名的哨兵值，免得和它的子项混进同一个重名检查组。
            n.parentRel = QStringLiteral("<主 XML 所在目录的上级>");
            n.newRel    = QString();
        } else {
            // 【2026-10-13】分组键就是"父目录自己的新相对路径"——递归传进来的
            // parentNewRel 正是它。以前这里又 mid(0, lastIndexOf('/')) 把父目录名的
            // 最后一段也削掉了，等于按"祖父目录"分组：45万 / 75万 这类并列目录下的
            // 同名 xml 被当成同一个目录内的重名，整片误报。父目录就是主 XML 所在
            // 目录时传进来的是空串，正好表示"都是这个目录的直接子项"，归同一组。
            n.parentRel = parentNewRel;
            n.newRel    = parentNewRel.isEmpty()
                            ? n.newName
                            : parentNewRel + QLatin1Char('/') + n.newName;
        }

        // 最终位置 = 父目录的最终位置 + 新名（父目录改过名时，这棵子树最终也跟着搬）
        n.finalAbsPath = parentNewAbs.isEmpty()
                           ? n.absPath
                           : QDir::cleanPath(parentNewAbs + QLatin1Char('/') + n.newName);

        out.append(n);

        if (n.isDir)
            collectNodes(nameItem, n.newRel, n.finalAbsPath, out);
    }
}

QList<KnowledgeRenameDialog::Node> KnowledgeRenameDialog::allNodes() const
{
    QList<Node> nodes;
    collectNodes(m_model->invisibleRootItem(), QString(), m_rootDirParent, nodes);
    return nodes;
}

bool KnowledgeRenameDialog::hasChanges() const
{
    const QList<Node> nodes = allNodes();
    for (const Node& n : nodes) {
        if (n.oldName != n.newName)
            return true;
    }
    return false;
}

// ============================================================================
// 预检
// ============================================================================

QStringList KnowledgeRenameDialog::validationErrors() const
{
    QStringList errs;

    static const QRegularExpression kBadChar(QStringLiteral(R"([\\/:*?"<>|])"));
    static const QStringList kReserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"), QStringLiteral("COM9"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"),
        QStringLiteral("LPT7"), QStringLiteral("LPT8"), QStringLiteral("LPT9")
    };

    const QList<Node> nodes = allNodes();

    // ---- 逐条 ----
    for (const Node& n : nodes) {
        if (n.newName.isEmpty()) {
            errs << QString::fromUtf8("名称不能为空：「%1」").arg(n.oldName);
            continue;
        }
        if (kBadChar.match(n.newName).hasMatch()) {
            errs << QString::fromUtf8("名称不能包含 \\ / : * ? \" < > | 这些字符：「%1」")
                    .arg(n.newName);
            continue;
        }
        if (n.newName.endsWith(QLatin1Char('.')) || n.newName.endsWith(QLatin1Char(' '))) {
            errs << QString::fromUtf8("名称不能以点或空格结尾：「%1」").arg(n.newName);
            continue;
        }
        // Windows 保留设备名（CON、PRN、COM1…），带不带后缀都不行
        const QString base = n.newName.section(QLatin1Char('.'), 0, 0).toUpper();
        if (kReserved.contains(base)) {
            errs << QString::fromUtf8("「%1」是系统保留名，不能用作文件名").arg(n.newName);
            continue;
        }
        // XML 文件必须留着 .xml 后缀，否则主 XML 的 Link 与综合流程都找不到它
        if (!n.isDir
            && n.oldName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive)
            && !n.newName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive)) {
            errs << QString::fromUtf8("XML 文件必须保留 .xml 后缀：「%1」→「%2」")
                    .arg(n.oldName, n.newName);
            continue;
        }
    }

    // ---- 同级重名（Windows 大小写不敏感，A.xml 与 a.xml 是同一个）----
    // 按"父目录改名后的相对路径"分组比对（父目录自己被改名时，它的子项也跟着落到新组）
    QHash<QString, QHash<QString, QString>> seen;   // parentRel -> (新名小写 -> 原名)
    for (const Node& n : nodes) {
        const QString key = n.parentRel.toLower();
        const QString low = n.newName.toLower();
        auto& bucket = seen[key];
        if (bucket.contains(low)) {
            errs << QString::fromUtf8("同一目录下重名：「%1」与「%2」都想叫「%3」")
                    .arg(bucket.value(low), n.oldName, n.newName);
        } else {
            bucket.insert(low, n.oldName);
        }
    }

    return errs;
}

// ============================================================================
// 改名结果
// ============================================================================

QList<KnowledgeRenameDialog::RenameOp> KnowledgeRenameDialog::renameOps() const
{
    const QList<Node> nodes = allNodes();

    QList<RenameOp> ops;
    for (const Node& n : nodes) {
        if (n.oldName == n.newName)
            continue;
        RenameOp op;
        op.oldAbsPath = n.absPath;
        // 【2026-10-09 之四】这里必须用"**就地**改名"的目标路径：父目录部分仍是老路径。
        // executeRenames 先改深层、最后才搬父目录，子项动手时父目录还没改名，
        // 写成 n.finalAbsPath（父目录的新名字）会指到一个还不存在的目录，rename 必失败
        // ——表现就是"同时改了根目录和里面的名字时，只有根目录改成功"。
        // 父目录随后改名时会把已经改好的这棵子树整体搬过去，最终位置自然对上。
        op.newAbsPath = QDir::cleanPath(QFileInfo(n.absPath).absolutePath()
                                        + QLatin1Char('/') + n.newName);
        op.isDir      = n.isDir;
        ops.append(op);
    }

    // 深层的先改：父目录改名时会把自己连同已改好的子树一起搬走
    std::sort(ops.begin(), ops.end(), [](const RenameOp& a, const RenameOp& b) {
        return depthOf(a.oldAbsPath) > depthOf(b.oldAbsPath);
    });
    return ops;
}

QString KnowledgeRenameDialog::remapLinkPath(const QString& linkText) const
{
    const QList<Node> nodes = allNodes();

    // 收集所有"名字变了"的条目，用 `/` 形式，按旧路径长度降序
    // （长前缀优先，避免 `1w` 抢在 `1w\子目录` 前面把路径切错）
    QList<QPair<QString, QString>> changed;
    for (const Node& n : nodes) {
        if (n.oldRel == n.newRel)
            continue;
        changed.append({ toSlash(n.oldRel), toSlash(n.newRel) });
    }
    if (changed.isEmpty())
        return linkText;

    std::sort(changed.begin(), changed.end(),
              [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
                  return a.first.length() > b.first.length();
              });

    // 只做 `\`→`/`（一字符换一字符，下标不变）和去空白，所以 mid() 的偏移仍然有效
    const QString s     = toSlash(linkText.trimmed());
    const QString lower = s.toLower();

    for (const auto& p : changed) {
        const QString oldLow = p.first.toLower();
        if (lower == oldLow)
            return toBackslash(p.second);
        if (lower.startsWith(oldLow + QLatin1Char('/')))
            return toBackslash(p.second + s.mid(p.first.length()));
    }
    return linkText;
}

QString KnowledgeRenameDialog::mainXmlNewAbsPath() const
{
    // 主 XML 一定是根目录的直接子项，它的新路径 = 根的新路径 + 自己的新名；
    // collectNodes 已经把这层关系算进 finalAbsPath 了（含"根也改名"的情况），直接取。
    const QList<Node> nodes = allNodes();
    for (const Node& n : nodes) {
        if (n.absPath.compare(m_mainXmlAbs, Qt::CaseInsensitive) == 0)
            return n.finalAbsPath;
    }
    return m_mainXmlAbs;
}

QString KnowledgeRenameDialog::mainXmlNewRelPath() const
{
    const QList<Node> nodes = allNodes();
    for (const Node& n : nodes) {
        if (n.absPath.compare(m_mainXmlAbs, Qt::CaseInsensitive) == 0)
            return toBackslash(n.newRel);
    }
    return toBackslash(m_mainXmlName);
}

// ============================================================================
// 点「确认」
// ============================================================================

void KnowledgeRenameDialog::onAccepted()
{
    const QStringList errs = validationErrors();
    if (!errs.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("名称有问题"),
            QString::fromUtf8("下面这些名称需要先改好：\n\n%1").arg(errs.join(QLatin1Char('\n'))));
        return;
    }

    // 改目录名 = 整棵子树搬家。如果目录里除了 XML 还有别的东西（数据文件等），
    // 一并搬走可能不是用户想要的，先问一句。
    const QList<RenameOp> ops = renameOps();
    for (const RenameOp& op : ops) {
        if (!op.isDir)
            continue;
        if (containsNonXmlFile(op.oldAbsPath)) {
            const auto ret = QMessageBox::question(this, QString::fromUtf8("确认改目录名"),
                QString::fromUtf8("目录「%1」里除 XML 外还有其它文件：\n%2\n\n"
                                  "改名会把这些文件一起搬过去。确定继续吗？")
                    .arg(QFileInfo(op.oldAbsPath).fileName(),
                         QDir::toNativeSeparators(op.oldAbsPath)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes)
                return;
        }
    }

    accept();
}

bool KnowledgeRenameDialog::containsNonXmlFile(const QString& dir)
{
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (QFileInfo(it.filePath()).suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) != 0)
            return true;
    }
    return false;
}

// ============================================================================
// 执行重命名
// ============================================================================

QStringList KnowledgeRenameDialog::executeRenames(const QList<RenameOp>& ops)
{
    QStringList errs;
    if (ops.isEmpty())
        return errs;

    // 按父目录分组，父目录深的先做（保证子条目先于其父目录改名）
    QMap<QString, QList<RenameOp>> groups;
    for (const RenameOp& op : ops)
        groups[QDir::cleanPath(QFileInfo(op.oldAbsPath).absolutePath())].append(op);

    QList<QString> dirs = groups.keys();
    std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) {
        return depthOf(a) > depthOf(b);
    });

    for (const QString& dir : dirs) {
        QList<RenameOp> left = groups.value(dir);

        // 同一个目录里可能存在"A→B、B→C"这种互相占位：目标名还被同目录另一个
        // 待改条目占着时先跳过，等它腾出位置再改。反复几轮直到改不动为止。
        bool progressed = true;
        while (!left.isEmpty() && progressed) {
            progressed = false;
            for (int i = 0; i < left.size(); ) {
                const RenameOp& op = left[i];
                const QString targetName = QFileInfo(op.newAbsPath).fileName();

                bool blocked = false;
                for (int j = 0; j < left.size(); ++j) {
                    if (j == i)
                        continue;
                    if (QFileInfo(left[j].oldAbsPath).fileName()
                            .compare(targetName, Qt::CaseInsensitive) == 0) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    ++i;
                    continue;
                }

                if (op.oldAbsPath.compare(op.newAbsPath, Qt::CaseInsensitive) == 0) {
                    // 只是大小写变了：Windows 上直接改名会被"目标已存在"挡住，走临时名
                    const QString tmp = op.oldAbsPath + QStringLiteral(".__renametmp__");
                    if (!QFile::rename(op.oldAbsPath, tmp)
                        || !QFile::rename(tmp, op.newAbsPath)) {
                        errs << QString::fromUtf8("重命名失败：%1 → %2")
                                .arg(QDir::toNativeSeparators(op.oldAbsPath),
                                     QDir::toNativeSeparators(op.newAbsPath));
                    }
                } else if (!QFile::rename(op.oldAbsPath, op.newAbsPath)) {
                    errs << QString::fromUtf8("重命名失败：%1 → %2")
                            .arg(QDir::toNativeSeparators(op.oldAbsPath),
                                 QDir::toNativeSeparators(op.newAbsPath));
                }

                left.removeAt(i);
                progressed = true;
            }
        }

        // 剩下的就是真环（A→B 且 B→A），不做临时名拆环，直接报清楚让用户分两步改
        for (const RenameOp& op : left) {
            errs << QString::fromUtf8("这两个名字互相占用，无法一次改成，请分两步：%1 → %2")
                    .arg(QFileInfo(op.oldAbsPath).fileName(),
                         QFileInfo(op.newAbsPath).fileName());
        }
    }

    return errs;
}
