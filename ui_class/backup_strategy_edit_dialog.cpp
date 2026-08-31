/*--------------QT---------------*/
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QTimeEdit>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLineEdit>

/*--------------SE---------------*/
#include "backup_strategy_edit_dialog.h"

CBackupStrategyEditDialog::CBackupStrategyEditDialog(const TimedBackupStrategy& strategy, QWidget* parent)
    : QDialog(parent)
    , m_strategy(strategy)
{
    setupUi();
    loadStrategyToUi();
}

CBackupStrategyEditDialog::~CBackupStrategyEditDialog()
{
}

void CBackupStrategyEditDialog::setupUi()
{
    setWindowTitle(tr("编辑备份策略"));

    // 使用布局自动管理窗口大小，不设置固定最小尺寸
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setSizeGripEnabled(true);

    // 主布局
    m_pMainLayout = new QVBoxLayout(this);
    m_pMainLayout->setSizeConstraint(QLayout::SetFixedSize);
    
    // 基本设置组
    QGroupBox* pGroupBasic = new QGroupBox(tr("基本设置"), this);
    QFormLayout* pFormBasic = new QFormLayout(pGroupBasic);
    
    // 备份类型（仅保留全量备份）
    m_pComboBoxBackupType = new QComboBox(pGroupBasic);
    m_pComboBoxBackupType->addItem(tr("全量备份"), static_cast<int>(BackupType::FullBackup));
    m_pComboBoxBackupType->setEnabled(false);
    pFormBasic->addRow(tr("备份类型:"), m_pComboBoxBackupType);
    
    // 执行频率
    m_pComboBoxFrequency = new QComboBox(pGroupBasic);
    m_pComboBoxFrequency->addItem(tr("每周1次"), static_cast<int>(BackupFrequency::Weekly));
    m_pComboBoxFrequency->addItem(tr("每天1次"), static_cast<int>(BackupFrequency::Daily));
    m_pComboBoxFrequency->addItem(tr("实时/每小时"), static_cast<int>(BackupFrequency::Hourly));
    m_pComboBoxFrequency->addItem(tr("每月1次"), static_cast<int>(BackupFrequency::Monthly));
    m_pComboBoxFrequency->addItem(tr("持续进行"), static_cast<int>(BackupFrequency::RealTime));
    pFormBasic->addRow(tr("执行频率:"), m_pComboBoxFrequency);

    // 数据源类型（新增）
    m_pGroupDataSource = new QGroupBox(tr("数据源类型"), this);
    QFormLayout* pFormDataSource = new QFormLayout(m_pGroupDataSource);
    m_pComboBoxDataSource = new QComboBox(m_pGroupDataSource);
    m_pComboBoxDataSource->addItem(tr("文件系统"), static_cast<int>(BackupDataSource::FileSystem));
    m_pComboBoxDataSource->addItem(tr("数据库"), static_cast<int>(BackupDataSource::Database));
    m_pComboBoxDataSource->addItem(tr("全部（文件+数据库）"), static_cast<int>(BackupDataSource::All));
    pFormDataSource->addRow(tr("数据源:"), m_pComboBoxDataSource);

    // 数据库连接配置（新增，仅当数据源为数据库时可见）
    m_pGroupDbConfig = new QGroupBox(tr("数据库连接配置"), this);
    QFormLayout* pFormDbConfig = new QFormLayout(m_pGroupDbConfig);

    m_pComboBoxDbType = new QComboBox(m_pGroupDbConfig);
    m_pComboBoxDbType->addItem(tr("PostGIS / PostgreSQL"), static_cast<int>(DatabaseType::PostgreSQL));
    m_pComboBoxDbType->addItem(tr("MySQL"), static_cast<int>(DatabaseType::MySQL));
    m_pComboBoxDbType->addItem(tr("Oracle"), static_cast<int>(DatabaseType::Oracle));
    m_pComboBoxDbType->addItem(tr("SQL Server"), static_cast<int>(DatabaseType::SQLServer));
    pFormDbConfig->addRow(tr("数据库类型:"), m_pComboBoxDbType);

    m_pLineEditDbHost = new QLineEdit(m_pGroupDbConfig);
    m_pLineEditDbHost->setPlaceholderText("127.0.0.1");
    pFormDbConfig->addRow(tr("主机地址:"), m_pLineEditDbHost);

    m_pLineEditDbPort = new QLineEdit(m_pGroupDbConfig);
    m_pLineEditDbPort->setText("5432");
    pFormDbConfig->addRow(tr("端口:"), m_pLineEditDbPort);

    m_pLineEditDbName = new QLineEdit(m_pGroupDbConfig);
    m_pLineEditDbName->setPlaceholderText("gis_db");
    pFormDbConfig->addRow(tr("数据库名:"), m_pLineEditDbName);

    m_pLineEditDbSchema = new QLineEdit(m_pGroupDbConfig);
    m_pLineEditDbSchema->setText("public");
    pFormDbConfig->addRow(tr("Schema:"), m_pLineEditDbSchema);

    m_pLineEditDbUser = new QLineEdit(m_pGroupDbConfig);
    m_pLineEditDbUser->setPlaceholderText("postgres");
    pFormDbConfig->addRow(tr("用户名:"), m_pLineEditDbUser);

    m_pLineEditDbPassword = new QLineEdit(m_pGroupDbConfig);
    m_pLineEditDbPassword->setEchoMode(QLineEdit::Password);
    pFormDbConfig->addRow(tr("密码:"), m_pLineEditDbPassword);

    m_pGroupDbConfig->setVisible(false);

    // 执行时间设置
    m_pGroupTime = new QGroupBox(tr("执行时间设置"), this);
    QGridLayout* pGridTime = new QGridLayout(m_pGroupTime);
    
    m_pLabelExecuteTime = new QLabel(tr("执行时间:"), m_pGroupTime);
    m_pTimeEditExecuteTime = new QTimeEdit(QTime(2, 0), m_pGroupTime);
    m_pTimeEditExecuteTime->setDisplayFormat("hh:mm");
    
    m_pLabelExecuteTime->setBuddy(m_pTimeEditExecuteTime);
    
    // 星期选择（用于周备份）
    m_pLabelWeekDay = new QLabel(tr("星期:"), m_pGroupTime);
    m_pComboBoxWeekDay = new QComboBox(m_pGroupTime);
    m_pComboBoxWeekDay->addItem(tr("周日"), 7);
    m_pComboBoxWeekDay->addItem(tr("周一"), 1);
    m_pComboBoxWeekDay->addItem(tr("周二"), 2);
    m_pComboBoxWeekDay->addItem(tr("周三"), 3);
    m_pComboBoxWeekDay->addItem(tr("周四"), 4);
    m_pComboBoxWeekDay->addItem(tr("周五"), 5);
    m_pComboBoxWeekDay->addItem(tr("周六"), 6);
    
    // 日期选择（用于月备份）
    m_pLabelMonthDay = new QLabel(tr("日期:"), m_pGroupTime);
    m_pComboBoxMonthDay = new QComboBox(m_pGroupTime);
    for (int i = 1; i <= 28; ++i)
    {
        m_pComboBoxMonthDay->addItem(tr("%1日").arg(i), i);
    }
    
    pGridTime->addWidget(m_pLabelExecuteTime, 0, 0);
    pGridTime->addWidget(m_pTimeEditExecuteTime, 0, 1);
    pGridTime->addWidget(m_pLabelWeekDay, 1, 0);
    pGridTime->addWidget(m_pComboBoxWeekDay, 1, 1);
    pGridTime->addWidget(m_pLabelMonthDay, 2, 0);
    pGridTime->addWidget(m_pComboBoxMonthDay, 2, 1);
    
    // 保留和存储设置
    QGroupBox* pGroupStorage = new QGroupBox(tr("保留与存储"), this);
    QFormLayout* pFormStorage = new QFormLayout(pGroupStorage);
    
    // 保留周期
    QHBoxLayout* pLayoutRetention = new QHBoxLayout();
    m_pSpinRetentionDays = new QSpinBox(pGroupStorage);
    m_pSpinRetentionDays->setRange(1, 365);
    m_pSpinRetentionDays->setValue(7);
    QLabel* pLabelDays = new QLabel(tr("天"), pGroupStorage);
    pLayoutRetention->addWidget(m_pSpinRetentionDays);
    pLayoutRetention->addWidget(pLabelDays);
    pLayoutRetention->addStretch();
    pFormStorage->addRow(tr("保留周期:"), pLayoutRetention);
    
    // 存储位置
    m_pComboBoxStorageLocation = new QComboBox(pGroupStorage);
    m_pComboBoxStorageLocation->addItem(tr("本地存储"), static_cast<int>(StorageLocation::Local));
    pFormStorage->addRow(tr("存储位置:"), m_pComboBoxStorageLocation);
    
    // 启用状态
    m_pCheckBoxEnabled = new QCheckBox(tr("启用此策略"), pGroupStorage);
    m_pCheckBoxEnabled->setChecked(true);
    pFormStorage->addRow("", m_pCheckBoxEnabled);
    
    // 添加到主布局
    m_pMainLayout->addWidget(pGroupBasic);
    m_pMainLayout->addWidget(m_pGroupDataSource);
    m_pMainLayout->addWidget(m_pGroupDbConfig);
    m_pMainLayout->addWidget(m_pGroupTime);
    m_pMainLayout->addWidget(pGroupStorage);
    
    // 对话框按钮
    QDialogButtonBox* pButtonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    m_pMainLayout->addWidget(pButtonBox);
    
    // 连接信号槽
    connect(m_pComboBoxBackupType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CBackupStrategyEditDialog::on_comboBox_BackupType_currentIndexChanged);
    connect(m_pComboBoxFrequency, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CBackupStrategyEditDialog::on_comboBox_BackupType_currentIndexChanged); // 复用处理
    connect(m_pComboBoxDataSource, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CBackupStrategyEditDialog::on_comboBox_BackupType_currentIndexChanged);
    connect(pButtonBox, &QDialogButtonBox::accepted, this, &CBackupStrategyEditDialog::on_buttonBox_accepted);
    connect(pButtonBox, &QDialogButtonBox::rejected, this, &CBackupStrategyEditDialog::on_buttonBox_rejected);
}

