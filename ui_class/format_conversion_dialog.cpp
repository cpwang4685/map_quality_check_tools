#include "format_conversion_dialog.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QCoreApplication>
#include <QPushButton>
#include <QTimer>
#include <QProcess>
#include <QSettings>
#include <QTreeWidget>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QListWidget>
#include <qgsapplication.h>
#include <qgssettings.h>
#include <qgsgui.h>

#include "../ui_task/se_format_convert_task.h"

FormatConversionDialog::FormatConversionDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);
    QgsGui::enableAutoGeometryRestore(this);

    // 【2026-09-10】收紧对话框顶部空白。
    // 平台 sci-fi 主题（resources/styles/ltzk_sci_fi.qss）里的 QGroupBox 是"标题浮在
    // 框外"的样式：QGroupBox { margin-top: 52px; padding: 8px 14px 14px 14px; }，
    // 这 52px 是给 QGroupBox::title 留的带子（title 用 subcontrol-origin: margin 定位）。
    // 本对话框最外层的 groupBox_main 只负责框住"转换设置/输入输出设置/日志设置"三个
    // 子分组、自身 title 为空，于是这段带子全程留白——对话框顶部到主布局之间凭空多出
    // ~52px。这里只把这一个 groupBox 的标题带收掉。
    // 用 ID 选择器，三个子分组（有标题、需要标题带）不受影响；背景/边框/圆角仍走平台
    // 全局 QSS——widget 级样式表只覆盖它显式写了的那两个属性。
    ui.groupBox_main->setStyleSheet(
        QStringLiteral("QGroupBox#groupBox_main { margin-top: 0px; padding-top: 4px; }"));

    setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);
    ui.widget_batchBtns->setVisible(false);
    ui.listWidget_SrcList->setVisible(false);

#ifndef Q_OS_WIN
    // 麒麟无 Microsoft Access Database Engine（ACE），隐藏 mdb2gdb 选项
    ui.radioButton_mdb2gdb->setVisible(false);
#endif

    connect(ui.Button_Save,          &QPushButton::clicked, this, &FormatConversionDialog::Button_Save_clicked);
    connect(ui.Button_Open,          &QPushButton::clicked, this, &FormatConversionDialog::Button_Open_clicked);
    connect(ui.Button_OK,            &QPushButton::clicked, this, &FormatConversionDialog::Button_OK_accepted);
    connect(ui.Button_Cancel,        &QPushButton::clicked, this, &FormatConversionDialog::Button_Cancel_rejected);
    connect(ui.Button_SaveLogFilePath, &QPushButton::clicked, this, &FormatConversionDialog::pushButton_SaveLog_clicked);

    connect(ui.radioButton_geojson2shp,  &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_gpkg2shp,     &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_geojson2gpkg, &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_shp2gpkg,     &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_gdb2shp,      &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_shp2gdb,      &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_mdb2gdb,      &QRadioButton::toggled, this, &FormatConversionDialog::onConversionTypeChanged);
    connect(ui.radioButton_BatchMode,    &QRadioButton::toggled, this, &FormatConversionDialog::onBatchToggled);
    connect(ui.Button_SelectData,        &QPushButton::clicked,  this, &FormatConversionDialog::Button_SelectData_clicked);
    connect(ui.Button_SelectFolder,      &QPushButton::clicked,  this, &FormatConversionDialog::Button_SelectFolder_clicked);
    connect(ui.Button_RemoveSelected,    &QPushButton::clicked,  this, &FormatConversionDialog::Button_RemoveSelected_clicked);
    connect(ui.listWidget_SrcList, &QListWidget::itemSelectionChanged, this, [this] {
        ui.Button_RemoveSelected->setEnabled(!ui.listWidget_SrcList->selectedItems().isEmpty());
    });

    // 重置按钮 — 恢复默认图层名/库名
    mBtnResetName = new QPushButton(QStringLiteral("↺"), this);
    mBtnResetName->setMaximumWidth(28);
    mBtnResetName->setToolTip(tr("重置为默认名称"));
    // 平台深色 QSS 给 QPushButton 加 padding(7px 16px)+min-height，28px 宽的符号按钮
    // 内容区被压没导致 ↺ 图案截断；widget 级局部样式仅清内边距、定字号，
    // 渐变背景/边框/悬停效果保持不变（与 merge_dialog 小符号按钮同款处理）。
    mBtnResetName->setStyleSheet(QStringLiteral("QPushButton { padding: 0px; font-size: 15px; }"));
    ui.gridLayout_io->addWidget(mBtnResetName, 5, 1);
    connect(mBtnResetName, &QPushButton::clicked, this, &FormatConversionDialog::resetAllNames);

    // placeholder 提示
    ui.lineEdit_LayerName->setPlaceholderText(tr("自动从源文件名填充"));
    ui.lineEdit_GdbName->setPlaceholderText(tr("自动从源文件名填充"));
    ui.lineEdit_OutputLogPath->setPlaceholderText(tr("默认跟随输出路径"));

    // 手动编辑跟踪
    connect(ui.lineEdit_LayerName, &QLineEdit::textEdited, this, [this] { m_bLayerNameManual = true; });
    connect(ui.lineEdit_GdbName,   &QLineEdit::textEdited, this, [this] { m_bGdbNameManual = true; });
    connect(ui.lineEdit_OutputLogPath, &QLineEdit::textEdited, this, [this] { m_bLogPathAutoFollow = false; });

    // 输入路径变化 → 自动填充名称
    connect(ui.lineEdit_InputDataPath, &QLineEdit::textChanged, this, &FormatConversionDialog::onInputPathEdited);
    // 输出路径变化 → 日志联动
    connect(ui.lineEdit_OutputPath, &QLineEdit::textChanged, this, &FormatConversionDialog::onOutputPathEdited);

    restoreState();
    ui.radioButton_geojson2shp->setChecked(true);

    ui.lineEdit_InputDataPath->setText(m_qstrInputDataPath);
    ui.lineEdit_OutputPath->setText(m_qstrSavePath);
    ui.lineEdit_OutputLogPath->setText(m_qstrOutputLogPath);

    onConversionTypeChanged();
    autoFillNames();
    autoUpdateLogPath();

    // 窗口打开后高度贴合内容，避免固定高度造成空旷；宽度保持不变
    QTimer::singleShot(0, this, [this] { resizeToContent(); });
}

