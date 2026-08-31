#include "calendar_helper.h"
#include <QAction>
#include <QCalendarWidget>
#include <QDialog>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QDate>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

// 生成一个简单的日历图标（16x16）
static QIcon createCalendarIcon(int size)
{
	QPixmap pix(size, size);
	pix.fill(Qt::transparent);

	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);

	int m = size / 6;           // margin
	int headerH = size / 5;     // 顶部色条高度

	// 日历顶部色条
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(74, 144, 217));
	p.drawRoundedRect(m, m, size - 2 * m, headerH, 1, 1);

	// 日历主体边框
	p.setPen(QPen(QColor(120, 120, 120), 1));
	p.setBrush(Qt::white);
	p.drawRoundedRect(m, m, size - 2 * m, size - 2 * m, 2, 2);

	// 覆盖色条下方多余的圆角（用矩形遮盖）
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	p.drawRect(m + 1, m + headerH, size - 2 * m - 2, 2);

	// 重新画色条（覆盖白色矩形覆盖的部分）
	p.setBrush(QColor(74, 144, 217));
	p.drawRoundedRect(m, m, size - 2 * m, headerH, 1, 1);

	// 日期数字行（模拟）
	int rowY1 = m + headerH + 3;
	int rowY2 = m + headerH + (size - 2 * m - headerH) / 2 + 1;
	int cellW = (size - 2 * m) / 4;
	p.setPen(QColor(180, 180, 180));
	QFont f = p.font();
	f.setPixelSize(qMax(5, size / 5));
	p.setFont(f);
	for (int i = 0; i < 3; ++i)
	{
		p.drawText(m + i * cellW, rowY1, cellW, size / 4, Qt::AlignCenter, "8");
	}
	for (int i = 0; i < 3; ++i)
	{
		p.drawText(m + i * cellW, rowY2, cellW, size / 4, Qt::AlignCenter, "15");
	}
	// 高亮"15"
	p.setPen(QColor(74, 144, 217));
	p.drawText(m + 1 * cellW, rowY2, cellW, size / 4, Qt::AlignCenter, "15");

	p.end();
	return QIcon(pix);
}

void setupDateEdit(QLineEdit* edit, int iconSize)
{
	if (!edit) return;

	QIcon icon = createCalendarIcon(iconSize);

	QAction* action = edit->addAction(icon, QLineEdit::TrailingPosition);
	action->setToolTip(QString::fromUtf8("选择日期"));

	QObject::connect(action, &QAction::triggered, edit, [edit]() {
		QDialog dlg(edit->window());
		dlg.setWindowTitle(QString::fromUtf8("选择日期"));
		dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

		QVBoxLayout* layout = new QVBoxLayout(&dlg);
		layout->setContentsMargins(10, 10, 10, 5);

		QCalendarWidget* cal = new QCalendarWidget(&dlg);
		cal->setGridVisible(true);
		cal->setFirstDayOfWeek(Qt::Monday);

		// 如果当前编辑框中有合法日期，则设置为日历当前日期
		QDate curDate = QDate::fromString(edit->text().trimmed(), "yyyy-MM-dd");
		if (curDate.isValid())
			cal->setSelectedDate(curDate);
		else
			cal->setSelectedDate(QDate::currentDate());

		layout->addWidget(cal);

		QDialogButtonBox* btnBox = new QDialogButtonBox(
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(btnBox);

		QObject::connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		QObject::connect(cal, &QCalendarWidget::activated, &dlg, &QDialog::accept);

		if (dlg.exec() == QDialog::Accepted)
			edit->setText(cal->selectedDate().toString("yyyy-MM-dd"));
	});
}
