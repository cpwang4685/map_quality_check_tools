#ifndef LOGIN_DIALOG_H
#define LOGIN_DIALOG_H

#include <QDialog>
#include "ui_login_dialog.h"
#include "database/product_metadata.h"

class ProductDAO;

class LoginDialog : public QDialog
{
	Q_OBJECT

public:
	explicit LoginDialog(ProductDAO* dao, QWidget* parent = nullptr);
	~LoginDialog() override;

	/** @brief 获取登录成功后的用户会话 */
	const UserSession& getSession() const { return mSession; }



private slots:
	void onLogin();

private:
	Ui::LoginDialog mUI;
	ProductDAO* mDAO;
	UserSession mSession;
};

#endif // LOGIN_DIALOG_H
