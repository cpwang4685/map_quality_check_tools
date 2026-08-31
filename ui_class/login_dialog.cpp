#include "login_dialog.h"
#include "database/product_dao.h"
#include "database/postgis_connector.h"
#include <libpq-fe.h>
#include <QMessageBox>
#include <QKeyEvent>

#include "ui_fit_helper.h"

LoginDialog::LoginDialog(ProductDAO* dao, QWidget* parent)
	: QDialog(parent)
	, mDAO(dao)
{
	mUI.setupUi(this);
	DialogFitHelper::install(this);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	connect(mUI.mLoginBtn, &QPushButton::clicked, this, &LoginDialog::onLogin);
	connect(mUI.mCancelBtn, &QPushButton::clicked, this, &QDialog::reject);

	// 允许回车键触发登录
	mUI.mPasswordEdit->installEventFilter(this);

	// 如果有session，显示之前的用户名
	if (!mSession.userName.isEmpty())
		mUI.mUserNameEdit->setText(mSession.userName);
}

LoginDialog::~LoginDialog()
{
}

void LoginDialog::onLogin()
{
	QString userName = mUI.mUserNameEdit->text().trimmed();
	QString password = mUI.mPasswordEdit->text();

	if (userName.isEmpty())
	{
		mUI.mStatusLabel->setText(QStringLiteral("请输入用户名"));
		mUI.mUserNameEdit->setFocus();
		return;
	}
	if (password.isEmpty())
	{
		mUI.mStatusLabel->setText(QStringLiteral("请输入密码"));
		mUI.mPasswordEdit->setFocus();
		return;
	}

	// 验证用户名和密码
	UserPermission perm = mDAO->authenticateUser(userName, password);
	if (perm.id <= 0)
	{
		mUI.mStatusLabel->setText(QStringLiteral("用户名或密码错误，请重试"));
		mUI.mPasswordEdit->selectAll();
		mUI.mPasswordEdit->setFocus();
		return;
	}

	// 登录成功，填充会话
	mSession.userName = perm.userName;
	mSession.role = perm.role;
	mSession.isLoggedIn = true;

	QMessageBox::information(this, QStringLiteral("登录成功"), QStringLiteral("登录成功"));
	accept();
}