FormatConversionDialog::~FormatConversionDialog()
{
    QgsSettings settings;
    settings.setValue(QStringLiteral("FormatConversion/InputDataPath"), m_qstrInputDataPath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("FormatConversion/SavePath"),      m_qstrSavePath,      QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("FormatConversion/OutputLogPath"), m_qstrOutputLogPath, QgsSettings::Section::Plugins);
}

void FormatConversionDialog::Button_Save_clicked()
{
    QString curPath = QCoreApplication::applicationDirPath();

    // SHP 目标 / GDB 目标 → 选择输出目录
    if (isShpTarget() || isGdbTarget())
    {
        QString selectedDir = QFileDialog::getExistingDirectory(this,
            tr("请选择转换后的数据存储路径"), curPath, QFileDialog::ShowDirsOnly);
        if (!selectedDir.isEmpty())
        {
            ui.lineEdit_OutputPath->setText(selectedDir);
            m_qstrSavePath = selectedDir;
        }
    }
    // GPKG 目标 → 保存文件；批量模式每个源文件 → <源名>.gpkg，改为选输出目录
    else
    {
        if (ui.radioButton_BatchMode->isChecked())
        {
            QString selectedDir = QFileDialog::getExistingDirectory(this,
                tr("请选择转换后的数据存储路径"), curPath, QFileDialog::ShowDirsOnly);
            if (!selectedDir.isEmpty())
            {
                ui.lineEdit_OutputPath->setText(selectedDir);
                m_qstrSavePath = selectedDir;
            }
        }
        else
        {
            QString filter = tr("gpkg 文件(*.gpkg)");
            QString strFileName = QFileDialog::getSaveFileName(this,
                tr("保存gpkg文件"), curPath, filter);
            if (!strFileName.isEmpty())
            {
                ui.lineEdit_OutputPath->setText(strFileName);
                m_qstrSavePath = strFileName;
            }
        }
    }
}