void CBackupStrategyEditDialog::loadStrategyToUi()
{
    // 设置备份类型
    int typeIndex = m_pComboBoxBackupType->findData(static_cast<int>(m_strategy.eBackupType));
    if (typeIndex >= 0)
    {
        m_pComboBoxBackupType->setCurrentIndex(typeIndex);
    }
    
    // 设置执行频率
    int freqIndex = m_pComboBoxFrequency->findData(static_cast<int>(m_strategy.eFrequency));
    if (freqIndex >= 0)
    {
        m_pComboBoxFrequency->setCurrentIndex(freqIndex);
    }

    // 设置数据源类型
    int dataSourceIndex = m_pComboBoxDataSource->findData(static_cast<int>(m_strategy.eDataSource));
    if (dataSourceIndex >= 0)
    {
        m_pComboBoxDataSource->setCurrentIndex(dataSourceIndex);
    }

    // 设置数据库配置
    BackupDataSource ds = static_cast<BackupDataSource>(m_pComboBoxDataSource->currentData().toInt());
    bool isDatabase = (ds == BackupDataSource::Database || ds == BackupDataSource::All);
    m_pGroupDbConfig->setVisible(isDatabase);

    if (isDatabase)
    {
        int dbTypeIndex = m_pComboBoxDbType->findData(static_cast<int>(m_strategy.eDbType));
        if (dbTypeIndex >= 0) m_pComboBoxDbType->setCurrentIndex(dbTypeIndex);
        m_pLineEditDbHost->setText(m_strategy.strDbHost);
        m_pLineEditDbPort->setText(QString::number(m_strategy.nDbPort));
        m_pLineEditDbName->setText(m_strategy.strDbName);
        m_pLineEditDbUser->setText(m_strategy.strDbUser);
        m_pLineEditDbPassword->setText(m_strategy.strDbPassword);
        m_pLineEditDbSchema->setText(m_strategy.strDbSchema);
    }
    
    // 解析执行时间
    QString execTime = m_strategy.strExecuteTime;
    
    // 设置星期（默认周日）
    if (execTime.contains(tr("周日")))
        m_pComboBoxWeekDay->setCurrentIndex(0);
    else if (execTime.contains(tr("周一")))
        m_pComboBoxWeekDay->setCurrentIndex(1);
    else if (execTime.contains(tr("周二")))
        m_pComboBoxWeekDay->setCurrentIndex(2);
    else if (execTime.contains(tr("周三")))
        m_pComboBoxWeekDay->setCurrentIndex(3);
    else if (execTime.contains(tr("周四")))
        m_pComboBoxWeekDay->setCurrentIndex(4);
    else if (execTime.contains(tr("周五")))
        m_pComboBoxWeekDay->setCurrentIndex(5);
    else if (execTime.contains(tr("周六")))
        m_pComboBoxWeekDay->setCurrentIndex(6);
    
    // 设置日期（用于月备份）
    if (execTime.contains(tr("1日")))
        m_pComboBoxMonthDay->setCurrentIndex(0);
    else if (execTime.contains("02:00") || execTime.contains("2:00"))
        m_pTimeEditExecuteTime->setTime(QTime(2, 0));
    else if (execTime.contains("03:00") || execTime.contains("3:00"))
        m_pTimeEditExecuteTime->setTime(QTime(3, 0));
    
    // 解析保留周期
    QString retention = m_strategy.strRetentionPeriod;
    if (retention.contains(tr("4周")) || retention.contains("28"))
        m_pSpinRetentionDays->setValue(28);
    else if (retention.contains(tr("7天")) || retention.contains("7"))
        m_pSpinRetentionDays->setValue(7);
    else if (retention.contains(tr("3天")) || retention.contains("3"))
        m_pSpinRetentionDays->setValue(3);
    else if (retention.contains(tr("12个月")) || retention.contains("365"))
        m_pSpinRetentionDays->setValue(365);
    
    // 设置存储位置
    int storageIndex = m_pComboBoxStorageLocation->findData(static_cast<int>(m_strategy.eStorageLocation));
    if (storageIndex >= 0)
    {
        m_pComboBoxStorageLocation->setCurrentIndex(storageIndex);
    }
    
    // 设置启用状态
    m_pCheckBoxEnabled->setChecked(m_strategy.bEnabled);
    
    // 根据当前选择更新控件可见性并调整窗口大小
    updateVisibility();
}

