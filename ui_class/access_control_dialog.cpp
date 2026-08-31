#include "access_control_dialog.h"
#include "database/product_dao.h"
#include "core/user_info_bar.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>
#include <QSplitter>
#include "qgsgui.h"

#include "ui_fit_helper.h"

AccessControlDialog::AccessControlDialog(ProductDAO* dao, QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
	, mDAO(dao)
{
	Q_ASSERT(mDAO);

	ui.setupUi(this);
	QgsGui::enableAutoGeometryRestore(this);
	DialogFitHelper::install(this);
	setupTableColumns();
	setupConnections();
	loadUserList();

	// 重构布局：将 HBoxLayout 包裹在 VBoxLayout 中，用户信息栏置顶
	auto* oldLayout = layout();
	auto* wrapperLayout = new QVBoxLayout();
	wrapperLayout->setContentsMargins(0, 0, 0, 0);
	addUserInfoBar(this, wrapperLayout);

	QHBoxLayout* oldHBox = qobject_cast<QHBoxLayout*>(oldLayout);
	if (oldHBox)
	{
		auto* contentLayout = new QHBoxLayout();
		QLayoutItem* item;
		while ((item = oldHBox->takeAt(0)))
			contentLayout->addItem(item);
		wrapperLayout->addLayout(contentLayout);
	}
	delete oldLayout;
	setLayout(wrapperLayout);

	setWindowTitle(QStringLiteral("权限管理"));
	resize(900, 600);
}

AccessControlDialog::~AccessControlDialog()
{
}

void AccessControlDialog::setCurrentOperator(const QString& operatorName)
{
	mCurrentOperator = operatorName;
}

// ===================== UI 初始化 =====================

void AccessControlDialog::setupTableColumns()
{
	// 去掉部门列，列数从5减为4
	ui.mUserTable->setColumnCount(4);
	ui.mUserTable->setHorizontalHeaderLabels({
		QStringLiteral("用户名"),
		QStringLiteral("角色"),
		QStringLiteral("授权人"),
		QStringLiteral("授权时间")
	});
	ui.mUserTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	ui.mUserTable->horizontalHeader()->setStretchLastSection(true);
}

void AccessControlDialog::setupConnections()
{
	connect(ui.mAddUserBtn, &QPushButton::clicked, this, &AccessControlDialog::onAddUser);
	connect(ui.mDeleteUserBtn, &QPushButton::clicked, this, &AccessControlDialog::onDeleteUser);
	connect(ui.mUserTable, &QTableWidget::itemClicked, this, &AccessControlDialog::onUserSelected);
	connect(ui.mSaveBtn, &QPushButton::clicked, this, &AccessControlDialog::onSavePermission);
	connect(ui.mResetBtn, &QPushButton::clicked, this, &AccessControlDialog::onResetForm);
	connect(ui.mRefreshBtn, &QPushButton::clicked, this, &AccessControlDialog::onRefreshList);

	// 角色变更时更新说明
	connect(ui.mRoleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
		QStringList descriptions = {
			QStringLiteral("数据入库员：可上传成果、编辑元数据、管理标签分类。"),
			QStringLiteral("数据审核员：可执行版本回溯和差异比对，修改元数据和标签分类。"),
			QStringLiteral("数据库管理员：最高权限，可管理数据入库员和数据审核员，增删人员。")
		};
		if (idx >= 0 && idx < descriptions.size())
			ui.mRoleDescriptionLabel->setText(descriptions[idx]);
	});
}

// ===================== 按钮事件 =====================

void AccessControlDialog::onAddUser()
{
	clearForm();
	ui.mUserNameEdit->setFocus();
}

void AccessControlDialog::onDeleteUser()
{
	int row = ui.mUserTable->currentRow();
	if (row < 0)
	{
		QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择要删除的用户"));
		return;
	}

	QString userName = ui.mUserTable->item(row, 0)->text();

	// 不允许删除自己
	if (userName == mCurrentOperator)
	{
		QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("不能删除当前登录的管理员账户"));
		return;
	}

	auto reply = QMessageBox::question(this, QStringLiteral("确认删除"),
		QStringLiteral("确定要删除用户 \"%1\" 吗？").arg(userName),
		QMessageBox::Yes | QMessageBox::No);

	if (reply == QMessageBox::Yes)
	{
		if (mDAO->removeUserPermission(userName))
		{
			QMessageBox::information(this, QStringLiteral("成功"), QStringLiteral("用户已删除"));
			loadUserList();
			clearForm();
		}
		else
		{
			QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("删除用户失败"));
		}
	}
}

void AccessControlDialog::onUserSelected()
{
	int row = ui.mUserTable->currentRow();
	if (row < 0 || row >= mUsers.size()) return;

	mCurrentUserId = mUsers[row].id;
	const auto& perm = mUsers[row];
	fillPermissionForm(perm);
}

