#include "param_config_dialog.h"

#include <algorithm>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QSet>
#include <QTextStream>
#include <QDomNodeList>
#include <QHeaderView>
#include <QCheckBox>
#include <QFileInfo>
#include <QColor>
#include <QTreeWidgetItemIterator>
#include <QLabel>
#include <QHBoxLayout>
#include <QMenu>
#include <QPixmap>
#include <QComboBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QPushButton>
#include <QDebug>
#include <QTimer>

#include "ui_fit_helper.h"

ParamConfigDialog::ParamConfigDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    ui.setupUi(this);
    DialogFitHelper::install(this);

    ui.pushButton_openXml->setAutoDefault(false);
    ui.pushButton_openXml->setDefault(false);
    ui.pushButton_saveConfig->setAutoDefault(false);
    ui.pushButton_saveAs->setAutoDefault(false);
    ui.pushButton_close->setAutoDefault(false);
    ui.pushButton_resetDefaults->setAutoDefault(false);
    ui.pushButton_expandAll->setAutoDefault(false);
    ui.pushButton_collapseAll->setAutoDefault(false);

    // ---- Init tree (single-column) ----
    QTreeWidget* tree = ui.treeWidget_checkItems;
    tree->setHeaderLabels({ QStringLiteral("节点") });
    tree->header()->setStretchLastSection(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setAnimated(true);
    tree->setExpandsOnDoubleClick(true);
    tree->setRootIsDecorated(true);
    tree->setAlternatingRowColors(false);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QTreeWidget::customContextMenuRequested,
            this, &ParamConfigDialog::onTreeContextMenu);

    // ---- Init table (7 columns) ----
    QTableWidget* table = ui.tableWidget_params;
    table->setColumnCount(7);
    table->setHorizontalHeaderLabels({
        QStringLiteral("序号"),
        QStringLiteral("参数键名"),
        QStringLiteral("参数名称"),
        QStringLiteral("参数值"),
        QStringLiteral("参数说明"),
        QStringLiteral("状态"),
        QStringLiteral("常用")
    });
    QHeaderView* hdr = table->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Fixed);
    hdr->setSectionResizeMode(1, QHeaderView::Fixed);
    hdr->setSectionResizeMode(2, QHeaderView::Fixed);
    hdr->setSectionResizeMode(3, QHeaderView::Interactive);
    hdr->setSectionResizeMode(4, QHeaderView::Stretch);
    hdr->setSectionResizeMode(5, QHeaderView::Fixed);
    hdr->setSectionResizeMode(6, QHeaderView::Fixed);
    table->setColumnWidth(0, 36);
    table->setColumnWidth(1, 100);
    table->setColumnWidth(2, 96);
    table->setColumnWidth(3, 70);
    table->setColumnWidth(5, 46);
    table->setColumnWidth(6, 40);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(true);
    table->setTextElideMode(Qt::ElideNone);
    table->setAlternatingRowColors(false);  // 由 populateTable 手动控色，避免干扰

    // ---- 搜索框前加🔍标签 ----
    QLayout* searchParentLayout = ui.lineEdit_search->parentWidget()->layout();
    int searchIdx = searchParentLayout->indexOf(ui.lineEdit_search);
    if (searchIdx >= 0) {
        QLabel* searchIconLabel = new QLabel(QString::fromUtf8("\xF0\x9F\x94\x8D"));
        searchIconLabel->setTextFormat(Qt::PlainText);
        searchIconLabel->setStyleSheet("font-size: 16px; padding-right: 2px;");
        QFont sicFont = searchIconLabel->font();
        sicFont.setFamily(QStringLiteral("Segoe UI Emoji"));
        searchIconLabel->setFont(sicFont);
        searchParentLayout->removeWidget(ui.lineEdit_search);
        QHBoxLayout* searchRow = new QHBoxLayout();
        searchRow->setSpacing(4);
        searchRow->addWidget(searchIconLabel);
        searchRow->addWidget(ui.lineEdit_search);
        static_cast<QBoxLayout*>(searchParentLayout)->insertLayout(searchIdx, searchRow);

        // 搜索结果信息条（搜索框下方）
        m_searchInfoBar = new QFrame();
        m_searchInfoBar->setStyleSheet(
            "QFrame#searchInfoBar { background-color: #1976D2; border-radius: 3px; }");
        m_searchInfoBar->setObjectName("searchInfoBar");
        m_searchInfoBar->setVisible(false);
        QHBoxLayout* infoLayout = new QHBoxLayout(m_searchInfoBar);
        infoLayout->setContentsMargins(6, 3, 6, 3);
        m_searchInfoLabel = new QLabel();
        m_searchInfoLabel->setStyleSheet("color: white; font-size: 12px;");
        infoLayout->addWidget(m_searchInfoLabel);
        static_cast<QBoxLayout*>(searchParentLayout)->insertWidget(searchIdx + 1, m_searchInfoBar);
    }

    ui.lineEdit_search->setPlaceholderText(
        QStringLiteral("搜索任务ID、参数名称或参数键名，回车逐一定位..."));

    // ---- 视图模式切换（单选按钮） ----
    QButtonGroup* viewModeGroup = new QButtonGroup(this);
    QRadioButton* radioFavorites = new QRadioButton(QStringLiteral("常用参数"));
    QRadioButton* radioAll = new QRadioButton(QStringLiteral("全部参数"));
    viewModeGroup->addButton(radioFavorites, 0);
    viewModeGroup->addButton(radioAll, 1);
    radioFavorites->setChecked(true);

    QString radioStyle = QStringLiteral(
        "QRadioButton { color: #555; font-size: 15px; }"
        "QRadioButton:checked { color: #1976D2; font-weight: bold; }");
    radioFavorites->setStyleSheet(radioStyle);
    radioAll->setStyleSheet(radioStyle);

    QVBoxLayout* viewModeLayout = new QVBoxLayout();
    viewModeLayout->setSpacing(2);
    viewModeLayout->addWidget(radioFavorites);
    viewModeLayout->addWidget(radioAll);

    {
        QLayout* viewParentLayout = ui.lineEdit_search->parentWidget()->layout();
        if (viewParentLayout) {
            int lineEditIdx = -1;
            for (int i = 0; i < viewParentLayout->count(); ++i) {
                QLayoutItem* li = viewParentLayout->itemAt(i);
                if (li && li->widget() == ui.lineEdit_search) {
                    lineEditIdx = i;
                    break;
                }
                if (li && li->layout()) {
                    QLayout* inner = li->layout();
                    for (int j = 0; j < inner->count(); ++j) {
                        if (inner->itemAt(j) && inner->itemAt(j)->widget() == ui.lineEdit_search) {
                            lineEditIdx = i;
                            break;
                        }
                    }
                }
            }
            if (lineEditIdx >= 0)
                static_cast<QBoxLayout*>(viewParentLayout)->insertLayout(lineEditIdx, viewModeLayout);
        }
    }

    // ---- Connect signals ----
    connect(ui.pushButton_openXml, &QPushButton::clicked,
            this, &ParamConfigDialog::onSelectFile);
    connect(ui.pushButton_saveConfig, &QPushButton::clicked,
            this, &ParamConfigDialog::onSaveToFile);
    connect(ui.pushButton_saveAs, &QPushButton::clicked,
            this, &ParamConfigDialog::onSaveAsFile);
    connect(ui.pushButton_resetDefaults, &QPushButton::clicked,
            this, &ParamConfigDialog::reloadFromFile);
    connect(ui.treeWidget_checkItems, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                Q_UNUSED(current);
                onBlockSelectionChanged();
            });
    connect(ui.pushButton_close, &QPushButton::clicked,
            this, &QDialog::close);
    connect(ui.pushButton_expandAll, &QPushButton::clicked,
            this, [this]() { ui.treeWidget_checkItems->expandAll(); });
    connect(ui.pushButton_collapseAll, &QPushButton::clicked,
            this, [this]() {
                QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
                while (*it) {
                    if ((*it)->data(0, Qt::UserRole + 1).toString() == "mission")
                        (*it)->setExpanded(false);
                    ++it;
                }
            });
    connect(ui.tableWidget_params, &QTableWidget::cellChanged,
            this, &ParamConfigDialog::onTableCellChanged);
    connect(ui.lineEdit_search, &QLineEdit::textChanged,
            this, &ParamConfigDialog::onSearchTextChanged);
    connect(ui.lineEdit_search, &QLineEdit::returnPressed,
            this, [this]() { navigateSearchResult(+1); });
    ui.lineEdit_search->installEventFilter(this);
    connect(ui.checkBox_filterMarked, &QCheckBox::toggled,
            this, [this](bool checked) {
                m_filterModifiedOnly = checked;
                applyTableFilter();
            });
    connect(viewModeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &ParamConfigDialog::onViewModeChanged);
    connect(ui.tableWidget_params, &QTableWidget::cellClicked,
            this, &ParamConfigDialog::onTableCellClicked);

    // ---- 追加文件按钮 ----
    QPushButton* appendFileBtn = new QPushButton(QStringLiteral("追加文件"));
    appendFileBtn->setAutoDefault(false);
    appendFileBtn->setDefault(false);
    connect(appendFileBtn, &QPushButton::clicked,
            this, &ParamConfigDialog::onAppendFile);

    // 插入到打开文件按钮右侧
    QPushButton* openBtn = ui.pushButton_openXml;
    QLayout* openParent = openBtn->parentWidget()->layout();
    if (openParent) {
        int openIdx = openParent->indexOf(openBtn);
        if (openIdx >= 0)
            static_cast<QBoxLayout*>(openParent)->insertWidget(openIdx + 1, appendFileBtn);
    }

    // ---- 底部重置按钮 ----
    QPushButton* resetRowBtn = new QPushButton(QStringLiteral("重置当前行"));
    resetRowBtn->setMinimumWidth(80);
    resetRowBtn->setAutoDefault(false);
    resetRowBtn->setDefault(false);
    QPushButton* resetMissionBtn = new QPushButton(QStringLiteral("重置当前任务"));
    resetMissionBtn->setMinimumWidth(96);
    resetMissionBtn->setAutoDefault(false);
    resetMissionBtn->setDefault(false);

    QHBoxLayout* bottomLayout = nullptr;
    QLayout* ml = this->layout();
    if (ml) {
        for (int i = 0; i < ml->count(); ++i) {
            QHBoxLayout* hbl = qobject_cast<QHBoxLayout*>(ml->itemAt(i)->layout());
            if (hbl && hbl->indexOf(ui.pushButton_saveConfig) >= 0) {
                bottomLayout = hbl;
                break;
            }
        }
    }
    if (bottomLayout) {
        int saveIdx = bottomLayout->indexOf(ui.pushButton_saveConfig);
        if (saveIdx >= 0) {
            bottomLayout->insertWidget(saveIdx, resetRowBtn);
            bottomLayout->insertWidget(saveIdx + 1, resetMissionBtn);
        }
    }

    connect(resetRowBtn, &QPushButton::clicked,
            this, &ParamConfigDialog::onResetCurrentRow);
    connect(resetMissionBtn, &QPushButton::clicked,
            this, &ParamConfigDialog::onResetCurrentMission);

    QPushButton* syncBtn = new QPushButton(QStringLiteral("同步到其他文件"));
    syncBtn->setMinimumWidth(104);
    syncBtn->setAutoDefault(false);
    syncBtn->setDefault(false);
    connect(syncBtn, &QPushButton::clicked,
            this, &ParamConfigDialog::onSyncToOtherFiles);
    if (bottomLayout) {
        int saveIdx2 = bottomLayout->indexOf(ui.pushButton_saveConfig);
        if (saveIdx2 >= 0)
            bottomLayout->insertWidget(saveIdx2 + 2, syncBtn);
    }

    clearTable();
    updateStatus(QStringLiteral("请选择XML配置文件"));
    updateWindowTitle();
}

ParamConfigDialog::~ParamConfigDialog()
{
}

// ===================================================================
// 文件解析（单文件）
// ===================================================================

