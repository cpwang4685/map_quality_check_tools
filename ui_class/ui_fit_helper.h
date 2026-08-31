#pragma once

#include <QObject>
#include <QWidget>
#include <QEvent>
#include <QSize>

// ============================================================================
// 平台深色 QSS(ltzk_sci_fi.qss)会放大控件高度:
//   QGroupBox   padding 18px/12px + font-size 16px + margin-top 16px
//   QLineEdit/SpinBox/ComboBox  padding 7px 纵向 + min-height
//   QPushButton padding 7px + min-height 28px
// 使按默认 Qt 尺寸设计的对话框内容总高超过固定设计尺寸(.ui geometry /
// setMinimumSize / enableAutoGeometryRestore 恢复值),而 QDialog 不会自动
// 撑大,导致布局压缩行高、输入框文字底部被裁、底部按钮被遮挡。
//
// 本工具在对话框首次 Show 之后(此时平台 QSS 已作用于控件,minimumSizeHint
// 已是放大后的值)把对话框最小尺寸撑到内容所需,保证内容完整显示。
// 平台自身也是靠事件过滤给插件窗口补套 QSS,沿用同一模式最稳妥。
//
// 用法: 在每个对话框构造函数末尾调用一次:
//     DialogFitHelper::install(this);
// ============================================================================
class DialogFitHelper : public QObject
{
public:
    // 安装到对话框上;helper 以 dlg 为父对象,随对话框一起销毁。
    static void install(QWidget* dlg)
    {
        auto* helper = new DialogFitHelper(dlg);
        dlg->installEventFilter(helper);
    }

protected:
    explicit DialogFitHelper(QWidget* parent)
        : QObject(parent) {}

    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (ev->type() == QEvent::Show && !m_done)
        {
            if (auto* w = qobject_cast<QWidget*>(obj))
            {
                m_done = true;
                // 首次 Show 时平台 QSS 已生效, minimumSizeHint 为放大后的值。
                // 只撑大、绝不缩小: 保持现有显式尺寸,但保证不小于内容所需。
                const QSize need = w->minimumSizeHint();
                const QSize curMin = w->minimumSize();
                w->setMinimumSize(qMax(curMin.width(), need.width()),
                                  qMax(curMin.height(), need.height()));
                if (w->height() < need.height() || w->width() < need.width())
                {
                    w->resize(qMax(w->width(), need.width()),
                              qMax(w->height(), need.height()));
                }
            }
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    bool m_done = false;
};
