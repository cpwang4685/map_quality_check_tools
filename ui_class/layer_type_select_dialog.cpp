/**
 * @file    layer_type_select_dialog.cpp
 * @brief   图层类型选择弹出对话框 — 3 列 x 5 行 (N/A/M) 复选框网格
 */

#include "layer_type_select_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSet>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>

#include "ui_fit_helper.h"

LayerTypeSelectDialog::LayerTypeSelectDialog(const QStringList& preSelected,
                                             QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QString::fromUtf8("选择缩编的图层类型"));
    resize(520, 300);
    setMinimumSize(420, 260);
    DialogFitHelper::install(this);

    auto* mainLayout = new QVBoxLayout(this);

    // 分组框
    auto* group = new QGroupBox(QString::fromUtf8(
        "勾选要导出的图层类型 (按 Ctrl 可多选/取消)"), this);
    auto* grid = new QGridLayout(group);
    grid->setHorizontalSpacing(20);
    grid->setVerticalSpacing(6);

    // 列标题 (加粗)
    const char* colHeaders[] = {
        "N \350\207\252\347\204\266\350\246\201\347\264\240",
        "A \344\272\272\345\267\245\350\246\201\347\264\240",
        "M \347\256\241\347\220\206\345\214\272\345\237\237"
    };
    for (int col = 0; col < 3; ++col) {
        auto* lbl = new QLabel(QString::fromUtf8(colHeaders[col]), this);
        QFont f = lbl->font(); f.setBold(true); lbl->setFont(f);
        grid->addWidget(lbl, 0, col, Qt::AlignLeft);
    }

    // 15 个图层类型, 按列排列
    const char* codes[15] = {
        "n_mou","n_hyd","n_ice","n_oce","n_agr",
        "a_wac","a_tra","a_bld","a_pip","a_yad",
        "m_adm","m_gna","m_nsp","m_oma","m_ome"
    };
    const char* labels[15] = {
        "n_mou \345\261\261\344\275\223",
        "n_hyd \346\260\264\344\275\223",
        "n_ice \345\206\260\351\233\252\345\234\260",
        "n_oce \346\265\267\346\264\213",
        "n_agr \345\206\234\346\236\227\347\224\250\345\234\260\344\270\216\345\205\266\344\273\226\345\234\237\345\234\260",
        "a_wac \346\260\264\345\210\251",
        "a_tra \344\272\244\351\200\232",
        "a_bld \345\273\272\346\236\204\347\255\221\347\211\251\345\217\212\350\256\276\346\226\275",
        "a_pip \347\256\241\347\272\277",
        "a_yad \351\231\242\350\220\275",
        "m_adm \350\241\214\346\224\277\345\214\272\345\210\222\345\215\225\345\205\203",
        "m_gna \345\234\260\345\220\215",
        "m_nsp \345\233\275\345\234\237\347\251\272\351\227\264\350\247\204\345\210\222\345\215\225\345\205\203",
        "m_oma \345\205\266\344\273\226\347\256\241\347\220\206\345\214\272\345\237\237",
        "m_ome \345\205\266\344\273\226\347\256\241\347\220\206\345\256\236\344\275\223"
    };
    const int colStarts[] = {0, 5, 10};
    QSet<QString> preSet = preSelected.toSet();   // Qt 5.14+ 才有区间构造，5.12 用 toSet()

    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 5; ++row) {
            int idx = colStarts[col] + row;
            m_checkboxes[idx] = new QCheckBox(
                QString::fromUtf8(labels[idx]), this);
            // 默认选中 a_bld，或恢复上次选择
            m_checkboxes[idx]->setChecked(
                preSelected.isEmpty() ? (idx == 7) : preSet.contains(QString(codes[idx])));
            grid->addWidget(m_checkboxes[idx], row + 1, col, Qt::AlignLeft);
        }
    }

    mainLayout->addWidget(group);

    // 快捷按钮行
    auto* hQuick = new QHBoxLayout();
    auto* btnAll = new QPushButton(QString::fromUtf8("全选"), this);
    auto* btnNone = new QPushButton(QString::fromUtf8("全不选"), this);
    auto* btnN = new QPushButton("N \350\207\252\347\204\266", this);
    auto* btnA = new QPushButton("A \344\272\272\345\267\245", this);
    auto* btnM = new QPushButton("M \347\256\241\347\220\206", this);
    hQuick->addWidget(btnAll);
    hQuick->addWidget(btnNone);
    hQuick->addSpacing(12);
    hQuick->addWidget(btnN);
    hQuick->addWidget(btnA);
    hQuick->addWidget(btnM);
    hQuick->addStretch();
    mainLayout->addLayout(hQuick);

    connect(btnAll, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < 15; ++i) m_checkboxes[i]->setChecked(true);
    });
    connect(btnNone, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < 15; ++i) m_checkboxes[i]->setChecked(false);
    });
    auto toggleCategory = [this](int start, int count) {
        for (int i = start; i < start + count; ++i) {
            auto* cb = m_checkboxes[i];
            // 如果该类别已全选 → 全不选; 否则 → 全选
            bool allChecked = true;
            for (int j = start; j < start + count; ++j)
                if (!m_checkboxes[j]->isChecked()) { allChecked = false; break; }
            cb->setChecked(!allChecked);
        }
    };
    connect(btnN, &QPushButton::clicked, this, [toggleCategory]() { toggleCategory(0, 5); });
    connect(btnA, &QPushButton::clicked, this, [toggleCategory]() { toggleCategory(5, 5); });
    connect(btnM, &QPushButton::clicked, this, [toggleCategory]() { toggleCategory(10, 5); });

    // 确定/取消
    auto* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    btnBox->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8("确定"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("取消"));
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);
}

QStringList LayerTypeSelectDialog::selectedTypes() const
{
    QStringList result;
    const char* codes[15] = {
        "n_mou","n_hyd","n_ice","n_oce","n_agr",
        "a_wac","a_tra","a_bld","a_pip","a_yad",
        "m_adm","m_gna","m_nsp","m_oma","m_ome"
    };
    for (int i = 0; i < 15; ++i) {
        if (m_checkboxes[i] && m_checkboxes[i]->isChecked())
            result << QString(codes[i]);
    }
    return result;
}
