#ifndef CALENDAR_HELPER_H
#define CALENDAR_HELPER_H

#include <QLineEdit>

/**
 * @brief 为 QLineEdit 添加日期选择功能
 *
 * 在 QLineEdit 右侧添加一个日历图标按钮，点击后弹出 QCalendarWidget 供用户选择日期。
 * 选中日期后自动填入 yyyy-MM-dd 格式的日期字符串。
 *
 * @param edit  目标 QLineEdit 控件
 * @param iconSize  图标大小（默认 16x16）
 */
void setupDateEdit(QLineEdit* edit, int iconSize = 16);

#endif // CALENDAR_HELPER_H
