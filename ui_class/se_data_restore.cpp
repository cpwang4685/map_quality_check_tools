/*--------------QT---------------*/
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QTextStream>
#include <QTextCursor>
#include <QFileInfo>
#include <QFileInfoList>
#include <QDesktopServices>
#include <QUrl>
#include <QColor>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QRegExp>
#include <QScreen>
#include <QApplication>
#include <QTimer>
#include <QListWidget>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

/*--------------QGIS---------------*/
#include "qgssettings.h"
#include "qgsgui.h"
#include "qgsapplication.h"
#include "qgsvectorlayer.h"
#include "qgsvectorfilewriter.h"
#include "qgsproject.h"
#include "qgswkbtypes.h"

/*--------------STL---------------*/
#include <functional>

/*--------------SE---------------*/
#include "se_data_restore.h"
#include "backup_strategy_edit_dialog.h"
#include "map_check_backup_manager.h"

#include "ui_fit_helper.h"

/*-----------------------------------*/

// ========== BackupRecord 辅助方法 ==========

QString BackupRecord::backupTypeName() const
{
    switch (eBackupType)
    {
    case BackupType::FullBackup:        return QObject::tr("全量备份");
    case BackupType::IncrementalBackup: return QObject::tr("增量备份");
    case BackupType::WALArchive:        return QObject::tr("WAL归档");
    case BackupType::LogicalExportBackup:return QObject::tr("逻辑导出备份");
    default: return QObject::tr("未知");
    }
}

QString BackupRecord::sizeDisplay() const
{
    if (nSizeBytes < 1024)
        return QString("%1 B").arg(nSizeBytes);
    else if (nSizeBytes < 1024 * 1024)
        return QString("%1 KB").arg(nSizeBytes / 1024.0, 0, 'f', 1);
    else if (nSizeBytes < 1024LL * 1024 * 1024)
        return QString("%1 MB").arg(nSizeBytes / (1024.0 * 1024.0), 0, 'f', 2);
    else
        return QString("%1 GB").arg(nSizeBytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

// ========== CSE_DataRestoreDialog 实现 ==========

CSE_DataRestoreDialog::CSE_DataRestoreDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);
    DialogFitHelper::install(this);

    // 设置窗口标志
    this->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    this->setAttribute(Qt::WA_NoSystemBackground, false);
    this->setAttribute(Qt::WA_TranslucentBackground, false);

    // 初始化 QButtonGroup，使每组内部互斥，组之间不互斥
    m_pScopeGroup = new QButtonGroup(this);
    m_pScopeGroup->addButton(ui.radioButton_BackupAllData, 0);
    m_pScopeGroup->addButton(ui.radioButton_BackupSelectedTables, 1);

    // 初始化默认备份策略
    initDefaultStrategies();

    // 恢复设置
    restoreState();

    // 从 BackupManager 同步策略（仅保留全量备份，过滤掉其他类型）
    CMapCheckBackupManager::instance()->loadSettings();
    const vector<TimedBackupStrategy>& savedStrategies = CMapCheckBackupManager::instance()->getStrategies();
    m_vecStrategies.clear();
    for (const auto& strategy : savedStrategies)
    {
        if (strategy.eBackupType == BackupType::FullBackup)
        {
            m_vecStrategies.push_back(strategy);
        }
    }

    // 刷新备份策略表
    refreshBackupStrategyTable();

    // 同步备份路径设置到 UI
    ui.lineEdit_SourcePath->setText(m_qstrSourcePath);
    ui.lineEdit_TargetPath->setText(m_qstrTargetPath);
    ui.lineEdit_DbTargetPath->setText(m_qstrTargetPath);
    ui.checkBox_EnableSchedule->setChecked(m_bEnableTimedBackup);
    ui.checkBox_AutoCleanup->setChecked(m_bKeepExpired);

    // 恢复 Tab 的初始状态
    ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
    ui.lineEdit_RestoreOriginalPath->setText(m_qstrSourcePath);
    ui.lineEdit_RestoreDstPath->setText(m_qstrRestoreDstPath);
    ui.checkBox_OverwriteExisting->setChecked(m_bOverwriteExisting);
    ui.stackedWidget_restoreTarget->setCurrentIndex(0);

    // 数据库备份配置：初始状态根据持久化设置选中，并动态显示/隐藏对应配置区
    ui.radioButton_SourceFile->setChecked(m_eBackupDataSource == BackupDataSource::FileSystem);
    ui.radioButton_SourceDatabase->setChecked(m_eBackupDataSource == BackupDataSource::Database);
    ui.radioButton_SourceAll->setChecked(m_eBackupDataSource == BackupDataSource::All);

    // 更新 groupBox 标题，使其语义匹配新的复合功能（备份来源选择 + 数据库配置）
    ui.groupBox_source->setTitle(tr("备份来源与数据库配置"));

	// 初始即根据备份来源显示/隐藏对应配置区
	updateBackupSourceVisibility();

	// 根据当前备份范围同步表选择控件的显隐
	updateBackupScopeWidgets();

    // 刷新备份列表
    if (!m_qstrBackupDir.isEmpty())
    {
        refreshBackupList();
    }

    // 确保表格选中第一行
    if (ui.tableWidget_backup_strategy->rowCount() > 0)
        ui.tableWidget_backup_strategy->selectRow(0);
    if (ui.tableWidget_BackupList->rowCount() > 0)
        ui.tableWidget_BackupList->selectRow(0);

    setStatusText(tr("就绪"));

    appendLog(tr("数据自动备份恢复系统已启动。"));
}

CSE_DataRestoreDialog::~CSE_DataRestoreDialog()
{
    saveState();
}

void CSE_DataRestoreDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    // 延迟到事件循环处理完毕后再计算尺寸，确保 tabBar、表格等控件尺寸已计算完成
    QTimer::singleShot(0, this, [this]() {
        updateRestoreTargetHeight();
        // 仅一站式入口，会深度失效 + 多轮重算 + 锁定高度
        applyBackupLayout();
    });
}

// ========== 初始化 ==========

void CSE_DataRestoreDialog::initDefaultStrategies()
{
    m_vecStrategies.clear();

    // 全量备份：每周1次
    TimedBackupStrategy fullBackup;
    fullBackup.eBackupType = BackupType::FullBackup;
    fullBackup.eFrequency = BackupFrequency::Weekly;
    fullBackup.strExecuteTime = tr("周日 02:00");
    fullBackup.strRetentionPeriod = tr("保留4周");
    fullBackup.eStorageLocation = StorageLocation::Local;
    fullBackup.bEnabled = true;
    m_vecStrategies.push_back(fullBackup);
}

// ========== 备份策略表格 ==========

void CSE_DataRestoreDialog::refreshBackupStrategyTable()
{
    ui.tableWidget_backup_strategy->clearContents();
    ui.tableWidget_backup_strategy->setRowCount(static_cast<int>(m_vecStrategies.size()));

    for (int i = 0; i < static_cast<int>(m_vecStrategies.size()); ++i)
    {
        const TimedBackupStrategy& strategy = m_vecStrategies[i];

        QTableWidgetItem* itemType = new QTableWidgetItem(strategy.backupTypeName());
        itemType->setFlags(itemType->flags() & ~Qt::ItemIsEditable);
        if (!strategy.bEnabled)
            itemType->setForeground(QColor(128, 128, 128));
        ui.tableWidget_backup_strategy->setItem(i, 0, itemType);

        QTableWidgetItem* itemFreq = new QTableWidgetItem(strategy.frequencyName());
        itemFreq->setFlags(itemFreq->flags() & ~Qt::ItemIsEditable);
        if (!strategy.bEnabled)
            itemFreq->setForeground(QColor(128, 128, 128));
        ui.tableWidget_backup_strategy->setItem(i, 1, itemFreq);

        QTableWidgetItem* itemTime = new QTableWidgetItem(strategy.strExecuteTime);
        itemTime->setFlags(itemTime->flags() & ~Qt::ItemIsEditable);
        if (!strategy.bEnabled)
            itemTime->setForeground(QColor(128, 128, 128));
        ui.tableWidget_backup_strategy->setItem(i, 2, itemTime);

        QTableWidgetItem* itemRet = new QTableWidgetItem(strategy.strRetentionPeriod);
        itemRet->setFlags(itemRet->flags() & ~Qt::ItemIsEditable);
        if (!strategy.bEnabled)
            itemRet->setForeground(QColor(128, 128, 128));
        ui.tableWidget_backup_strategy->setItem(i, 3, itemRet);

        QTableWidgetItem* itemSto = new QTableWidgetItem(strategy.storageLocationName());
        itemSto->setFlags(itemSto->flags() & ~Qt::ItemIsEditable);
        if (!strategy.bEnabled)
            itemSto->setForeground(QColor(128, 128, 128));
        ui.tableWidget_backup_strategy->setItem(i, 4, itemSto);
    }

    ui.tableWidget_backup_strategy->resizeColumnsToContents();
    ui.tableWidget_backup_strategy->horizontalHeader()->setStretchLastSection(true);

    // 根据策略行数动态限制表格最大高度，防止产生大片空白
    int nRowCount = ui.tableWidget_backup_strategy->rowCount();
    int nHeaderHeight = ui.tableWidget_backup_strategy->horizontalHeader()->height();
    int nRowHeight = nRowCount > 0 ? ui.tableWidget_backup_strategy->rowHeight(0) : 24;
    int nMaxTableHeight = nHeaderHeight + nRowCount * nRowHeight + 4;
    nMaxTableHeight = qMax(nMaxTableHeight, 80);
    nMaxTableHeight = qMin(nMaxTableHeight, 280); // 最多显示约10行
    ui.tableWidget_backup_strategy->setMaximumHeight(nMaxTableHeight);
    ui.tableWidget_backup_strategy->setMinimumHeight(nMaxTableHeight);
}

