#include "db_config_dialog.h"
#include "database/postgis_connector.h"
#include "database/schema_manager.h"
#include "database/product_dao.h"
#include "database/product_metadata.h"
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QPushButton>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDir>
#include "qgsgui.h"

#include "ui_fit_helper.h"

namespace {

// 麒麟"连接并初始化"闪退定位用的临时调试日志（/tmp 或 %TEMP% 下）
// 与 nmo_bridge_debug.log 同模式：无条件写、按行 flush，崩溃前数据不丢。
void dbConfigLog(const QString& msg)
{
	QFile f(QDir::tempPath() + QStringLiteral("/db_config_dialog.log"));
	if (f.open(QIODevice::Append | QIODevice::Text))
	{
		QTextStream ts(&f);
		ts << QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"))
		   << QLatin1Char(' ') << msg << QLatin1Char('\n');
		ts.flush();
	}
}

} // namespace

DbConfigDialog::DbConfigDialog(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
{
	ui.setupUi(this);

	QgsGui::enableAutoGeometryRestore(this);
	DialogFitHelper::install(this);

	loadSettings();

	setWindowTitle("数据库连接");
	// 去掉默认的问号帮助按钮（未实现帮助内容，多余）
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumSize(480, 450);

	// 按钮连接
	connect(ui.mTestBtn, &QPushButton::clicked, this, &DbConfigDialog::onTestConnection);
	connect(ui.mConnectBtn, &QPushButton::clicked, this, &DbConfigDialog::onConnect);

	// 自动检测当前连接状态
	auto* db = PostgisConnector::instance();
	if (db->isConnected())
	{
		setStatus("当前已连接到数据库", "green");
		ui.mConnectBtn->setText("重新连接并初始化");
	}
}

DbConfigDialog::~DbConfigDialog()
{
}

void DbConfigDialog::loadSettings()
{
	QSettings settings("GarMap", "MapProductManager");
	ui.mHostEdit->setText(settings.value("db/host", "localhost").toString());
	ui.mPortSpin->setValue(settings.value("db/port", 5432).toInt());
	ui.mDbNameEdit->setText(settings.value("db/database", "map_products").toString());
	ui.mSchemaCombo->setCurrentText(settings.value("db/schema", "public").toString());
	ui.mUserEdit->setText(settings.value("db/user", "postgres").toString());
	ui.mSavePwdCheck->setChecked(settings.value("db/savePassword", false).toBool());

	if (ui.mSavePwdCheck->isChecked())
	{
		ui.mPasswordEdit->setText(settings.value("db/password", "").toString());
	}
}

void DbConfigDialog::saveSettings()
{
	QSettings settings("GarMap", "MapProductManager");
	settings.setValue("db/host", ui.mHostEdit->text().trimmed());
	settings.setValue("db/port", ui.mPortSpin->value());
	settings.setValue("db/database", ui.mDbNameEdit->text().trimmed());
	settings.setValue("db/schema", ui.mSchemaCombo->currentText().trimmed());
	settings.setValue("db/user", ui.mUserEdit->text().trimmed());
	settings.setValue("db/savePassword", ui.mSavePwdCheck->isChecked());

	if (ui.mSavePwdCheck->isChecked())
	{
		settings.setValue("db/password", ui.mPasswordEdit->text());
	}
	else
	{
		settings.remove("db/password");
	}
}

void DbConfigDialog::setStatus(const QString& msg, const QString& color, bool showProgress)
{
	ui.mStatusLabel->setText(msg);
	ui.mStatusLabel->setStyleSheet(
		QString("QLabel { color: %1; font-weight: bold; padding: 4px; }").arg(color));
	ui.mProgressBar->setVisible(showProgress);
	if (showProgress)
	{
		ui.mProgressBar->setValue(0);
	}
	QApplication::processEvents();
}

void DbConfigDialog::onTestConnection()
{
	// 禁用按钮防止重复点击
	ui.mTestBtn->setEnabled(false);
	ui.mConnectBtn->setEnabled(false);

	setStatus("正在测试连接...", "orange", true);

	auto* db = PostgisConnector::instance();

	// 如果已连接，先断开
	if (db->isConnected())
	{
		db->disconnect();
	}

	bool success = db->connect(host(), port(), database(), user(), password());

	if (success)
	{
		setStatus("测试连接成功！PostGIS版本: " + 
			db->executeQueryOne("SELECT PostGIS_Version()").value("postgis_version").toString(), 
			"green");
		db->disconnect(); // 测试后断开
	}
	else
	{
		setStatus("连接失败: " + db->lastError(), "red");
	}

	ui.mTestBtn->setEnabled(true);
	ui.mConnectBtn->setEnabled(true);
}

void DbConfigDialog::onConnect()
{
	// 闪退定位：每次点击"连接并初始化"，先清空旧日志再记录步骤
	// （麒麟 /tmp/db_config_dialog.log，Windows %TEMP%\db_config_dialog.log）
	{
		QFile f(QDir::tempPath() + QStringLiteral("/db_config_dialog.log"));
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		{
			QTextStream ts(&f);
			ts << QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"))
			   << QStringLiteral(" === onConnect begin ===\n");
			ts.flush();
		}
	}
	dbConfigLog(QStringLiteral("步骤1: 禁用按钮"));

	// 禁用按钮防止重复点击
	ui.mTestBtn->setEnabled(false);
	ui.mConnectBtn->setEnabled(false);

	auto* db = PostgisConnector::instance();

	// 如果已连接，先断开
	if (db->isConnected())
	{
		dbConfigLog(QStringLiteral("步骤2: 断开旧连接"));
		db->disconnect();
	}

	// 步骤1: 连接数据库
	setStatus("正在连接数据库...", "orange", true);
	dbConfigLog(QStringLiteral("步骤3: 连接数据库 host=%1 port=%2 db=%3 user=%4 schema=%5")
		.arg(host(), QString::number(port()), database(), user(), schema()));

	if (!db->connect(host(), port(), database(), user(), password()))
	{
		dbConfigLog(QStringLiteral("步骤3: 连接失败 - %1").arg(db->lastError()));
		setStatus("连接失败: " + db->lastError(), "red");
		ui.mTestBtn->setEnabled(true);
		ui.mConnectBtn->setEnabled(true);
		return;
	}

	setStatus("数据库已连接，正在初始化Schema...", "orange", true);

	// 步骤2: 设置搜索路径
	QString schemaName = ui.mSchemaCombo->currentText().trimmed();
	if (!schemaName.isEmpty() && schemaName != "public")
	{
		dbConfigLog(QStringLiteral("步骤4: 创建Schema %1").arg(schemaName));
		db->executeNonQuery(QString("CREATE SCHEMA IF NOT EXISTS %1").arg(schemaName));
		db->setSearchPath(schemaName);
	}

	// 步骤3: 初始化Schema
	dbConfigLog(QStringLiteral("步骤5: 初始化Schema开始"));
	SchemaManager schemaMgr;
	bool schemaOk = schemaMgr.initializeSchema();
	dbConfigLog(QStringLiteral("步骤5: 初始化Schema完成 ok=%1").arg(schemaOk));
	if (!schemaOk)
	{
		QString errDetail = schemaMgr.lastError();
		if (errDetail.isEmpty()) {
			errDetail = "未知错误";
		}
		dbConfigLog(QStringLiteral("步骤5: Schema初始化失败 - %1").arg(errDetail));
		setStatus("Schema初始化失败: " + errDetail, "red");
		ui.mTestBtn->setEnabled(true);
		ui.mConnectBtn->setEnabled(true);
		return;
	}

	// 步骤4: 确保默认账户存在
	setStatus("正在创建默认账户...", "orange", true);
	dbConfigLog(QStringLiteral("步骤6: 确保默认账户"));

	ProductDAO dao;
	// 管理员账户
	UserPermission adminPerm = dao.getUserPermission("admin");
	if (adminPerm.id <= 0)
	{
		dbConfigLog(QStringLiteral("步骤6: 创建 admin 账户"));
		dao.addUserWithPassword(QStringLiteral("admin"), QStringLiteral("123456"),
			AccessRole::DatabaseAdmin, QStringLiteral("system"));
	}
	else if (adminPerm.passwordHash.isEmpty())
	{
		dbConfigLog(QStringLiteral("步骤6: 重置 admin 密码"));
		dao.changePassword(QStringLiteral("admin"), QStringLiteral("123456"));
	}

	// 成功
	dbConfigLog(QStringLiteral("步骤7: 保存设置"));
	saveSettings();
	setStatus("数据库连接成功，Schema已初始化完成", "green");

	dbConfigLog(QStringLiteral("步骤8: 弹出成功提示"));
	QMessageBox::information(this, "连接成功",
		QString("PostGIS数据库连接成功！\n\n"
				"服务器: %1:%2\n"
				"数据库: %3\n"
				"Schema: %4\n\n"
				"数据库表结构已初始化，可以开始使用智能综合功能。")
		.arg(host())
		.arg(port())
		.arg(database())
		.arg(schemaName));

	dbConfigLog(QStringLiteral("步骤9: accept 关闭对话框"));
	ui.mTestBtn->setEnabled(true);
	ui.mConnectBtn->setEnabled(true);
	accept();
}

QString DbConfigDialog::host() const { return ui.mHostEdit->text().trimmed(); }
int DbConfigDialog::port() const { return ui.mPortSpin->value(); }
QString DbConfigDialog::database() const { return ui.mDbNameEdit->text().trimmed(); }
QString DbConfigDialog::schema() const { return ui.mSchemaCombo->currentText().trimmed(); }
QString DbConfigDialog::user() const { return ui.mUserEdit->text().trimmed(); }
QString DbConfigDialog::password() const { return ui.mPasswordEdit->text(); }

bool DbConfigDialog::configureAndConnect(QWidget* parent)
{
	DbConfigDialog dlg(parent);
	return dlg.exec() == QDialog::Accepted;
}