ParsedFileData ParamConfigDialog::parseOneFile(const QString& path)
{
    ParsedFileData result;
    result.filePath = path;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this,
            QStringLiteral("打开失败"),
            QStringLiteral("无法打开文件: %1").arg(file.errorString()));
        return result;
    }

    QString xml = QString::fromUtf8(file.readAll());
    file.close();

    if (!result.xmlDoc.setContent(xml)) {
        QMessageBox::warning(this,
            QStringLiteral("解析失败"),
            QStringLiteral("XML文件格式不正确，无法解析"));
        return result;
    }

    QDomElement root = result.xmlDoc.documentElement();

    QDomNodeList missionBlocks = root.elementsByTagName("MissionBlock");

    auto parseParamChildren = [](QDomElement paramElem) -> QVector<ParsedParameter> {
        QVector<ParsedParameter> r;
        QDomNodeList children = paramElem.childNodes();
        for (int c = 0; c < children.size(); ++c) {
            QDomNode node = children.at(c);
            if (!node.isElement()) continue;
            QDomElement childElem = node.toElement();
            ParsedParameter param;
            param.name = childElem.tagName();
            param.value = childElem.text().trimmed();
            param.originalValue = param.value;
            param.description = childElem.attribute("note");
            param.originalDescription = param.description;
            param.favorited = (childElem.attribute("favorite") == QStringLiteral("true"));
            param.originalFavorited = param.favorited;
            param.domElement = childElem;
            r.append(param);
        }
        return r;
    };

    auto extractFilePaths = [](QDomElement paraElem) -> QVector<ParsedParameter> {
        QVector<ParsedParameter> r;
        QDomNodeList fps = paraElem.elementsByTagName("FilePath");
        for (int f = 0; f < fps.size(); ++f) {
            QDomElement fpElem = fps.at(f).toElement();
            if (fpElem.isNull()) continue;
            ParsedParameter pp;
            pp.name = fpElem.tagName();
            pp.value = fpElem.text().trimmed();
            pp.originalValue = pp.value;
            pp.description = fpElem.attribute("note");
            if (pp.description.isEmpty() && !fpElem.parentNode().isNull()) {
                QDomElement parentElem = fpElem.parentNode().toElement();
                if (!parentElem.isNull() && parentElem.tagName() != "ParaIn"
                    && parentElem.tagName() != "ParaOut")
                    pp.description = parentElem.attribute("note");
            }
            pp.originalDescription = pp.description;
            pp.favorited = (fpElem.attribute("favorite") == QStringLiteral("true"));
            pp.originalFavorited = pp.favorited;
            pp.domElement = fpElem;
            r.append(pp);
        }
        return r;
    };

    if (missionBlocks.isEmpty()) {
        QDomNodeList missions = root.elementsByTagName("Mission");
        if (!missions.isEmpty()) {
            ParsedMissionBlock virtualBlock;
            virtualBlock.note = QStringLiteral("(根节点)");
            virtualBlock.domElement = root;
            for (int m = 0; m < missions.size(); ++m) {
                QDomElement missionElem = missions.at(m).toElement();
                ParsedMission mission;
                mission.id = missionElem.attribute("id");
                mission.note = missionElem.attribute("note");
                mission.domElement = missionElem;
                QDomNodeList paraIns = missionElem.elementsByTagName("ParaIn");
                for (int pi = 0; pi < paraIns.size(); ++pi) {
                    QDomNodeList parameters = paraIns.at(pi).toElement()
                        .elementsByTagName("Parameter");
                    for (int p = 0; p < parameters.size(); ++p) {
                        mission.params.append(
                            parseParamChildren(parameters.at(p).toElement()));
                    }
                    mission.inputPaths.append(extractFilePaths(paraIns.at(pi).toElement()));
                }
                QDomNodeList paraOuts = missionElem.elementsByTagName("ParaOut");
                for (int po = 0; po < paraOuts.size(); ++po) {
                    mission.outputPaths.append(extractFilePaths(paraOuts.at(po).toElement()));
                }
                virtualBlock.missions.append(mission);
            }
            result.blocks.append(virtualBlock);
            return result;
        }

        QDomNodeList params = root.elementsByTagName("Parameter");
        if (!params.isEmpty()) {
            ParsedMissionBlock virtualBlock;
            virtualBlock.note = QStringLiteral("(根节点)");
            virtualBlock.domElement = root;
            ParsedMission virtualMission;
            virtualMission.id = "0";
            virtualMission.note = QStringLiteral("(直接参数)");
            virtualMission.domElement = root;
            for (int p = 0; p < params.size(); ++p) {
                virtualMission.params.append(
                    parseParamChildren(params.at(p).toElement()));
            }
            virtualBlock.missions.append(virtualMission);
            result.blocks.append(virtualBlock);
            return result;
        }
    }

    for (int mb = 0; mb < missionBlocks.size(); ++mb) {
        QDomElement mbElem = missionBlocks.at(mb).toElement();
        ParsedMissionBlock block;
        block.note = mbElem.attribute("note");
        block.domElement = mbElem;

        QDomNodeList missions = mbElem.elementsByTagName("Mission");
        if (missions.isEmpty()) {
            result.blocks.append(block);
            continue;
        }

        for (int m = 0; m < missions.size(); ++m) {
            QDomElement missionElem = missions.at(m).toElement();
            ParsedMission mission;
            mission.id = missionElem.attribute("id");
            mission.note = missionElem.attribute("note");
            mission.domElement = missionElem;

            QDomNodeList paraIns = missionElem.elementsByTagName("ParaIn");
            for (int pi = 0; pi < paraIns.size(); ++pi) {
                QDomNodeList parameters = paraIns.at(pi).toElement()
                    .elementsByTagName("Parameter");
                for (int p = 0; p < parameters.size(); ++p) {
                    mission.params.append(
                        parseParamChildren(parameters.at(p).toElement()));
                }
                mission.inputPaths.append(extractFilePaths(paraIns.at(pi).toElement()));
            }
            QDomNodeList paraOuts = missionElem.elementsByTagName("ParaOut");
            for (int po = 0; po < paraOuts.size(); ++po) {
                mission.outputPaths.append(extractFilePaths(paraOuts.at(po).toElement()));
            }

            block.missions.append(mission);
        }

        result.blocks.append(block);
    }

    return result;
}

// ===================================================================
// 文件选择
// ===================================================================

void ParamConfigDialog::onSelectFile()
{
    QStringList paths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择XML配置文件（可多选）"),
        QString(),
        QStringLiteral("XML文件 (*.xml)"));
    if (paths.isEmpty()) return;

    // 打开新文件即抛弃旧文件的所有修改，直接重置状态后加载
    m_files.clear();
    m_currentFileIndex = -1;
    m_currentMbIndex = -1;
    m_currentMissionIndex = -1;
    ui.treeWidget_checkItems->clear();
    clearTable();
    ui.lineEdit_search->clear();

    int totalBlocks = 0;
    int totalMissions = 0;
    for (const QString& path : paths) {
        ParsedFileData fd = parseOneFile(path);
        if (fd.blocks.isEmpty() && QFileInfo(path).exists()) continue; // 解析失败跳过
        totalBlocks += fd.blocks.size();
        for (const auto& mb : fd.blocks)
            totalMissions += mb.missions.size();
        m_files.append(fd);
    }

    if (m_files.isEmpty()) {
        updateStatus(QStringLiteral("未能加载任何文件"));
        return;
    }

    m_currentFileIndex = 0;
    m_filterModifiedOnly = false;
    ui.checkBox_filterMarked->setChecked(false);
    refreshParamTree();
    updateStatus(QStringLiteral("已加载 %1 个文件，%2 个任务块，%3 个Mission")
                 .arg(m_files.size()).arg(totalBlocks).arg(totalMissions));
    updateWindowTitle();
}

void ParamConfigDialog::loadXmlFile(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        updateStatus(QStringLiteral("XML 文件不存在: %1").arg(path));
        return;
    }

    // 重置状态后加载
    m_files.clear();
    m_currentFileIndex = -1;
    m_currentMbIndex = -1;
    m_currentMissionIndex = -1;
    ui.treeWidget_checkItems->clear();
    clearTable();
    ui.lineEdit_search->clear();

    ParsedFileData fd = parseOneFile(path);
    if (fd.blocks.isEmpty()) {
        updateStatus(QStringLiteral("未能解析文件: %1").arg(path));
        return;
    }

    m_files.append(fd);
    m_currentFileIndex = 0;
    m_filterModifiedOnly = false;
    ui.checkBox_filterMarked->setChecked(false);
    refreshParamTree();
    updateStatus(QStringLiteral("已加载: %1").arg(QFileInfo(path).fileName()));
    updateWindowTitle();
}

void ParamConfigDialog::onAppendFile()
{
    QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("追加XML配置文件"),
        QString(),
        QStringLiteral("XML文件 (*.xml)"));
    if (path.isEmpty()) return;

    ParsedFileData fd = parseOneFile(path);
    if (fd.blocks.isEmpty()) return;

    m_files.append(fd);
    collectCurrentMissionValues();
    refreshParamTree();
    updateStatus(QStringLiteral("已追加: %1").arg(path));
}

// ===================================================================
// XML 解析
// ===================================================================

bool ParamConfigDialog::parseXmlFile(const QString& path)
{
    // Legacy wrapper: parse single file, populate legacy members for backward compat
    ParsedFileData fd = parseOneFile(path);
    if (fd.blocks.isEmpty()) return false;
    m_xmlFilePath = fd.filePath;
    m_xmlDoc = fd.xmlDoc;
    m_missionBlocks = fd.blocks;
    return true;
}

// ===================================================================
// 树 — 4 层，用左侧装饰色条区分层级（专业、不刺眼）
// ===================================================================

static QIcon makeColorBar(QColor color, int w, int h) {
    QPixmap pix(w, h);
    pix.fill(color);
    return QIcon(pix);
}

// 在驼峰式大小写交界处插入零宽空格，使 Qt wordWrap 能在单词边界折行
static QString camelCaseForWrap(const QString& s) {
    QString result;
    result.reserve(s.length() + 4);
    for (int i = 0; i < s.length(); ++i) {
        if (i > 0 && s[i].isUpper() && s[i - 1].isLower())
            result += QChar(0x200B);  // 零宽空格
        result += s[i];
    }
    return result;
}

// 找到第一个符号（非字母/数字/中文）作为"参数名称"和"参数说明"的分界
static int findNameSep(const QString& s) {
    for (int i = 0; i < s.length(); ++i) {
        if (!s[i].isLetterOrNumber())
            return i;
    }
    return -1;
}

