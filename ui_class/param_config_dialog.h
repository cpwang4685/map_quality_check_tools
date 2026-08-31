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

struct ParsedParameter {
    QString name;          // element tag name
    QString value;         // current text content
    QString originalValue; // value at parse/save time, for change detection
    QString description;   // note attribute
    QString originalDescription; // description at parse/save time
    bool favorited = false;
    bool originalFavorited = false;
    QDomElement domElement;
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
};

class ParamConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ParamConfigDialog(QWidget* parent = nullptr,
                               Qt::WindowFlags fl = Qt::WindowFlags());
    ~ParamConfigDialog() override;

    /// 从外部直接加载 XML 文件（不弹出文件对话框）
    void loadXmlFile(const QString& path);

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
    void onTableCellChanged(int row, int col);
    void onSearchTextChanged(const QString& text);
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
    void onViewModeChanged(int index);
    void onResetCurrentRow();
    void onResetCurrentMission();
    void onTreeContextMenu(const QPoint& pos);
    void onRemoveFile(int fileIdx);
    void onSyncToOtherFiles();
    bool fileHasModification(int fileIdx) const;

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
    bool m_showingPaths = false;      // true = 当前表格显示路径而非参数
    bool m_showingInputPaths = true; // true = 路径表格显示输入路径，false = 输出路径
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
};

#endif // PARAM_CONFIG_DIALOG_H
