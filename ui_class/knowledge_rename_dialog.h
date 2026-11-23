#pragma once
#ifndef KNOWLEDGE_RENAME_DIALOG_H
#define KNOWLEDGE_RENAME_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>

class QTreeView;
class QStandardItem;
class QStandardItemModel;
class QSpinBox;
class QLabel;
class QPushButton;

/// 【2026-10-09】知识库重命名对话框。
///
/// 背景：改比例尺时"自动跟着改名"那套是靠文件名猜一个旧比例尺数字，再把整棵
/// 子树里含该数字（及其 万/w/千/k 简写）的名字做全局字符串替换。真实知识库里
/// 同一个"1:1 万"有 10000、1w 两套写法（主 XML 叫 10000自动综合知识库.xml，
/// 子 XML 目录叫 综合知识库\1w\），两者靠"人知道的业务约定"对应，字符串替换
/// 推不出来——例：1万→5千 时会把 1w 换成 0.5w，业务上没人这么命名。
///
/// 所以改名的判断权交给用户：本对话框把主 XML 所在目录整棵树摆出来，用户
/// 双击改名字；程序只做两件确定性的事——
///   ① 按用户输入执行重命名（renameOps / executeRenames）
///   ② 把主 XML 的 <Link> 路径同步成新路径（remapLinkPath）
///
/// 交互约定：
///   · 双击改名只写内存，不落盘；
///   · 点「确认」才一次性预检 + 执行；点「取消」整个保存操作终止；
///   · 什么都没改也可以直接确认（hasChanges()==false，调用方跳过改名）。
///
/// 【2026-10-09 之二】树的根 = 主 XML 所在的**那个目录本身**（不是它的内容），
/// 所以这个目录自己也能改名——真实知识库里"主 XML 所在文件夹"的名字往往也带
/// 比例尺（如 1万知识库\10000自动综合知识库.xml），保存时要一起改。
///
/// 另存为时根目录**不给改**（setRootDirNameEditable(false)）：副本的"根"就是
/// 用户上一步在文件对话框里选的目标目录，在这里再改一次等于去改用户刚指定的
/// 文件夹，属于意料之外的副作用。要改名字，回上一步换个目标目录即可。
///
/// 注意：子 XML 内部的路径（形如 1w\交通\xxx.shp）是"综合时的输出目录"，
/// 与本知识库的目录结构无关，本对话框刻意不碰它们。
class KnowledgeRenameDialog : public QDialog
{
    Q_OBJECT

public:
    /// 一次重命名操作（绝对路径）
    ///
    /// ⚠ newAbsPath 是"**就地**改名的目标路径"：父目录部分仍是**老**路径。
    /// 因为 executeRenames 是先改深层条目、最后才搬父目录的——子项动手时父目录
    /// 还没改名字，写成父目录的新路径会指向一个还不存在的目录，QFile::rename 直接
    /// 失败。父目录改名后会把已改好的子树整体搬走，最终位置自然就对上了。
    struct RenameOp {
        QString oldAbsPath;
        QString newAbsPath;
        bool    isDir = false;
    };

    /// @param mainXmlPath     主 XML 的绝对路径（树的根 = 它所在的目录）
    /// @param linkedRelPaths  主 XML 里所有 <Link> 的文本（原样，可能是 `综合知识库\1w\a.xml`），
    ///                        只用于在树里标记"这一项被主 XML 引用"
    explicit KnowledgeRenameDialog(const QString& mainXmlPath,
                                   const QStringList& linkedRelPaths,
                                   QWidget* parent = nullptr);
    ~KnowledgeRenameDialog() override;

    // ---- 比例尺（对话框顶部展示）----

    /// 识别到的原比例尺（分母，如 10000）。对话框里允许用户手改。
    int  oldScale() const;
    void setOldScale(int scale);

    /// 目标比例尺（分母）。只读展示，来自上层「自定义综合比例尺」的选择。
    void setNewScale(int scale);

    /// 树根（主 XML 所在目录）能不能改名。默认能；另存为流程要传 false。
    /// 目录本身是系统根（D:\ 或 /）时无论传什么都不能改。
    void setRootDirNameEditable(bool editable);

    // ---- 改名结果 ----

    /// 用户是否有任何改名改动
    bool hasChanges() const;

    /// 待执行的重命名列表，已按"目录深度从深到浅"排好序
    /// （先改深层条目的名字，父目录改名时会连着已改好的子树一起搬走）
    QList<RenameOp> renameOps() const;