void ParamConfigDialog::refreshParamTree()
{
    QTreeWidget* tree = ui.treeWidget_checkItems;
    m_currentFileIndex = -1;
    m_currentMbIndex = -1;
    m_currentMissionIndex = -1;
    tree->clear();
    clearTable();
    m_pendingHighlightRow = -1;

    // ---- 根节点：汇总信息 ----
    QTreeWidgetItem* rootItem = new QTreeWidgetItem();
    if (m_files.isEmpty()) {
        rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x82 (未选择文件)"));
    } else if (m_files.size() == 1) {
        int totalBlocks = 0, totalMissions = 0;
        for (const auto& mb : m_files[0].blocks) {
            totalBlocks++;
            totalMissions += mb.missions.size();
        }
        rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x84 ") + QFileInfo(m_files[0].filePath).fileName()
            + QStringLiteral("  [%1个任务块, %2个Mission]").arg(totalBlocks).arg(totalMissions));
    } else {
        int totalBlocks = 0, totalMissions = 0;
        for (const auto& fd : m_files) {
            totalBlocks += fd.blocks.size();
            for (const auto& mb : fd.blocks)
                totalMissions += mb.missions.size();
        }
        rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x82 %1 个文件, %2 个任务块, %3 个Mission")
            .arg(m_files.size()).arg(totalBlocks).arg(totalMissions));
    }
    rootItem->setData(0, Qt::UserRole, -1);
    rootItem->setData(0, Qt::UserRole + 1, QStringLiteral("root"));
    QFont rootFont = rootItem->font(0);
    rootFont.setBold(true);
    rootFont.setPointSize(rootFont.pointSize() + 1);
    rootFont.setFamily(QStringLiteral("Segoe UI Emoji"));
    rootItem->setFont(0, rootFont);
    tree->addTopLevelItem(rootItem);

    if (m_files.isEmpty()) {
        QTreeWidgetItem* emptyItem = new QTreeWidgetItem();
        emptyItem->setText(0, QStringLiteral("(无参数块)"));
        emptyItem->setData(0, Qt::UserRole, -1);
        emptyItem->setData(0, Qt::UserRole + 1, QStringLiteral("empty"));
        QFont emptyFont = emptyItem->font(0);
        emptyFont.setItalic(true);
        emptyItem->setFont(0, emptyFont);
        rootItem->addChild(emptyItem);
        tree->expandAll();
        return;
    }

    bool anyFavInTree = false;
    bool anyFileShown = false;
    bool singleFile = (m_files.size() == 1);

    for (int fileIdx = 0; fileIdx < m_files.size(); ++fileIdx) {
        const auto& fd = m_files[fileIdx];
        QString fileName = QFileInfo(fd.filePath).fileName();
        bool fileModified = fileHasModification(fileIdx);
        bool fileShown = false;

        for (int mbIdx = 0; mbIdx < fd.blocks.size(); ++mbIdx) {
            const auto& mb = fd.blocks[mbIdx];

            int mbTotalParams = 0, mbModified = 0;
            for (const auto& ms : mb.missions) {
                mbTotalParams += ms.params.size();
                for (const auto& p : ms.params)
                    if (p.value != p.originalValue || p.description != p.originalDescription) ++mbModified;
            }

            if (m_showingFavoritesOnly) {
                bool hasFav = false;
                for (const auto& ms : mb.missions) {
                    for (const auto& p : ms.params)
                        if (p.favorited) { hasFav = true; break; }
                    if (hasFav) break;
                    for (const auto& p : ms.inputPaths)
                        if (p.favorited) { hasFav = true; break; }
                    if (hasFav) break;
                    for (const auto& p : ms.outputPaths)
                        if (p.favorited) { hasFav = true; break; }
                    if (hasFav) break;
                }
                if (!hasFav) continue;
            }

            anyFileShown = true;

            // 多文件模式生成文件节点，单文件模式跳过
            if (!singleFile && !fileShown) {
                fileShown = true;
                QTreeWidgetItem* fileItem = new QTreeWidgetItem();
                fileItem->setText(0, QStringLiteral("\xF0\x9F\x93\x84 ") + fileName
                    + (fileModified ? QStringLiteral(" *") : QString()));
                fileItem->setData(0, Qt::UserRole, fileIdx);
                fileItem->setData(0, Qt::UserRole + 1, QStringLiteral("file"));
                QFont fileFont = fileItem->font(0);
                fileFont.setFamily(QStringLiteral("Segoe UI Emoji"));
                fileFont.setBold(true);
                fileItem->setFont(0, fileFont);
                rootItem->addChild(fileItem);
            }
            // 单文件时 block 直接挂在 root 下；多文件时挂文件节点下
            QTreeWidgetItem* blockParent = singleFile ? rootItem
                : rootItem->child(rootItem->childCount() - 1);

            // ---- 二级节点：MissionBlock ----
            QString mbNote = mb.note.isEmpty()
                ? QStringLiteral("(未命名任务块 #%1)").arg(mbIdx + 1)
                : mb.note;
            QString mbMeta = QStringLiteral("[%1个Mission, %2个参数]")
                .arg(mb.missions.size()).arg(mbTotalParams);

            QTreeWidgetItem* mbItem = new QTreeWidgetItem();
            mbItem->setText(0, QStringLiteral("\xF0\x9F\x93\xA6 ") + mbNote + QStringLiteral("  %1").arg(mbMeta));
            mbItem->setData(0, Qt::UserRole, fileIdx);
            mbItem->setData(1, Qt::UserRole, mbIdx);
            mbItem->setData(0, Qt::UserRole + 1, QStringLiteral("block"));
            QFont mbFont = mbItem->font(0);
            mbFont.setFamily(QStringLiteral("Segoe UI Emoji"));
            mbFont.setBold(true);
            mbItem->setFont(0, mbFont);
            blockParent->addChild(mbItem);

            if (mb.missions.isEmpty()) {
                QTreeWidgetItem* emptyItem = new QTreeWidgetItem();
                emptyItem->setText(0, QStringLiteral("(无Mission)"));
                emptyItem->setData(0, Qt::UserRole, fileIdx);
                emptyItem->setData(1, Qt::UserRole, mbIdx);
                emptyItem->setData(0, Qt::UserRole + 1, QStringLiteral("empty"));
                QFont emptyFont = emptyItem->font(0);
                emptyFont.setItalic(true);
                emptyItem->setFont(0, emptyFont);
                mbItem->addChild(emptyItem);
                continue;
            }

            for (int mIdx = 0; mIdx < mb.missions.size(); ++mIdx) {
                const auto& mission = mb.missions[mIdx];

                int missionModified = 0;
                for (const auto& p : mission.params)
                    if (p.value != p.originalValue || p.description != p.originalDescription) ++missionModified;

                if (m_showingFavoritesOnly) {
                    bool mHasFav = false;
                    for (const auto& p : mission.params)
                        if (p.favorited) { mHasFav = true; break; }
                    if (!mHasFav)
                        for (const auto& p : mission.inputPaths)
                            if (p.favorited) { mHasFav = true; break; }
                    if (!mHasFav)
                        for (const auto& p : mission.outputPaths)
                            if (p.favorited) { mHasFav = true; break; }
                    if (!mHasFav) continue;
                }

                anyFavInTree = true;

                // ---- 三级节点：Mission ----
                QString mNote = mission.note.isEmpty()
                    ? QStringLiteral("Mission #%1").arg(mission.id)
                    : mission.note;

                QTreeWidgetItem* mItem = new QTreeWidgetItem();
                mItem->setText(0, QStringLiteral("\xF0\x9F\x93\x81 ") + mNote
                    + QStringLiteral("  [ID=%1]").arg(mission.id));
                mItem->setData(0, Qt::UserRole, fileIdx);
                mItem->setData(1, Qt::UserRole, mbIdx);
                mItem->setData(2, Qt::UserRole, mIdx);
                mItem->setData(0, Qt::UserRole + 1, QStringLiteral("mission"));
                QFont mFont = mItem->font(0);
                mFont.setFamily(QStringLiteral("Segoe UI Emoji"));
                mItem->setFont(0, mFont);
                if (!mission.params.isEmpty() && missionModified > 0) {
                    QTreeWidgetItem* modHint = new QTreeWidgetItem();
                    modHint->setText(0, QStringLiteral("▸ 已修改 %1 个参数").arg(missionModified));
                    modHint->setData(0, Qt::UserRole, fileIdx);
                    modHint->setData(1, Qt::UserRole, mbIdx);
                    modHint->setData(2, Qt::UserRole, mIdx);
                    modHint->setData(0, Qt::UserRole + 1, QStringLiteral("mod-hint"));
                    modHint->setForeground(0, QColor("#c0392b"));
                    QFont mhFont = modHint->font(0);
                    mhFont.setBold(true);
                    modHint->setFont(0, mhFont);
                    mItem->addChild(modHint);
                }
                mbItem->addChild(mItem);

                // ---- 四级叶子节点：Parameter ----
                for (int pIdx = 0; pIdx < mission.params.size(); ++pIdx) {
                    const auto& param = mission.params[pIdx];
                    if (m_showingFavoritesOnly && !param.favorited) continue;

                    int spaceIdx = findNameSep(param.description);
                    QString displayName = param.description.isEmpty() ? param.name
                        : param.description.left(spaceIdx > 0 ? spaceIdx : param.description.length());
                    bool modified = (param.value != param.originalValue || param.description != param.originalDescription);

                    QTreeWidgetItem* pItem = new QTreeWidgetItem();
                    pItem->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + displayName + ": " + param.value);
                    pItem->setData(0, Qt::UserRole, fileIdx);
                    pItem->setData(1, Qt::UserRole, mbIdx);
                    pItem->setData(2, Qt::UserRole, mIdx);
                    pItem->setData(3, Qt::UserRole, pIdx);
                    QFont pf = pItem->font(0);
                    pf.setFamily(QStringLiteral("Segoe UI Emoji"));
                    pItem->setFont(0, pf);
                    pItem->setData(0, Qt::UserRole + 1, QStringLiteral("param"));
                    if (modified) {
                        QFont pf2 = pItem->font(0);
                        pf2.setBold(true);
                        pItem->setFont(0, pf2);
                        pItem->setForeground(0, QColor("#c0392b"));
                    }
                    mItem->addChild(pItem);
                }

                // ---- 路径节点：输入路径 ----
                if (!mission.inputPaths.isEmpty()) {
                    bool showInPaths = true;
                    if (m_showingFavoritesOnly) {
                        showInPaths = false;
                        for (const auto& p : mission.inputPaths)
                            if (p.favorited) { showInPaths = true; break; }
                    }
                    if (showInPaths) {
                    int inPathModified = 0;
                    for (const auto& p : mission.inputPaths)
                        if (p.value != p.originalValue || p.description != p.originalDescription) ++inPathModified;

                    QTreeWidgetItem* inGroup = new QTreeWidgetItem();
                    inGroup->setText(0, QStringLiteral("\xF0\x9F\x93\xA5 输入路径 [%1]")
                        .arg(mission.inputPaths.size()));
                    inGroup->setData(0, Qt::UserRole, fileIdx);
                    inGroup->setData(1, Qt::UserRole, mbIdx);
                    inGroup->setData(2, Qt::UserRole, mIdx);
                    inGroup->setData(0, Qt::UserRole + 1, QStringLiteral("inpath-group"));
                    QFont inFont = inGroup->font(0);
                    inFont.setFamily(QStringLiteral("Segoe UI Emoji"));
                    inFont.setBold(true);
                    inGroup->setFont(0, inFont);
                    mItem->addChild(inGroup);

                    for (int pIdx = 0; pIdx < mission.inputPaths.size(); ++pIdx) {
                        const auto& pp = mission.inputPaths[pIdx];
                        if (m_showingFavoritesOnly && !pp.favorited) continue;
                        bool mod = (pp.value != pp.originalValue || pp.description != pp.originalDescription);
                        QTreeWidgetItem* ipItem = new QTreeWidgetItem();
                        ipItem->setText(0, QStringLiteral("  \xF0\x9F\x93\x84 %1").arg(pp.value));
                        ipItem->setData(0, Qt::UserRole, fileIdx);
                        ipItem->setData(1, Qt::UserRole, mbIdx);
                        ipItem->setData(2, Qt::UserRole, mIdx);
                        ipItem->setData(3, Qt::UserRole, pIdx);
                        ipItem->setData(0, Qt::UserRole + 1, QStringLiteral("inpath"));
                        QFont ipf = ipItem->font(0);
                        ipf.setFamily(QStringLiteral("Segoe UI Emoji"));
                        ipItem->setFont(0, ipf);
                        if (mod) {
                            QFont pf2 = ipItem->font(0); pf2.setBold(true); ipItem->setFont(0, pf2);
                            ipItem->setForeground(0, QColor("#c0392b"));
                        }
                        inGroup->addChild(ipItem);
                    }
                    }
                }
                // ---- 路径节点：输出路径 ----
                if (!mission.outputPaths.isEmpty()) {
                    bool showOutPaths = true;
                    if (m_showingFavoritesOnly) {
                        showOutPaths = false;
                        for (const auto& p : mission.outputPaths)
                            if (p.favorited) { showOutPaths = true; break; }
                    }
                    if (showOutPaths) {
                    int outPathModified = 0;
                    for (const auto& p : mission.outputPaths)
                        if (p.value != p.originalValue || p.description != p.originalDescription) ++outPathModified;

                    QTreeWidgetItem* outGroup = new QTreeWidgetItem();
                    outGroup->setText(0, QStringLiteral("\xF0\x9F\x93\xA4 输出路径 [%1]")
                        .arg(mission.outputPaths.size()));
                    outGroup->setData(0, Qt::UserRole, fileIdx);
                    outGroup->setData(1, Qt::UserRole, mbIdx);
                    outGroup->setData(2, Qt::UserRole, mIdx);
                    outGroup->setData(0, Qt::UserRole + 1, QStringLiteral("outpath-group"));
                    QFont outFont = outGroup->font(0);
                    outFont.setFamily(QStringLiteral("Segoe UI Emoji"));
                    outFont.setBold(true);
                    outGroup->setFont(0, outFont);
                    mItem->addChild(outGroup);

                    for (int pIdx = 0; pIdx < mission.outputPaths.size(); ++pIdx) {
                        const auto& pp = mission.outputPaths[pIdx];
                        if (m_showingFavoritesOnly && !pp.favorited) continue;
                        bool mod = (pp.value != pp.originalValue || pp.description != pp.originalDescription);
                        QTreeWidgetItem* opItem = new QTreeWidgetItem();
                        opItem->setText(0, QStringLiteral("  \xF0\x9F\x93\x84 %1").arg(pp.value));
                        opItem->setData(0, Qt::UserRole, fileIdx);
                        opItem->setData(1, Qt::UserRole, mbIdx);
                        opItem->setData(2, Qt::UserRole, mIdx);
                        opItem->setData(3, Qt::UserRole, pIdx);
                        opItem->setData(0, Qt::UserRole + 1, QStringLiteral("outpath"));
                        QFont opf = opItem->font(0);
                        opf.setFamily(QStringLiteral("Segoe UI Emoji"));
                        opItem->setFont(0, opf);
                        if (mod) {
                            QFont pf2 = opItem->font(0); pf2.setBold(true); opItem->setFont(0, pf2);
                            opItem->setForeground(0, QColor("#c0392b"));
                        }
                        outGroup->addChild(opItem);
                    }
                    }
                }
            }
        }
    }

    if (m_showingFavoritesOnly && !anyFileShown) {
        QTreeWidgetItem* hintItem = new QTreeWidgetItem();
        hintItem->setText(0, QStringLiteral("暂无常用参数，请切换到\"全部参数\"视图后点击☆标记"));
        hintItem->setData(0, Qt::UserRole, -1);
        hintItem->setData(0, Qt::UserRole + 1, QStringLiteral("empty"));
        QFont hintFont = hintItem->font(0);
        hintFont.setItalic(true);
        hintItem->setFont(0, hintFont);
        rootItem->addChild(hintItem);
    }

    tree->expandAll();

    // 自动选中第一个有参数的 Mission
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 1).toString() == "mission") {
            int fIdx = (*it)->data(0, Qt::UserRole).toInt();
            int mbIdx = (*it)->data(1, Qt::UserRole).toInt();
            int mIdx = (*it)->data(2, Qt::UserRole).toInt();
            if (fIdx >= 0 && fIdx < m_files.size() &&
                mbIdx >= 0 && mbIdx < m_files[fIdx].blocks.size() &&
                mIdx >= 0 && mIdx < m_files[fIdx].blocks[mbIdx].missions.size() &&
                !m_files[fIdx].blocks[mbIdx].missions[mIdx].params.isEmpty()) {
                tree->setCurrentItem(*it);
                break;
            }
        }
        ++it;
    }
}

// ===================================================================
// 树节点选择
// ===================================================================

