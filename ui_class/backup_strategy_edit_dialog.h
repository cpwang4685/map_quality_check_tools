#ifndef BACKUP_STRATEGY_EDIT_DIALOG_H
#define BACKUP_STRATEGY_EDIT_DIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QTimeEdit>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QVBoxLayout>

#include "map_check_common.h"

// 备份策略编辑对话框
class CBackupStrategyEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CBackupStrategyEditDialog(const TimedBackupStrategy& strategy, QWidget* parent = nullptr);
    ~CBackupStrategyEditDialog() override;

    // 获取编辑后的策略
    TimedBackupStrategy getStrategy() const;

private slots:
    void on_comboBox_BackupType_currentIndexChanged(int index);
    void on_buttonBox_accepted();
    void on_buttonBox_rejected();

private:
    void setupUi();
    void loadStrategyToUi();
    void updateVisibility();                    // 根据选择更新控件显隐并自动调整窗口

private:
    TimedBackupStrategy m_strategy;
    
    // 主布局
    QVBoxLayout* m_pMainLayout = nullptr;

    // 执行时间设置组及其子控件（需要整体控制显隐）
    QGroupBox* m_pGroupTime = nullptr;

    // UI 控件
    QComboBox* m_pComboBoxBackupType = nullptr;
    QComboBox* m_pComboBoxFrequency = nullptr;
    QLabel* m_pLabelExecuteTime = nullptr;
    QTimeEdit* m_pTimeEditExecuteTime = nullptr;
    QLabel* m_pLabelWeekDay = nullptr;
    QComboBox* m_pComboBoxWeekDay = nullptr;
    QLabel* m_pLabelMonthDay = nullptr;
    QComboBox* m_pComboBoxMonthDay = nullptr;
    QSpinBox* m_pSpinRetentionDays = nullptr;
    QComboBox* m_pComboBoxStorageLocation = nullptr;
    QCheckBox* m_pCheckBoxEnabled = nullptr;

    // 数据源类型控件（新增）
    QGroupBox* m_pGroupDataSource = nullptr;
    QComboBox* m_pComboBoxDataSource = nullptr;
    QGroupBox* m_pGroupDbConfig = nullptr;
    QComboBox* m_pComboBoxDbType = nullptr;
    QLineEdit* m_pLineEditDbHost = nullptr;
    QLineEdit* m_pLineEditDbPort = nullptr;
    QLineEdit* m_pLineEditDbName = nullptr;
    QLineEdit* m_pLineEditDbUser = nullptr;
    QLineEdit* m_pLineEditDbPassword = nullptr;
    QLineEdit* m_pLineEditDbSchema = nullptr;
};

#endif // BACKUP_STRATEGY_EDIT_DIALOG_H
