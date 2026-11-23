#pragma once
#ifndef SCALE_SELECTOR_DIALOG_H
#define SCALE_SELECTOR_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include "ui_scale_selector_dialog.h"

class KnowledgeRenameDialog;

/// 自定义综合比例尺选择对话框
class ScaleSelectorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ScaleSelectorDialog(QWidget* parent = nullptr,
                                 Qt::WindowFlags fl = Qt::WindowFlags());
    ~ScaleSelectorDialog() override;

    /// 获取当前选定的比例尺分母值（如 10000 表示 1:10000）
    int selectedScale() const;

    /// 设置初始比例尺
    void setScale(int scale);

    /// 设置主 XML 配置文件路径
    void setXmlFilePath(const QString& path);

    /// 【2026-10-09】取当前主 XML 配置文件路径。
    /// 改名之后这里拿到的是**新**路径，调用方（地图综合对话框）要拿回去更新自己的
    /// 配置框，否则下一次点保存就会指向一个已经不存在的老文件名。
    QString xmlFilePath() const;

    /// 【2026-10-09】识别"当前比例尺"。
    /// 【2026-10-13】统计范围从"主 XML 自己"扩到"主 XML + 它所有 <Link> 的子 XML"：
    /// 主 XML 一般只是个 Link 容器，<Scale note="比例尺"> 都在子 XML 里，只看主 XML
    /// 必然识别不到。规则：统计各 <Scale note="比例尺">值</Scale>（跳过 XML 注释内部）
    /// 在全部这些文件里出现的总次数，取次数最多的那个；主 XML 文件名里的数字若也在
    /// 候选里则优先（`10000自动综合知识库.xml` → 10000）。识别不出返回 -1。
    static int detectCurrentScale(const QString& mainXmlPath);

    /// 【2026-10-09】收集主 XML 里所有 <Link> 的路径文本（原样返回，不做归一化）。
    /// 给 KnowledgeRenameDialog 标"这一项被主 XML 引用"用。
    static QStringList collectLinkPaths(const QString& mainXmlPath);

private slots:
    void onSave();
    void onSaveAs();
    void onPresetChanged(int index);
    void onCustomScaleChanged(int value);