void ParamConfigDialog::onBlockSelectionChanged()
{
    if (m_selectingTreeItem) return;
    collectCurrentMissionValues();

    QTreeWidgetItem* item = ui.treeWidget_checkItems->currentItem();
    if (!item) {
        clearTable();
        m_currentFileIndex = -1;
        m_currentMbIndex = -1;
        m_currentMissionIndex = -1;
        return;
    }

    QString itemType = item->data(0, Qt::UserRole + 1).toString();

    if (itemType == "param") {
        QTreeWidgetItem* parent = item->parent();
        if (parent) {
            m_pendingHighlightRow = item->data(3, Qt::UserRole).toInt();
            bool oldBlocked = ui.treeWidget_checkItems->blockSignals(true);
            ui.treeWidget_checkItems->setCurrentItem(parent);
            ui.treeWidget_checkItems->blockSignals(oldBlocked);
            onBlockSelectionChanged();
            return;
        }
    }

    if (itemType == "inpath" || itemType == "outpath") {
        QTreeWidgetItem* parent = item->parent();
        if (parent) {
            m_pendingHighlightRow = item->data(3, Qt::UserRole).toInt();
            bool oldBlocked = ui.treeWidget_checkItems->blockSignals(true);
            ui.treeWidget_checkItems->setCurrentItem(parent);
            ui.treeWidget_checkItems->blockSignals(oldBlocked);
            onBlockSelectionChanged();
            return;
        }
    }

    if (itemType == "inpath-group" || itemType == "outpath-group") {
        m_currentFileIndex = item->data(0, Qt::UserRole).toInt();
        m_currentMbIndex = item->data(1, Qt::UserRole).toInt();
        m_currentMissionIndex = item->data(2, Qt::UserRole).toInt();
        if (m_currentFileIndex >= 0 && m_currentFileIndex < m_files.size() &&
            m_currentMbIndex >= 0 && m_currentMbIndex < m_files[m_currentFileIndex].blocks.size() &&
            m_currentMissionIndex >= 0 &&
            m_currentMissionIndex < m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) {
            m_showingPaths = true;
            m_showingInputPaths = (itemType == "inpath-group");
            const auto& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];
            const auto& paths = m_showingInputPaths
                ? mission.inputPaths : mission.outputPaths;
            QString typeLabel = m_showingInputPaths
                ? QStringLiteral("输入") : QStringLiteral("输出");
            populatePathTable(paths, typeLabel);
            return;
        }
    }

    if (itemType == "mission") {
        m_showingPaths = false;
        m_currentFileIndex = item->data(0, Qt::UserRole).toInt();
        m_currentMbIndex = item->data(1, Qt::UserRole).toInt();
        m_currentMissionIndex = item->data(2, Qt::UserRole).toInt();
        if (m_currentFileIndex >= 0 && m_currentFileIndex < m_files.size() &&
            m_currentMbIndex >= 0 && m_currentMbIndex < m_files[m_currentFileIndex].blocks.size() &&
            m_currentMissionIndex >= 0 &&
            m_currentMissionIndex < m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) {
            populateTable(m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex]);
            int modCount = 0;
            for (const auto& p : m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex].params)
                if (p.value != p.originalValue || p.description != p.originalDescription) ++modCount;
            updateStatus(QStringLiteral("编辑: %1%2")
                         .arg(item->text(0))
                         .arg(modCount > 0 ? QStringLiteral("  —  已修改 %1 个参数").arg(modCount) : QString()));
            return;
        }
    }

    if (itemType == "mod-hint") {
        QTreeWidgetItem* parent = item->parent();
        if (parent) {
            ui.treeWidget_checkItems->setCurrentItem(parent);
            return;
        }
    }

    if (itemType == "file") {
        int fIdx = item->data(0, Qt::UserRole).toInt();
        if (fIdx >= 0 && fIdx < m_files.size()) {
            clearTable();
            m_currentFileIndex = fIdx;
            m_currentMbIndex = -1;
            m_currentMissionIndex = -1;
            int totalBlocks = m_files[fIdx].blocks.size();
            int totalMissions = 0;
            for (const auto& mb : m_files[fIdx].blocks)
                totalMissions += mb.missions.size();
            updateStatus(QStringLiteral("文件: %1 — %2 个任务块，%3 个Mission")
                         .arg(QFileInfo(m_files[fIdx].filePath).fileName())
                         .arg(totalBlocks).arg(totalMissions));
            return;
        }
    }

    clearTable();
    m_currentFileIndex = -1;
    m_currentMbIndex = -1;
    m_currentMissionIndex = -1;

    if (itemType == "block") {
        int fIdx = item->data(0, Qt::UserRole).toInt();
        int mbIdx = item->data(1, Qt::UserRole).toInt();
        if (fIdx >= 0 && fIdx < m_files.size() &&
            mbIdx >= 0 && mbIdx < m_files[fIdx].blocks.size()) {
            const auto& mb = m_files[fIdx].blocks[mbIdx];
            m_currentFileIndex = fIdx;
            updateStatus(QStringLiteral("任务块: %1 — 包含 %2 个Mission，请展开选择一个Mission查看参数")
                         .arg(mb.note).arg(mb.missions.size()));
        }
    } else if (itemType == "root") {
        int total = 0, totalBlocks = 0;
        for (const auto& fd : m_files) {
            totalBlocks += fd.blocks.size();
            for (const auto& mb : fd.blocks) total += mb.missions.size();
        }
        updateStatus(QStringLiteral("共 %1 个文件，%2 个任务块，%3 个Mission")
                     .arg(m_files.size()).arg(totalBlocks).arg(total));
    }
}

// ===================================================================
// 表格
// ===================================================================

void ParamConfigDialog::clearTable()
{
    m_updatingTable = true;
    QTableWidget* table = ui.tableWidget_params;
    table->clearContents();
    table->setRowCount(1);
    table->setSpan(0, 0, 1, 7);
    QTableWidgetItem* hint = new QTableWidgetItem(
        QStringLiteral("← 请点击左侧 Mission 节点查看参数"));
    hint->setTextAlignment(Qt::AlignCenter);
    hint->setFlags(hint->flags() & ~Qt::ItemIsEditable);
    hint->setForeground(QColor("#aaa"));
    QFont hf = hint->font();
    hf.setItalic(true);
    hf.setPointSize(hf.pointSize() + 2);
    hint->setFont(hf);
    table->setItem(0, 0, hint);
    table->horizontalHeader()->setVisible(false);
    table->verticalHeader()->setVisible(false);
    m_showingPaths = false;
    m_updatingTable = false;
}