// ========== 备份列表 ==========

void CSE_DataRestoreDialog::refreshBackupList()
{
    ui.tableWidget_BackupList->clearContents();
    ui.tableWidget_BackupList->setRowCount(0);

    if (m_qstrBackupDir.isEmpty())
        return;

    // 扫描备份目录
    scanBackupRecords(m_qstrBackupDir);

    // 获取当前筛选条件
    QString filterText = ui.comboBox_BackupTypeFilter->currentText();

    // 过滤并显示
    int row = 0;
    vector<BackupRecord> filtered;
    for (const auto& record : m_vecBackupRecords)
    {
        if (filterText == tr("全部类型") || record.backupTypeName() == filterText)
        {
            filtered.push_back(record);
        }
    }

    ui.tableWidget_BackupList->setRowCount(static_cast<int>(filtered.size()));
    for (const auto& record : filtered)
    {
        QTableWidgetItem* itemName = new QTableWidgetItem(record.strName);
        itemName->setFlags(itemName->flags() & ~Qt::ItemIsEditable);
        ui.tableWidget_BackupList->setItem(row, 0, itemName);

        QTableWidgetItem* itemType = new QTableWidgetItem(record.backupTypeName());
        itemType->setFlags(itemType->flags() & ~Qt::ItemIsEditable);
        ui.tableWidget_BackupList->setItem(row, 1, itemType);

        QTableWidgetItem* itemTime = new QTableWidgetItem(
            record.dtCreateTime.toString("yyyy-MM-dd hh:mm:ss"));
        itemTime->setFlags(itemTime->flags() & ~Qt::ItemIsEditable);
        ui.tableWidget_BackupList->setItem(row, 2, itemTime);

        QTableWidgetItem* itemSize = new QTableWidgetItem(record.sizeDisplay());
        itemSize->setFlags(itemSize->flags() & ~Qt::ItemIsEditable);
        ui.tableWidget_BackupList->setItem(row, 3, itemSize);

        QTableWidgetItem* itemPath = new QTableWidgetItem(record.strFullPath);
        itemPath->setFlags(itemPath->flags() & ~Qt::ItemIsEditable);
        ui.tableWidget_BackupList->setItem(row, 4, itemPath);

        ++row;
    }

    ui.tableWidget_BackupList->resizeColumnsToContents();
    ui.tableWidget_BackupList->horizontalHeader()->setStretchLastSection(true);

    // 根据实际行数设置恢复列表最小高度：行少则列表紧凑，行多时可随布局拉伸填充中间空余
    int nRowCount = ui.tableWidget_BackupList->rowCount();
    int nRowHeight = (nRowCount > 0) ? ui.tableWidget_BackupList->rowHeight(0) : 25;
    int nHeaderHeight = ui.tableWidget_BackupList->horizontalHeader()->height();
    int nDesiredHeight = nHeaderHeight + nRowHeight * qMax(nRowCount, 1) + 4;
    ui.tableWidget_BackupList->setMinimumHeight(qMax(nDesiredHeight, 160));
    ui.tableWidget_BackupList->setMaximumHeight(QWIDGETSIZE_MAX);

    setStatusText(tr("已扫描 %1 个备份记录，显示 %2 个")
        .arg(m_vecBackupRecords.size())
        .arg(filtered.size()));

    if (isVisible() && ui.tabWidget_main->currentIndex() == 1)
        adjustDialogHeightForBackupTab();
}

void CSE_DataRestoreDialog::scanBackupRecords(const QString& backupDir)
{
    m_vecBackupRecords.clear();

    QDir dir(backupDir);
    if (!dir.exists())
        return;

    QFileInfoList dirList = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);

    for (const QFileInfo& dirInfo : dirList)
    {
        QString name = dirInfo.fileName();
        BackupRecord record;
        record.strName = name;
        record.strFullPath = dirInfo.absoluteFilePath();
        record.dtCreateTime = dirInfo.birthTime();
        if (!record.dtCreateTime.isValid())
            record.dtCreateTime = dirInfo.lastModified();
        record.nSizeBytes = calculateDirSize(dirInfo.absoluteFilePath());

        // 根据文件夹前缀判断备份类型
        if (name.startsWith("FullBackup"))
            record.eBackupType = BackupType::FullBackup;
        else if (name.startsWith("IncrementalBackup"))
            record.eBackupType = BackupType::IncrementalBackup;
        else if (name.startsWith("WALArchive"))
            record.eBackupType = BackupType::WALArchive;
        else if (name.startsWith("LogicalExportBackup"))
            record.eBackupType = BackupType::LogicalExportBackup;
        else
            record.eBackupType = BackupType::FullBackup; // 默认

        m_vecBackupRecords.push_back(record);
    }
}

qint64 CSE_DataRestoreDialog::calculateDirSize(const QString& dirPath) const
{
    qint64 totalSize = 0;
    QDir dir(dirPath);
    QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QFileInfo& info : list)
    {
        if (info.isDir())
            totalSize += calculateDirSize(info.absoluteFilePath());
        else
            totalSize += info.size();
    }
    return totalSize;
}

// ========== 配置读写 ==========

void CSE_DataRestoreDialog::restoreState()
{
    // ---- 从数据库连接配置 UI（DbConfigDialog）共享的注册表配置加载连接参数 ----
    // 配置 UI 连接成功后把 host/port/database/schema/user/password 写入
    // QSettings("GarMap","MapProductManager")，这里读取同一批键，实现与配置 UI
    // 共享同一个数据库（与地图数据下载 UI mapdata_download 的做法一致）。
    // 不再读取 db_config.json（新配置 UI 已不再输出该文件）。
    QSettings dbSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
    QString host   = dbSettings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString();
    int port       = dbSettings.value(QStringLiteral("db/port"), 5432).toInt();
    QString dbname = dbSettings.value(QStringLiteral("db/database"), QStringLiteral("map_products")).toString();
    QString schema = dbSettings.value(QStringLiteral("db/schema"), QStringLiteral("public")).toString();
    QString user   = dbSettings.value(QStringLiteral("db/user"), QStringLiteral("postgres")).toString();
    // 仅当配置 UI 勾选“保存密码”时才持久化密码，未勾选则留空由用户手动输入
    QString password;
    if (dbSettings.value(QStringLiteral("db/savePassword"), false).toBool())
        password = dbSettings.value(QStringLiteral("db/password")).toString();

    // 填充备份数据库连接字段
    ui.lineEdit_backupDbHost->setText(host);
    ui.lineEdit_backupDbPort->setText(QString::number(port));
    ui.lineEdit_backupDbName->setText(dbname);
    ui.lineEdit_backupDbSchema->setText(schema);
    ui.lineEdit_backupDbUser->setText(user);
    ui.lineEdit_backupDbPassword->setText(password);

    // 填充恢复数据库连接字段（恢复到数据库）
    ui.lineEdit_dbHost->setText(host);
    ui.lineEdit_DbPort->setText(QString::number(port));
    ui.lineEdit_DbName->setText(dbname);
    ui.lineEdit_dbSchema->setText(schema);
    ui.lineEdit_DbUser->setText(user);
    ui.lineEdit_DbPassword->setText(password);

    // ---- 自动测试连接并显示状态 ----
    {
        QString connName = "restore_auto_test";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
            db.setHostName(host);
            db.setPort(port);
            db.setDatabaseName(dbname);
            db.setUserName(user);
            db.setPassword(password);
            if (db.open())
            {
                setStatusText(tr("✓ 连接成功 (%1:%2/%3)")
                    .arg(host).arg(port).arg(dbname));
                ui.label_status->setStyleSheet("QLabel { color: #4CAF50; }");
                db.close();
            }
            else
            {
                setStatusText(tr("✗ 连接失败: %1").arg(db.lastError().text()));
                ui.label_status->setStyleSheet("QLabel { color: red; }");
            }
        }
        QSqlDatabase::removeDatabase(connName);
    }

    const QgsSettings settings;

    m_qstrSourcePath = settings.value(
        QStringLiteral("TimedBackup/SourcePath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
    m_qstrTargetPath = settings.value(
        QStringLiteral("TimedBackup/TargetPath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
    m_bEnableTimedBackup = settings.value(
        QStringLiteral("TimedBackup/EnableTimedBackup"), false, QgsSettings::Section::Plugins).toBool();
    m_bKeepExpired = settings.value(
        QStringLiteral("TimedBackup/KeepExpired"), true, QgsSettings::Section::Plugins).toBool();

    // 恢复专用设置
    m_qstrBackupDir = settings.value(
        QStringLiteral("DataRestore/BackupDir"), QString(), QgsSettings::Section::Plugins).toString();
    m_qstrRestoreDstPath = settings.value(
        QStringLiteral("DataRestore/RestoreDstPath"), QString(), QgsSettings::Section::Plugins).toString();
    m_bOverwriteExisting = settings.value(
        QStringLiteral("DataRestore/OverwriteExisting"), true, QgsSettings::Section::Plugins).toBool();

    // 非连接字段（数据库连接字段已由上面的共享配置填充，不再从 QgsSettings 覆盖）
    m_eBackupDataSource = static_cast<BackupDataSource>(settings.value(
        QStringLiteral("DataRestore/BackupDataSource"), 0, QgsSettings::Section::Plugins).toInt());
    int scopeIdx = settings.value(QStringLiteral("DataRestore/BackupDbScope"), 0, QgsSettings::Section::Plugins).toInt();
    if (m_pScopeGroup) m_pScopeGroup->button(scopeIdx)->setChecked(true);

    // 恢复日志
    QString logText = settings.value(
        QStringLiteral("DataRestore/LogText"), QString(), QgsSettings::Section::Plugins).toString();
    if (!logText.isEmpty())
        ui.textEdit_Log->setPlainText(logText);
}

void CSE_DataRestoreDialog::saveState()
{
    QgsSettings settings;
    settings.setValue(QStringLiteral("TimedBackup/SourcePath"), m_qstrSourcePath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("TimedBackup/TargetPath"), m_qstrTargetPath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("TimedBackup/EnableTimedBackup"), m_bEnableTimedBackup, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("TimedBackup/KeepExpired"), m_bKeepExpired, QgsSettings::Section::Plugins);

    settings.setValue(QStringLiteral("DataRestore/BackupDir"), m_qstrBackupDir, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/RestoreDstPath"), m_qstrRestoreDstPath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/OverwriteExisting"), m_bOverwriteExisting, QgsSettings::Section::Plugins);

    settings.setValue(QStringLiteral("DataRestore/DbHost"), ui.lineEdit_backupDbHost->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/DbPort"), ui.lineEdit_backupDbPort->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/DbName"), ui.lineEdit_backupDbName->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/DbSchema"), ui.lineEdit_backupDbSchema->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/DbUser"), ui.lineEdit_backupDbUser->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/DbPassword"), ui.lineEdit_backupDbPassword->text(), QgsSettings::Section::Plugins);

    // 保存数据库备份配置
    settings.setValue(QStringLiteral("DataRestore/BackupDataSource"), static_cast<int>(m_eBackupDataSource), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/BackupDbHost"), ui.lineEdit_backupDbHost->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/BackupDbPort"), ui.lineEdit_backupDbPort->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/BackupDbName"), ui.lineEdit_backupDbName->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/BackupDbSchema"), ui.lineEdit_backupDbSchema->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/BackupDbUser"), ui.lineEdit_backupDbUser->text(), QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("DataRestore/BackupDbPassword"), ui.lineEdit_backupDbPassword->text(), QgsSettings::Section::Plugins);

    // 保存单选状态
    int scopeIdx = m_pScopeGroup ? m_pScopeGroup->checkedId() : 0;
    settings.setValue(QStringLiteral("DataRestore/BackupDbScope"), scopeIdx, QgsSettings::Section::Plugins);

    QString logText = ui.textEdit_Log->toPlainText();
    if (logText.length() > 10000)
        logText = logText.right(10000);
    settings.setValue(QStringLiteral("DataRestore/LogText"), logText, QgsSettings::Section::Plugins);

    // 同步定时备份配置到管理器并持久化
    CMapCheckBackupManager::instance()->setSourcePath(m_qstrSourcePath);
    CMapCheckBackupManager::instance()->setTargetPath(m_qstrTargetPath);
    CMapCheckBackupManager::instance()->setStrategies(m_vecStrategies);
    CMapCheckBackupManager::instance()->setEnabled(m_bEnableTimedBackup);
    CMapCheckBackupManager::instance()->setKeepExpired(m_bKeepExpired);
    CMapCheckBackupManager::instance()->saveSettings();
}

