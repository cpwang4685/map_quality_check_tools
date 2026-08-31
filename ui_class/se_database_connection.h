#ifndef SE_DATABASE_CONNECTION_H
#define SE_DATABASE_CONNECTION_H

#include <QDialog>

#include "ui_database_connection.h"

#include <QString>
#include <QMetaType>

// 数据库连接信息
struct DatabaseConnectionInfo
{
	QString strName;		// 连接名称
	QString strDbType;		// 数据库类型
	QString strHost;		// 主机
	QString strPort;		// 端口
	QString strDbName;		// 数据库名
	QString strUsername;	// 用户名
	QString strPassword;	// 密码

	DatabaseConnectionInfo()
	{}

	bool isValid() const
	{
		return !strName.isEmpty() && !strHost.isEmpty() && !strDbName.isEmpty() && !strUsername.isEmpty();
	}
};

Q_DECLARE_METATYPE(DatabaseConnectionInfo)

class CSE_DatabaseConnectionDialog : public QDialog
{
	Q_OBJECT

public:
	CSE_DatabaseConnectionDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~CSE_DatabaseConnectionDialog() override;

	// 获取连接信息
	DatabaseConnectionInfo getConnectionInfo() const;

	// 设置编辑的连接信息
	void setConnectionInfo(const DatabaseConnectionInfo& info);

public slots:
	// 测试连接
	void on_Button_TestConnection_clicked();

	// 添加连接
	void on_Button_AddConnection_clicked();

	// 取消
	void on_Button_Cancel_clicked();

private:
	Ui_SeDatabaseConnectionDialog ui;

	DatabaseConnectionInfo m_connectionInfo;

	// 从界面控件加载连接信息
	void loadFromUi();

	// 验证输入
	bool validateInput() const;

	// 构建数据库连接字符串名
	QString buildConnectionString() const;
};

#endif // SE_DATABASE_CONNECTION_H