    /// 把一条 <Link> 文本按改名结果显示成新路径；未命中任何改名则原样返回。
    /// 用的是"整段路径前缀精确匹配"，不是字符串替换——所以 `预处理\数据备份.xml`
    /// 不会因为某个目录改名而被误伤。
    QString remapLinkPath(const QString& linkText) const;

    /// 主 XML 改名后的**绝对**路径（`/` 分隔，QDir::cleanPath 过的）。
    /// 根目录本身也改了名时它跟着变，所以保存流程要用这个而不是自己拼
    /// "老目录 + 新文件名"。
    QString mainXmlNewAbsPath() const;

    /// 主 XML 改名后的相对路径（相对"主 XML 所在目录"，`\` 分隔）。
    /// 主 XML 一定是根目录的直接子项，所以实际就等于新文件名。
    /// 另存为流程用它拼副本里的路径（副本的根 = 用户选的目标目录，根不改名）。
    QString mainXmlNewRelPath() const;

    /// 预检结果：空 = 通过。非空时是逐条错误说明（非法字符 / 后缀 / 同级重名 / 保留名 等）。
    QStringList validationErrors() const;

    /// 按"同目录内先腾位再占位"的顺序执行重命名。
    /// 返回空 = 全部成功；否则是失败明细。静态是为了让调用方不依赖对话框实例。
    static QStringList executeRenames(const QList<RenameOp>& ops);

private slots:
    void onItemChanged(QStandardItem* item);
    void onAccepted();

private:
    /// 树的一个节点（由当前的编辑状态算出来）
    ///
    /// 相对路径 oldRel/newRel 一律相对**主 XML 所在目录**（也就是 `<Link>` 的基准），
    /// 根的相对路径恒为空串 —— 这样"根目录改名"不会影响 <Link> 的映射：主 XML 跟着
    /// 它所在的目录一起搬走，里面的相对链接还是老样子，本来就不用改。
    ///
    /// ⚠ finalAbsPath（最终位置）和 RenameOp::newAbsPath（就地改名的目标）**不是**
    /// 一回事，别混用：前者给 mainXmlNewAbsPath() 报"改完在哪"用，后者才是执行用的。
    struct Node {
        QString absPath;       // 原绝对路径
        QString finalAbsPath;  // **全部改完之后**的位置（父目录的新名字也算进去了）
        QString parentRel;     // 新父目录的相对路径（同名检查用，`/` 分隔）
        QString oldRel;       // 原相对路径（`/` 分隔，相对主 XML 所在目录）
        QString newRel;       // 新相对路径（同上）
        QString oldName;      // 原名
        QString newName;      // 新名
        bool    isDir = false;
    };

    void buildTree();
    void appendEntry(QStandardItem* parentItem, const QString& absPath);
    /// 按当前编辑状态走一遍树。parentNewAbs = 父目录改名后的绝对路径。
    void collectNodes(QStandardItem* parentItem, const QString& parentNewRel,
                      const QString& parentNewAbs, QList<Node>& out) const;
    /// 按 m_rootEditable 决定根节点能不能双击编辑（并写对应的"说明"文字）
    void applyRootEditability();
    /// 按当前编辑状态收集整棵树（getter 们都用它，省得每处重复传三个参数）
    QList<Node> allNodes() const;
    /// 递归检查某目录（含子目录）里有没有非 .xml 文件——改目录名前的二次确认用
    static bool containsNonXmlFile(const QString& dir);

    QTreeView*          m_tree = nullptr;
    QStandardItemModel* m_model = nullptr;
    QSpinBox*           m_spinOldScale = nullptr;
    QLabel*             m_labelTarget = nullptr;
    QLabel*             m_labelHint = nullptr;

    QString m_rootDir;      // 主 XML 所在目录（绝对，`/` 分隔）= 树根
    QString m_rootDirParent;// 树根的上级目录（根改名后的新路径由它拼出来）
    bool    m_rootEditable = true;   // 树根能不能改名（另存为时 false）
    QString m_rootLockReason;        // 不能改的原因（显示在根节点的"说明"里）
    QString m_mainXmlAbs;   // 主 XML 的绝对路径
    QString m_mainXmlName;  // 主 XML 的原文件名
    QStringList m_linkedRel;  // 被主 XML 引用的相对路径（已归一化：小写 + `/` 分隔）

    bool m_loading = false;   // 建树期间屏蔽 itemChanged
};

#endif // KNOWLEDGE_RENAME_DIALOG_H
