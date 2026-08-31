#ifndef DB_CONFIG_DIALOG_H
#define DB_CONFIG_DIALOG_H

#include <QDialog>
#include "ui_db_config_dialog.h"

/**
 * @brief 数据库连接配置对话框
 * 
 * 提供PostGIS/PostgreSQL数据库连接配置界面，支持：
 * - 主机、端口、数据库名、Schema、用户名、密码配置
 * - 测试连接（不初始化Schema）
 * - 连接并初始化数据库Schema
 * - 配置持久化（QSettings）
 * - 连接状态实时显示
 */
class DbConfigDialog : public QDialog
{
	Q_OBJECT

public:
	explicit DbConfigDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~DbConfigDialog() override;

	QString host() const;
	int port() const;
	QString database() const;
	QString schema() const;
	QString user() const;
	QString password() const;

	/// 便捷静态方法：显示配置对话框并尝试连接
	static bool configureAndConnect(QWidget* parent = nullptr);

private slots:
	void onTestConnection();
	void onConnect();

private:
	void loadSettings();
	void saveSettings();
	void setStatus(const QString& msg, const QString& color, bool showProgress = false);

	Ui::DbConfigDialog ui;
};

#endif // DB_CONFIG_DIALOG_H