void FormatConversionDialog::Button_Open_clicked()
{
    QString curPath = QCoreApplication::applicationDirPath();

    // gdb2shp → 选择 gdb 目录（GDB 是目录型数据，单文件模式选目录）
    if (ui.radioButton_gdb2shp->isChecked())
    {
        QString directory = QFileDialog::getExistingDirectory(this,
            tr("请选择GDB数据目录"), curPath, QFileDialog::ShowDirsOnly);
        if (!directory.isEmpty())
        {
            m_srcFileList.clear();
            m_qstrInputDataPath = directory;
            ui.lineEdit_InputDataPath->setText(directory);
        }
        return;
    }

    // 单文件模式
    if (ui.radioButton_geojson2gpkg->isChecked() || ui.radioButton_geojson2shp->isChecked())
    {
        QString filter = tr("GeoJSON 文件(*.geojson *.json)");
        QString fileName = QFileDialog::getOpenFileName(this,
            tr("选择GeoJSON文件"), curPath, filter);
        if (!fileName.isEmpty())
        {
            m_srcFileList.clear();
            m_qstrInputDataPath = fileName;
            ui.lineEdit_InputDataPath->setText(fileName);
        }
    }
    // shp2gpkg / shp2gdb → 选择 shp 文件
    else if (ui.radioButton_shp2gpkg->isChecked() || ui.radioButton_shp2gdb->isChecked())
    {
        QString filter = tr("shp 文件(*.shp)");
        QString strFileName = QFileDialog::getOpenFileName(this,
            tr("选择shp文件"), curPath, filter);
        if (!strFileName.isEmpty())
        {
            m_srcFileList.clear();
            m_qstrInputDataPath = strFileName;
            ui.lineEdit_InputDataPath->setText(strFileName);
        }
    }
    // gpkg2shp → 选择单个 gpkg 文件
    else if (ui.radioButton_gpkg2shp->isChecked())
    {
        QString filter = tr("gpkg 文件(*.gpkg)");
        QString strFileName = QFileDialog::getOpenFileName(this,
            tr("选择gpkg文件"), curPath, filter);
        if (!strFileName.isEmpty())
        {
            m_srcFileList.clear();
            m_qstrInputDataPath = strFileName;
            ui.lineEdit_InputDataPath->setText(strFileName);
        }
    }
    // mdb2gdb → 选择单个 mdb 文件
    else if (ui.radioButton_mdb2gdb->isChecked())
    {
        QString filter = tr("mdb 文件(*.mdb)");
        QString strFileName = QFileDialog::getOpenFileName(this,
            tr("选择mdb文件"), curPath, filter);
        if (!strFileName.isEmpty())
        {
            m_srcFileList.clear();
            m_qstrInputDataPath = strFileName;
            ui.lineEdit_InputDataPath->setText(strFileName);
        }
    }
}

void FormatConversionDialog::currentSourceFilter(QStringList& extensions, bool& allowFiles, bool& showGdbDirs) const
{
    allowFiles = true;
    showGdbDirs = false;
    if (ui.radioButton_gdb2shp->isChecked())
    {
        allowFiles = false;
        showGdbDirs = true;
    }
    else if (ui.radioButton_geojson2shp->isChecked() || ui.radioButton_geojson2gpkg->isChecked())
    {
        extensions << QStringLiteral("geojson") << QStringLiteral("json");
    }
    else if (ui.radioButton_shp2gpkg->isChecked() || ui.radioButton_shp2gdb->isChecked())
    {
        extensions << QStringLiteral("shp");
    }
    else if (ui.radioButton_gpkg2shp->isChecked())
    {
        extensions << QStringLiteral("gpkg");
    }
    else if (ui.radioButton_mdb2gdb->isChecked())
    {
        extensions << QStringLiteral("mdb");
    }
}

void FormatConversionDialog::Button_SelectData_clicked()
{
    QStringList extensions;
    bool allowFiles = true;
    bool showGdbDirs = false;
    currentSourceFilter(extensions, allowFiles, showGdbDirs);
    if (!allowFiles) return;

    // 系统原生文件对话框，多选（Shift 连选、Ctrl 逐个多选）
    QString filter;
    if (!extensions.isEmpty())
    {
        QStringList pats;
        for (const QString& e : extensions)
            pats << QStringLiteral("*.") + e;
        filter = QStringLiteral("数据文件 (%1)").arg(pats.join(QStringLiteral(" ")));
    }
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要转换的文件"), QString(), filter);
    if (files.isEmpty()) return;

    appendSourcePaths(files);
    resizeToContent();
}

void FormatConversionDialog::Button_SelectFolder_clicked()
{
    // 系统原生文件夹对话框；文件夹＝整个文件夹参与转换
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要转换的文件夹（整个文件夹参与转换）"));
    if (dir.isEmpty()) return;

    appendSourcePaths(QStringList() << dir);
    resizeToContent();
}