private:
    Ui::ScaleSelectorDialog ui;

    /// comboBox 中动态插入的自定义比例尺项的索引，-1 表示无
    int m_customItemIndex = -1;

    /// 主 XML 配置文件路径（来自地图综合 UI）
    QString m_xmlFilePath;

    /// 自定义比例尺行的可见性控制
    void setCustomRowVisible(bool visible);

    /// 解析 comboBox 中的比例尺文本为数字分母
    static int parsePresetScale(const QString& text);

    // ---- XML 处理 ----

    /// Link 条目
    struct LinkEntry {
        bool    run = true;
        QString path;
    };

    /// 收集所有需要处理的 XML 文件路径（主 XML + 所有 Link 引用的子 XML）
    QStringList collectXmlFiles(const QString& mainXmlPath, QVector<LinkEntry>& outLinks) const;

    /// 【2026-10-09 改 / 2026-10-26 改】修改内存中的 XML 内容：匹配
    /// <Scale note="比例尺">数字</Scale>，**只改值等于 oldScale 的那些**。
    ///   · 为什么按值过滤：真实知识库里同一份 XML 里既有 10000（本次要改的），也有 0 和
    ///     空值（表示"不限"/"沿用"，业务上有意义），还有别的比例尺（如 <Mission> 段）。
    ///     以前不分青红皂白把所有 >0 的值都改成新比例尺，是错的。
    ///   · 【2026-10-26】注释内部的匹配**现在也改**，而且**不受 oldScale 过滤**：注释是
    ///     说明文字，里面的数字只是描述，一律换成新比例尺才不会和正文对不上，也才能把
    ///     "已经改了一半的库"再保存一次补齐（不必猜注释里原来写的是多少）。
    ///     只有 0 / 空值仍然不动（注释里也是）。原因：真实库里注释块常是一套备用流程
    ///     （甘肃 25 万 14_静态注记生成.xml 的 84~960 行），里面的 <Scale> 是 450000，
    ///     跳过的话另存出来看着像漏改，甲方核对时无法区分。
    ///     注：识别当前比例尺（detectCurrentScale）仍只统计正文——注释不参与投票，
    ///     免得注释里的旧值把"当前比例尺"认错。
    ///   · oldScale <= 0（调用方没传/识别失败）时退回旧的"改所有 >0 值"的行为，此时 0 与
    ///     空值依然动不到。
    static QString modifyScaleInMemory(const QString& content, int oldScale, int newScale);

    /// 修改一个 XML 文件中的比例尺（原地覆写，保留原有 UTF-8 BOM 与行尾）
    static bool modifyScaleInFile(const QString& filePath, int oldScale, int newScale);

    /// 【2026-10-26】离屏自测（D:\tmp_cmp\cattest\test_scale.cpp）用：只放开访问权限，
    /// 不改变任何行为。用 friend struct 而不是把它们挪到 public，是为了不扩大类的对外接口。
    friend struct ScaleDialogSelfTest;

    /// 递归拷贝目录树，失败返回 false
    static bool copyDirectoryRecursive(const QString& srcDir, const QString& dstDir);

    // ---- 【2026-10-09 新增】改名 + 回写 ----

    /// 算出 content 里所有 XML 注释 <!-- ... --> 的区间 [start, end)。
    /// 注释不嵌套，扫一遍即可。
    static QVector<QPair<int, int>> commentRanges(const QString& content);

    /// 按 KnowledgeRenameDialog 的改名结果，把主 XML 里每条 <Link> 的路径写成新路径
    /// （整段前缀精确匹配，不是字符串替换）。原地覆写。@return true = 成功（含"无需改动"）
    static bool rewriteLinkPathsInXml(const QString& xmlPath,
                                      const KnowledgeRenameDialog& renameDlg);

    /// 把主 XML 根元素的 notice 属性改成新的文件名（不含扩展名）。原地覆写。
    /// 找不到 notice 属性则什么都不做。@return true = 成功
    static bool setNoticeInXml(const QString& xmlPath, const QString& newBaseName);

    /// 读整个 XML 文件为字符串。显式剥掉 UTF-8 BOM 并记住有没有，
    /// 避免依赖 QString::fromUtf8/toUtf8 往返是否保留 BOM 这一实现细节。
    static QString readXmlFile(const QString& filePath, bool* hasBom, bool* ok = nullptr);

    /// 按 readXmlFile 报的 hasBom 决定要不要写回 BOM。原地覆写。
    static bool writeXmlFile(const QString& filePath, const QString& content, bool hasBom);

    // ---- 路径重命名（跟进比例尺变化）：【2026-10-09 已停用】 ----
    //
    // 下面三个是"从文件名猜一个旧比例尺数字，再把整棵子树里含该数字（及其 万/w/千/k
    // 简写）的名字做全局字符串替换"那套的老实现。它在真实知识库上是错的：
    // 同一个 1:1 万在库里同时有 `10000`（主 XML 名）和 `1w`（子目录名）两套写法，
    // 靠字符串替换推不出对应关系——1万→5千 时它会把 `1w` 换成 `0.5w`，业务上没人
    // 这么命名；oldScale 猜成 1 时更是把每个 `1` 都改掉。
    //
    // 按用户决定：改名判断权交给 KnowledgeRenameDialog（双击改名 + 确认执行），
    // 这三个函数**保留代码备查、但已无调用点**。将来要删，直接删这三段 + .cpp 里
    // 对应的 buildScaleReplacements / applyScaleReplacements / containsAnyOldScale。

    /// 从文件名中提取首个连续数字作为旧比例尺；未找到返回 -1。
    /// 仅剩一处调用：detectCurrentScale() 用它给"文件名里的数字"加权。
    static int extractScaleFromFileName(const QString& filePath);

    /// 【已停用】更新主 XML 中 Link 文本的路径（把旧比例尺数字替换为新比例尺）
    static bool updateLinkPathsInXml(const QString& xmlPath, int oldScale, int newScale);

    /// 【已停用】递归重命名目录树中包含旧比例尺数字的文件和文件夹
    static int renameScaleRelatedPaths(const QString& rootDir, int oldScale, int newScale);
};

#endif // SCALE_SELECTOR_DIALOG_H