void AccessControlDialog::onSavePermission()
{
	if (!validateInput())
		return;

	QString userName = ui.mUserNameEdit->text().trimmed();
	QString password = ui.mPasswordEdit->text().trimmed();
	AccessRole role = stringToAccessRole(ui.mRoleCombo->currentText());
	QString permissions = ui.mPermissionsEdit->toPlainText().trimmed();

	// 新建用户（mCurrentUserId <= 0）：必须设置初始密码
	if (mCurrentUserId <= 0)
	{
		if (password.isEmpty())
		{
			QMessageBox::warning(this, QStringLiteral("提示"),
				QStringLiteral("新建用户时必须设置初始密码"));
			ui.mPasswordEdit->setFocus();
			return;
		}

		if (mDAO->addUserWithPassword(userName, password, role, mCurrentOperator))
		{
			// 如果有扩展权限，更新一次（addUserWithPassword 只插入了基本字段）
			if (!permissions.isEmpty())
			{
				UserPermission perm;
				perm.userName = userName;
				perm.role = role;
				perm.passwordHash.clear(); // 不覆盖密码
				perm.permissions = permissions;
				perm.grantedBy = mCurrentOperator;
				mDAO->setUserPermission(perm);
			}
			QMessageBox::information(this, QStringLiteral("成功"),
				QStringLiteral("用户 \"%1\" 已创建，角色：%2").arg(userName, accessRoleToString(role)));
			loadUserList();
			clearForm();
		}
		else
		{
			QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("创建用户失败，用户名可能已存在"));
		}
	}
	// 编辑已有用户：只更新角色和权限，若填写了密码则同时更新
	else
	{
		// 不允许降级自己
		if (userName == mCurrentOperator && role != AccessRole::DatabaseAdmin)
		{
			QMessageBox::warning(this, QStringLiteral("提示"),
				QStringLiteral("不能修改当前管理员账户的角色"));
			return;
		}

		if (!password.isEmpty())
		{
			mDAO->changePassword(userName, password);
		}
		mDAO->changeUserRole(userName, role, mCurrentOperator);

		// 更新扩展权限
		UserPermission perm;
		perm.userName = userName;
		perm.role = role;
		perm.passwordHash.clear();
		perm.permissions = permissions;
		perm.grantedBy = mCurrentOperator;
		mDAO->setUserPermission(perm);

		QMessageBox::information(this, QStringLiteral("成功"),
			QStringLiteral("用户 \"%1\" 权限已更新").arg(userName));
		loadUserList();
	}
}

void AccessControlDialog::onResetForm()
{
	if (mCurrentUserId > 0)
	{
		for (const auto& user : mUsers)
		{
			if (user.id == mCurrentUserId)
			{
				fillPermissionForm(user);
				return;
			}
		}
	}
	clearForm();
}

void AccessControlDialog::onRefreshList()
{
	loadUserList();
}

// ===================== 数据加载 =====================

void AccessControlDialog::loadUserList()
{
	mUsers = mDAO->getAllPermissions();

	ui.mUserTable->setRowCount(0);
	for (int i = 0; i < mUsers.size(); ++i)
	{
		const auto& perm = mUsers[i];
		ui.mUserTable->insertRow(i);
		ui.mUserTable->setItem(i, 0, new QTableWidgetItem(perm.userName));
		ui.mUserTable->setItem(i, 1, new QTableWidgetItem(accessRoleToString(perm.role)));
		ui.mUserTable->setItem(i, 2, new QTableWidgetItem(perm.grantedBy));
		ui.mUserTable->setItem(i, 3, new QTableWidgetItem(perm.grantedAt.toString("yyyy-MM-dd hh:mm")));
	}
}

void AccessControlDialog::fillPermissionForm(const UserPermission& perm)
{
	mCurrentUserId = perm.id;
	ui.mUserNameEdit->setText(perm.userName);
	ui.mRoleCombo->setCurrentText(accessRoleToString(perm.role));
	ui.mPasswordEdit->clear();           // 不显示已有密码哈希，编辑时可选填新密码
	ui.mPasswordEdit->setPlaceholderText(QStringLiteral("如需修改密码请输入新密码，否则留空"));
	ui.mPermissionsEdit->setText(perm.permissions);
	ui.mGrantedInfoLabel->setText(QStringLiteral("授权人: %1 | 授权时间: %2")
		.arg(perm.grantedBy)
		.arg(perm.grantedAt.toString("yyyy-MM-dd hh:mm:ss")));
}

void AccessControlDialog::collectPermissionForm(UserPermission& perm)
{
	perm.id = mCurrentUserId;
	perm.userName = ui.mUserNameEdit->text().trimmed();
	perm.role = stringToAccessRole(ui.mRoleCombo->currentText());
	perm.permissions = ui.mPermissionsEdit->toPlainText().trimmed();
}

void AccessControlDialog::clearForm()
{
	mCurrentUserId = -1;
	ui.mUserNameEdit->clear();
	ui.mRoleCombo->setCurrentIndex(0);
	ui.mPasswordEdit->clear();
	ui.mPasswordEdit->setPlaceholderText(QStringLiteral("为新用户设置初始密码（修改角色时可不填）"));
	ui.mPermissionsEdit->clear();
	ui.mGrantedInfoLabel->clear();
}

bool AccessControlDialog::validateInput()
{
	QString userName = ui.mUserNameEdit->text().trimmed();
	if (userName.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请输入用户名"));
		ui.mUserNameEdit->setFocus();
		return false;
	}

	if (userName.contains(' ') || userName.contains('\''))
	{
		QMessageBox::warning(this, QStringLiteral("提示"),
			QStringLiteral("用户名不能包含空格或单引号"));
		ui.mUserNameEdit->setFocus();
		return false;
	}

	return true;
}