void FormatConversionDialog::appendSourcePaths(const QStringList& paths)
{
    QStringList extensions;
    bool allowFiles = true;
    bool showGdbDirs = false;
    currentSourceFilter(extensions, allowFiles, showGdbDirs);

    QStringList rejected;
    for (const QString& p : paths)
    {
        const QFileInfo fi(p);
        if (fi.isDir())
        {
            if (!showGdbDirs && fi.fileName().endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
            {
                rejected << p;
                continue;
            }
        }
        else if (fi.isFile())
        {
            if (!extensions.contains(fi.suffix().toLower()))
            {
                rejected << p;
                continue;
            }
        }

        QString canon = QFileInfo(p).canonicalFilePath();
        if (canon.isEmpty()) canon = QDir::cleanPath(p);
        bool bExists = false;
        for (const QString& old : m_srcFileList)
        {
            QString oldCanon = QFileInfo(old).canonicalFilePath();
            if (oldCanon.isEmpty()) oldCanon = QDir::cleanPath(old);
            if (QString::compare(canon, oldCanon, Qt::CaseInsensitive) == 0)
            {
                bExists = true;
                break;
            }
        }
        if (bExists) continue;

        m_srcFileList << p;
        QString label = (fi.isDir() ? QStringLiteral("[文件夹] ") : QStringLiteral("[文件] ")) + p;
        auto* item = new QListWidgetItem(label, ui.listWidget_SrcList);
        item->setData(Qt::UserRole, p);
        item->setToolTip(p);
    }

    if (!rejected.isEmpty())
    {
        QString names;
        const int showCount = qMin(5, rejected.size());
        for (int i = 0; i < showCount; ++i)
            names += QStringLiteral("\n  ") + QFileInfo(rejected[i]).fileName();
        if (rejected.size() > showCount)
            names += QStringLiteral("\n  …");
        QMessageBox::information(this, tr("提示"),
            tr("已忽略 %1 个与当前源格式不符的条目，未加入列表：%2")
                .arg(rejected.size()).arg(names));
    }

    // 批量模式下库名跟随首个源条目自动填充
    autoFillNames();
}

void FormatConversionDialog::Button_RemoveSelected_clicked()
{
    const QList<QListWidgetItem*> sel = ui.listWidget_SrcList->selectedItems();
    for (QListWidgetItem* it : sel)
    {
        m_srcFileList.removeAll(it->data(Qt::UserRole).toString());
        delete ui.listWidget_SrcList->takeItem(ui.listWidget_SrcList->row(it));
    }
    if (ui.listWidget_SrcList->count() == 0)
        ui.Button_RemoveSelected->setEnabled(false);
    // 移除后库名跟随新的首个源条目
    autoFillNames();
}

void FormatConversionDialog::Button_OK_accepted()
{
    // 日志级别
    int iLogLevel = ui.comboBox_LogLevel->currentIndex();
    string strOutputLogPath = ui.lineEdit_OutputLogPath->text().toUtf8().toStdString();

    // 检查日志路径
    if (!CheckFileOrDirExist(ui.lineEdit_OutputLogPath->text()))
    {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("格式转换工具"));
        msgBox.setText(tr("文件夹：%1不存在！").arg(ui.lineEdit_OutputLogPath->text()));
        msgBox.setStandardButtons(QMessageBox::Yes);
        msgBox.setDefaultButton(QMessageBox::Yes);
        msgBox.exec();
        return;
    }

    // 批量模式：要求清单里已添加数据；单文件模式：检查输入路径存在
    if (ui.radioButton_BatchMode->isChecked())
    {
        if (ui.listWidget_SrcList->count() == 0)
        {
            QMessageBox::warning(this, tr("提示"),
                tr("请先点击“选择数据”添加要转换的文件或文件夹"));
            return;
        }

        // 预检：文件夹条目里没有任何可转换数据 → 移出清单并提示
        QStringList srcExts;
        bool allowFiles = true;
        bool showGdbDirs = false;
        currentSourceFilter(srcExts, allowFiles, showGdbDirs);

        QStringList removed;
        for (int i = m_srcFileList.size() - 1; i >= 0; --i)
        {
            const QString p = m_srcFileList[i];
            const QFileInfo fi(p);
            if (fi.isFile()) continue;

            if (showGdbDirs)
            {
                if (fi.fileName().endsWith(QStringLiteral(".gdb"), Qt::CaseInsensitive))
                    continue;
                QDirIterator it(p, QStringList() << QStringLiteral("*.gdb") << QStringLiteral("*.GDB"),
                                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                                QDirIterator::Subdirectories);
                if (it.hasNext()) continue;
            }
            else
            {
                QDirIterator it(p, QDir::Files | QDir::Readable,
                                QDirIterator::Subdirectories);
                bool bFound = false;
                while (it.hasNext())
                {
                    it.next();
                    if (srcExts.contains(it.fileInfo().suffix().toLower()))
                    {
                        bFound = true;
                        break;
                    }
                }
                if (bFound) continue;
            }

            removed << p;
            m_srcFileList.removeAt(i);
            for (int r = 0; r < ui.listWidget_SrcList->count(); ++r)
            {
                if (ui.listWidget_SrcList->item(r)->data(Qt::UserRole).toString() == p)
                {
                    delete ui.listWidget_SrcList->takeItem(r);
                    break;
                }
            }
        }

        if (!removed.isEmpty())
        {
            QString names;
            const int showCount = qMin(5, removed.size());
            for (int i = 0; i < showCount; ++i)
                names += QStringLiteral("\n  ") + removed[i];
            if (removed.size() > showCount)
                names += QStringLiteral("\n  …");
            const QString fmtDesc = showGdbDirs ? QStringLiteral("GDB")
                                                : srcExts.join(QLatin1Char('/'));
            if (m_srcFileList.isEmpty())
            {
                QMessageBox::warning(this, tr("提示"),
                    tr("清单中的条目均未找到任何可转换的 %1 数据，已全部移除：%2\n无法开始转换。")
                        .arg(fmtDesc).arg(names));
                return;
            }
            QMessageBox::information(this, tr("提示"),
                tr("以下 %1 个条目中未找到任何可转换的 %2 数据，已从清单移除：%3")
                    .arg(removed.size()).arg(fmtDesc).arg(names));
        }
    }
    else if (!CheckFileOrDirExist(ui.lineEdit_InputDataPath->text()))
    {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("格式转换工具"));
        msgBox.setText(tr("文件路径：%1不存在！").arg(ui.lineEdit_InputDataPath->text()));
        msgBox.setStandardButtons(QMessageBox::Yes);
        msgBox.setDefaultButton(QMessageBox::Yes);
        msgBox.exec();
        return;
    }

    // 输出到文件夹时检查输出路径（SHP/GDB 输出均为目录；GPKG 批量模式也是目录）
    if (isShpTarget() || isGdbTarget()
        || (ui.radioButton_BatchMode->isChecked() && isGpkgTarget()))
    {
        if (!CheckFileOrDirExist(ui.lineEdit_OutputPath->text()))
        {
            QMessageBox msgBox;
            msgBox.setWindowTitle(tr("格式转换工具"));
            msgBox.setText(tr("文件夹：%1不存在！").arg(ui.lineEdit_OutputPath->text()));
            msgBox.setStandardButtons(QMessageBox::Yes);
            msgBox.setDefaultButton(QMessageBox::Yes);
            msgBox.exec();
            return;
        }
    }

    // 对于 GPKG 目标（单文件），确保父目录存在
    if (ui.radioButton_geojson2gpkg->isChecked() || ui.radioButton_shp2gpkg->isChecked())
    {
        QFileInfo fi(ui.lineEdit_OutputPath->text());
        QDir().mkpath(fi.absolutePath());
    }

    // SHP 目标需要填写图层名（批量模式按源文件名自动命名、gdb2shp 用 GDB 内图层原名，均无需填写）
    if (!ui.radioButton_BatchMode->isChecked() && isShpTarget()
        && !ui.radioButton_gdb2shp->isChecked())
    {
        if (ui.lineEdit_LayerName->text().trimmed().isEmpty())
        {
            QMessageBox::warning(this, tr("提示"), tr("请输入SHP图层名"));
            return;
        }
    }
    // GDB 目标需要填写库名：单个模式=输出库名；批量模式=所有源数据汇入的同一个库
    if (isGdbTarget())
    {
        if (ui.lineEdit_GdbName->text().trimmed().isEmpty())
        {
            QMessageBox::warning(this, tr("提示"), tr("请输入GDB库名"));
            return;
        }
    }

    // MDB 源需要本机安装 Microsoft Access Database Engine（ACE）
    // （麒麟平台 mdb2gdb 已隐藏，此检测仅 Windows 生效）