void ParamConfigDialog::populateTable(const ParsedMission& mission, int searchHighlightRow)
{
    m_updatingTable = true;
    QTableWidget* table = ui.tableWidget_params;
    table->horizontalHeader()->setVisible(true);
    table->verticalHeader()->setVisible(false);

    // 强制清空旧内容（销毁所有旧 cell widget，杜绝 combo 信号残留）
    table->clearContents();
    table->setRowCount(0);

    if (mission.params.isEmpty()) {
        m_updatingTable = false;
        updateStatus(QStringLiteral("此 Mission 没有可编辑的参数块"));
        return;
    }

    table->setRowCount(mission.params.size());

    for (int i = 0; i < mission.params.size(); ++i) {
        const ParsedParameter& param = mission.params[i];

        int spaceIdx = findNameSep(param.description);
        QString shortName = param.description.isEmpty() ? param.name
            : param.description.left(spaceIdx > 0 ? spaceIdx : param.description.length());
        QString detail = (spaceIdx > 0) ? param.description.mid(spaceIdx + 1) : QString();
        bool modified = (param.value != param.originalValue || param.description != param.originalDescription);

        // 序号
        QTableWidgetItem* seqItem = new QTableWidgetItem(QString::number(i + 1));
        seqItem->setFlags(seqItem->flags() & ~Qt::ItemIsEditable);
        seqItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(i, 0, seqItem);

        // ID（插入零宽空格使驼峰式命名可自动换行，保持大小写完整）
        QTableWidgetItem* idItem = new QTableWidgetItem(camelCaseForWrap(param.name));
        table->setItem(i, 1, idItem);

        // 参数名称
        QTableWidgetItem* nameItem = new QTableWidgetItem(shortName);
        table->setItem(i, 2, nameItem);

        // 参数值 — 布尔参数（IsXxx）用下拉框
        bool isBool = param.name.startsWith(QStringLiteral("Is"), Qt::CaseInsensitive);
        if (isBool) {
            QComboBox* combo = new QComboBox();
            combo->addItems({QStringLiteral("true"), QStringLiteral("false")});
            combo->setCurrentIndex(
                param.value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ? 0 : 1);
            combo->setFont(table->font());
            QColor comboBg = (i == searchHighlightRow) ? QColor("#29B6F6")
                : modified ? QColor("#fff3cd")
                : (i % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5");
            combo->setStyleSheet(
                QString("QComboBox { background-color: %1; font-family: inherit; }"
                        "QComboBox QAbstractItemView { "
                        "  selection-background-color: #29B6F6;"
                        "  selection-color: #000000;"
                        "  outline: none;"
                        "}"
                        "QComboBox QAbstractItemView::item {"
                        "  color: #000000;"
                        "  padding: 2px 4px;"
                        "}"
                        "QComboBox QAbstractItemView::item:selected {"
                        "  color: #000000;"
                        "  background-color: #29B6F6;"
                        "}")
                .arg(comboBg.name()));
            int fIdx = m_currentFileIndex;
            int mbIdx = m_currentMbIndex;
            int mIdx = m_currentMissionIndex;
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, i, fIdx, mbIdx, mIdx, combo](int idx) {
                if (m_updatingTable) return;
                if (fIdx < 0 || mbIdx < 0 || mIdx < 0) return;
                if (fIdx >= m_files.size()) return;
                if (mbIdx >= m_files[fIdx].blocks.size()) return;
                if (mIdx >= m_files[fIdx].blocks[mbIdx].missions.size()) return;
                ParsedMission& ms = m_files[fIdx].blocks[mbIdx].missions[mIdx];
                if (i >= ms.params.size()) return;
                ParsedParameter& p = ms.params[i];
                p.value = (idx == 0) ? QStringLiteral("true") : QStringLiteral("false");
                bool mod = (p.value != p.originalValue || p.description != p.originalDescription);
                QTableWidget* t = ui.tableWidget_params;
                m_updatingTable = true;
                QTableWidgetItem* si = t->item(i, 5);
                if (si) {
                    if (mod) {
                        si->setText(QStringLiteral("已修改"));
                        si->setForeground(QColor("#c0392b"));
                        QFont sf = si->font(); sf.setBold(true); si->setFont(sf);
                    } else {
                        si->setText(QStringLiteral("-"));
                        si->setForeground(QColor("#aaa"));
                        QFont sf = si->font(); sf.setBold(false); si->setFont(sf);
                    }
                }
                QColor bg = mod ? QColor("#fff3cd") : ((i % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5"));
                for (int c = 0; c < 7; ++c) {
                    QTableWidgetItem* cell = t->item(i, c);
                    if (cell) cell->setBackground(bg);
                }
                combo->setStyleSheet(
                    QString("QComboBox { background-color: %1; }"
                            "QComboBox QAbstractItemView { selection-background-color: #29B6F6; selection-color: #000000; outline: none; }"
                            "QComboBox QAbstractItemView::item { color: #000000; padding: 2px 4px; }"
                            "QComboBox QAbstractItemView::item:selected { color: #000000; background-color: #29B6F6; }")
                    .arg(bg.name()));
                m_updatingTable = false;
                applyTableFilter();
                updateWindowTitle();
                QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
                while (*it) {
                    if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                        (*it)->data(0, Qt::UserRole).toInt() == fIdx &&
                        (*it)->data(1, Qt::UserRole).toInt() == mbIdx &&
                        (*it)->data(2, Qt::UserRole).toInt() == mIdx &&
                        (*it)->data(3, Qt::UserRole).toInt() == i) {
                        int sp = findNameSep(p.description);
                        QString dn = p.description.isEmpty() ? p.name
                            : p.description.left(sp > 0 ? sp : p.description.length());
                        (*it)->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + dn + ": " + p.value);
                        if (mod) {
                            QFont pf = (*it)->font(0); pf.setBold(true); (*it)->setFont(0, pf);
                            (*it)->setForeground(0, QColor("#c0392b"));
                        } else {
                            QFont pf = (*it)->font(0); pf.setBold(false); (*it)->setFont(0, pf);
                            (*it)->setForeground(0, QColor(Qt::black));
                        }
                        break;
                    }
                    ++it;
                }
            });
            table->setCellWidget(i, 3, combo);
        } else {
            QTableWidgetItem* valItem = new QTableWidgetItem(param.value);
            table->setItem(i, 3, valItem);
        }

        // 参数说明
        QTableWidgetItem* descItem = new QTableWidgetItem(detail);
        descItem->setToolTip(detail);
        table->setItem(i, 4, descItem);

        // 状态（只读）：已修改 / 未修改
        QTableWidgetItem* statusItem = new QTableWidgetItem();
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (modified) {
            statusItem->setText(QStringLiteral("已修改"));
            statusItem->setForeground(QColor("#c0392b"));
            QFont sf = statusItem->font();
            sf.setBold(true);
            statusItem->setFont(sf);
        } else {
            statusItem->setText(QStringLiteral("-"));
            statusItem->setForeground(QColor("#aaa"));
        }
        table->setItem(i, 5, statusItem);

        // 收藏列（★/☆）
        QTableWidgetItem* favItem = new QTableWidgetItem(
            param.favorited ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
        favItem->setFlags(favItem->flags() & ~Qt::ItemIsEditable);
        favItem->setTextAlignment(Qt::AlignCenter);
        favItem->setData(Qt::UserRole, param.favorited ? 1 : 0);
        favItem->setToolTip(param.favorited
            ? QStringLiteral("点击取消收藏") : QStringLiteral("点击添加收藏"));
        table->setItem(i, 6, favItem);

        // 整行统一背景色（必须在所有 7 列 setItem 之后）
        QColor rowBg;
        if (i == searchHighlightRow)
            rowBg = QColor("#29B6F6");          // 搜索命中 → 蓝色
        else if (modified)
            rowBg = QColor("#fff3cd");          // 已修改 → 黄色
        else
            rowBg = (i % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5");
        for (int col = 0; col < 7; ++col) {
            QTableWidgetItem* cell = table->item(i, col);
            if (cell) cell->setBackground(rowBg);
        }
        table->resizeRowToContents(i);
    }

    m_updatingTable = false;

    table->clearSpans();
    table->scrollToTop();
    applyTableFilter();
    QTimer::singleShot(0, this, [this]() { applyTableFilter(); });

    // 滚动到目标行：优先搜索命中行，其次等待高亮行（点击参数叶子）
    int scrollRow = (searchHighlightRow >= 0) ? searchHighlightRow : m_pendingHighlightRow;
    if (scrollRow >= 0 && scrollRow < table->rowCount()) {
        table->scrollToItem(table->item(scrollRow, 0), QAbstractItemView::EnsureVisible);
    }
    m_pendingHighlightRow = -1;
}

void ParamConfigDialog::populatePathTable(const QVector<ParsedParameter>& paths, const QString& typeLabel)
{
    m_updatingTable = true;
    QTableWidget* table = ui.tableWidget_params;
    table->horizontalHeader()->setVisible(true);
    table->verticalHeader()->setVisible(false);
    table->clearContents();
    table->setRowCount(0);

    if (paths.isEmpty()) {
        m_updatingTable = false;
        updateStatus(QStringLiteral("此 Mission 没有%1路径").arg(typeLabel));
        return;
    }

    table->setRowCount(paths.size());

    for (int i = 0; i < paths.size(); ++i) {
        const ParsedParameter& pp = paths[i];
        bool modified = (pp.value != pp.originalValue || pp.description != pp.originalDescription);

        QTableWidgetItem* seqItem = new QTableWidgetItem(QString::number(i + 1));
        seqItem->setFlags(seqItem->flags() & ~Qt::ItemIsEditable);
        seqItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(i, 0, seqItem);

        QTableWidgetItem* keyItem = new QTableWidgetItem(camelCaseForWrap(pp.name));
        table->setItem(i, 1, keyItem);

        QTableWidgetItem* typeItem = new QTableWidgetItem(typeLabel);
        typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(i, 2, typeItem);

        QTableWidgetItem* valItem = new QTableWidgetItem(pp.value);
        table->setItem(i, 3, valItem);

        QTableWidgetItem* descItem = new QTableWidgetItem(pp.description);
        descItem->setToolTip(pp.description);
        table->setItem(i, 4, descItem);

        QTableWidgetItem* statusItem = new QTableWidgetItem();
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (modified) {
            statusItem->setText(QStringLiteral("已修改"));
            statusItem->setForeground(QColor("#c0392b"));
            QFont sf = statusItem->font(); sf.setBold(true); statusItem->setFont(sf);
        } else {
            statusItem->setText(QStringLiteral("-"));
            statusItem->setForeground(QColor("#aaa"));
        }
        table->setItem(i, 5, statusItem);

        QTableWidgetItem* favItem = new QTableWidgetItem(
            pp.favorited ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
        favItem->setFlags(favItem->flags() & ~Qt::ItemIsEditable);
        favItem->setTextAlignment(Qt::AlignCenter);
        favItem->setData(Qt::UserRole, pp.favorited ? 1 : 0);
        favItem->setToolTip(pp.favorited
            ? QStringLiteral("点击取消收藏") : QStringLiteral("点击添加收藏"));
        table->setItem(i, 6, favItem);

        QColor rowBg;
        if (i == m_pendingHighlightRow)
            rowBg = QColor("#29B6F6");
        else if (modified)
            rowBg = QColor("#fff3cd");
        else
            rowBg = (i % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5");
        for (int col = 0; col < 7; ++col) {
            QTableWidgetItem* cell = table->item(i, col);
            if (cell) cell->setBackground(rowBg);
        }
        table->resizeRowToContents(i);
    }

    m_updatingTable = false;
    table->clearSpans();
    table->scrollToTop();
    applyTableFilter();
    QTimer::singleShot(0, this, [this]() { applyTableFilter(); });

    int modCount = 0;
    for (const auto& p : paths)
        if (p.value != p.originalValue || p.description != p.originalDescription) ++modCount;
    updateStatus(QStringLiteral("%1路径 — 共 %2 条%3")
        .arg(typeLabel).arg(paths.size())
        .arg(modCount > 0 ? QStringLiteral("，已修改 %1 条").arg(modCount) : QString()));
}

void ParamConfigDialog::applyTableFilter()
{
    QTableWidget* table = ui.tableWidget_params;
    bool searchActive = !ui.lineEdit_search->text().trimmed().isEmpty();

    qDebug() << "[applyTableFilter] m_showingFavoritesOnly=" << m_showingFavoritesOnly
             << "searchActive=" << searchActive
             << "m_filterModifiedOnly=" << m_filterModifiedOnly
             << "rowCount=" << table->rowCount();

    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* statusItem = table->item(row, 5);
        bool modified = statusItem && statusItem->text() == QStringLiteral("已修改");
        QTableWidgetItem* favItem = table->item(row, 6);
        bool favorited = favItem && (favItem->data(Qt::UserRole).toInt() != 0);

        bool hidden = false;
        if (!searchActive && m_showingFavoritesOnly && !favorited)
            hidden = true;
        if (m_filterModifiedOnly && !modified)
            hidden = true;
        table->setRowHidden(row, hidden);

        qDebug() << "  row" << row
                 << "favItemPtr=" << (favItem != nullptr)
                 << "favorited=" << favorited
                 << "modified=" << modified
                 << "hidden=" << hidden
                 << "isRowHidden=" << table->isRowHidden(row);
    }
}


void ParamConfigDialog::onTableCellClicked(int row, int col)
{
    if (col != 6) return;
    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];
    QTableWidget* table = ui.tableWidget_params;

    if (m_showingPaths) {
        QVector<ParsedParameter>& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;
        if (row < 0 || row >= pathVec.size()) return;
        ParsedParameter& pp = pathVec[row];
        pp.favorited = !pp.favorited;
        pp.domElement.setAttribute(QStringLiteral("favorite"),
            pp.favorited ? QStringLiteral("true") : QStringLiteral("false"));

        m_updatingTable = true;
        QTableWidgetItem* fi = table->item(row, 6);
        if (fi) {
            fi->setText(pp.favorited
                ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
            fi->setData(Qt::UserRole, pp.favorited ? 1 : 0);
            fi->setToolTip(pp.favorited
                ? QStringLiteral("点击取消收藏") : QStringLiteral("点击添加收藏"));
        }
        m_updatingTable = false;
    } else {
        if (row < 0 || row >= mission.params.size()) return;
        ParsedParameter& param = mission.params[row];
        param.favorited = !param.favorited;
        param.domElement.setAttribute(QStringLiteral("favorite"),
            param.favorited ? QStringLiteral("true") : QStringLiteral("false"));

        m_updatingTable = true;
        QTableWidgetItem* fi = table->item(row, 6);
        if (fi) {
            fi->setText(param.favorited
                ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
            fi->setData(Qt::UserRole, param.favorited ? 1 : 0);
            fi->setToolTip(param.favorited
                ? QStringLiteral("点击取消收藏") : QStringLiteral("点击添加收藏"));
        }
        m_updatingTable = false;
    }

    updateWindowTitle();

    if (m_showingFavoritesOnly) {
        bool anyFav = false;
        for (const auto& p : mission.params)
            if (p.favorited) { anyFav = true; break; }
        if (!anyFav) {
            for (const auto& p : mission.inputPaths)
                if (p.favorited) { anyFav = true; break; }
        }
        if (!anyFav) {
            for (const auto& p : mission.outputPaths)
                if (p.favorited) { anyFav = true; break; }
        }
        if (!anyFav) {
            refreshParamTree();
            return;
        }
    }

    applyTableFilter();
}

void ParamConfigDialog::onViewModeChanged(int index)
{
    m_showingFavoritesOnly = (index == 0);
    collectCurrentMissionValues();
    refreshParamTree();
}

void ParamConfigDialog::onTableCellChanged(int row, int col)
{
    if (m_updatingTable) return;
    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];
    QTableWidget* table = ui.tableWidget_params;

    auto updateRowStatus = [&](int r, bool mod) {
        m_updatingTable = true;
        QTableWidgetItem* si = table->item(r, 5);
        if (si) {
            if (mod) {
                si->setText(QStringLiteral("已修改"));
                si->setForeground(QColor("#c0392b"));
                QFont sf = si->font(); sf.setBold(true); si->setFont(sf);
                for (int c = 0; c < 7; ++c) {
                    QTableWidgetItem* cell = table->item(r, c);
                    if (cell) cell->setBackground(QColor("#fff3cd"));
                }
            } else {
                si->setText(QStringLiteral("-"));
                si->setForeground(QColor("#aaa"));
                QFont sf = si->font(); sf.setBold(false); si->setFont(sf);
                QColor altBg = (r % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5");
                for (int c = 0; c < 7; ++c) {
                    QTableWidgetItem* cell = table->item(r, c);
                    if (cell) cell->setBackground(altBg);
                }
            }
        }
        m_updatingTable = false;
        applyTableFilter();
        updateWindowTitle();
    };

    if (m_showingPaths) {
        QVector<ParsedParameter>& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;
        if (row < 0 || row >= pathVec.size()) return;
        ParsedParameter& pp = pathVec[row];

        if (col == 1 && table->item(row, col))
            pp.name = table->item(row, col)->text().remove(QChar(0x200B)).trimmed();

        if (col == 3 && table->item(row, col)) {
            pp.value = table->item(row, col)->text().trimmed();
            bool modified = (pp.value != pp.originalValue) || (pp.description != pp.originalDescription);
            updateRowStatus(row, modified);
            QString treeType = m_showingInputPaths ? "inpath" : "outpath";
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == treeType &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    (*it)->setText(0, QStringLiteral("  \xF0\x9F\x93\x84 %1").arg(pp.value));
                    if (modified) {
                        QFont pf = (*it)->font(0); pf.setBold(true); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor("#c0392b"));
                    } else {
                        QFont pf = (*it)->font(0); pf.setBold(false); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor(Qt::black));
                    }
                    break;
                }
                ++it;
            }
        }
        if (col == 4 && table->item(row, col)) {
            pp.description = table->item(row, col)->text().trimmed();
            bool modified = (pp.value != pp.originalValue) || (pp.description != pp.originalDescription);
            updateRowStatus(row, modified);
            // 同步树节点格式（路径叶子文本不变，仅更新红字/加粗状态）
            QString treeType = m_showingInputPaths ? "inpath" : "outpath";
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == treeType &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    if (modified) {
                        QFont pf = (*it)->font(0); pf.setBold(true); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor("#c0392b"));
                    } else {
                        QFont pf = (*it)->font(0); pf.setBold(false); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor(Qt::black));
                    }
                    break;
                }
                ++it;
            }
        }
        return;
    }

    if (row < 0 || row >= mission.params.size()) return;
    ParsedParameter& param = mission.params[row];

    switch (col) {
    case 1: // ID
        if (table->item(row, col))
            param.name = table->item(row, col)->text().remove(QChar(0x200B)).trimmed();
        break;
    case 2: // 参数名称
        if (table->item(row, col)) {
            QString newShort = table->item(row, col)->text().trimmed();
            int spaceIdx = findNameSep(param.description);
            if (spaceIdx > 0)
                param.description = newShort + param.description.mid(spaceIdx);
            else
                param.description = newShort;
            bool modified = (param.value != param.originalValue) || (param.description != param.originalDescription);
            updateRowStatus(row, modified);
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    int sp = findNameSep(param.description);
                    QString dn = param.description.isEmpty() ? param.name
                        : param.description.left(sp > 0 ? sp : param.description.length());
                    (*it)->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + dn + ": " + param.value);
                    if (modified) {
                        QFont pf = (*it)->font(0); pf.setBold(true); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor("#c0392b"));
                    } else {
                        QFont pf = (*it)->font(0); pf.setBold(false); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor(Qt::black));
                    }
                    break;
                }
                ++it;
            }
        }
        break;
    case 3: { // 参数值
        if (table->item(row, col))
            param.value = table->item(row, col)->text().trimmed();
        bool modified = (param.value != param.originalValue) || (param.description != param.originalDescription);
        updateRowStatus(row, modified);
        QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                (*it)->data(3, Qt::UserRole).toInt() == row) {
                int sp = findNameSep(param.description);
                QString dn = param.description.isEmpty() ? param.name
                    : param.description.left(sp > 0 ? sp : param.description.length());
                (*it)->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + dn + ": " + param.value);
                if (modified) {
                    QFont pf = (*it)->font(0); pf.setBold(true); (*it)->setFont(0, pf);
                    (*it)->setForeground(0, QColor("#c0392b"));
                } else {
                    QFont pf = (*it)->font(0); pf.setBold(false); (*it)->setFont(0, pf);
                    (*it)->setForeground(0, QColor(Qt::black));
                }
                break;
            }
            ++it;
        }
        break;
    }
    case 4: // 参数说明
        if (table->item(row, col)) {
            QString newDetail = table->item(row, col)->text().trimmed();
            int spaceIdx = findNameSep(param.description);
            QString shortPart = spaceIdx > 0
                ? param.description.left(spaceIdx)
                : param.description;
            param.description = newDetail.isEmpty() ? shortPart
                : shortPart + " " + newDetail;
            bool modified = (param.value != param.originalValue) || (param.description != param.originalDescription);
            updateRowStatus(row, modified);
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    int sp = findNameSep(param.description);
                    QString dn = param.description.isEmpty() ? param.name
                        : param.description.left(sp > 0 ? sp : param.description.length());
                    (*it)->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + dn + ": " + param.value);
                    if (modified) {
                        QFont pf = (*it)->font(0); pf.setBold(true); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor("#c0392b"));
                    } else {
                        QFont pf = (*it)->font(0); pf.setBold(false); (*it)->setFont(0, pf);
                        (*it)->setForeground(0, QColor(Qt::black));
                    }
                    break;
                }
                ++it;
            }
        }
        break;
    }
}

// ===================================================================
// 搜索：直接查数据并填充表格，不依赖树节点信号链
// ===================================================================

