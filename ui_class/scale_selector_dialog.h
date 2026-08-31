#pragma once
#ifndef SCALE_SELECTOR_DIALOG_H
#define SCALE_SELECTOR_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>
#include "ui_scale_selector_dialog.h"

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

    /// 修改内存中的 XML 内容：匹配 <Scale note="比例尺">数字</Scale> 并替换数字
    static QString modifyScaleInMemory(const QString& content, int newScale);

    /// 修改一个 XML 文件中的比例尺（原地覆写）
    static bool modifyScaleInFile(const QString& filePath, int newScale);

    /// 递归拷贝目录树，失败返回 false
    static bool copyDirectoryRecursive(const QString& srcDir, const QString& dstDir);

    // ---- 路径重命名（跟进比例尺变化） ----

    /// 从文件名中提取首个连续数字作为旧比例尺；未找到返回 -1
    static int extractScaleFromFileName(const QString& filePath);

    /// 更新主 XML 中 Link 文本的路径（将旧比例尺数字替换为新比例尺）
    /// @return true 表示有修改
    static bool updateLinkPathsInXml(const QString& xmlPath, int oldScale, int newScale);

    /// 递归重命名目录树中包含旧比例尺数字的文件和文件夹（深度优先从深到浅）
    /// @return 成功重命名的条目数
    static int renameScaleRelatedPaths(const QString& rootDir, int oldScale, int newScale);
};

#endif // SCALE_SELECTOR_DIALOG_H