#ifdef Q_OS_WIN
    if (isMdbSource() && !isAccessEngineInstalled())
    {
        QString installer = accessEngineInstallerPath();
        if (!installer.isEmpty())
        {
            if (QMessageBox::question(this, tr("格式转换工具"),
                tr("读取 MDB 需要 Microsoft Access Database Engine（微软 Access 数据库引擎），\n"
                   "检测到本机尚未安装。是否立即安装？（安装时需要管理员权限）")) == QMessageBox::Yes)
            {
                QProcess::startDetached(QStringLiteral("powershell"), {
                    QStringLiteral("-NoProfile"),
                    QStringLiteral("-Command"),
                    QStringLiteral("Start-Process -FilePath '%1' -ArgumentList '/quiet','/norestart' -Verb RunAs").arg(installer)
                });
                QMessageBox::information(this, tr("格式转换工具"),
                    tr("安装程序已启动，请等待安装完成后重新执行转换。"));
            }
        }
        else
        {
            QMessageBox::warning(this, tr("格式转换工具"),
                tr("读取 MDB 需要 Microsoft Access Database Engine，本机未安装且插件目录未附带安装包。\n"
                   "请联系管理员安装 Access Database Engine 2016（64位）后重试。"));
        }
        return;
    }
#endif

    ui.progressBar->reset();

    QString qInputPath = !m_srcFileList.isEmpty()
        ? m_srcFileList.first() : ui.lineEdit_InputDataPath->text();
    string strInputPath  = qInputPath.toUtf8().toStdString();
    string strOutputPath = ui.lineEdit_OutputPath->text().toUtf8().toStdString();

    // 根据 radio button 确定源/目标格式
    string strSrcDriver, strTgtDriver, strSrcExt, strTgtExt;
    QString taskName;

    if (ui.radioButton_geojson2shp->isChecked())
    {
        strSrcDriver = "GeoJSON"; strTgtDriver = "ESRI Shapefile";
        strSrcExt = "geojson"; strTgtExt = "shp";
        taskName = tr("geojson转shp");
    }
    else if (ui.radioButton_gpkg2shp->isChecked())
    {
        strSrcDriver = "GPKG"; strTgtDriver = "ESRI Shapefile";
        strSrcExt = "gpkg"; strTgtExt = "shp";
        taskName = tr("gpkg转shp");
    }
    else if (ui.radioButton_geojson2gpkg->isChecked())
    {
        strSrcDriver = "GeoJSON"; strTgtDriver = "GPKG";
        strSrcExt = "geojson"; strTgtExt = "gpkg";
        taskName = tr("geojson转gpkg");
    }
    else if (ui.radioButton_shp2gpkg->isChecked())
    {
        strSrcDriver = "ESRI Shapefile"; strTgtDriver = "GPKG";
        strSrcExt = "shp"; strTgtExt = "gpkg";
        taskName = tr("shp转gpkg");
    }
    else if (ui.radioButton_gdb2shp->isChecked())
    {
        strSrcDriver = "OpenFileGDB"; strTgtDriver = "ESRI Shapefile";
        strSrcExt = "gdb"; strTgtExt = "shp";
        taskName = tr("gdb转shp");
    }
    else if (ui.radioButton_shp2gdb->isChecked())
    {
        strSrcDriver = "ESRI Shapefile"; strTgtDriver = "OpenFileGDB";
        strSrcExt = "shp"; strTgtExt = "gdb";
        taskName = tr("shp转gdb");
    }
    else if (ui.radioButton_mdb2gdb->isChecked())
    {
        strSrcDriver = "PGeo"; strTgtDriver = "OpenFileGDB";
        strSrcExt = "mdb"; strTgtExt = "gdb";
        taskName = tr("mdb转gdb");
    }
    else
    {
        QMessageBox::warning(this, tr("提示"), tr("请选择转换类型"));
        return;
    }

    // gdb2shp：输出名 = GDB 内图层原名，不透传"图层名"给转换任务，
    // 否则会被拼成 库名_图层名 / 库名（2026-09-11 甲方口径）
    string strLayerName = ui.radioButton_gdb2shp->isChecked()
        ? string()
        : ui.lineEdit_LayerName->text().trimmed().toUtf8().toStdString();
    string strGdbName   = ui.lineEdit_GdbName->text().trimmed().toUtf8().toStdString();

    bool bBatch = ui.radioButton_BatchMode->isChecked();
    SeFormatConvertTask* task = new SeFormatConvertTask(
        taskName, strInputPath, strOutputPath,
        strSrcDriver, strTgtDriver, strSrcExt, strTgtExt,
        iLogLevel, strOutputLogPath, !bBatch,
        strLayerName, strGdbName);
    if (bBatch && !m_srcFileList.isEmpty())
        task->setSrcFileList(m_srcFileList);

    connect(task, &SeFormatConvertTask::taskFinished, this, &FormatConversionDialog::onTaskFinished);
    connect(task, &QgsTask::progressChanged, this, [this](double p) {
        ui.progressBar->setValue(static_cast<int>(p));
    });

    ui.Button_OK->setEnabled(false); // 任务运行中禁止重复发起
    QgsApplication::taskManager()->addTask(task);

    // 保存设置
    QgsSettings settings;
    settings.setValue(QStringLiteral("FormatConversion/InputDataPath"), m_qstrInputDataPath, QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("FormatConversion/SavePath"),      m_qstrSavePath,      QgsSettings::Section::Plugins);
    settings.setValue(QStringLiteral("FormatConversion/OutputLogPath"), m_qstrOutputLogPath, QgsSettings::Section::Plugins);
}

