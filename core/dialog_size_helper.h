#ifndef DIALOG_SIZE_HELPER_H
#define DIALOG_SIZE_HELPER_H

// ============================================================
// 弹出窗口统一尺寸策略（与成果模块 core/dialog_size_helper.h 一致）：
//   弹出后按布局 sizeHint 计算能完整展示所有内容的尺寸；
//   - 内容在可用屏幕范围内放得下 → 固定窗口尺寸（setFixedSize），
//     禁止拉伸出大片空白；
//   - 内容超出屏幕（如超长表单）→ 限制到可用屏幕的 90% 并保持
//     可缩放，由内部滚动条兜底。
//
// 本工程差异（源侧无）：成果模块用 QWidget::screen()（Qt 5.14+ 才有），
// 麒麟是 Qt 5.12.12，没有该成员；这里改为"父窗口所在屏幕 → 窗口中心点
// 所在屏幕 → 主屏"的兼容写法，Windows / 麒麟共用同一份代码。
//
// 适用范围：内容固定的"小弹窗"类对话框（表单/向导/选择器等）。
// 不适用于需要自由调整大小的主工作台对话框（含 QSplitter、
// 目录树+表格的工作区类窗口，如成果存储与管理、元数据管理等），
// 这类窗口保持原 resize() 的可缩放行为。
// ============================================================

#include <QDialog>
#include <QLayout>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <QGuiApplication>

namespace DialogSizeHelper
{

// 取对话框所在屏幕（Qt 5.12 兼容：不能在 5.12 上调用 QWidget::screen()）
inline const QScreen* screenOf(const QDialog* dlg)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    if (const QScreen* s = dlg->screen())
        return s;
#endif
    // 一般都有父窗口：先取父窗口原生窗口所在屏幕；
    // 构造期还没有原生句柄时，按父窗口中心点找屏幕
    if (const QWidget* host = dlg->parentWidget())
    {
        const QWidget* win = host->window();
        if (win)
        {
            if (const QWindow* wh = win->windowHandle())
            {
                if (const QScreen* s = wh->screen())
                    return s;
            }
            if (const QScreen* s = QGuiApplication::screenAt(win->frameGeometry().center()))
                return s;
        }
    }
    // 没有父窗口（或上面都没找到）：按对话框自身中心点找，最后退回主屏
    if (dlg->isVisible())
    {
        if (const QScreen* s = QGuiApplication::screenAt(dlg->frameGeometry().center()))
            return s;
    }
    return QGuiApplication::primaryScreen();
}

// 弹出前调用（构造函数末尾、setupUi/布局构建完成之后）：
// 先按内容自适应，再决定固定或可缩放。
inline void fitToContents(QDialog* dlg)
{
    if (!dlg || !dlg->layout())
        return;

    // 强制布局重算，保证 sizeHint 反映全部控件
    dlg->layout()->activate();
    const QSize hint = dlg->sizeHint().expandedTo(dlg->minimumSizeHint());

    const QScreen* screen = screenOf(dlg);
    if (!screen)
        return;
    const QSize avail = screen->availableSize();
    const QSize bound(avail.width() * 9 / 10, avail.height() * 9 / 10);

    if (hint.width() <= bound.width() && hint.height() <= bound.height())
    {
        // 内容完整展示：固定尺寸，避免无意义的拉伸空白
        dlg->setFixedSize(hint);
    }
    else
    {
        // 内容超出屏幕：限制到屏幕 90%，保持可缩放（滚动条兜底）
        dlg->resize(QSize(qMin(hint.width(), bound.width()),
                          qMin(hint.height(), bound.height())));
    }
}

} // namespace DialogSizeHelper

#endif // DIALOG_SIZE_HELPER_H
