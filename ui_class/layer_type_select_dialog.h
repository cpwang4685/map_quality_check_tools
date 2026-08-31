#pragma once
#ifndef LAYER_TYPE_SELECT_DIALOG_H
#define LAYER_TYPE_SELECT_DIALOG_H

#include <QDialog>
#include <QCheckBox>
#include <QStringList>

/**
 * @brief 图层类型选择弹出对话框 — 3 列 x 5 行网格 (N 自然 | A 人工 | M 管理)
 *
 * 用法:
 *   LayerTypeSelectDialog dlg(currentSelection, parent);
 *   if (dlg.exec() == QDialog::Accepted)
 *       QStringList sel = dlg.selectedTypes();
 */
class LayerTypeSelectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LayerTypeSelectDialog(const QStringList& preSelected = {},
                                   QWidget* parent = nullptr);
    ~LayerTypeSelectDialog() = default;

    QStringList selectedTypes() const;

private:
    QCheckBox* m_checkboxes[15] = {};
};

#endif // LAYER_TYPE_SELECT_DIALOG_H