void FormatConversionDialog::Button_Cancel_rejected()
{
    reject();
}

void FormatConversionDialog::pushButton_SaveLog_clicked()
{
    QString curPath = m_qstrOutputLogPath;
    QString selectedDir = QFileDialog::getExistingDirectory(this,
        tr("请选择日志文件保存路径"), curPath, QFileDialog::ShowDirsOnly);
    if (!selectedDir.isEmpty())
    {
        m_qstrOutputLogPath = selectedDir;
        ui.lineEdit_OutputLogPath->setText(m_qstrOutputLogPath);
    }
}

void FormatConversionDialog::restoreState()
{
    const QgsSettings settings;
    m_qstrInputDataPath  = settings.value(QStringLiteral("FormatConversion/InputDataPath"),  QDir::homePath(), QgsSettings::Section::Plugins).toString();
    m_qstrSavePath       = settings.value(QStringLiteral("FormatConversion/SavePath"),       QDir::homePath(), QgsSettings::Section::Plugins).toString();
    m_qstrOutputLogPath  = settings.value(QStringLiteral("FormatConversion/OutputLogPath"),  QDir::homePath(), QgsSettings::Section::Plugins).toString();
}

void FormatConversionDialog::resizeToContent()
{
    // 先把挂起的 LayoutRequest 事件处理掉，清掉布局缓存；再 invalidate+activate 强制重算，
    // 按最新总高度收缩（宽度保持不变），防止隐藏控件后仍按批量的过期高度显示。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    layout()->invalidate();
    layout()->activate();
    const int w = width();
    const int h = layout()->totalSizeHint().height();
    if (h > 0)
        resize(w, h);
}