void ParamConfigDialog::onSearchTextChanged(const QString& text)
{
    QString kw = text.trimmed();
    m_searchResults.clear();
    m_currentSearchIndex = -1;
    if (m_searchInfoBar) m_searchInfoBar->setVisible(false);

    if (kw.isEmpty()) {
        for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
            for (int mbIdx = 0; mbIdx < m_files[fIdx].blocks.size(); ++mbIdx) {
                for (int mIdx = 0; mIdx < m_files[fIdx].blocks[mbIdx].missions.size(); ++mIdx) {
                    const auto& ms = m_files[fIdx].blocks[mbIdx].missions[mIdx];
                    if (ms.params.isEmpty()) continue;
                    if (m_showingFavoritesOnly) {
                        bool hasFav = false;
                        for (const auto& p : ms.params)
                            if (p.favorited) { hasFav = true; break; }
                        if (!hasFav) continue;
                    }
                    m_currentFileIndex = fIdx;
                    m_currentMbIndex = mbIdx;
                    m_currentMissionIndex = mIdx;
                    populateTable(ms);
                    updateStatus(QStringLiteral("就绪"));
                    return;
                }
            }
        }
        clearTable();
        updateStatus(QStringLiteral("就绪"));
        return;
    }

    // 收集全部结果
    // 1) 搜任务 ID
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        for (int mbIdx = 0; mbIdx < m_files[fIdx].blocks.size(); ++mbIdx) {
            for (int mIdx = 0; mIdx < m_files[fIdx].blocks[mbIdx].missions.size(); ++mIdx) {
                const auto& mission = m_files[fIdx].blocks[mbIdx].missions[mIdx];
                if (!mission.id.isEmpty() && mission.id.contains(kw, Qt::CaseInsensitive)) {
                    if (m_showingFavoritesOnly) {
                        bool hasFav = false;
                        for (const auto& p : mission.params)
                            if (p.favorited) { hasFav = true; break; }
                        if (!hasFav) continue;
                    }
                    m_searchResults.append({fIdx, mbIdx, mIdx, -1, 1});
                }
            }
        }
    }

    // 2) 搜参数数据
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        for (int mbIdx = 0; mbIdx < m_files[fIdx].blocks.size(); ++mbIdx) {
            for (int mIdx = 0; mIdx < m_files[fIdx].blocks[mbIdx].missions.size(); ++mIdx) {
                const auto& mission = m_files[fIdx].blocks[mbIdx].missions[mIdx];
                for (int pIdx = 0; pIdx < mission.params.size(); ++pIdx) {
                    const auto& param = mission.params[pIdx];
                    int spIdx = findNameSep(param.description);
                    QString paramName = param.description.isEmpty() ? QString()
                        : (spIdx > 0 ? param.description.left(spIdx) : param.description);
                    if (m_showingFavoritesOnly && !param.favorited)
                        continue;
                    if (param.name.contains(kw, Qt::CaseInsensitive) ||
                        paramName.contains(kw, Qt::CaseInsensitive)) {
                        m_searchResults.append({fIdx, mbIdx, mIdx, pIdx, 2});
                    }
                }
            }
        }
    }

    // 3) 搜树节点文本
    QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
    while (*it) {
        QString type = (*it)->data(0, Qt::UserRole + 1).toString();
        if ((type == "block" || type == "mission" || type == "param") &&
            (*it)->text(0).contains(kw, Qt::CaseInsensitive)) {
            int fIdx = (*it)->data(0, Qt::UserRole).toInt();
            if (type == "mission") {
                int mbIdx = (*it)->data(1, Qt::UserRole).toInt();
                int mIdx = (*it)->data(2, Qt::UserRole).toInt();
                if (fIdx >= 0 && fIdx < m_files.size() &&
                    mbIdx >= 0 && mbIdx < m_files[fIdx].blocks.size() &&
                    mIdx >= 0 && mIdx < m_files[fIdx].blocks[mbIdx].missions.size()) {
                    if (m_showingFavoritesOnly) {
                        bool hasFav = false;
                        for (const auto& p : m_files[fIdx].blocks[mbIdx].missions[mIdx].params)
                            if (p.favorited) { hasFav = true; break; }
                        if (!hasFav) { ++it; continue; }
                    }
                    m_searchResults.append({fIdx, mbIdx, mIdx, -1, 3});
                }
            } else if (type == "param") {
                int mbIdx = (*it)->data(1, Qt::UserRole).toInt();
                int mIdx = (*it)->data(2, Qt::UserRole).toInt();
                int pIdx = (*it)->data(3, Qt::UserRole).toInt();
                if (fIdx >= 0 && fIdx < m_files.size() &&
                    mbIdx >= 0 && mbIdx < m_files[fIdx].blocks.size() &&
                    mIdx >= 0 && mIdx < m_files[fIdx].blocks[mbIdx].missions.size() &&
                    pIdx >= 0 && pIdx < m_files[fIdx].blocks[mbIdx].missions[mIdx].params.size()) {
                    if (m_showingFavoritesOnly && !m_files[fIdx].blocks[mbIdx].missions[mIdx].params[pIdx].favorited)
                        { ++it; continue; }
                    m_searchResults.append({fIdx, mbIdx, mIdx, pIdx, 3});
                }
            } else {
                // block 类型仅记录，无具体 mission
                m_searchResults.append({fIdx, -1, -1, -1, 3});
            }
        }
        ++it;
    }

    // 去重：三级搜索可能命中同一 (fileIdx, mbIdx, mIdx, pIdx)，保留最优 priority
    QHash<QString, int> bestIdx; // key = "fIdx|mbIdx|mIdx|pIdx", value = index in m_searchResults
    for (int i = 0; i < m_searchResults.size(); ++i) {
        const auto& r = m_searchResults[i];
        QString key = QStringLiteral("%1|%2|%3|%4")
            .arg(r.fileIdx).arg(r.mbIdx).arg(r.mIdx).arg(r.pIdx);
        auto it2 = bestIdx.find(key);
        if (it2 == bestIdx.end()) {
            bestIdx.insert(key, i);
        } else if (m_searchResults[*it2].priority > r.priority) {
            bestIdx[key] = i; // 用更优 priority 替换
        }
    }
    QVector<SearchResult> deduped;
    deduped.reserve(bestIdx.size());
    for (int idx : bestIdx.values())
        deduped.append(m_searchResults[idx]);
    // 按原顺序（priority 组内保持遍历顺序）
    std::sort(deduped.begin(), deduped.end(),
        [](const SearchResult& a, const SearchResult& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            if (a.fileIdx != b.fileIdx) return a.fileIdx < b.fileIdx;
            if (a.mbIdx != b.mbIdx) return a.mbIdx < b.mbIdx;
            if (a.mIdx != b.mIdx) return a.mIdx < b.mIdx;
            return a.pIdx < b.pIdx;
        });
    m_searchResults = deduped;

    // 自动定位到第一个结果
    if (!m_searchResults.isEmpty()) {
        navigateSearchResult(+1);
    } else {
        if (m_searchInfoBar) m_searchInfoBar->setVisible(false);
        updateStatus(QStringLiteral("搜索 \"%1\" — 无匹配结果").arg(kw));
    }
}

void ParamConfigDialog::navigateSearchResult(int direction)
{
    if (m_searchResults.isEmpty()) return;

    m_currentSearchIndex += direction;
    if (m_currentSearchIndex >= m_searchResults.size()) m_currentSearchIndex = 0;
    if (m_currentSearchIndex < 0) m_currentSearchIndex = m_searchResults.size() - 1;

    const auto& r = m_searchResults[m_currentSearchIndex];
    QString kw = ui.lineEdit_search->text().trimmed();

    if (r.fileIdx >= 0 && r.fileIdx < m_files.size()) {
        if (r.mbIdx >= 0 && r.mIdx >= 0 &&
            r.mbIdx < m_files[r.fileIdx].blocks.size() &&
            r.mIdx < m_files[r.fileIdx].blocks[r.mbIdx].missions.size()) {
            m_currentFileIndex = r.fileIdx;
            m_currentMbIndex = r.mbIdx;
            m_currentMissionIndex = r.mIdx;
            populateTable(m_files[r.fileIdx].blocks[r.mbIdx].missions[r.mIdx],
                          r.pIdx >= 0 ? r.pIdx : -1);
            selectTreeMission(r.fileIdx, r.mbIdx, r.mIdx);
        } else {
            // block 类型：仅树节点定位
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                QString type = (*it)->data(0, Qt::UserRole + 1).toString();
                int fIdx = (*it)->data(0, Qt::UserRole).toInt();
                if (type == "block" && fIdx == r.fileIdx) {
                    QTreeWidgetItem* p = (*it)->parent();
                    while (p) { p->setExpanded(true); p = p->parent(); }
                    ui.treeWidget_checkItems->scrollToItem(*it);
                    break;
                }
                ++it;
            }
        }
    }

    QString fileName;
    if (r.fileIdx >= 0 && r.fileIdx < m_files.size()) {
        fileName = QFileInfo(m_files[r.fileIdx].filePath).fileName();
    }
    if (m_searchInfoBar && m_searchInfoLabel) {
        m_searchInfoLabel->setText(QStringLiteral("%1/%2  %3  |  Enter 下一条  Shift+Enter 上一条")
            .arg(m_currentSearchIndex + 1).arg(m_searchResults.size()).arg(fileName));
        m_searchInfoBar->setVisible(true);
    }
    updateStatus(QStringLiteral("匹配结果 %1/%2 | 文件：%3 | Enter 下一条  Shift+Enter 上一条")
        .arg(m_currentSearchIndex + 1).arg(m_searchResults.size()).arg(fileName));
}

void ParamConfigDialog::selectTreeMission(int fileIdx, int mbIdx, int mIdx)
{
    QTreeWidget* tree = ui.treeWidget_checkItems;
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 1).toString() == "mission" &&
            (*it)->data(0, Qt::UserRole).toInt() == fileIdx &&
            (*it)->data(1, Qt::UserRole).toInt() == mbIdx &&
            (*it)->data(2, Qt::UserRole).toInt() == mIdx) {
            QTreeWidgetItem* p = (*it)->parent();
            while (p) { p->setExpanded(true); p = p->parent(); }
            m_selectingTreeItem = true;
            tree->setCurrentItem(*it);
            m_selectingTreeItem = false;
            tree->scrollToItem(*it, QAbstractItemView::PositionAtCenter);
            return;
        }
        ++it;
    }
}

// ===================================================================
// 收集编辑值
// ===================================================================

void ParamConfigDialog::collectCurrentMissionValues()
{
    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];
    QTableWidget* table = ui.tableWidget_params;

    if (m_showingPaths) {
        QVector<ParsedParameter>& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;

        for (int row = 0; row < pathVec.size() && row < table->rowCount(); ++row) {
            ParsedParameter& pp = pathVec[row];
            if (QTableWidgetItem* item = table->item(row, 1))
                pp.name = item->text().remove(QChar(0x200B)).trimmed();
            if (QTableWidgetItem* item = table->item(row, 3))
                pp.value = item->text().trimmed();
            if (QTableWidgetItem* item = table->item(row, 4))
                pp.description = item->text().trimmed();
        }
    } else {
        for (int row = 0; row < mission.params.size() && row < table->rowCount(); ++row) {
            ParsedParameter& param = mission.params[row];

            if (QTableWidgetItem* item = table->item(row, 1))
                param.name = item->text().remove(QChar(0x200B)).trimmed();
            // 列3: value 可能通过 ComboBox 变更，需回读
            if (QComboBox* combo = qobject_cast<QComboBox*>(table->cellWidget(row, 3)))
                param.value = combo->currentText();
            else if (QTableWidgetItem* item = table->item(row, 3))
                param.value = item->text().trimmed();
            // 列2/列4: description 编辑已由 onTableCellChanged 实时更新，
            // 此处不再重建，避免空 description 被 param.name 回退值覆盖
        }
    }
}

void ParamConfigDialog::collectFormValues()
{
    collectCurrentMissionValues();
}

// ===================================================================
// 保存
// ===================================================================

void ParamConfigDialog::onSaveToFile()
{
    if (m_files.isEmpty()) {
        QMessageBox::warning(this,
            QStringLiteral("未选择文件"),
            QStringLiteral("请先选择XML配置文件"));
        return;
    }

    collectCurrentMissionValues();

    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        auto& fd = m_files[fIdx];

        auto saveParam = [&fd](ParsedParameter& param) {
            if (param.domElement.isNull()) return;
            if (param.domElement.tagName() != param.name) {
                QDomElement parent = param.domElement.parentNode().toElement();
                if (!parent.isNull()) {
                    QDomDocument doc = param.domElement.ownerDocument();
                    QDomElement newElem = doc.createElement(param.name);
                    QDomNamedNodeMap attrs = param.domElement.attributes();
                    for (int a = 0; a < attrs.size(); ++a) {
                        QDomAttr attr = attrs.item(a).toAttr();
                        if (!attr.isNull() && attr.name() != "note")
                            newElem.setAttribute(attr.name(), attr.value());
                    }
                    newElem.setAttribute("note", param.description);
                    newElem.setAttribute("favorite", param.favorited ? "true" : "false");
                    QDomText textNode = doc.createTextNode(param.value);
                    newElem.appendChild(textNode);
                    parent.replaceChild(newElem, param.domElement);
                    param.domElement = newElem;
                    return;
                }
            }
            param.domElement.setAttribute("note", param.description);
            param.domElement.setAttribute("favorite", param.favorited ? "true" : "false");
            while (param.domElement.hasChildNodes())
                param.domElement.removeChild(param.domElement.firstChild());
            param.domElement.appendChild(fd.xmlDoc.createTextNode(param.value));
        };

        for (auto& mb : fd.blocks) {
            for (auto& mission : mb.missions) {
                for (auto& param : mission.params)
                    saveParam(param);
                for (auto& pp : mission.inputPaths)
                    saveParam(pp);
                for (auto& pp : mission.outputPaths)
                    saveParam(pp);
            }
        }

        QFile file(fd.filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this,
                QStringLiteral("保存失败"),
                QStringLiteral("无法写入文件: %1").arg(file.errorString()));
            continue;
        }

        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        fd.xmlDoc.save(stream, 4);
        file.close();

        for (auto& mb : fd.blocks)
            for (auto& mission : mb.missions) {
                for (auto& param : mission.params) {
                    param.originalValue = param.value;
                    param.originalDescription = param.description;
                    param.originalFavorited = param.favorited;
                }
                for (auto& pp : mission.inputPaths) {
                    pp.originalValue = pp.value;
                    pp.originalDescription = pp.description;
                    pp.originalFavorited = pp.favorited;
                }
                for (auto& pp : mission.outputPaths) {
                    pp.originalValue = pp.value;
                    pp.originalDescription = pp.description;
                    pp.originalFavorited = pp.favorited;
                }
            }
    }

    m_pendingHighlightRow = -1;
    refreshParamTree();

    updateStatus(QStringLiteral("已保存 %1 个文件").arg(m_files.size()));
    updateWindowTitle();
    QMessageBox::information(this,
        QStringLiteral("保存成功"),
        QStringLiteral("已保存 %1 个文件").arg(m_files.size()));
}

