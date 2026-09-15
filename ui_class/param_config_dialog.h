#pragma once
#ifndef PARAM_CONFIG_DIALOG_H
#define PARAM_CONFIG_DIALOG_H

#include <QDialog>
#include <QFrame>
#include <QTreeWidgetItem>
#include <QLineEdit>
#include <QCloseEvent>
#include <QDomDocument>
#include <QDomElement>
#include "ui_param_config.h"

class QTimer;

struct ParsedParameter {
    QString name;          // element tag name
    QString originalName;  // name at parse/save time（改名检测 + 同步匹配键）
    QString value;         // current text content
    QString originalValue; // value at parse/save time, for change detection
    QString description;   // note attribute
    QString originalDescription; // description at parse/save time
    QString tip;           // tip attribute（给用户看的中文用途提示）
    QString originalTip;
    bool favorited = false;
    bool originalFavorited = false;
    QDomElement domElement;

    bool modified() const {
        return value != originalValue || description != originalDescription
            || tip != originalTip || favorited != originalFavorited
            || name != originalName;
    }
    // 不含收藏：仅值/说明/提示/名变化，供同步等不传播收藏的场景使用
    bool contentModified() const {
        return value != originalValue || description != originalDescription
            || tip != originalTip || name != originalName;
    }
};

struct ParsedLink {
    QString path;          // Link 文本内容（子XML相对路径）
    QString originalPath;
    bool run = true;       // run 属性：true=启用
    bool originalRun = true;
    bool favorited = false;
    bool originalFavorited = false;
    QDomElement domElement;

    bool modified() const {
        return path != originalPath || run != originalRun
            || favorited != originalFavorited;
    }
};

struct ParsedMission {
    QString id;
    QString note;
    QVector<ParsedParameter> params;
    QVector<ParsedParameter> inputPaths;   // ParaIn > FilePath
    QVector<ParsedParameter> outputPaths;  // ParaOut > FilePath
    QDomElement domElement;
};

struct ParsedMissionBlock {
    QString note;
    QVector<ParsedMission> missions;
    QDomElement domElement;
};

struct ParsedFileData {
    QString filePath;
    QDomDocument xmlDoc;
    QVector<ParsedMissionBlock> blocks;
    bool isLinkFile = false;      // 根元素为 MapGeneBatchProcessingLink
    QString relativePath;         // 链接文件根元素 relativePath 属性（目标GDB路径）
    QVector<ParsedLink> links;    // 链接文件的 Link 子元素
    bool isUnknown = false;       // 结构未识别（成功解析但无 Mission/Parameter/Link，只读查看）
};

class ParamConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ParamConfigDialog(QWidget* parent = nullptr,
                               Qt::WindowFlags fl = Qt::WindowFlags());
    ~ParamConfigDialog() override;

    /// 从外部直接加载 XML 文件（不弹出文件对话框）
    /// 被 generalization_config_dialog.cpp 的"参数配置"按钮调用（打开即带入在
    /// 地图综合里选好的 XML），勿删。
    /// 【2026-09-10】返回是否加载成功：调用方据此给出提示，避免"点了参数配置却是个
    /// 空窗口"这种静默失败。
    bool loadXmlFile(const QString& path);

private:
    void onSelectFile();
    void onAppendFile();
    ParsedFileData parseOneFile(const QString& path);
    bool parseXmlFile(const QString& path);
    void refreshParamTree();
    void onBlockSelectionChanged();
    void clearTable();
    void populateTable(const ParsedMission& mission, int searchHighlightRow = -1);
    void populatePathTable(const QVector<ParsedParameter>& paths, const QString& typeLabel);
    void populateLinkTable(const ParsedFileData& fd, int searchHighlightRow = -1);
    void populateRawView(int fileIdx, const QString& elemPath);
    void onTableCellChanged(int row, int col);
    void onSearchTextChanged(const QString& text);
    void doSearch(const QString& kw);
    void navigateSearchResult(int direction); // +1=下一个, -1=上一个
    void collectCurrentMissionValues();
    void collectFormValues();
    void onSaveToFile();
    void onSaveAsFile();
    void reloadFromFile();
    void updateStatus(const QString& msg);
    void selectTreeMission(int fileIdx, int mbIdx, int mIdx);
    void applyTableFilter();
    void updateWindowTitle();
    bool hasAnyModification() const;
    void onTableCellClicked(int row, int col);
    void syncTreeToTableRow(int row);   // 表格行 -> 左侧树同步选中对应叶子节点
    void repaintRowBackground(int row, bool highlighted); // 按高亮/修改状态重绘整行底色
    void onViewModeChanged(int index);
    void onResetCurrentRow();
    void onResetCurrentMission();
    void onTreeContextMenu(const QPoint& pos);
    void onRemoveFile(int fileIdx);
    void onSyncToOtherFiles();
    bool fileHasModification(int fileIdx) const;
    QString paramLeafText(const ParsedParameter& p) const;   // 树中 ⚙ 叶子文本
    QString pathLeafText(const ParsedParameter& p) const;    // 树中 📄 路径叶子文本
    QString linkLeafText(const ParsedLink& l) const;         // 树中 🔗 叶子文本

    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

    Ui::ParamConfigDialog ui;

    QString m_xmlFilePath;
    QDomDocument m_xmlDoc;
    QVector<ParsedMissionBlock> m_missionBlocks;
    QVector<ParsedFileData> m_files;
    int m_currentFileIndex = -1;
    int m_currentMbIndex = -1;
    int m_currentMissionIndex = -1;
    bool m_updatingTable = false;
    bool m_filterModifiedOnly = false;
    bool m_showingFavoritesOnly = true;
    int m_pendingHighlightRow = -1;
    int m_highlightedRow = -1;        // 表格中当前蓝色的那一行（全表最多一行）
    bool m_showingPaths = false;      // true = 当前表格显示路径而非参数
    bool m_showingInputPaths = true; // true = 路径表格显示输入路径，false = 输出路径
    bool m_showingLinks = false;     // true = 当前表格显示链接列表（链接文件）
    bool m_selectingTreeItem = false; // 守卫：防止 selectTreeMission 触发递归回调
    QFrame* m_searchInfoBar = nullptr;
    QLabel* m_searchInfoLabel = nullptr;

    // 搜索循环导航
    struct SearchResult {
        int fileIdx; int mbIdx; int mIdx; int pIdx; // pIdx=-1 表示任务级匹配
        int priority; // 1=ID, 2=param, 3=tree
    };
    QVector<SearchResult> m_searchResults;
    int m_currentSearchIndex = -1;

    // 搜索防抖（single-shot 150ms）
    QTimer* m_searchTimer = nullptr;
};

#endif // PARAM_CONFIG_DIALOG_H