// ========== 日志 ==========

void CSE_DataRestoreDialog::appendLog(const QString& msg)
{
    QString line = QString("[%1] %2")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"), msg);
    ui.textEdit_Log->append(line);

    QTextCursor cursor = ui.textEdit_Log->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui.textEdit_Log->setTextCursor(cursor);
}

void CSE_DataRestoreDialog::setStatusText(const QString& text)
{
	ui.label_status->setText(text);
}

void CSE_DataRestoreDialog::updateBackupSourceVisibility()
{
	bool bShowDb = (m_eBackupDataSource == BackupDataSource::Database
		|| m_eBackupDataSource == BackupDataSource::All);
	bool bShowFile = (m_eBackupDataSource == BackupDataSource::FileSystem
		|| m_eBackupDataSource == BackupDataSource::All);

	// 使用 setVisible + setMaximumHeight(0) + setMinimumHeight(0) 三重组合确保隐藏控件完全折叠
	// 仅靠 setVisible 在 QTabWidget/QGroupBox 等复杂控件上仍可能保留 minimumSizeHint 高度
	// （QTabWidget 的 tabBar、QGroupBox 的标题等），导致布局中留下空白
	if (bShowDb) {
		ui.tabWidget_dbType->setVisible(true);
		ui.tabWidget_dbType->setMaximumHeight(QWIDGETSIZE_MAX);
		ui.tabWidget_dbType->setMinimumHeight(0);
		ui.tabWidget_dbType->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		// 不在这里读 sizeHint() 设置 Minimum——此时缓存可能尚未刷新，留给 applyBackupLayout
		// 统一通过 forceLayoutDeepInvalidate 强制重算后再做最终尺寸锁定。
	} else {
		ui.tabWidget_dbType->setVisible(false);
		ui.tabWidget_dbType->setMaximumHeight(0);
		ui.tabWidget_dbType->setMinimumHeight(0);
		ui.tabWidget_dbType->setFixedHeight(0);
	}

	if (bShowFile) {
		ui.groupBox_path->setVisible(true);
		ui.groupBox_path->setMaximumHeight(QWIDGETSIZE_MAX);
		ui.groupBox_path->setMinimumHeight(0);
	} else {
		ui.groupBox_path->setVisible(false);
		ui.groupBox_path->setMaximumHeight(0);
		ui.groupBox_path->setMinimumHeight(0);
		ui.groupBox_path->setFixedHeight(0);
	}

	// 关键修复：切换数据源后，必须同步刷新表选择控件的状态（避免用户先选过手动模式再切回数据库时残留）
	updateBackupScopeWidgets();

	// 一站式应用布局：深度失效 + 多轮重算 + 锁定窗口高度
	applyBackupLayout();
}

void CSE_DataRestoreDialog::adjustDialogHeightForBackupTab()
{
    // 窗口未显示时各控件尺寸尚未计算，跳过
    if (!isVisible())
        return;

    // 暂时解除尺寸锁定，让窗口能够自适应
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);

    // 关键：在测量之前必须强制所有相关布局彻底失效。
    // 之前单层 invalidate+activate 无法 100% 触达子布局的 sizeHint 缓存，
    // 导致切换数据源后 cachedSizeHint 仍保留上一次「大尺寸」状态。
    // 这里走递归失效 + updateGeometry + 多轮 processEvents，保证下一次查询的是
    // 当前可见控件的真实 sizeHint。
    QWidget* pCurrentTab = ui.tabWidget_main->currentWidget();
    if (pCurrentTab) {
        forceLayoutDeepInvalidate(pCurrentTab);
    }
    forceLayoutDeepInvalidate(ui.tabWidget_dbType);
    if (layout()) {
        layout()->invalidate();
        layout()->activate();
    }
    QApplication::sendPostedEvents(this, QEvent::LayoutRequest);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    int nTargetHeight = calculateHeightForCurrentTab();

    // 限制窗口不超出屏幕可用区域
    QScreen* pScreen = QApplication::primaryScreen();
    if (pScreen)
    {
        int nMaxHeight = pScreen->availableGeometry().height() - 80;
        if (nMaxHeight > 0)
            nTargetHeight = qMin(nTargetHeight, nMaxHeight);
    }

    // 保持当前宽度，将窗口高度精确设置为当前内容高度
    int nTargetWidth = qMax(width(), minimumWidth());
    resize(nTargetWidth, nTargetHeight);

    // 不锁定窗口高度：仅将高度 resize 为当前内容高度作为默认，用户仍可手动上下拉伸窗口
    // （原 setFixedHeight 会把 min/max 高度都锁成同一值，导致窗口高度无法手动调整）
}