void ParamConfigDialog::onSaveAsFile()
{
    if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) {
        QMessageBox::warning(this,
            QStringLiteral("未选择文件"),
            QStringLiteral("请先在左侧树中选择一个文件"));
        return;
    }

    QString savePath = QFileDialog::getSaveFileName(this,
        QStringLiteral("另存为XML配置文件"),
        QString(),
        QStringLiteral("XML文件 (*.xml)"));
    if (savePath.isEmpty()) return;

    collectCurrentMissionValues();

    auto& fd = m_files[m_currentFileIndex];

    auto saveParamSA = [&fd](ParsedParameter& param) {
        if (param.domElement.isNull()) return;
        if (param.domElement.tagName() != param.name) {
            QDomElement parent = param.domElement.parentNode().toElement();
            if (!parent.isNull()) {
                QDomDocument doc = param.domElement.ownerDocument();
                QDomElement newElem = doc.createElement(param.name);
                QDomNamedNodeMap attrs = param.domElement.attributes();
                for (int a = 0; a < attrs.size(); ++a) {
                    QDomAttr attr = attrs.item(a).toAttr();
                    if (!attr.isNull() && attr.name() != "note")
                        newElem.setAttribute(attr.name(), attr.value());
                }
                newElem.setAttribute("note", param.description);
                newElem.setAttribute("favorite", param.favorited ? "true" : "false");
                QDomText textNode = doc.createTextNode(param.value);
                newElem.appendChild(textNode);
                parent.replaceChild(newElem, param.domElement);
                param.domElement = newElem;
                return;
            }
        }
        param.domElement.setAttribute("note", param.description);
        param.domElement.setAttribute("favorite", param.favorited ? "true" : "false");
        while (param.domElement.hasChildNodes())
            param.domElement.removeChild(param.domElement.firstChild());
        param.domElement.appendChild(fd.xmlDoc.createTextNode(param.value));
    };

    for (auto& mb : fd.blocks) {
        for (auto& mission : mb.missions) {
            for (auto& param : mission.params)
                saveParamSA(param);
            for (auto& pp : mission.inputPaths)
                saveParamSA(pp);
            for (auto& pp : mission.outputPaths)
                saveParamSA(pp);
        }
    }

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this,
            QStringLiteral("保存失败"),
            QStringLiteral("无法写入文件: %1").arg(file.errorString()));
        return;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    fd.xmlDoc.save(stream, 4);
    file.close();

    for (auto& mb : fd.blocks)
        for (auto& mission : mb.missions) {
            for (auto& param : mission.params) {
                param.originalValue = param.value;
                param.originalDescription = param.description;
                param.originalFavorited = param.favorited;
            }
            for (auto& pp : mission.inputPaths) {
                pp.originalValue = pp.value;
                pp.originalDescription = pp.description;
                pp.originalFavorited = pp.favorited;
            }
            for (auto& pp : mission.outputPaths) {
                pp.originalValue = pp.value;
                pp.originalDescription = pp.description;
                pp.originalFavorited = pp.favorited;
            }
        }

    m_pendingHighlightRow = -1;
    refreshParamTree();

    updateStatus(QStringLiteral("已另存为: %1").arg(savePath));
    updateWindowTitle();
    QMessageBox::information(this,
        QStringLiteral("保存成功"),
        QStringLiteral("参数已另存到:\n%1").arg(savePath));
}

void ParamConfigDialog::reloadFromFile()
{
    if (m_files.isEmpty()) return;

    ui.lineEdit_search->clear();
    m_filterModifiedOnly = false;
    ui.checkBox_filterMarked->setChecked(false);

    // 重新解析所有文件
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        ParsedFileData fd = parseOneFile(m_files[fIdx].filePath);
        if (!fd.blocks.isEmpty())
            m_files[fIdx] = fd;
    }

    refreshParamTree();
    updateWindowTitle();
    updateStatus(QStringLiteral("已重新加载 %1 个文件").arg(m_files.size()));
}

// ===================================================================
// 修改检测 & 窗口标题
// ===================================================================

bool ParamConfigDialog::hasAnyModification() const
{
    for (const auto& fd : m_files)
        for (const auto& mb : fd.blocks)
            for (const auto& ms : mb.missions) {
                for (const auto& p : ms.params)
                    if (p.value != p.originalValue || p.description != p.originalDescription
                        || p.favorited != p.originalFavorited)
                        return true;
                for (const auto& p : ms.inputPaths)
                    if (p.value != p.originalValue || p.description != p.originalDescription
                        || p.favorited != p.originalFavorited)
                        return true;
                for (const auto& p : ms.outputPaths)
                    if (p.value != p.originalValue || p.description != p.originalDescription
                        || p.favorited != p.originalFavorited)
                        return true;
            }
    return false;
}

void ParamConfigDialog::updateWindowTitle()
{
    QString title = QStringLiteral("参数配置");
    if (hasAnyModification())
        title += QStringLiteral(" *");
    setWindowTitle(title);
}

void ParamConfigDialog::closeEvent(QCloseEvent* event)
{
    collectCurrentMissionValues();
    if (hasAnyModification()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("未保存的修改"));
        msgBox.setText(QStringLiteral("参数已修改，是否保存？"));
        msgBox.setIcon(QMessageBox::Warning);
        QPushButton* btnSave = msgBox.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
        QPushButton* btnDiscard = msgBox.addButton(QStringLiteral("不保存"), QMessageBox::DestructiveRole);
        QPushButton* btnCancel = msgBox.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        msgBox.setDefaultButton(btnSave);
        msgBox.exec();
        if (msgBox.clickedButton() == btnSave) {
            onSaveToFile();
            event->accept();
        } else if (msgBox.clickedButton() == btnDiscard) {
            event->accept();
        } else {
            event->ignore();
        }
    } else {
        event->accept();
    }
}

bool ParamConfigDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == ui.lineEdit_search && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return && (ke->modifiers() & Qt::ShiftModifier)) {
            navigateSearchResult(-1);
            return true;
        }
    }
    return QDialog::eventFilter(obj, event);
}

// ===================================================================
// 重置
// ===================================================================

