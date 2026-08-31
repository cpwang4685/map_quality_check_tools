/*--------------SE---------------*/
#include "se_database_connection.h"

/*--------------QT---------------*/
#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QApplication>

#include "ui_fit_helper.h"


CSE_DatabaseConnectionDialog::CSE_DatabaseConnectionDialog(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
{
	ui.setupUi(this);
	DialogFitHelper::install(this);

	this->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

	// 注意：按钮信号已由 setupUi() 中的 connectSlotsByName 自动连接
	// （on_Button_TestConnection/AddConnection/Cancel_clicked），
	// 不需要手动 connect，否则会造成槽函数被调用两次。

	// 默认端口
	ui.lineEdit_port->setText("5432");
}

CSE_DatabaseConnectionDialog::~CSE_DatabaseConnectionDialog()
{
}

DatabaseConnectionInfo CSE_DatabaseConnectionDialog::getConnectionInfo() const
{
	return m_connectionInfo;
}

void CSE_DatabaseConnectionDialog::setConnectionInfo(const DatabaseConnectionInfo& info)
{
	m_connectionInfo = info;

	ui.lineEdit_connectionName->setText(info.strName);
	ui.comboBox_dbType->setCurrentText(info.strDbType);
	ui.lineEdit_host->setText(info.strHost);
	ui.lineEdit_port->setText(info.strPort);
	ui.lineEdit_dbName->setText(info.strDbName);
	ui.lineEdit_username->setText(info.strUsername);
	ui.lineEdit_password->setText(info.strPassword);
}

QString CSE_DatabaseConnectionDialog::buildConnectionString() const
{
	QString driver;
	if (m_connectionInfo.strDbType == "PostgreSQL" || m_connectionInfo.strDbType == "PostGIS")
	{
		driver = "QPSQL";
	}
	else if (m_connectionInfo.strDbType == "MySQL")
	{
		driver = "QMYSQL";
	}
	else if (m_connectionInfo.strDbType == "Oracle")
	{
		driver = "QOCI";
	}
	else
	{
		driver = "QPSQL";
	}

	// 生成唯一连接名，避免与已有连接冲突
	return QString("conn_test_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
}

void CSE_DatabaseConnectionDialog::loadFromUi()
{
	m_connectionInfo.strName = ui.lineEdit_connectionName->text().trimmed();
	m_connectionInfo.strDbType = ui.comboBox_dbType->currentText();
	m_connectionInfo.strHost = ui.lineEdit_host->text().trimmed();
	m_connectionInfo.strPort = ui.lineEdit_port->text().trimmed();
	m_connectionInfo.strDbName = ui.lineEdit_dbName->text().trimmed();
	m_connectionInfo.strUsername = ui.lineEdit_username->text().trimmed();
	m_connectionInfo.strPassword = ui.lineEdit_password->text();
}

bool CSE_DatabaseConnectionDialog::validateInput() const
{
	if (!m_connectionInfo.isValid())
	{
		QMessageBox::warning(const_cast<CSE_DatabaseConnectionDialog*>(this), tr("新建连接"), tr("请填写完整的连接信息！"));
		return false;
	}
	return true;
}

void CSE_DatabaseConnectionDialog::on_Button_TestConnection_clicked()
{
	loadFromUi();
	if (!validateInput())
	{
		return;
	}

	QApplication::setOverrideCursor(Qt::WaitCursor);

	QString connName = buildConnectionString();

	{
		QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
		db.setHostName(m_connectionInfo.strHost);
		db.setPort(m_connectionInfo.strPort.toInt());
		db.setDatabaseName(m_connectionInfo.strDbName);
		db.setUserName(m_connectionInfo.strUsername);
		db.setPassword(m_connectionInfo.strPassword);

		if (db.open())
		{
			// 检查 PostGIS 扩展
			QSqlQuery query(db);
			QString postgisInfo;
			if (query.exec("SELECT PostGIS_Version()") && query.next())
			{
				postgisInfo = tr("\nPostGIS 版本：%1").arg(query.value(0).toString());
			}

			QMessageBox::information(this, tr("新建连接"), tr("连接成功"));

			db.close();
		}
		else
		{
			QMessageBox::critical(this, tr("新建连接"), tr("连接失败"));
		}
	}

	QSqlDatabase::removeDatabase(connName);
	QApplication::restoreOverrideCursor();
}

void CSE_DatabaseConnectionDialog::on_Button_AddConnection_clicked()
{
	loadFromUi();
	if (!validateInput())
	{
		return;
	}

	// 添加前先测试连接
	QApplication::setOverrideCursor(Qt::WaitCursor);

	QString connName = buildConnectionString();

	{
		QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
		db.setHostName(m_connectionInfo.strHost);
		db.setPort(m_connectionInfo.strPort.toInt());
		db.setDatabaseName(m_connectionInfo.strDbName);
		db.setUserName(m_connectionInfo.strUsername);
		db.setPassword(m_connectionInfo.strPassword);

		if (!db.open())
		{
			QApplication::restoreOverrideCursor();
			QMessageBox::critical(this, tr("新建连接"), tr("连接失败"));
			db.close();
			QSqlDatabase::removeDatabase(connName);
			return;
		}
		db.close();
	}

	QSqlDatabase::removeDatabase(connName);
	QApplication::restoreOverrideCursor();

	accept();
}

void CSE_DatabaseConnectionDialog::on_Button_Cancel_clicked()
{
	reject();
}