void FormatConversionDialog::onConversionTypeChanged()
{
    bool bBatch = ui.radioButton_BatchMode->isChecked();
    bool bShpTarget = isShpTarget();
    bool bGdbTarget = isGdbTarget();

    // 批量模式下切换格式后，旧清单按旧格式过滤已失效，清空重选
    if (bBatch)
    {
        m_srcFileList.clear();
        ui.listWidget_SrcList->clear();
    }

    // 批量模式：隐藏路径输入行与浏览按钮，显示“选择文件/选择文件夹”+清单+移除选中
    bool bGdbSrc = ui.radioButton_gdb2shp->isChecked();
    ui.label_input->setVisible(!bBatch);
    ui.lineEdit_InputDataPath->setVisible(!bBatch);
    ui.Button_Open->setVisible(!bBatch);
    ui.widget_batchBtns->setVisible(bBatch);
    ui.listWidget_SrcList->setVisible(bBatch);

    // GDB 源是目录，批量时只有"选择文件夹"可用
    ui.Button_SelectData->setEnabled(!bGdbSrc);

    ui.Button_Open->setText(bGdbSrc ? tr("选择目录") : tr("浏览"));
    if (!bBatch)
    {
        ui.lineEdit_InputDataPath->setPlaceholderText(
            bGdbSrc ? tr("请选择GDB数据目录") : tr("请选择单个数据文件"));
    }

    // 批量模式下 SHP 图层名按源文件名自动命名，隐藏输入；
    // gdb2shp 的输出名一律用 GDB 内部图层自己的名字（2026-09-11 甲方口径），
    // 没有"图层名"需要用户填，一并隐藏 —— 否则会自动填入 GDB 目录名并拼进输出名。
    bool bShowLayerName = bShpTarget && !bBatch && !bGdbSrc;
    ui.label_layerName->setVisible(bShowLayerName);
    ui.lineEdit_LayerName->setVisible(bShowLayerName);
    if (!bShowLayerName) ui.lineEdit_LayerName->clear();

    // GDB 目标统一需要库名：单个模式=输出库名；批量模式=所有源数据汇入的同一个库
    bool bShowGdbName = bGdbTarget;
    ui.label_gdbName->setVisible(bShowGdbName);
    ui.lineEdit_GdbName->setVisible(bShowGdbName);
    if (!bShowGdbName) ui.lineEdit_GdbName->clear();

    mBtnResetName->setVisible(bShowLayerName || bShowGdbName);
    if (bShowGdbName && !bShowLayerName)
        ui.gridLayout_io->addWidget(mBtnResetName, 10, 1);   // 放到 GDB 库名行
    else if (bShowLayerName)
        ui.gridLayout_io->addWidget(mBtnResetName, 8, 1);   // 放回 SHP 图层名行

    autoFillNames();
    resizeToContent();
}

void FormatConversionDialog::onBatchToggled(bool checked)
{
    m_srcFileList.clear();
    ui.listWidget_SrcList->clear();
    m_qstrInputDataPath.clear();
    ui.lineEdit_InputDataPath->clear();
    m_bLayerNameManual = false;
    m_bGdbNameManual = false;
    // 批量模式输出到目录；单个模式 GPKG 选的是文件路径，切换后需重选目录
    if (checked)
    {
        QFileInfo outFi(ui.lineEdit_OutputPath->text());
        if (outFi.isFile())
        {
            ui.lineEdit_OutputPath->clear();
            m_qstrSavePath.clear();
        }
    }
    onConversionTypeChanged();

    // 切到批量模式时自动弹出数据选择窗口，用户不用自己找按钮
    if (checked)
    {
        if (ui.radioButton_gdb2shp->isChecked())
            Button_SelectFolder_clicked();
        else
            Button_SelectData_clicked();
    }

    // 事件循环转完后（含模态选择窗口）再精确收放一次，覆盖任何过期缓存导致的高度残留
    QTimer::singleShot(0, this, [this] { resizeToContent(); });
}

