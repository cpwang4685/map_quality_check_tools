#ifndef USER_INFO_BAR_H
#define USER_INFO_BAR_H

#include <QBoxLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include "database/product_metadata.h"

/**
 * @brief 为对话框在右上角添加固定高度的用户信息栏
 *
 * @param parent  父窗口
 * @param layout  布局（QBoxLayout），信息栏插入到顶部
 */
inline void addUserInfoBar(QWidget* parent, QLayout* layout)
{
	auto* frame = new QFrame(parent);
	frame->setObjectName(QStringLiteral("userInfoFrame"));
	frame->setFixedHeight(56);
	frame->setStyleSheet(
		"QFrame#userInfoFrame {"
		"  background-color: #eef3f7; border-bottom: 1px solid #d0d7de;"
		"}");
	frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	auto* hLayout = new QHBoxLayout(frame);
	hLayout->setContentsMargins(0, 0, 8, 0);

	auto* label = new QLabel(frame);
	label->setObjectName(QStringLiteral("userInfoLabel"));
	label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	label->setStyleSheet("font-size: 28px; font-weight: bold; color: #2c3e50;");

	if (gCurrentUserSession.isLoggedIn)
	{
		label->setText(QStringLiteral("用户名：%1    用户类型：%2")
			.arg(gCurrentUserSession.userName)
			.arg(accessRoleToString(gCurrentUserSession.role)));
	}
	else
	{
		label->setText(QStringLiteral(""));
	}

	hLayout->addStretch();
	hLayout->addWidget(label);

	QBoxLayout* boxLayout = qobject_cast<QBoxLayout*>(layout);
	if (boxLayout)
		boxLayout->insertWidget(0, frame);
	else
		layout->addWidget(frame);
}

#endif // USER_INFO_BAR_H