int CSE_DataRestoreDialog::calculateHeightForCurrentTab()
{
    int nCurrentIndex = ui.tabWidget_main->currentIndex();
    QWidget* pCurrentTab = ui.tabWidget_main->widget(nCurrentIndex);
    if (!pCurrentTab)
        return height();

    // 多轮 deep invalidation + processEvents，确保各层 sizeHint 缓存稳定到当前可见状态。
    // 反复循环是必须的：单次 invalidate 不一定能穿透到所有子布局。
    for (int nIter = 0; nIter < 5; ++nIter) {
        forceLayoutDeepInvalidate(pCurrentTab);
        forceLayoutDeepInvalidate(ui.tabWidget_dbType);
        if (layout()) {
            layout()->invalidate();
            layout()->activate();
        }
        QApplication::sendPostedEvents(this, QEvent::LayoutRequest);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    // 读取可见控件的真实 sizeHint 高度。
    // 用 widget->sizeHint() 而非 layout()->minimumSize()：前者会重新计算，
    // 后者仍可能携带隐藏控件的旧 minimum（尤其是 listWidget_backupTables 的 80px）。
    int nPageContentHeight = pCurrentTab->sizeHint().height();
    if (pCurrentTab->layout()) {
        // 布局侧的 sizeHint 才是各控件之和，先走一遍嵌套失效再读
        pCurrentTab->layout()->invalidate();
        pCurrentTab->layout()->activate();
        nPageContentHeight = pCurrentTab->layout()->sizeHint().height();
    }

    // QTabWidget 固定开销：tabBar 高度 + 上下边框 + tabBar 与内容区间距
    int nTabBarHeight = ui.tabWidget_main->tabBar()->height();
    int nFrameWidth = style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    int nTabWidgetOverhead = qMax(nTabBarHeight, 28) + nFrameWidth * 2 + 4;

    // 底部状态栏高度
    int nBottomHeight = qMax(ui.label_status->sizeHint().height(), 20);

    // Dialog 整体边距与主布局间距
    QMargins dialogMargins = layout()->contentsMargins();
    int nTotal = nPageContentHeight + nTabWidgetOverhead + nBottomHeight
                 + dialogMargins.top() + dialogMargins.bottom()
                 + layout()->spacing();

    // 最小高度兜底
    return qMax(nTotal, 200);
}

void CSE_DataRestoreDialog::activateLayoutRecursively(QLayout* pLayout) const
{
    if (!pLayout)
        return;

    pLayout->invalidate();
    pLayout->activate();

    for (int i = 0; i < pLayout->count(); ++i)
    {
        QLayoutItem* pItem = pLayout->itemAt(i);
        if (!pItem)
            continue;

        if (QLayout* pChildLayout = pItem->layout())
        {
            activateLayoutRecursively(pChildLayout);
        }
        else if (QWidget* pWidget = pItem->widget())
        {
            if (pWidget->layout())
                activateLayoutRecursively(pWidget->layout());
        }
    }
}

void CSE_DataRestoreDialog::forceLayoutDeepInvalidate(QWidget* pRoot)
{
    if (!pRoot)
        return;

    // 自底向上深度失效。updateGeometry() 标记控件几何信息已变更，
    // 强制 Qt 在下一次布局查询时重新计算 sizeHint，而不是使用残留的缓存。
    std::function<void(QWidget*)> recurse = [&](QWidget* pW) {
        if (!pW)
            return;
        // 先标记自身几何失效
        pW->updateGeometry();
        if (pW->layout()) {
            pW->layout()->invalidate();
        }
        const QObjectList& children = pW->children();
        for (int i = 0; i < children.size(); ++i) {
            QWidget* pChild = qobject_cast<QWidget*>(children.at(i));
            if (pChild) {
                recurse(pChild);
            }
        }
        // 子树失效后再激活一次
        if (pW->layout()) {
            pW->layout()->activate();
        }
    };
    recurse(pRoot);
}

void CSE_DataRestoreDialog::applyBackupLayout()
{
    // 只有在窗口已显示且当前位于备份 Tab 时才生效
    if (!isVisible() || ui.tabWidget_main->currentIndex() != 0)
        return;

    // 1. 先解锁窗口高度锁定，让后面 resize 才能生效
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);

    // 2. 深度失效备份 Tab 内所有相关布局 + 数据库配置 Tab
    QWidget* pCurrentTab = ui.tabWidget_main->currentWidget();
    if (pCurrentTab)
        forceLayoutDeepInvalidate(pCurrentTab);
    if (ui.tabWidget_dbType)
        forceLayoutDeepInvalidate(ui.tabWidget_dbType);
    if (layout()) {
        layout()->invalidate();
        layout()->activate();
    }

    // 3. 让事件循环把任何残留的 LayoutRequest 处理干净，避免下一轮 activate 读到旧缓存
    QApplication::sendPostedEvents(this, QEvent::LayoutRequest);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // 4. 调整高度（其中包含多轮失效 -> processEvents -> sizeHint -> setFixedHeight 流程）
    adjustDialogHeightForBackupTab();
}

void CSE_DataRestoreDialog::updateRestoreTargetHeight()
{
    // 固定堆叠控件高度为所有页面中的最大高度，切换 radio 选项时只改变页面内容，
    // 不改变堆叠控件高度，从而避免上下控件被挤压或产生大片空白
    int nMaxHeight = 0;
    for (int i = 0; i < ui.stackedWidget_restoreTarget->count(); ++i)
    {
        QWidget* pPage = ui.stackedWidget_restoreTarget->widget(i);
        if (!pPage)
            continue;
        if (pPage->layout())
        {
            pPage->layout()->invalidate();
            pPage->layout()->activate();
        }
        nMaxHeight = qMax(nMaxHeight, pPage->sizeHint().height());
    }
    if (nMaxHeight > 0)
        ui.stackedWidget_restoreTarget->setFixedHeight(nMaxHeight + 4);
}

// ========== 备份 Tab 槽函数 ==========