TimedBackupStrategy CBackupStrategyEditDialog::getStrategy() const
{
    return m_strategy;
}

void CBackupStrategyEditDialog::on_comboBox_BackupType_currentIndexChanged(int /*index*/)
{
    updateVisibility();
}

void CBackupStrategyEditDialog::updateVisibility()
{
    BackupFrequency freq = static_cast<BackupFrequency>(m_pComboBoxFrequency->currentData().toInt());
    
    // 根据频率显示/隐藏时间相关控件
    bool showWeekDay = (freq == BackupFrequency::Weekly);
    bool showMonthDay = (freq == BackupFrequency::Monthly);
    bool showTimeEdit = (freq != BackupFrequency::RealTime && freq != BackupFrequency::Hourly);
    
    // 星期选择控件（仅周备份时显示）
    if (m_pLabelWeekDay)      m_pLabelWeekDay->setVisible(showWeekDay);
    if (m_pComboBoxWeekDay)   m_pComboBoxWeekDay->setVisible(showWeekDay);
    
    // 日期选择控件（仅月备份时显示）
    if (m_pLabelMonthDay)     m_pLabelMonthDay->setVisible(showMonthDay);
    if (m_pComboBoxMonthDay)  m_pComboBoxMonthDay->setVisible(showMonthDay);
    
    // 时间编辑控件（实时/每小时备份时隐藏）
    if (m_pLabelExecuteTime)  m_pLabelExecuteTime->setVisible(showTimeEdit);
    if (m_pTimeEditExecuteTime) m_pTimeEditExecuteTime->setVisible(showTimeEdit);
    
    // 时间设置组本身始终可见（包含至少一行控件）
    // 但如果所有子控件都隐藏了，则隐藏整个组
    bool hasVisibleChild = showWeekDay || showMonthDay || showTimeEdit;
    if (m_pGroupTime)         m_pGroupTime->setVisible(hasVisibleChild);
    
    // 根据数据源类型显示/隐藏数据库配置
    BackupDataSource ds = static_cast<BackupDataSource>(m_pComboBoxDataSource->currentData().toInt());
    bool isDatabase = (ds == BackupDataSource::Database || ds == BackupDataSource::All);
    if (m_pGroupDbConfig)     m_pGroupDbConfig->setVisible(isDatabase);
    
    // 关键：根据内容自动调整窗口大小
    adjustSize();
}

