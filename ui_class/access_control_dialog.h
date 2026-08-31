#ifndef ACCESS_CONTROL_DIALOG_H
#define ACCESS_CONTROL_DIALOG_H

#include <QDialog>
#include "database/product_metadata.h"
#include "ui_access_control_dialog.h"

class ProductDAO;

/**
 * @brief 权限管理对话框（仅数据库管理员可用）
 * 
 * 管理数据入库员和数据审核员的账户，包括增删人员、设置初始密码。
 * 按岗位职责划分：数据入库员、数据审核员、数据库管理员。
 */
class AccessControlDialog : public QDialog
{
	Q_OBJECT

public:
	explicit AccessControlDialog(ProductDAO* dao, QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~AccessControlDialog() override;

	/** @brief 设置当前操作的管理员名称 */
	void setCurrentOperator(const QString& operatorName);

private slots:
	void onAddUser();
	void onDeleteUser();
	void onUserSelected();
	void onSavePermission();
	void onResetForm();
	void onRefreshList();

private:
	void setupConnections();
	void setupTableColumns();
	void loadUserList();
	void fillPermissionForm(const UserPermission& perm);
	void collectPermissionForm(UserPermission& perm);
	void clearForm();
	bool validateInput();

	// 数据
	QList<UserPermission> mUsers;
	int mCurrentUserId = -1;
	QString mCurrentOperator;

	ProductDAO* mDAO = nullptr;

	Ui::AccessControlDialog ui;
};

#endif // ACCESS_CONTROL_DIALOG_H