void CSE_DataRestoreDialog::on_Button_SourceBrowse_clicked()
{
    QString curPath = m_qstrSourcePath.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_qstrSourcePath;
    QString selectedDir = QFileDialog::getExistingDirectory(
        this, tr("选择备份源路径"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrSourcePath = selectedDir;
        ui.lineEdit_SourcePath->setText(m_qstrSourcePath);
        appendLog(tr("已选择备份源路径: %1").arg(m_qstrSourcePath));
        setStatusText(tr("备份源路径已设置"));
    }
}

void CSE_DataRestoreDialog::on_Button_TargetBrowse_clicked()
{
    QString curPath = m_qstrTargetPath.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_qstrTargetPath;
    QString selectedDir = QFileDialog::getExistingDirectory(
        this, tr("选择备份目标路径"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrTargetPath = selectedDir;
        ui.lineEdit_TargetPath->setText(m_qstrTargetPath);
        ui.lineEdit_DbTargetPath->setText(m_qstrTargetPath);

        // 同时更新恢复 Tab 的备份目录
        m_qstrBackupDir = selectedDir;
        ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
        refreshBackupList();

        appendLog(tr("已选择备份目标路径: %1").arg(m_qstrTargetPath));
        setStatusText(tr("备份目标路径已设置，备份列表已刷新"));
    }
}

void CSE_DataRestoreDialog::on_Button_AddStrategy_clicked()
{
    TimedBackupStrategy newStrategy;
    newStrategy.eBackupType = BackupType::FullBackup;
    newStrategy.eFrequency = BackupFrequency::Daily;
    newStrategy.strExecuteTime = tr("每日 02:00");
    newStrategy.strRetentionPeriod = tr("保留7天");
    newStrategy.eStorageLocation = StorageLocation::Local;
    newStrategy.bEnabled = true;

    CBackupStrategyEditDialog editDlg(newStrategy, this);
    if (editDlg.exec() == QDialog::Accepted)
    {
        TimedBackupStrategy strategy = editDlg.getStrategy();
        m_vecStrategies.push_back(strategy);
        refreshBackupStrategyTable();
        adjustDialogHeightForBackupTab();
        appendLog(tr("已添加备份策略: %1").arg(strategy.backupTypeName()));
        setStatusText(tr("新策略已添加"));
    }
}

void CSE_DataRestoreDialog::on_Button_EditStrategy_clicked()
{
    int row = ui.tableWidget_backup_strategy->currentRow();
    if (row < 0 || row >= static_cast<int>(m_vecStrategies.size()))
    {
        QMessageBox::warning(this, tr("提示"), tr("请先选择一条策略进行编辑。"));
        return;
    }

    TimedBackupStrategy& strategy = m_vecStrategies[row];
    CBackupStrategyEditDialog editDlg(strategy, this);
    if (editDlg.exec() == QDialog::Accepted)
    {
        strategy = editDlg.getStrategy();
        refreshBackupStrategyTable();
        adjustDialogHeightForBackupTab();
        appendLog(tr("已编辑策略: %1").arg(strategy.backupTypeName()));
        setStatusText(tr("策略已更新"));
    }
}

void CSE_DataRestoreDialog::on_Button_DeleteStrategy_clicked()
{
    int row = ui.tableWidget_backup_strategy->currentRow();
    if (row < 0 || row >= static_cast<int>(m_vecStrategies.size()))
    {
        QMessageBox::warning(this, tr("提示"), tr("请先选择一条策略进行删除。"));
        return;
    }

    TimedBackupStrategy strategy = m_vecStrategies[row];
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("确认删除"),
        tr("确定要删除备份策略 \"%1\" 吗？").arg(strategy.backupTypeName()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes)
    {
        m_vecStrategies.erase(m_vecStrategies.begin() + row);
        refreshBackupStrategyTable();
        adjustDialogHeightForBackupTab();
        appendLog(tr("已删除策略: %1").arg(strategy.backupTypeName()));
        setStatusText(tr("策略已删除"));
    }
}

void CSE_DataRestoreDialog::on_Button_ImmediateBackup_clicked()
{
    if (m_qstrTargetPath.isEmpty())
    {
        QMessageBox::warning(this, tr("路径错误"), tr("请先设置备份目标路径！"));
        return;
    }

    // 构建策略
    TimedBackupStrategy strategy;
    int row = ui.tableWidget_backup_strategy->currentRow();
    if (row < 0 || row >= static_cast<int>(m_vecStrategies.size()))
    {
        QStringList items;
        items << tr("全量备份") << tr("增量备份") << tr("WAL归档") << tr("逻辑导出备份") << tr("数据库备份");
        bool ok = false;
        QString selected = QInputDialog::getItem(
            this, tr("选择备份类型"), tr("请选择要执行的备份类型:"),
            items, 0, false, &ok);
        if (!ok || selected.isEmpty())
            return;

        if (selected == tr("全量备份"))
        { strategy.eBackupType = BackupType::FullBackup; strategy.eFrequency = BackupFrequency::Weekly; }
        else if (selected == tr("增量备份"))
        { strategy.eBackupType = BackupType::IncrementalBackup; strategy.eFrequency = BackupFrequency::Daily; }
        else if (selected == tr("WAL归档"))
        { strategy.eBackupType = BackupType::WALArchive; strategy.eFrequency = BackupFrequency::Hourly; }
        else if (selected == tr("逻辑导出备份"))
        { strategy.eBackupType = BackupType::LogicalExportBackup; strategy.eFrequency = BackupFrequency::Monthly; }
        else
        { strategy.eBackupType = BackupType::DatabaseBackup; strategy.eFrequency = BackupFrequency::Daily; }
        strategy.strExecuteTime = tr("立即执行");
        strategy.strRetentionPeriod = tr("保留4周");
        strategy.eStorageLocation = StorageLocation::Local;
        strategy.bEnabled = true;
        strategy.eDataSource = m_eBackupDataSource;
    }
    else
    {
        strategy = m_vecStrategies[row];
    }

    // 如果是从UI设置的数据库备份模式覆盖数据源
    if (m_eBackupDataSource != BackupDataSource::FileSystem)
    {
        strategy.eDataSource = m_eBackupDataSource;
        strategy.eBackupType = BackupType::DatabaseBackup;
    }

    // 设置数据库连接信息（从 UI 控件读取）
    if (strategy.eDataSource == BackupDataSource::Database || strategy.eDataSource == BackupDataSource::All)
    {
        strategy.eDbType = DatabaseType::PostgreSQL; // 当前仅支持 PostgreSQL
        strategy.strDbHost = ui.lineEdit_backupDbHost->text().trimmed();
        strategy.nDbPort = ui.lineEdit_backupDbPort->text().trimmed().toInt();
        if (strategy.nDbPort <= 0) strategy.nDbPort = 5432;
        strategy.strDbName = ui.lineEdit_backupDbName->text().trimmed();
        strategy.strDbUser = ui.lineEdit_backupDbUser->text().trimmed();
        strategy.strDbPassword = ui.lineEdit_backupDbPassword->text();
        strategy.strDbSchema = ui.lineEdit_backupDbSchema->text().trimmed();
        if (strategy.strDbSchema.isEmpty()) strategy.strDbSchema = "public";

        // 备份范围（单选）
        if (ui.radioButton_BackupAllData->isChecked())
            strategy.eDbBackupScope = DatabaseBackupScope::WholeDatabase;
        else
            strategy.eDbBackupScope = DatabaseBackupScope::SelectedTables;

        // 选中的表
        strategy.lstDbTables.clear();
        for (int i = 0; i < ui.listWidget_backupTables->count(); ++i)
        {
            if (ui.listWidget_backupTables->item(i)->checkState() == Qt::Checked)
                strategy.lstDbTables.append(ui.listWidget_backupTables->item(i)->text());
        }

        // 备份方式固定为全量备份
        strategy.eDbBackupMethod = DatabaseBackupMethod::FullBackup;

        // 备份格式固定为 .sql
        strategy.eDbBackupFormat = DatabaseBackupFormat::SQL;
    }

    // 文件系统备份检查
    if ((strategy.eDataSource == BackupDataSource::FileSystem || strategy.eDataSource == BackupDataSource::All) &&
        m_qstrSourcePath.isEmpty())
    {
        QMessageBox::warning(this, tr("路径错误"), tr("请先设置备份源路径！"));
        return;
    }

    CMapCheckBackupManager::instance()->clearLastError();
    CMapCheckBackupManager::instance()->setSourcePath(m_qstrSourcePath);
    CMapCheckBackupManager::instance()->setTargetPath(m_qstrTargetPath);
    CMapCheckBackupManager::instance()->setKeepExpired(m_bKeepExpired);

    appendLog(tr("开始执行备份: %1 ...").arg(strategy.backupTypeName()));
    setStatusText(tr("正在执行备份..."));

    bool result = CMapCheckBackupManager::instance()->executeBackup(strategy);
    if (result)
    {
        appendLog(tr("备份成功: %1 -> %2").arg(strategy.backupTypeName(), m_qstrTargetPath));
        QMessageBox::information(this, tr("备份完成"), tr("备份成功!"));
        setStatusText(tr("备份完成"));

        // 自动刷新备份列表
        m_qstrBackupDir = m_qstrTargetPath;
        ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
        refreshBackupList();
    }
    else
    {
        QString errorDetail = CMapCheckBackupManager::instance()->lastErrorString();
        if (errorDetail.isEmpty())
        {
            errorDetail = tr("未知错误，请查看日志获取详细信息。");
        }

        if (strategy.eDataSource == BackupDataSource::Database)
        {
            appendLog(tr("数据库备份失败: %1").arg(errorDetail));
            QMessageBox::warning(this, tr("备份失败"), tr("数据库备份失败。"));
            appendLog(tr("具体原因: %1").arg(errorDetail));
        }
        else
        {
            appendLog(tr("备份失败: %1").arg(errorDetail));
            QMessageBox::warning(this, tr("备份失败"), tr("备份失败。"));
            appendLog(tr("具体原因: %1").arg(errorDetail));
        }
        setStatusText(tr("备份失败"));
    }
}



void CSE_DataRestoreDialog::on_lineEdit_SourcePath_textChanged(const QString& text)
{
    m_qstrSourcePath = text;
}

void CSE_DataRestoreDialog::on_lineEdit_TargetPath_textChanged(const QString& text)
{
    m_qstrTargetPath = text;
    if (ui.lineEdit_DbTargetPath->text() != text)
    {
        ui.lineEdit_DbTargetPath->blockSignals(true);
        ui.lineEdit_DbTargetPath->setText(text);
        ui.lineEdit_DbTargetPath->blockSignals(false);
    }
}

void CSE_DataRestoreDialog::on_Button_DbTargetBrowse_clicked()
{
    QString curPath = m_qstrTargetPath.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_qstrTargetPath;
    QString selectedDir = QFileDialog::getExistingDirectory(
        this, tr("选择数据库备份保存路径"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrTargetPath = selectedDir;
        ui.lineEdit_TargetPath->setText(m_qstrTargetPath);
        ui.lineEdit_DbTargetPath->setText(m_qstrTargetPath);

        // 同时更新恢复 Tab 的备份目录
        m_qstrBackupDir = selectedDir;
        ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
        refreshBackupList();

        appendLog(tr("已选择数据库备份保存路径: %1").arg(m_qstrTargetPath));
        setStatusText(tr("数据库备份保存路径已设置"));
    }
}

void CSE_DataRestoreDialog::on_lineEdit_DbTargetPath_textChanged(const QString& text)
{
    m_qstrTargetPath = text;
    if (ui.lineEdit_TargetPath->text() != text)
    {
        ui.lineEdit_TargetPath->blockSignals(true);
        ui.lineEdit_TargetPath->setText(text);
        ui.lineEdit_TargetPath->blockSignals(false);
    }
}

void CSE_DataRestoreDialog::on_checkBox_EnableTimedBackup_stateChanged(int state)
{
    m_bEnableTimedBackup = (state == Qt::Checked);
    if (m_bEnableTimedBackup)
        appendLog(tr("定时备份功能已启用。"));
    else
        appendLog(tr("定时备份功能已禁用。"));
}

void CSE_DataRestoreDialog::on_checkBox_KeepExpired_stateChanged(int state)
{
    m_bKeepExpired = (state == Qt::Checked);
}

// ========== 恢复 Tab 槽函数 ==========

void CSE_DataRestoreDialog::on_Button_BrowseBackupDir_clicked()
{
    QString curPath = m_qstrBackupDir.isEmpty()
        ? (m_qstrTargetPath.isEmpty() ? QCoreApplication::applicationDirPath() : m_qstrTargetPath)
        : m_qstrBackupDir;
    QString selectedDir = QFileDialog::getExistingDirectory(
        this, tr("选择备份存放目录"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrBackupDir = selectedDir;
        ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
        refreshBackupList();
        appendLog(tr("已选择备份目录: %1").arg(m_qstrBackupDir));
        setStatusText(tr("备份目录已更新，列表已刷新"));
    }
}

void CSE_DataRestoreDialog::on_Button_BrowseRestoreOriginal_clicked()
{
    QString curPath = m_qstrSourcePath.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_qstrSourcePath;
    QString selectedDir = QFileDialog::getExistingDirectory(
        this, tr("选择原始位置路径"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrSourcePath = selectedDir;
        ui.lineEdit_RestoreOriginalPath->setText(m_qstrSourcePath);
        appendLog(tr("已选择原始位置路径: %1").arg(m_qstrSourcePath));
    }
}

void CSE_DataRestoreDialog::on_lineEdit_RestoreOriginalPath_textChanged(const QString& text)
{
    m_qstrSourcePath = text;
}

void CSE_DataRestoreDialog::on_lineEdit_RestoreDstPath_textChanged(const QString& text)
{
    m_qstrRestoreDstPath = text;
}

void CSE_DataRestoreDialog::on_Button_BrowseRestoreTarget_clicked()
{
    QString curPath = m_qstrRestoreDstPath.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_qstrRestoreDstPath;
    QString selectedDir = QFileDialog::getExistingDirectory(
        this, tr("选择恢复到目录"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrRestoreDstPath = selectedDir;
        ui.lineEdit_RestoreDstPath->setText(m_qstrRestoreDstPath);
        appendLog(tr("已选择恢复目标目录: %1").arg(m_qstrRestoreDstPath));
    }
}

void CSE_DataRestoreDialog::on_Button_RefreshBackupList_clicked()
{
    if (m_qstrBackupDir.isEmpty())
    {
        // 尝试使用备份目标路径
        if (!m_qstrTargetPath.isEmpty())
        {
            m_qstrBackupDir = m_qstrTargetPath;
            ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
        }
        else
        {
            QMessageBox::information(this, tr("提示"),
                tr("请先在\"备份\"标签页设置备份目标路径，或在此处浏览选择备份目录。"));
            return;
        }
    }
    refreshBackupList();
    appendLog(tr("备份列表已刷新"));
    setStatusText(tr("备份列表已刷新"));
}

void CSE_DataRestoreDialog::on_Button_Restore_clicked()
{
	int row = ui.tableWidget_BackupList->currentRow();
	if (row < 0)
	{
		QMessageBox::warning(this, tr("提示"), tr("请先在备份列表中选择一个备份记录。"));
		return;
	}

	// 获取当前筛选后的备份记录
	QString filterText = ui.comboBox_BackupTypeFilter->currentText();
	vector<BackupRecord> filtered;
	for (const auto& r : m_vecBackupRecords)
	{
		if (filterText == tr("全部类型") || r.backupTypeName() == filterText)
			filtered.push_back(r);
	}

	if (row >= static_cast<int>(filtered.size()))
		return;

	const BackupRecord& record = filtered[row];
	bool result = false;
	QString restoreTargetDesc;

	if (ui.radioButton_RestoreToDatabase->isChecked())
	{
		// ===== 恢复到数据库 =====
		QString host = ui.lineEdit_dbHost->text().trimmed();
		QString portStr = ui.lineEdit_DbPort->text().trimmed();
		QString dbName = ui.lineEdit_DbName->text().trimmed();
		QString username = ui.lineEdit_DbUser->text().trimmed();
		QString password = ui.lineEdit_DbPassword->text();
		QString schema = ui.lineEdit_dbSchema->text().trimmed();

		if (host.isEmpty() || dbName.isEmpty() || username.isEmpty())
		{
			QMessageBox::warning(this, tr("配置不完整"), tr("请填写完整的数据库连接信息。"));
			return;
		}
		int port = portStr.isEmpty() ? 5432 : portStr.toInt();
		if (schema.isEmpty()) schema = "public";

		restoreTargetDesc = QString("数据库 %1@%2:%3/%4").arg(username, host).arg(port).arg(dbName);

		QString confirmMsg = tr("确认恢复备份 \"%1\" ?").arg(record.strName);

		QMessageBox::StandardButton reply = QMessageBox::question(
			this, tr("确认恢复"), confirmMsg,
			QMessageBox::Yes | QMessageBox::No);

		if (reply != QMessageBox::Yes)
			return;

		appendLog(tr("开始恢复备份到数据库: %1 -> %2").arg(record.strName, restoreTargetDesc));
		setStatusText(tr("正在恢复数据到数据库..."));

		result = performRestoreToDatabase(record, host, port, dbName, username, password, schema, m_bOverwriteExisting);
	}
	else
	{
		// ===== 恢复到文件系统 =====
		QString dstPath;
		if (ui.radioButton_RestoreToOriginal->isChecked())
		{
			dstPath = ui.lineEdit_RestoreOriginalPath->text();
			if (dstPath.isEmpty())
			{
				QMessageBox::warning(this, tr("路径错误"), tr("请先设置原始位置路径！"));
				return;
			}
		}
		else
		{
			dstPath = ui.lineEdit_RestoreDstPath->text();
			if (dstPath.isEmpty())
			{
				QMessageBox::warning(this, tr("路径错误"), tr("请先设置恢复到目录！"));
				return;
			}
		}
		restoreTargetDesc = dstPath;

		QString confirmMsg = tr("确认将备份 \"%1\" 恢复到 %2 ?")
			.arg(record.strName, dstPath);

		QMessageBox::StandardButton reply = QMessageBox::question(
			this, tr("确认恢复"), confirmMsg,
			QMessageBox::Yes | QMessageBox::No);

		if (reply != QMessageBox::Yes)
			return;

		appendLog(tr("开始恢复备份: %1 -> %2").arg(record.strName, dstPath));
		setStatusText(tr("正在恢复数据..."));

		result = performRestore(record, dstPath, m_bOverwriteExisting);
	}

	if (result)
	{
		appendLog(tr("恢复成功: %1 已恢复到 %2").arg(record.strName, restoreTargetDesc));
		QMessageBox::information(this, tr("恢复完成"), tr("数据恢复成功!"));
		setStatusText(tr("恢复完成"));
	}
	else
	{
		appendLog(tr("恢复失败: %1").arg(record.strName));
			QMessageBox::warning(this, tr("恢复失败"), tr("恢复失败"));
		setStatusText(tr("恢复失败"));
	}
}

void CSE_DataRestoreDialog::on_Button_DeleteBackup_clicked()
{
    int row = ui.tableWidget_BackupList->currentRow();
    if (row < 0)
    {
        QMessageBox::warning(this, tr("提示"), tr("请先在备份列表中选择一个备份记录。"));
        return;
    }

    QString filterText = ui.comboBox_BackupTypeFilter->currentText();
    vector<BackupRecord> filtered;
    for (const auto& r : m_vecBackupRecords)
    {
        if (filterText == tr("全部类型") || r.backupTypeName() == filterText)
            filtered.push_back(r);
    }

    if (row >= static_cast<int>(filtered.size()))
        return;

    const BackupRecord& record = filtered[row];

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("确认删除"),
        tr("确定要永久删除备份 \"%1\" 吗？").arg(record.strName),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    QDir backupDir(record.strFullPath);
    if (backupDir.removeRecursively())
    {
        appendLog(tr("已删除备份: %1").arg(record.strName));
        setStatusText(tr("备份已删除"));
        refreshBackupList();
    }
    else
    {
        QMessageBox::warning(this, tr("删除失败"), tr("删除失败"));
        appendLog(tr("删除备份失败: %1").arg(record.strName));
    }
}

void CSE_DataRestoreDialog::on_Button_RestoreApply_clicked()
{
    saveState();
    appendLog(tr("恢复配置已保存。"));
    appendLog(tr("- 备份目录: %1").arg(m_qstrBackupDir));

    QString restoreTarget;
    if (ui.radioButton_RestoreToOriginal->isChecked())
        restoreTarget = ui.lineEdit_RestoreOriginalPath->text();
    else if (ui.radioButton_RestoreToCustom->isChecked())
        restoreTarget = ui.lineEdit_RestoreDstPath->text();
    else if (ui.radioButton_RestoreToDatabase->isChecked())
        restoreTarget = QString("数据库 %1@%2/%3").arg(ui.lineEdit_backupDbUser->text(), ui.lineEdit_backupDbHost->text(), ui.lineEdit_backupDbName->text());

    appendLog(tr("- 恢复目标: %1").arg(restoreTarget));
    appendLog(tr("- 覆盖已存在文件: %1").arg(m_bOverwriteExisting ? tr("是") : tr("否")));
    setStatusText(tr("恢复配置已保存"));
    QMessageBox::information(this, tr("配置已保存"), tr("配置已保存"));
}

void CSE_DataRestoreDialog::on_comboBox_BackupTypeFilter_currentIndexChanged(int /*index*/)
{
    refreshBackupList();
}

void CSE_DataRestoreDialog::on_radioButton_RestoreToOriginal_toggled(bool checked)
{
    if (checked)
    {
        ui.stackedWidget_restoreTarget->setCurrentIndex(0);
        updateRestoreTargetHeight();
        adjustDialogHeightForBackupTab();
    }
}

void CSE_DataRestoreDialog::on_radioButton_RestoreToCustom_toggled(bool checked)
{
    if (checked)
    {
        ui.stackedWidget_restoreTarget->setCurrentIndex(1);
        updateRestoreTargetHeight();
        adjustDialogHeightForBackupTab();
    }
}

void CSE_DataRestoreDialog::on_radioButton_RestoreToDatabase_toggled(bool checked)
{
    if (checked)
    {
        ui.stackedWidget_restoreTarget->setCurrentIndex(2);
        updateRestoreTargetHeight();
        adjustDialogHeightForBackupTab();
    }
}

void CSE_DataRestoreDialog::on_checkBox_OverwriteExisting_stateChanged(int state)
{
    m_bOverwriteExisting = (state == Qt::Checked);
}

// ========== 恢复核心操作 ==========

bool CSE_DataRestoreDialog::performRestore(const BackupRecord& record, const QString& dstPath, bool overwrite)
{
    QDir srcDir(record.strFullPath);
    if (!srcDir.exists())
    {
        appendLog(tr("备份目录不存在: %1").arg(record.strFullPath));
        return false;
    }

    QDir dstDir(dstPath);
    if (!dstDir.exists())
    {
        if (!dstDir.mkpath(dstPath))
        {
            appendLog(tr("创建目标目录失败: %1").arg(dstPath));
            return false;
        }
    }

    return restoreDirectory(srcDir, dstDir, overwrite);
}

bool CSE_DataRestoreDialog::restoreDirectory(const QDir& srcDir, const QDir& dstDir, bool overwrite)
{
    if (!srcDir.exists())
        return false;

    if (!dstDir.exists())
        dstDir.mkpath(dstDir.absolutePath());

    QFileInfoList fileInfoList = srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);
    for (const QFileInfo& fileInfo : fileInfoList)
    {
        QString dstFilePath = dstDir.absolutePath() + QDir::separator() + fileInfo.fileName();

        if (fileInfo.isDir())
        {
            QDir subSrcDir(fileInfo.filePath());
            QDir subDstDir(dstFilePath);
            if (!restoreDirectory(subSrcDir, subDstDir, overwrite))
                return false;
        }
        else
        {
            QFileInfo dstFileInfo(dstFilePath);

            // 如果目标文件已存在
            if (dstFileInfo.exists())
            {
                if (!overwrite)
                {
                    // 不覆盖，跳过
                    continue;
                }
                // 覆盖：先删除再复制
                QFile::remove(dstFilePath);
            }

            if (!QFile::copy(fileInfo.filePath(), dstFilePath))
            {
                appendLog(tr("恢复文件失败: %1 -> %2")
                    .arg(fileInfo.filePath(), dstFilePath));
                return false;
            }
        }
    }
    // 所有文件复制完成 = 恢复成功（缺失此 return 是未定义行为：MSVC 碰巧返回残留
    // 值通过、GCC 下返回不可控垃圾值导致麒麟闪退/结果不定）
    return true;
}

bool CSE_DataRestoreDialog::performRestoreToDatabase(const BackupRecord& record,
	const QString& host, int port, const QString& dbName,
	const QString& username, const QString& password,
	const QString& schema, bool overwrite)
{
	QDir srcDir(record.strFullPath);
	if (!srcDir.exists())
	{
		appendLog(tr("备份目录不存在: %1").arg(record.strFullPath));
		return false;
	}

	// 先检查是否是数据库备份（包含 .sql/.dump/.sql.gz 文件）
	QFileInfoList allFiles = srcDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
	bool hasDbBackupFiles = false;
	for (const QFileInfo& fi : allFiles)
	{
		if (fi.suffix().compare("dump", Qt::CaseInsensitive) == 0 ||
			fi.suffix().compare("sql", Qt::CaseInsensitive) == 0 ||
			fi.fileName().endsWith(".sql.gz", Qt::CaseInsensitive))
		{
			hasDbBackupFiles = true;
			break;
		}
	}

	if (hasDbBackupFiles)
	{
		// 使用 pg_restore / psql 恢复数据库备份文件
		appendLog(tr("检测到数据库备份文件，使用 pg_restore/psql 恢复..."));
		return CMapCheckBackupManager::instance()->restoreBackupToDatabase(
			record.strFullPath, host, port, dbName, username, password, schema, overwrite);
	}

	// 原有的 SHP 文件导入逻辑
	// 遍历备份目录中所有 .shp 文件
	QStringList shpFiles;
	QFileInfoList fileInfoList = srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);
	for (const QFileInfo& fileInfo : fileInfoList)
	{
		if (fileInfo.isDir())
		{
			// 递归查找子目录中的 .shp
			QDir subDir(fileInfo.filePath());
			QFileInfoList subFiles = subDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files);
			for (const QFileInfo& subFile : subFiles)
			{
				if (subFile.suffix().compare("shp", Qt::CaseInsensitive) == 0)
					shpFiles.append(subFile.absoluteFilePath());
			}
		}
		else if (fileInfo.suffix().compare("shp", Qt::CaseInsensitive) == 0)
		{
			shpFiles.append(fileInfo.absoluteFilePath());
		}
	}

	if (shpFiles.isEmpty())
	{
		appendLog(tr("备份目录中未找到 .shp 文件或数据库备份文件，无法恢复到数据库。"));
		return false;
	}

	appendLog(tr("发现 %1 个 .shp 文件待导入...").arg(shpFiles.size()));

	bool allSuccess = true;
	for (const QString& shpPath : shpFiles)
	{
		QFileInfo fi(shpPath);
		QString tableName = fi.baseName();
		// 清理表名，去除不合法字符
		tableName.replace(QRegExp("[^a-zA-Z0-9_]"), "_");
		if (tableName.at(0).isDigit()) tableName.prepend("t_");

		appendLog(tr("  正在导入: %1 -> %2.%3").arg(fi.fileName(), schema, tableName));
		if (!importFileToPostGIS(shpPath, host, port, dbName, username, password, schema, tableName, overwrite))
		{
			appendLog(tr("  ✗ 导入失败: %1").arg(fi.fileName()));
			allSuccess = false;
		}
		else
		{
			appendLog(tr("  ✓ 导入成功: %1.%2").arg(schema, tableName));
		}
	}

	return allSuccess;
}

bool CSE_DataRestoreDialog::importFileToPostGIS(const QString& filePath,
	const QString& host, int port, const QString& dbName,
	const QString& username, const QString& password,
	const QString& schema, const QString& tableName, bool overwrite)
{
	// 使用 QgsVectorFileWriter 将 SHP 文件导入到 PostGIS
	QgsVectorLayer* shpLayer = new QgsVectorLayer(filePath, QFileInfo(filePath).baseName(), "ogr");
	if (!shpLayer || !shpLayer->isValid())
	{
		delete shpLayer;
		appendLog(tr("  ✗ 无法加载 SHP 文件: %1").arg(filePath));
		return false;
	}

	// 构建 PostGIS URI
	QString postgisUri = QString("dbname='%1' host=%2 port=%3 user='%4' password='%5' sslmode=disable type=%6 schema=\"%7\" table=\"%8\" (geom)")
		.arg(dbName, host, QString::number(port), username, password)
		.arg(QgsWkbTypes::displayString(shpLayer->wkbType()), schema, tableName);

	// 如果覆盖，先删除已有表
	if (overwrite)
	{
		QString connName = QString("restore_tmp_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
		{
			QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
			db.setHostName(host);
			db.setPort(port);
			db.setDatabaseName(dbName);
			db.setUserName(username);
			db.setPassword(password);
			if (db.open())
			{
				QSqlQuery query(db);
				query.exec(QString("DROP TABLE IF EXISTS \"%1\".\"%2\" CASCADE").arg(schema, tableName));
				db.close();
			}
		}
		QSqlDatabase::removeDatabase(connName);
	}
	else
	{
		// 不覆盖时先检查表是否已存在
		QString connName = QString("restore_check_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
		bool tableExists = false;
		{
			QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
			db.setHostName(host);
			db.setPort(port);
			db.setDatabaseName(dbName);
			db.setUserName(username);
			db.setPassword(password);
			if (db.open())
			{
				QSqlQuery query(db);
				if (query.exec(QString("SELECT 1 FROM information_schema.tables WHERE table_schema='%1' AND table_name='%2'").arg(schema, tableName)) && query.next())
				{
					tableExists = true;
				}
				db.close();
			}
		}
		QSqlDatabase::removeDatabase(connName);
		if (tableExists)
		{
			appendLog(tr("  ⊘ 表已存在，跳过: %1.%2").arg(schema, tableName));
			delete shpLayer;
			return true;
		}
	}

	QgsVectorFileWriter::SaveVectorOptions options;
	options.driverName = "PostgreSQL";
	options.layerName = tableName;
	options.fileEncoding = "UTF-8";

	QString errorMsg;
	QgsVectorFileWriter::WriterError error = QgsVectorFileWriter::writeAsVectorFormatV2(
		shpLayer, postgisUri, QgsProject::instance()->transformContext(), options,
		nullptr, &errorMsg);

	delete shpLayer;

	if (error != QgsVectorFileWriter::NoError)
	{
		appendLog(tr("  ✗ 写入 PostGIS 失败: %1").arg(errorMsg.isEmpty() ? tr("未知错误") : errorMsg));
		return false;
	}

    return true;
}

void CSE_DataRestoreDialog::on_Button_TestDBConnection_clicked()
{
	// 数据恢复 → 恢复到数据库 子页面里的"测试连接"
	QString host = ui.lineEdit_dbHost->text().trimmed();
	QString portStr = ui.lineEdit_DbPort->text().trimmed();
	QString dbName = ui.lineEdit_DbName->text().trimmed();
	QString username = ui.lineEdit_DbUser->text().trimmed();
	QString password = ui.lineEdit_DbPassword->text();

	if (host.isEmpty() || dbName.isEmpty() || username.isEmpty())
	{
			QMessageBox::warning(this, tr("配置不完整"), tr("请填写完整的数据库连接信息。"));
		return;
	}

	int port = portStr.isEmpty() ? 5432 : portStr.toInt();
	QString connName = QString("restore_db_test_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);

	QApplication::setOverrideCursor(Qt::WaitCursor);
	{
		QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
		db.setHostName(host);
		db.setPort(port);
		db.setDatabaseName(dbName);
		db.setUserName(username);
		db.setPassword(password);

		if (db.open())
		{
			appendLog(tr("数据恢复-测试连接成功：%1@%2:%3/%4")
				.arg(username, host).arg(port).arg(dbName));
			setStatusText(tr("数据库连接成功"));
			QMessageBox::information(this, tr("连接测试"), tr("数据库连接成功!"));
			db.close();
		}
		else
		{
			QString err = db.lastError().text();
			appendLog(tr("数据恢复-测试连接失败：%1").arg(err));
			setStatusText(tr("数据库连接失败"));
			QMessageBox::critical(this, tr("连接测试"), tr("连接失败"));
		}
	}
	QSqlDatabase::removeDatabase(connName);
	QApplication::restoreOverrideCursor();
}

void CSE_DataRestoreDialog::on_Button_TestRestoreDbConnection_clicked()
{
	// 自动备份 标签页里的"测试连接"
	QString host = ui.lineEdit_backupDbHost->text().trimmed();
	QString portStr = ui.lineEdit_backupDbPort->text().trimmed();
	QString dbName = ui.lineEdit_backupDbName->text().trimmed();
	QString username = ui.lineEdit_backupDbUser->text().trimmed();
	QString password = ui.lineEdit_backupDbPassword->text();

	if (host.isEmpty() || dbName.isEmpty() || username.isEmpty())
	{
			QMessageBox::warning(this, tr("配置不完整"), tr("请填写完整的数据库连接信息。"));
		return;
	}

	int port = portStr.isEmpty() ? 5432 : portStr.toInt();
	QString connName = QString("restore_test_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);

	QApplication::setOverrideCursor(Qt::WaitCursor);
	{
		QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
		db.setHostName(host);
		db.setPort(port);
		db.setDatabaseName(dbName);
		db.setUserName(username);
		db.setPassword(password);

		if (db.open())
		{
			QMessageBox::information(this, tr("连接测试"), tr("数据库连接成功!"));
			db.close();
		}
		else
		{
			QMessageBox::critical(this, tr("连接测试"), tr("数据库连接失败!"));
		}
	}
	QSqlDatabase::removeDatabase(connName);
	QApplication::restoreOverrideCursor();
}

// ========== 日志 Tab 槽函数 ==========

void CSE_DataRestoreDialog::on_Button_ClearLog_clicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("确认清空"), tr("确定要清空所有日志吗？"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes)
    {
        ui.textEdit_Log->clear();
        appendLog(tr("日志已清空"));
        setStatusText(tr("日志已清空"));
    }
}

void CSE_DataRestoreDialog::on_Button_ExportLog_clicked()
{
    QString defaultFileName = QString("backup_restore_log_%1.txt")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString filePath = QFileDialog::getSaveFileName(
        this, tr("导出日志"), defaultFileName,
        tr("文本文件 (*.txt);;所有文件 (*)"));
    if (filePath.isEmpty())
        return;

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QTextStream stream(&file);
        stream << ui.textEdit_Log->toPlainText();
        file.close();
        appendLog(tr("日志已导出到: %1").arg(filePath));
        setStatusText(tr("日志已导出"));
        QMessageBox::information(this, tr("导出成功"), tr("日志已成功导出!"));
    }
    else
    {
        QMessageBox::warning(this, tr("导出失败"), tr("日志导出失败，无法写入文件。"));
    }
}

void CSE_DataRestoreDialog::on_tabWidget_main_currentChanged(int index)
{
    if (index == 0) // 自动备份 Tab
    {
        updateBackupSourceVisibility();
    }
    else if (index == 1) // 数据恢复 Tab
    {
        // 如果没有设置备份目录，尝试使用目标路径
        if (m_qstrBackupDir.isEmpty() && !m_qstrTargetPath.isEmpty())
        {
            m_qstrBackupDir = m_qstrTargetPath;
            ui.lineEdit_BackupDir->setText(m_qstrBackupDir);
        }
        if (!m_qstrBackupDir.isEmpty())
        {
            refreshBackupList();
        }
        updateRestoreTargetHeight();
    }

    // 切换 Tab 后按当前页面内容自适应窗口高度
    adjustDialogHeightForBackupTab();
}

// ========== 数据库备份配置槽函数 ==========

void CSE_DataRestoreDialog::on_radioButton_SourceFile_toggled(bool checked)
{
	if (checked)
	{
		m_eBackupDataSource = BackupDataSource::FileSystem;
		updateBackupSourceVisibility();
		// 同步表选择控件状态（确保源切换时上一轮残留的可见性被重置）
		updateBackupScopeWidgets();
		applyBackupLayout();
		appendLog(tr("备份数据源设置为：文件夹源文件"));
	}
}

void CSE_DataRestoreDialog::on_radioButton_SourceDatabase_toggled(bool checked)
{
	if (checked)
	{
		m_eBackupDataSource = BackupDataSource::Database;
		updateBackupSourceVisibility();
		// 同步表选择控件状态（确保源切换时上一轮残留的可见性被重置）
		updateBackupScopeWidgets();
		applyBackupLayout();
		appendLog(tr("备份数据源设置为：连接数据库"));
	}
}

void CSE_DataRestoreDialog::on_radioButton_SourceAll_toggled(bool checked)
{
	if (checked)
	{
		m_eBackupDataSource = BackupDataSource::All;
		updateBackupSourceVisibility();
		// 同步表选择控件状态（确保源切换时上一轮残留的可见性被重置）
		updateBackupScopeWidgets();
		applyBackupLayout();
		appendLog(tr("备份数据源设置为：全部(文件夹+数据库)"));
	}
}

void CSE_DataRestoreDialog::on_Button_GetBackupDbList_clicked()
{
    // 连接数据库并获取表列表
    QString host = ui.lineEdit_backupDbHost->text().trimmed();
    QString portStr = ui.lineEdit_backupDbPort->text().trimmed();
    QString dbName = ui.lineEdit_backupDbName->text().trimmed();
    QString username = ui.lineEdit_backupDbUser->text().trimmed();
    QString password = ui.lineEdit_backupDbPassword->text();
    QString schema = ui.lineEdit_backupDbSchema->text().trimmed();

    if (host.isEmpty() || dbName.isEmpty() || username.isEmpty())
    {
        QMessageBox::warning(this, tr("配置不完整"),
            tr("请先填写数据库连接信息并测试连接。"));
        return;
    }

    int port = portStr.isEmpty() ? 5432 : portStr.toInt();
    if (schema.isEmpty()) schema = "public";

    QString connName = QString("backup_db_list_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    ui.listWidget_backupTables->clear();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
        db.setHostName(host);
        db.setPort(port);
        db.setDatabaseName(dbName);
        db.setUserName(username);
        db.setPassword(password);

        if (db.open())
        {
            QSqlQuery query(db);
            QString sql = QString(
                "SELECT table_name FROM information_schema.tables "
                "WHERE table_schema='%1' AND table_type='BASE TABLE' "
                "ORDER BY table_name").arg(schema);

            if (query.exec(sql))
            {
                int tableCount = 0;
                while (query.next())
                {
                    QString tableName = query.value(0).toString();
                    QListWidgetItem* item = new QListWidgetItem(tableName);
                    item->setCheckState(Qt::Checked);
                    ui.listWidget_backupTables->addItem(item);
                    ++tableCount;
                }
                appendLog(tr("成功获取数据库表列表：Schema '%1' 共 %2 个表").arg(schema).arg(tableCount));
                setStatusText(tr("已加载 %1 个数据库表").arg(tableCount));

                // 加载成功后，自动切到"手动选择备份"模式，让用户能直接选择要备份的表
                if (tableCount > 0)
                {
                    ui.radioButton_BackupSelectedTables->setChecked(true);
                    updateBackupScopeWidgets();

                    // 修复：列表内容填充后强制刷新内部布局，避免最后一行被按钮遮挡。
                    // doItemsLayout() 强制重新计算所有项位置/尺寸，
                    // updateGeometry() 让 Qt 下一次布局时重新查询 sizeHint，
                    // viewport()->update() 触发内部重绘，使滚动条位置与内容同步。
                    ui.listWidget_backupTables->doItemsLayout();
                    ui.listWidget_backupTables->updateGeometry();
                    if (QWidget* pViewport = ui.listWidget_backupTables->viewport()) {
                        pViewport->update();
                    }
                }
            }
            else
            {
                QMessageBox::warning(this, tr("查询失败"), tr("无法获取表列表。"));
            }
            db.close();
        }
        else
        {
			QMessageBox::critical(this, tr("连接失败"), tr("连接失败"));
        }
    }
    QSqlDatabase::removeDatabase(connName);
    QApplication::restoreOverrideCursor();
}

void CSE_DataRestoreDialog::updateBackupScopeWidgets()
{
    bool bManualSelect = ui.radioButton_BackupSelectedTables->isChecked();

    // 使用 setVisible + setMaximumHeight(0) + setMinimumHeight(0) 三重组合确保隐藏控件完全折叠
    // QListWidget 即使 setVisible(false)，其 minimumSize 仍可能保留（80px），
    // 且其在 verticalLayout_postgres 中 verstretch=1，会强制占据剩余垂直空间
    if (bManualSelect) {
        ui.listWidget_backupTables->setVisible(true);
        // 列表高度跟随内容自动收缩，最大不超过 180px（≈7-8行+滚动条）
        // AdjustToContents: 有 N 行就撑 N 行，无多余空白；超出 180px 时自动出现垂直滚动条
        ui.listWidget_backupTables->setMaximumHeight(180);
        ui.listWidget_backupTables->setMinimumHeight(0);
        ui.listWidget_backupTables->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        ui.listWidget_backupTables->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);

        ui.Button_SelectAllTables->setVisible(true);
        ui.Button_SelectAllTables->setMaximumHeight(QWIDGETSIZE_MAX);
        ui.Button_SelectAllTables->setMinimumHeight(0);

        ui.Button_InvertTableSelection->setVisible(true);
        ui.Button_InvertTableSelection->setMaximumHeight(QWIDGETSIZE_MAX);
        ui.Button_InvertTableSelection->setMinimumHeight(0);

        ui.Button_RefreshBackupTables->setVisible(true);
        ui.Button_RefreshBackupTables->setMaximumHeight(QWIDGETSIZE_MAX);
        ui.Button_RefreshBackupTables->setMinimumHeight(0);
    } else {
        ui.listWidget_backupTables->setVisible(false);
        ui.listWidget_backupTables->setMaximumHeight(0);
        ui.listWidget_backupTables->setMinimumHeight(0);
        ui.listWidget_backupTables->setFixedHeight(0);

        ui.Button_SelectAllTables->setVisible(false);
        ui.Button_SelectAllTables->setMaximumHeight(0);
        ui.Button_SelectAllTables->setMinimumHeight(0);
        ui.Button_SelectAllTables->setFixedHeight(0);

        ui.Button_InvertTableSelection->setVisible(false);
        ui.Button_InvertTableSelection->setMaximumHeight(0);
        ui.Button_InvertTableSelection->setMinimumHeight(0);
        ui.Button_InvertTableSelection->setFixedHeight(0);

        ui.Button_RefreshBackupTables->setVisible(false);
        ui.Button_RefreshBackupTables->setMaximumHeight(0);
        ui.Button_RefreshBackupTables->setMinimumHeight(0);
        ui.Button_RefreshBackupTables->setFixedHeight(0);
    }

    // 一站式应用布局：深度失效 + 多轮重算 + 锁定窗口高度。
    // 替代原先 activateLayoutRecursively+adjustDialogHeightForBackupTab 的组合，避免
    // 单层失效导致 sizeHint 缓存残留，从而彻底修复「多次来回切换产生差异化空白」的 BUG。
    applyBackupLayout();
}

void CSE_DataRestoreDialog::on_radioButton_BackupAllData_toggled(bool checked)
{
    // 切换到 "备份全部数据" 时隐藏表列表
    updateBackupScopeWidgets();
}

void CSE_DataRestoreDialog::on_radioButton_BackupSelectedTables_toggled(bool checked)
{
    // 切换到 "手动选择备份" 时显示表列表
    updateBackupScopeWidgets();
}

void CSE_DataRestoreDialog::on_Button_SelectAllTables_clicked()
{
    for (int i = 0; i < ui.listWidget_backupTables->count(); ++i)
    {
        ui.listWidget_backupTables->item(i)->setCheckState(Qt::Checked);
    }
}

void CSE_DataRestoreDialog::on_Button_InvertTableSelection_clicked()
{
    for (int i = 0; i < ui.listWidget_backupTables->count(); ++i)
    {
        QListWidgetItem* item = ui.listWidget_backupTables->item(i);
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
}

void CSE_DataRestoreDialog::on_Button_RefreshBackupTables_clicked()
{
    on_Button_GetBackupDbList_clicked();
}

void CSE_DataRestoreDialog::on_Button_GetDatabases_clicked()
{
    // "获取数据库"按钮的槽函数，与刷新表列表共用同一逻辑
    on_Button_GetBackupDbList_clicked();
}