void CBackupStrategyEditDialog::on_buttonBox_accepted()
{
    // 获取用户设置的值并更新策略
    m_strategy.eBackupType = static_cast<BackupType>(m_pComboBoxBackupType->currentData().toInt());
    m_strategy.eFrequency = static_cast<BackupFrequency>(m_pComboBoxFrequency->currentData().toInt());
    m_strategy.eStorageLocation = static_cast<StorageLocation>(m_pComboBoxStorageLocation->currentData().toInt());
    m_strategy.bEnabled = m_pCheckBoxEnabled->isChecked();

    // 数据源配置
    m_strategy.eDataSource = static_cast<BackupDataSource>(m_pComboBoxDataSource->currentData().toInt());
    BackupDataSource ds = m_strategy.eDataSource;
    if (ds == BackupDataSource::Database || ds == BackupDataSource::All)
    {
        m_strategy.eDbType = static_cast<DatabaseType>(m_pComboBoxDbType->currentData().toInt());
        m_strategy.strDbHost = m_pLineEditDbHost->text().trimmed();
        m_strategy.nDbPort = m_pLineEditDbPort->text().trimmed().toInt();
        if (m_strategy.nDbPort <= 0) m_strategy.nDbPort = 5432;
        m_strategy.strDbName = m_pLineEditDbName->text().trimmed();
        m_strategy.strDbUser = m_pLineEditDbUser->text().trimmed();
        m_strategy.strDbPassword = m_pLineEditDbPassword->text();
        m_strategy.strDbSchema = m_pLineEditDbSchema->text().trimmed();
        if (m_strategy.strDbSchema.isEmpty()) m_strategy.strDbSchema = "public";
    }
    
    // 构建执行时间描述字符串
    QTime execTime = m_pTimeEditExecuteTime->time();
    QString timeStr = execTime.toString("hh:mm");
    
    switch (m_strategy.eFrequency)
    {
    case BackupFrequency::Weekly:
        {
            QString weekDay = m_pComboBoxWeekDay->currentText();
            m_strategy.strExecuteTime = tr("%1 %2").arg(weekDay, timeStr);
            break;
        }
    case BackupFrequency::Daily:
        m_strategy.strExecuteTime = tr("每日 %1").arg(timeStr);
        break;
    case BackupFrequency::Hourly:
    case BackupFrequency::RealTime:
        m_strategy.strExecuteTime = tr("持续进行");
        break;
    case BackupFrequency::Monthly:
        {
            QString monthDay = m_pComboBoxMonthDay->currentText();
            m_strategy.strExecuteTime = tr("每月%1 %2").arg(monthDay, timeStr);
            break;
        }
    default:
        m_strategy.strExecuteTime = timeStr;
        break;
    }
    
    // 构建保留周期描述
    int days = m_pSpinRetentionDays->value();
    if (days >= 365)
        m_strategy.strRetentionPeriod = tr("保留12个月");
    else if (days >= 28)
        m_strategy.strRetentionPeriod = tr("保留%1周").arg(days / 7);
    else
        m_strategy.strRetentionPeriod = tr("保留%1天").arg(days);
    
    accept();
}

void CBackupStrategyEditDialog::on_buttonBox_rejected()
{
    reject();
}