void FormatConversionDialog::onTaskFinished(bool result)
{
    ui.Button_OK->setEnabled(true);
    if (result)
        ui.progressBar->setValue(100);

    SeFormatConvertTask* t = qobject_cast<SeFormatConvertTask*>(sender());
    if (result && (!t || t->successCount() == t->totalCount()))
    {
        QMessageBox::information(this, tr("格式转换"), tr("格式转换完成!"));
    }
    else if (result && t)
    {
        // 部分失败：如实报成功 X/N 和失败名单，不再笼统提示"完成"
        QString names;
        const QStringList failed = t->failedNames();
        const int showCount = qMin(5, failed.size());
        for (int k = 0; k < showCount; ++k)
            names += QStringLiteral("\n  ") + failed[k];
        if (failed.size() > showCount)
            names += QStringLiteral("\n  …");
        QMessageBox::warning(this, tr("格式转换"),
            tr("转换部分完成：成功 %1/%2，失败 %3 个：%4\n详情见日志。")
                .arg(t->successCount()).arg(t->totalCount())
                .arg(failed.size()).arg(names));
    }
    else
    {
        QMessageBox::warning(this, tr("格式转换"), tr("格式转换失败，请查看日志了解详情。"));
    }
}

bool FormatConversionDialog::CheckFileOrDirExist(const QString& path)
{
    QFileInfo info(path);
    return info.exists();
}

QString FormatConversionDialog::cleanFileName(const QString& fileName)
{
    QString result;
    for (const QChar& ch : fileName)
    {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-')
            result += ch;
        else if (ch == '.' && result.isEmpty())
            continue;
        else
            result += '_';
    }
    while (result.startsWith('_'))
        result.remove(0, 1);
    while (result.endsWith('_'))
        result.chop(1);
    if (result.length() > 32)
        result = result.left(32);
    if (result.isEmpty())
        result = QStringLiteral("vector_layer");
    return result;
}

void FormatConversionDialog::autoFillNames()
{
    QString src;
    if (ui.radioButton_BatchMode->isChecked())
    {
        // 批量：库名默认取首个源条目的名（文件夹用文件夹名，文件用基名）
        if (m_srcFileList.isEmpty()) return;
        src = m_srcFileList.first();
    }
    else
    {
        src = ui.lineEdit_InputDataPath->text();
        if (src.isEmpty()) return;
    }
    QFileInfo fi(src);
    QString baseName = fi.isDir() ? fi.fileName() : fi.completeBaseName();
    if (baseName.isEmpty())
        baseName = fi.fileName();
    if (baseName.isEmpty()) return;
    QString clean = cleanFileName(baseName);

    // gdb2shp 不自动填 SHP 图层名：输出名由 GDB 内图层名决定（2026-09-11 甲方口径）
    if (!m_bLayerNameManual && !ui.radioButton_gdb2shp->isChecked())
        ui.lineEdit_LayerName->setText(clean);
    if (!m_bGdbNameManual)
        ui.lineEdit_GdbName->setText(clean);
}

void FormatConversionDialog::autoUpdateLogPath()
{
    if (!m_bLogPathAutoFollow) return;
    QString outputPath = ui.lineEdit_OutputPath->text();
    if (outputPath.isEmpty()) return;
    QFileInfo fi(outputPath);
    QString logDir = fi.isDir() ? outputPath : fi.absolutePath();
    ui.lineEdit_OutputLogPath->setText(logDir);
}

void FormatConversionDialog::onInputPathEdited(const QString& /*text*/)
{
    m_bLayerNameManual = false;
    m_bGdbNameManual = false;
    autoFillNames();
}

void FormatConversionDialog::onOutputPathEdited(const QString& /*text*/)
{
    autoUpdateLogPath();
}

void FormatConversionDialog::resetAllNames()
{
    m_bLayerNameManual = false;
    m_bGdbNameManual = false;
    autoFillNames();
}

bool FormatConversionDialog::isShpTarget() const
{
    return ui.radioButton_geojson2shp->isChecked()
        || ui.radioButton_gpkg2shp->isChecked()
        || ui.radioButton_gdb2shp->isChecked();
}

bool FormatConversionDialog::isGdbTarget() const
{
    return ui.radioButton_shp2gdb->isChecked()
        || ui.radioButton_mdb2gdb->isChecked();
}

bool FormatConversionDialog::isGpkgTarget() const
{
    return ui.radioButton_geojson2gpkg->isChecked()
        || ui.radioButton_shp2gpkg->isChecked();
}

bool FormatConversionDialog::isMdbSource() const
{
    return ui.radioButton_mdb2gdb->isChecked();
}

bool FormatConversionDialog::isAccessEngineInstalled()
{
    QSettings odbc(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers"),
                   QSettings::NativeFormat);
    return odbc.contains(QStringLiteral("Microsoft Access Driver (*.mdb, *.accdb)"));
}

QString FormatConversionDialog::accessEngineInstallerPath()
{
    QStringList candidates;
    candidates << QDir(QgsApplication::pluginPath())
                     .filePath(QStringLiteral("deps/AccessDatabaseEngine_X64.exe"))
               << QDir(QCoreApplication::applicationDirPath())
                     .filePath(QStringLiteral("deps/AccessDatabaseEngine_X64.exe"));
    for (const QString& p : candidates)
    {
        if (QFileInfo::exists(p))
            return p;
    }
    return QString();
}