void ParamConfigDialog::onResetCurrentRow()
{
    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    int row = ui.tableWidget_params->currentRow();
    if (row < 0) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];

    if (m_showingPaths) {
        QVector<ParsedParameter>& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;
        if (row >= pathVec.size()) return;
        ParsedParameter& pp = pathVec[row];
        if (pp.value == pp.originalValue && pp.description == pp.originalDescription) return;
        collectCurrentMissionValues();
        pp.value = pp.originalValue;
        pp.description = pp.originalDescription;
        QString label = m_showingInputPaths ? QStringLiteral("输入") : QStringLiteral("输出");
        populatePathTable(pathVec, label);
        ui.tableWidget_params->selectRow(row);
        QString treeType = m_showingInputPaths ? "inpath" : "outpath";
        QTreeWidgetItemIterator tit(ui.treeWidget_checkItems);
        while (*tit) {
            if ((*tit)->data(0, Qt::UserRole + 1).toString() == treeType &&
                (*tit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                (*tit)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                (*tit)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                (*tit)->data(3, Qt::UserRole).toInt() == row) {
                QFont pf = (*tit)->font(0); pf.setBold(false); (*tit)->setFont(0, pf);
                (*tit)->setForeground(0, QColor(Qt::black));
                break;
            }
            ++tit;
        }
        updateWindowTitle();
        updateStatus(QStringLiteral("已重置第 %1 行路径").arg(row + 1));
        return;
    }

    if (row >= mission.params.size()) return;

    ParsedParameter& param = mission.params[row];
    if (param.value == param.originalValue && param.description == param.originalDescription) return;

    collectCurrentMissionValues();
    param.value = param.originalValue;
    param.description = param.originalDescription;

    populateTable(mission);
    ui.tableWidget_params->selectRow(row);

    QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
    while (*it) {
        QString type = (*it)->data(0, Qt::UserRole + 1).toString();
        if (type == "param" &&
            (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
            (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
            (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
            (*it)->data(3, Qt::UserRole).toInt() == row) {
            int sp = findNameSep(param.description);
            QString dn = param.description.isEmpty() ? param.name
                : param.description.left(sp > 0 ? sp : param.description.length());
            (*it)->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + dn + ": " + param.value);
            QFont pf = (*it)->font(0);
            pf.setBold(false);
            (*it)->setFont(0, pf);
            (*it)->setForeground(0, QColor(Qt::black));
            break;
        }
        ++it;
    }

    updateWindowTitle();
    updateStatus(QStringLiteral("已重置第 %1 行参数").arg(row + 1));
}

void ParamConfigDialog::onResetCurrentMission()
{
    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];

    bool anyModified = false;
    for (const auto& p : mission.params)
        if (p.value != p.originalValue || p.description != p.originalDescription) { anyModified = true; break; }
    if (!anyModified)
        for (const auto& p : mission.inputPaths)
            if (p.value != p.originalValue || p.description != p.originalDescription) { anyModified = true; break; }
    if (!anyModified)
        for (const auto& p : mission.outputPaths)
            if (p.value != p.originalValue || p.description != p.originalDescription) { anyModified = true; break; }
    if (!anyModified) {
        updateStatus(QStringLiteral("当前任务无修改，无需重置"));
        return;
    }

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(QStringLiteral("重置任务"));
    msgBox.setText(QStringLiteral("确定要重置当前任务的全部参数为初始值吗？\n此操作将丢弃所有未保存的修改。"));
    msgBox.setIcon(QMessageBox::Question);
    QPushButton* btnYes = msgBox.addButton(QStringLiteral("确定"), QMessageBox::YesRole);
    QPushButton* btnNo = msgBox.addButton(QStringLiteral("取消"), QMessageBox::NoRole);
    msgBox.setDefaultButton(btnNo);
    msgBox.exec();
    if (msgBox.clickedButton() != btnYes) return;

    collectCurrentMissionValues();

    for (auto& param : mission.params) {
        param.value = param.originalValue;
        param.description = param.originalDescription;
    }
    for (auto& pp : mission.inputPaths) {
        pp.value = pp.originalValue;
        pp.description = pp.originalDescription;
    }
    for (auto& pp : mission.outputPaths) {
        pp.value = pp.originalValue;
        pp.description = pp.originalDescription;
    }

    if (m_showingPaths) {
        const auto& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;
        QString label = m_showingInputPaths ? QStringLiteral("输入") : QStringLiteral("输出");
        populatePathTable(pathVec, label);
    } else {
        populateTable(mission);
    }

    int fIdx = m_currentFileIndex;
    int mbIdx = m_currentMbIndex;
    int mIdx = m_currentMissionIndex;
    QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
    while (*it) {
        QString type = (*it)->data(0, Qt::UserRole + 1).toString();
        if (type == "mission" &&
            (*it)->data(0, Qt::UserRole).toInt() == fIdx &&
            (*it)->data(1, Qt::UserRole).toInt() == mbIdx &&
            (*it)->data(2, Qt::UserRole).toInt() == mIdx) {
            for (int c = (*it)->childCount() - 1; c >= 0; --c) {
                QTreeWidgetItem* child = (*it)->child(c);
                if (child->data(0, Qt::UserRole + 1).toString() == "mod-hint") {
                    (*it)->removeChild(child);
                    delete child;
                }
            }
        }
        if (type == "param" &&
            (*it)->data(0, Qt::UserRole).toInt() == fIdx &&
            (*it)->data(1, Qt::UserRole).toInt() == mbIdx &&
            (*it)->data(2, Qt::UserRole).toInt() == mIdx) {
            int pIdx = (*it)->data(3, Qt::UserRole).toInt();
            if (pIdx < mission.params.size()) {
                const auto& param = mission.params[pIdx];
                int sp = findNameSep(param.description);
                QString dn = param.description.isEmpty() ? param.name
                    : param.description.left(sp > 0 ? sp : param.description.length());
                (*it)->setText(0, QStringLiteral("  \xE2\x9A\x99 ") + dn + ": " + param.value);
            }
            QFont pf = (*it)->font(0);
            pf.setBold(false);
            (*it)->setFont(0, pf);
            (*it)->setForeground(0, QColor(Qt::black));
        }
        if ((type == "inpath" || type == "outpath") &&
            (*it)->data(0, Qt::UserRole).toInt() == fIdx &&
            (*it)->data(1, Qt::UserRole).toInt() == mbIdx &&
            (*it)->data(2, Qt::UserRole).toInt() == mIdx) {
            QFont pf = (*it)->font(0);
            pf.setBold(false);
            (*it)->setFont(0, pf);
            (*it)->setForeground(0, QColor(Qt::black));
        }
        ++it;
    }

    updateWindowTitle();
    updateStatus(QStringLiteral("已重置当前任务全部参数"));
}

// ===================================================================
// 文件修改检测
// ===================================================================

bool ParamConfigDialog::fileHasModification(int fileIdx) const
{
    if (fileIdx < 0 || fileIdx >= m_files.size()) return false;
    const auto& fd = m_files[fileIdx];
    for (const auto& mb : fd.blocks)
        for (const auto& ms : mb.missions) {
            for (const auto& p : ms.params)
                if (p.value != p.originalValue || p.description != p.originalDescription
                    || p.favorited != p.originalFavorited)
                    return true;
            for (const auto& p : ms.inputPaths)
                if (p.value != p.originalValue || p.description != p.originalDescription
                    || p.favorited != p.originalFavorited)
                    return true;
            for (const auto& p : ms.outputPaths)
                if (p.value != p.originalValue || p.description != p.originalDescription
                    || p.favorited != p.originalFavorited)
                    return true;
        }
    return false;
}

// ===================================================================
// 树右键菜单
// ===================================================================

void ParamConfigDialog::onTreeContextMenu(const QPoint& pos)
{
    QTreeWidget* tree = ui.treeWidget_checkItems;
    QTreeWidgetItem* item = tree->itemAt(pos);
    if (!item) return;

    QString type = item->data(0, Qt::UserRole + 1).toString();
    if (type != "file") return;

    int fileIdx = item->data(0, Qt::UserRole).toInt();
    if (fileIdx < 0 || fileIdx >= m_files.size()) return;

    QMenu menu(tree);
    QAction* removeAction = menu.addAction(QStringLiteral("移除文件"));
    if (menu.exec(tree->viewport()->mapToGlobal(pos)) == removeAction)
        onRemoveFile(fileIdx);
}

// ===================================================================
// 移除文件
// ===================================================================

void ParamConfigDialog::onRemoveFile(int fileIdx)
{
    if (fileIdx < 0 || fileIdx >= m_files.size()) return;
    if (m_files.size() <= 1) {
        QMessageBox::information(this,
            QStringLiteral("无法移除"),
            QStringLiteral("至少保留一个文件"));
        return;
    }

    collectCurrentMissionValues();
    m_files.removeAt(fileIdx);

    if (m_currentFileIndex > fileIdx)
        m_currentFileIndex--;
    else if (m_currentFileIndex == fileIdx)
        m_currentFileIndex = -1;

    m_currentMbIndex = -1;
    m_currentMissionIndex = -1;
    refreshParamTree();
    updateStatus(QStringLiteral("已移除文件"));
}

// ===================================================================
// 批量同步 Mission 参数到其他文件
// ===================================================================

void ParamConfigDialog::onSyncToOtherFiles()
{
    if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    const auto& fd = m_files[m_currentFileIndex];
    if (m_currentMbIndex >= fd.blocks.size()) return;
    if (m_currentMissionIndex >= fd.blocks[m_currentMbIndex].missions.size()) return;

    const auto& curMission = fd.blocks[m_currentMbIndex].missions[m_currentMissionIndex];
    QString curId = curMission.id;
    if (curId.isEmpty()) {
        updateStatus(QStringLiteral("当前 Mission 无 ID，无法同步"));
        return;
    }

    // 收集当前 Mission 中已修改的参数/路径（按键名索引，仅 value 或 description 有变化的）
    collectCurrentMissionValues();
    struct ChangedItem { QString value; QString description; };
    QHash<QString, ChangedItem> changedParams;
    for (const auto& p : curMission.params) {
        if (p.value != p.originalValue || p.description != p.originalDescription)
            changedParams[p.name] = { p.value, p.description };
    }
    QHash<QString, ChangedItem> changedInputs;
    for (const auto& p : curMission.inputPaths) {
        if (p.value != p.originalValue || p.description != p.originalDescription)
            changedInputs[p.value] = { p.value, p.description };
    }
    QHash<QString, ChangedItem> changedOutputs;
    for (const auto& p : curMission.outputPaths) {
        if (p.value != p.originalValue || p.description != p.originalDescription)
            changedOutputs[p.value] = { p.value, p.description };
    }

    if (changedParams.isEmpty() && changedInputs.isEmpty() && changedOutputs.isEmpty()) {
        updateStatus(QStringLiteral("当前 Mission 无修改，无需同步"));
        return;
    }

    // 收集同名 Mission 的其他文件，同时记录每个目标匹配的参数子集
    struct SyncTarget {
        int fileIdx; int mbIdx; int mIdx; QString fileName;
        QStringList matchedParamNames;   // 该目标中含有的已修改参数名
        QStringList matchedInputNames;   // 该目标中含有的已修改输入路径
        QStringList matchedOutputNames;  // 该目标中含有的已修改输出路径
    };
    QVector<SyncTarget> validTargets;
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        if (fIdx == m_currentFileIndex) continue;
        const auto& ofd = m_files[fIdx];
        for (int bIdx = 0; bIdx < ofd.blocks.size(); ++bIdx) {
            for (int mIdx = 0; mIdx < ofd.blocks[bIdx].missions.size(); ++mIdx) {
                if (ofd.blocks[bIdx].missions[mIdx].id != curId) continue;

                const ParsedMission& tm = ofd.blocks[bIdx].missions[mIdx];
                SyncTarget st;
                st.fileIdx = fIdx;
                st.mbIdx = bIdx;
                st.mIdx = mIdx;
                st.fileName = QFileInfo(ofd.filePath).fileName();

                // 收集该目标中实际存在的已修改参数
                for (auto it = changedParams.begin(); it != changedParams.end(); ++it) {
                    for (const auto& tp : tm.params) {
                        if (tp.name == it.key()) { st.matchedParamNames.append(it.key()); break; }
                    }
                }
                for (auto it = changedInputs.begin(); it != changedInputs.end(); ++it) {
                    for (const auto& tp : tm.inputPaths) {
                        if (tp.value == it.key() || tp.name == it.key()) { st.matchedInputNames.append(it.key()); break; }
                    }
                }
                for (auto it = changedOutputs.begin(); it != changedOutputs.end(); ++it) {
                    for (const auto& tp : tm.outputPaths) {
                        if (tp.value == it.key() || tp.name == it.key()) { st.matchedOutputNames.append(it.key()); break; }
                    }
                }

                int totalMatch = st.matchedParamNames.size() + st.matchedInputNames.size() + st.matchedOutputNames.size();
                if (totalMatch > 0) validTargets.append(st);
            }
        }
    }

    if (validTargets.isEmpty()) {
        int totalChanged = changedParams.size() + changedInputs.size() + changedOutputs.size();
        if (totalChanged > 0) {
            bool hasSameMission = false;
            for (int fIdx = 0; fIdx < m_files.size() && !hasSameMission; ++fIdx) {
                if (fIdx == m_currentFileIndex) continue;
                for (int bIdx = 0; bIdx < m_files[fIdx].blocks.size() && !hasSameMission; ++bIdx) {
                    for (int mIdx = 0; mIdx < m_files[fIdx].blocks[bIdx].missions.size(); ++mIdx) {
                        if (m_files[fIdx].blocks[bIdx].missions[mIdx].id == curId) {
                            hasSameMission = true; break;
                        }
                    }
                }
            }
            QString msg = hasSameMission
                ? QStringLiteral("Mission [%1] 有 %2 个参数发生修改，\n"
                    "但其他文件的同名 Mission 中均不存在对应参数，无法同步。")
                    .arg(curId).arg(totalChanged)
                : QStringLiteral("没有其他文件包含同名 Mission [%1]。").arg(curId);
            QMessageBox::information(this, QStringLiteral("同步到其他文件"), msg);
        } else {
            updateStatus(QStringLiteral("当前 Mission 无修改，无需同步"));
        }
        return;
    }

    // 弹窗 — 按参数分组，逐文件勾选
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("同步到其他文件"));
    dlg.setMinimumWidth(480);
    QVBoxLayout* dlgLayout = new QVBoxLayout(&dlg);
    dlgLayout->setSpacing(2);

    int totalChanged = changedParams.size() + changedInputs.size() + changedOutputs.size();
    QLabel* hint = new QLabel(QStringLiteral(
        "Mission [%1] 共计 %2 个参数发生修改，下方按参数展示可同步条目：")
        .arg(curId).arg(totalChanged));
    hint->setWordWrap(true);
    dlgLayout->addWidget(hint);

    // 全选 / 全不选
    QHBoxLayout* selRow = new QHBoxLayout();
    QPushButton* btnSelectAll = new QPushButton(QStringLiteral("全选"));
    QPushButton* btnDeselectAll = new QPushButton(QStringLiteral("全不选"));
    btnSelectAll->setFixedWidth(60);
    btnDeselectAll->setFixedWidth(60);
    selRow->addWidget(btnSelectAll);
    selRow->addWidget(btnDeselectAll);
    selRow->addStretch();
    dlgLayout->addLayout(selRow);

    // 数据结构：每个 checkbox 对应一个 (参数, 目标文件)
    struct CheckEntry {
        QCheckBox* cb;
        int fileIdx; int mbIdx; int mIdx;
        QString paramKey;
        int itemType; // 0=param, 1=inputPath, 2=outputPath
    };
    QVector<CheckEntry> allChecks;

    // 去重计数：validTargets 中不重复的文件数
    QSet<int> allTargetFileSet;
    for (const auto& st : validTargets) allTargetFileSet.insert(st.fileIdx);
    int totalUniqueFiles = allTargetFileSet.size();

    auto addSection = [&](const QString& label, int matchFileCount) {
        QString html;
        if (matchFileCount > 0) {
            html = QStringLiteral(
                "<b style='color:#555'>━━ %1  【可同步至 %2/%3 个文件】</b>")
                .arg(label).arg(matchFileCount).arg(totalUniqueFiles);
        } else {
            html = QStringLiteral(
                "<b style='color:#aaa'>━━ %1  【无匹配目标】</b>")
                .arg(label);
        }
        QLabel* sec = new QLabel(html);
        sec->setTextFormat(Qt::RichText);
        dlgLayout->addWidget(sec);
    };

    auto addCheckRow = [&](const SyncTarget& st, const QString& paramKey, int itemType) {
        QWidget* row = new QWidget();
        QHBoxLayout* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 1, 0, 1);
        rowLayout->setSpacing(4);

        QCheckBox* cb = new QCheckBox();
        cb->setChecked(false);
        rowLayout->addWidget(cb);

        QLabel* lbl = new QLabel(QStringLiteral("%1  —  %2  [ID=%3]")
            .arg(st.fileName)
            .arg(m_files[st.fileIdx].blocks[st.mbIdx].missions[st.mIdx].note)
            .arg(curId));
        rowLayout->addWidget(lbl, 1);
        dlgLayout->addWidget(row);
        allChecks.append({cb, st.fileIdx, st.mbIdx, st.mIdx, paramKey, itemType});
    };

    auto addMissingRow = [&](const SyncTarget& st) {
        QLabel* lbl = new QLabel(QStringLiteral(
            "    <span style='color:#999;'>%1  —  缺少</span>").arg(st.fileName));
        lbl->setTextFormat(Qt::RichText);
        dlgLayout->addWidget(lbl);
    };

    // 按参数分组输出
    auto outputParamGroup = [&](const QString& label, const QHash<QString, ChangedItem>& changedMap,
                                int itemType) {
        for (auto it = changedMap.begin(); it != changedMap.end(); ++it) {
            const QString& key = it.key();
            QVector<int> matchIdx, missIdx;
            QSet<int> matchFileSet;
            for (int i = 0; i < validTargets.size(); ++i) {
                bool matched = false;
                if (itemType == 0) matched = validTargets[i].matchedParamNames.contains(key);
                else if (itemType == 1) matched = validTargets[i].matchedInputNames.contains(key);
                else matched = validTargets[i].matchedOutputNames.contains(key);
                if (matched) {
                    matchIdx.append(i);
                    matchFileSet.insert(validTargets[i].fileIdx);
                } else {
                    missIdx.append(i);
                }
            }
            QString secLabel = label.arg(key);
            addSection(secLabel, matchFileSet.size());
            for (int i : matchIdx)
                addCheckRow(validTargets[i], key, itemType);
            if (!matchIdx.isEmpty()) {
                for (int i : missIdx)
                    addMissingRow(validTargets[i]);
            }
        }
    };

    outputParamGroup(QStringLiteral("参数 %1"), changedParams, 0);
    outputParamGroup(QStringLiteral("输入路径 %1"), changedInputs, 1);
    outputParamGroup(QStringLiteral("输出路径 %1"), changedOutputs, 2);

    // 无可勾选项时全选按钮置灰
    if (allChecks.isEmpty()) {
        btnSelectAll->setEnabled(false);
        btnDeselectAll->setEnabled(false);
    }

    connect(btnSelectAll, &QPushButton::clicked, [&allChecks]() {
        for (auto& e : allChecks) e.cb->setChecked(true);
    });
    connect(btnDeselectAll, &QPushButton::clicked, [&allChecks]() {
        for (auto& e : allChecks) e.cb->setChecked(false);
    });

    QHBoxLayout* btnRow = new QHBoxLayout();
    QPushButton* btnOk = new QPushButton(QStringLiteral("同步"));
    QPushButton* btnCancel = new QPushButton(QStringLiteral("取消"));
    btnRow->addStretch();
    btnRow->addWidget(btnOk);
    btnRow->addWidget(btnCancel);
    dlgLayout->addLayout(btnRow);

    connect(btnOk, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    // 同步：遍历已勾选的 (参数, 文件) 对
    int syncedItems = 0;
    QSet<int> syncedFileIndices;
    for (const auto& e : allChecks) {
        if (!e.cb->isChecked()) continue;
        ParsedMission& target = m_files[e.fileIdx].blocks[e.mbIdx].missions[e.mIdx];

        if (e.itemType == 0) {
            auto it = changedParams.find(e.paramKey);
            if (it == changedParams.end()) continue;
            for (auto& tp : target.params) {
                if (tp.name == e.paramKey) {
                    tp.value = it->value;
                    tp.description = it->description;
                    break;
                }
            }
        } else if (e.itemType == 1) {
            auto it = changedInputs.find(e.paramKey);
            if (it == changedInputs.end()) continue;
            for (auto& tp : target.inputPaths) {
                if (tp.value == e.paramKey || tp.name == e.paramKey) {
                    tp.value = it->value;
                    tp.description = it->description;
                    break;
                }
            }
        } else {
            auto it = changedOutputs.find(e.paramKey);
            if (it == changedOutputs.end()) continue;
            for (auto& tp : target.outputPaths) {
                if (tp.value == e.paramKey || tp.name == e.paramKey) {
                    tp.value = it->value;
                    tp.description = it->description;
                    break;
                }
            }
        }
        syncedItems++;
        syncedFileIndices.insert(e.fileIdx);
    }

    refreshParamTree();
    updateStatus(QStringLiteral("已将 %1 个参数同步到 %2 个文件").arg(syncedItems).arg(syncedFileIndices.size()));
}

void ParamConfigDialog::updateStatus(const QString& msg)
{
    ui.label_status->setText(msg);
}
