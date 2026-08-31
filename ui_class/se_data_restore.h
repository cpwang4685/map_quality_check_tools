#ifndef SE_DATA_RESTORE_H
#define SE_DATA_RESTORE_H

#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QButtonGroup>

#include "ui_data_restore.h"
#include "map_check_common.h"

#include <vector>
#include <string>

using namespace std;

// 备份记录信息结构
struct BackupRecord
{
    QString strName;          // 备份文件夹名称
    BackupType eBackupType;   // 备份类型
    QDateTime dtCreateTime;   // 创建时间
    qint64 nSizeBytes;        // 大小（字节）
    QString strFullPath;      // 完整路径

    BackupRecord()
        : eBackupType(BackupType::FullBackup)
        , nSizeBytes(0)
    {}

    QString backupTypeName() const;
    QString sizeDisplay() const;
};

class CSE_DataRestoreDialog : public QDialog
{
    Q_OBJECT

public:
    CSE_DataRestoreDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~CSE_DataRestoreDialog() override;

protected:
    // 第一次 show 时真正应用布局的 sizeHint
    void showEvent(QShowEvent* event) override;

private:
    Ui_DataRestoreDialog ui;

    // ===== 备份相关成员 =====
    vector<TimedBackupStrategy> m_vecStrategies;
    QString m_qstrSourcePath;
    QString m_qstrTargetPath;
    bool m_bEnableTimedBackup = false;
    bool m_bKeepExpired = true;

    // ===== 数据库备份配置（新增） =====
    BackupDataSource m_eBackupDataSource = BackupDataSource::FileSystem;

    // 数据库备份范围/方式/格式的 QButtonGroup（分组互斥）
	QButtonGroup* m_pScopeGroup = nullptr;

    // ===== 恢复相关成员 =====
    QString m_qstrBackupDir;         // 备份存放目录
    QString m_qstrRestoreDstPath;     // 恢复目标目录
    bool m_bOverwriteExisting = true; // 是否覆盖已存在文件
    vector<BackupRecord> m_vecBackupRecords;  // 可用备份记录列表

private:
    // 初始化
    void initDefaultStrategies();
    void refreshBackupStrategyTable();
    void refreshBackupList();

    // 配置读写
    void restoreState();
    void saveState();

    // 日志
    void appendLog(const QString& msg);

    // 工具函数
    void scanBackupRecords(const QString& backupDir);
    qint64 calculateDirSize(const QString& dirPath) const;
    QString formatFileSize(qint64 bytes) const;

	// 恢复操作
	bool performRestore(const BackupRecord& record, const QString& dstPath, bool overwrite);

	// 恢复到数据库
	bool performRestoreToDatabase(const BackupRecord& record,
		const QString& host, int port, const QString& dbName,
		const QString& username, const QString& password,
		const QString& schema, bool overwrite);

	// 复制目录（恢复用）
	bool restoreDirectory(const QDir& srcDir, const QDir& dstDir, bool overwrite);

	// 将单个文件导入到PostGIS
	bool importFileToPostGIS(const QString& filePath,
		const QString& host, int port, const QString& dbName,
		const QString& username, const QString& password,
		const QString& schema, const QString& tableName, bool overwrite);

	// 设置状态栏文字
	void setStatusText(const QString& text);

	// 根据备份来源显示/隐藏对应配置区，并自适应窗口高度
	void updateBackupSourceVisibility();

	// 根据备份范围选项显示/隐藏表选择相关控件
	void updateBackupScopeWidgets();

	// 调整窗口高度以适应当前备份 tab 内容
	void adjustDialogHeightForBackupTab();

	// 精确计算当前 tab 页面所需的窗口总高度（避开 QTabWidget sizeHint 取最大页面的陷阱）
	int calculateHeightForCurrentTab();

	// 递归激活布局，确保 setVisible 后子布局立即重新计算
	void activateLayoutRecursively(QLayout* pLayout) const;

	// 递归强制所有子布局失效 + 标记各子控件几何失效，
	// 让 sizeHint/cache 在下一轮测量时彻底刷新回当前可见状态
	void forceLayoutDeepInvalidate(QWidget* pRoot);

	// 一站式应用「自动备份」Tab 布局：解锁窗口 → 深度失效 → 重算高度 → 锁定
	void applyBackupLayout();

	// 切换恢复目标类型后，让 stackedWidget 高度跟随当前页面内容，并重新调整窗口
	void updateRestoreTargetHeight();

public slots:
    // ===== 备份 Tab 槽函数 =====
    void on_Button_SourceBrowse_clicked();
    void on_Button_TargetBrowse_clicked();
    void on_Button_AddStrategy_clicked();
    void on_Button_EditStrategy_clicked();
    void on_Button_DeleteStrategy_clicked();
    void on_Button_ImmediateBackup_clicked();

    void on_lineEdit_SourcePath_textChanged(const QString& text);
    void on_lineEdit_TargetPath_textChanged(const QString& text);
    void on_checkBox_EnableTimedBackup_stateChanged(int state);
    void on_checkBox_KeepExpired_stateChanged(int state);

    // ===== 数据库备份配置槽函数（新增） =====
    void on_radioButton_SourceFile_toggled(bool checked);
    void on_radioButton_SourceDatabase_toggled(bool checked);
    void on_radioButton_SourceAll_toggled(bool checked);
    void on_Button_GetBackupDbList_clicked();
    void on_Button_GetDatabases_clicked();
    void on_radioButton_BackupAllData_toggled(bool checked);
    void on_radioButton_BackupSelectedTables_toggled(bool checked);
    void on_Button_DbTargetBrowse_clicked();
    void on_lineEdit_DbTargetPath_textChanged(const QString& text);
    void on_Button_SelectAllTables_clicked();
    void on_Button_InvertTableSelection_clicked();
    void on_Button_RefreshBackupTables_clicked();

    // ===== 恢复 Tab 槽函数 =====
    void on_Button_BrowseBackupDir_clicked();
    void on_Button_BrowseRestoreOriginal_clicked();
    void on_Button_BrowseRestoreTarget_clicked();
    void on_Button_RefreshBackupList_clicked();
    void on_Button_Restore_clicked();
    void on_Button_DeleteBackup_clicked();
    void on_Button_RestoreApply_clicked();

    void on_comboBox_BackupTypeFilter_currentIndexChanged(int index);
    void on_radioButton_RestoreToOriginal_toggled(bool checked);
    void on_radioButton_RestoreToCustom_toggled(bool checked);
    void on_radioButton_RestoreToDatabase_toggled(bool checked);
    void on_checkBox_OverwriteExisting_stateChanged(int state);
    void on_Button_TestDBConnection_clicked();
    void on_Button_TestRestoreDbConnection_clicked();
    void on_lineEdit_RestoreOriginalPath_textChanged(const QString& text);
    void on_lineEdit_RestoreDstPath_textChanged(const QString& text);

    // ===== 日志 Tab 槽函数 =====
    void on_Button_ClearLog_clicked();
    void on_Button_ExportLog_clicked();

    // Tab 切换时同步数据
    void on_tabWidget_main_currentChanged(int index);
};

#endif // SE_DATA_RESTORE_H
