#ifndef SRC_SELECT_DIALOG_H
#define SRC_SELECT_DIALOG_H

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

// 数据选择窗口：文件与文件夹在同一棵树里打勾，文件夹懒加载
// （格式转换与接边共用；勾选文件夹＝整个文件夹参与处理）
class SrcSelectDialog : public QDialog
{
public:
    SrcSelectDialog(const QStringList& extensions, bool allowFiles, bool showGdbDirs,
                    QWidget* parent)
        : QDialog(parent)
        , m_extensions(extensions)
        , m_allowFiles(allowFiles)
        , m_showGdbDirs(showGdbDirs)
    {
        setWindowTitle(QStringLiteral("选择数据"));
        resize(520, 560);

        auto* hint = new QLabel(QStringLiteral("勾选要添加的文件夹或文件（勾选文件夹＝整个文件夹参与），双击或点箭头展开文件夹"), this);
        hint->setWordWrap(true);

        m_tree = new QTreeWidget(this);
        m_tree->setHeaderHidden(true);
        m_tree->setColumnCount(1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(hint);
        layout->addWidget(m_tree, 1);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_tree, &QTreeWidget::itemExpanded, this,
                [this](QTreeWidgetItem* item) { populateItem(item); });

        for (const QFileInfo& drive : QDir::drives())
        {
            auto* item = new QTreeWidgetItem(QStringList(drive.absolutePath()));
            item->setData(0, PathRole, drive.absolutePath());
            item->setCheckState(0, Qt::Unchecked);
            item->addChild(new QTreeWidgetItem);
            m_tree->addTopLevelItem(item);
        }
    }

    QStringList selectedPaths() const
    {
        QStringList result;
        collectChecked(m_tree->invisibleRootItem(), result);
        return result;
    }

private:
    enum { PathRole = Qt::UserRole };

    QStringList m_extensions;
    bool m_allowFiles;
    bool m_showGdbDirs;
    QTreeWidget* m_tree = nullptr;

    void populateItem(QTreeWidgetItem* item)
    {
        if (item->childCount() == 1 && item->child(0)->data(0, PathRole).isNull())
            delete item->takeChild(0);
        if (item->childCount() > 0) return;

        QDir dir(item->data(0, PathRole).toString());
        const QFileInfoList subDirs = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
        for (const QFileInfo& fi : subDirs)
        {
            if (!m_showGdbDirs && fi.fileName().endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
                continue;
            auto* child = new QTreeWidgetItem(QStringList(fi.fileName()));
            child->setData(0, PathRole, fi.absoluteFilePath());
            child->setCheckState(0, Qt::Unchecked);
            child->addChild(new QTreeWidgetItem);
            item->addChild(child);
        }
        if (m_allowFiles)
        {
            const QFileInfoList files = dir.entryInfoList(
                QDir::Files | QDir::Readable, QDir::Name);
            for (const QFileInfo& fi : files)
            {
                if (!m_extensions.contains(fi.suffix().toLower()))
                    continue;
                auto* child = new QTreeWidgetItem(QStringList(fi.fileName()));
                child->setData(0, PathRole, fi.absoluteFilePath());
                child->setCheckState(0, Qt::Unchecked);
                item->addChild(child);
            }
        }
    }

    void collectChecked(QTreeWidgetItem* item, QStringList& out) const
    {
        for (int i = 0; i < item->childCount(); ++i)
        {
            QTreeWidgetItem* child = item->child(i);
            const QString path = child->data(0, PathRole).toString();
            if (path.isEmpty()) continue;
            if (child->checkState(0) == Qt::Checked)
                out << path;
            else
                collectChecked(child, out);
        }
    }
};

#endif // SRC_SELECT_DIALOG_H
