#include "param_config_dialog.h"

#include <algorithm>
#include <functional>
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
#include <QPair>
#include <QScrollArea>

#include "ui_fit_helper.h"

// 参数表格 7 列布局（参数/路径/链接视图共用；构造时与每次填充前统一恢复，
// 防止只读视图改列数后残留 2 列状态）
static void setupParamsTableColumns(QTableWidget* table)
{
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
}

// 只读属性视图：2 列（属性/值）
static void setupRawTableColumns(QTableWidget* table)
{
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({
        QStringLiteral("属性"),
        QStringLiteral("值")
    });
    QHeaderView* hdr = table->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Fixed);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setColumnWidth(0, 160);
}

// 原始树节点文本：元素名 + 属性摘要 + 直接文本截断
static QString rawNodeText(const QDomElement& elem)
{
    QString text = elem.tagName();
    if (elem.hasAttributes()) {
        QStringList attrs;
        QDomNamedNodeMap am = elem.attributes();
        for (int a = 0; a < am.size(); ++a) {
            QDomAttr at = am.item(a).toAttr();
            if (!at.isNull())
                attrs << (at.name() + QStringLiteral("=\"") + at.value() + QStringLiteral("\""));
        }
        text += QStringLiteral("  [") + attrs.join(QStringLiteral(", ")) + QStringLiteral("]");
    }
    QString t;
    QDomNodeList cn = elem.childNodes();
    for (int c = 0; c < cn.size(); ++c)
        if (cn.at(c).isText()) t += cn.at(c).toText().data();
    t = t.trimmed();
    if (!t.isEmpty()) {
        if (t.length() > 60) t = t.left(60) + QStringLiteral("…");
        text += QStringLiteral("  =  ") + t;
    }
    return text;
}

// 按元素子节点序号路径定位元素（与树构建一致，只数元素子节点）
static QDomElement elementAtPath(const QDomElement& root, const QString& path)
{
    QDomElement cur = root;
    if (path.isEmpty()) return cur;
    const QStringList idxs = path.split(QLatin1Char(','));
    for (const QString& s : idxs) {
        int idx = s.toInt();
        QDomNodeList children = cur.childNodes();
        int seen = 0;
        QDomElement next;
        for (int c = 0; c < children.size(); ++c) {
            QDomNode n = children.at(c);
            if (!n.isElement()) continue;
            if (seen == idx) { next = n.toElement(); break; }
            ++seen;
        }
        if (next.isNull()) return QDomElement();
        cur = next;
    }
    return cur;
}

// 递归构建原始元素树（只读查看）
static void appendRawTree(QTreeWidgetItem* parentItem, const QDomElement& elem,
                          int fileIdx, const QString& path)
{
    QDomNodeList children = elem.childNodes();
    int elemIdx = 0;
    for (int c = 0; c < children.size(); ++c) {
        QDomNode n = children.at(c);
        if (!n.isElement()) continue;
        QDomElement childElem = n.toElement();
        QString childPath = path.isEmpty()
            ? QString::number(elemIdx) : path + QStringLiteral(",") + QString::number(elemIdx);
        QTreeWidgetItem* rawItem = new QTreeWidgetItem();
        rawItem->setText(0, rawNodeText(childElem));
        rawItem->setData(0, Qt::UserRole, fileIdx);
        rawItem->setData(1, Qt::UserRole, childPath);
        rawItem->setData(0, Qt::UserRole + 1, QStringLiteral("raw"));
        QFont rf = rawItem->font(0);
        rf.setFamily(QStringLiteral("Segoe UI Emoji"));
        rawItem->setFont(0, rf);
        parentItem->addChild(rawItem);
        ++elemIdx;
        appendRawTree(rawItem, childElem, fileIdx, childPath);
    }
}

ParamConfigDialog::ParamConfigDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
{
    setAttribute(Qt::WA_DeleteOnClose);
    ui.setupUi(this);
    // 麒麟上屏幕分辨率/DPI 与设计尺寸差异大，按内容自适应收放，避免布局被裁剪
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
    setupParamsTableColumns(table);
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
    // 搜索防抖：停止输入 150ms 后才执行全量搜索
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    connect(m_searchTimer, &QTimer::timeout,
            this, [this]() { doSearch(ui.lineEdit_search->text().trimmed()); });
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

    // QIODevice 重载按 XML 声明的 encoding 自动转码（GBK/UTF-8/UTF-16 等）
    if (!result.xmlDoc.setContent(&file)) {
        file.close();
        QMessageBox::warning(this,
            QStringLiteral("解析失败"),
            QStringLiteral("XML文件格式不正确，无法解析"));
        return result;
    }
    file.close();

    QDomElement root = result.xmlDoc.documentElement();

    QDomNodeList missionBlocks = root.elementsByTagName("MissionBlock");

    // FilePath 标签泛化：FilePath、FilePath2、FilePath3… 均按路径处理
    auto isFilePathTag = [](const QString& tag) {
        if (!tag.startsWith(QStringLiteral("FilePath"))) return false;
        const QString rest = tag.mid(QStringLiteral("FilePath").size());
        for (const QChar& c : rest)
            if (!c.isDigit()) return false;
        return true;
    };

    auto buildParamFrom = [](const QDomElement& e) {
        ParsedParameter p;
        p.name = e.tagName();
        p.originalName = p.name;
        p.value = e.text().trimmed();
        p.originalValue = p.value;
        p.description = e.attribute("note");
        p.originalDescription = p.description;
        p.tip = e.attribute("tip");
        p.originalTip = p.tip;
        p.favorited = (e.attribute("favorite") == QStringLiteral("true"));
        p.originalFavorited = p.favorited;
        p.domElement = e;
        return p;
    };

    // 自身无 note 时取父元素 note；结构层（ParaIn/ParaOut/Mission）不取
    auto fillParentNote = [](ParsedParameter& p) {
        if (!p.description.isEmpty()) return;
        QDomElement parentElem = p.domElement.parentNode().toElement();
        if (parentElem.isNull()) return;
        const QString pt = parentElem.tagName();
        if (pt == QStringLiteral("ParaIn") || pt == QStringLiteral("ParaOut")
            || pt == QStringLiteral("Mission"))
            return;
        p.description = parentElem.attribute("note");
        p.originalDescription = p.description;
    };

    struct WalkResult {
        QVector<QDomElement> filePaths;
        QVector<QDomElement> bareLeaves;
    };
    // 子树收集：FilePathN → 路径；有文本的叶子 → 裸参数（无包装参数）。
    // descendExcluded：是否进入被排除子树（进入后只收 FilePathN，不收裸参数）——
    // ParaIn/ParaOut 下 Parameter 子树内也挂路径（如 MissionWhileNum>FilePath），需进入；
    // Mission 级兜底不进入（ParaIn/ParaOut 由各自循环收集，防重复）。
    std::function<void(const QDomElement&, const QSet<QString>&, bool, bool, WalkResult&)> walk;
    walk = [&](const QDomElement& e, const QSet<QString>& excludeTags,
               bool descendExcluded, bool insideExcluded, WalkResult& r) {
        if (isFilePathTag(e.tagName())) { r.filePaths.append(e); return; }
        const bool selfExcluded = excludeTags.contains(e.tagName());
        if (selfExcluded && !descendExcluded) return;
        const bool nextInside = insideExcluded || selfExcluded;
        bool hasElemChild = false;
        for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
            if (!n.isElement()) continue;
            hasElemChild = true;
            walk(n.toElement(), excludeTags, descendExcluded, nextInside, r);
        }
        if (!hasElemChild && !e.text().trimmed().isEmpty()
            && !insideExcluded && !selfExcluded)
            r.bareLeaves.append(e);
    };

    auto parseParamChildren = [&](QDomElement paramElem) {
        QVector<ParsedParameter> r;
        QDomNodeList children = paramElem.childNodes();
        for (int c = 0; c < children.size(); ++c) {
            QDomNode node = children.at(c);
            if (!node.isElement()) continue;
            r.append(buildParamFrom(node.toElement()));
        }
        return r;
    };

    const QSet<QString> paraExcludes{ QStringLiteral("Parameter") };
    const QSet<QString> missionExcludes{ QStringLiteral("ParaIn"), QStringLiteral("ParaOut") };

    auto applyWalkResult = [&](const WalkResult& wr, ParsedMission& mission,
                               QVector<ParsedParameter>* pathDest) {
        for (const QDomElement& e : wr.filePaths) {
            ParsedParameter pp = buildParamFrom(e);
            fillParentNote(pp);
            if (pathDest) pathDest->append(pp);
        }
        for (const QDomElement& e : wr.bareLeaves) {
            ParsedParameter pp = buildParamFrom(e);
            fillParentNote(pp);
            mission.params.append(pp);
        }
    };

    auto parseMission = [&](QDomElement missionElem) {
        ParsedMission mission;
        mission.id = missionElem.attribute("id");
        mission.note = missionElem.attribute("note");
        mission.domElement = missionElem;

        // Mission 级兜底：排除 ParaIn/ParaOut 子树，收集直挂 FilePathN 与裸参数
        WalkResult r0;
        walk(missionElem, missionExcludes, false, false, r0);
        applyWalkResult(r0, mission, &mission.inputPaths);

        QDomNodeList paraIns = missionElem.elementsByTagName("ParaIn");
        for (int pi = 0; pi < paraIns.size(); ++pi) {
            QDomElement paraInElem = paraIns.at(pi).toElement();
            QDomNodeList parameters = paraInElem.elementsByTagName("Parameter");
            for (int p = 0; p < parameters.size(); ++p)
                mission.params.append(parseParamChildren(parameters.at(p).toElement()));
            WalkResult r;
            walk(paraInElem, paraExcludes, true, false, r);
            applyWalkResult(r, mission, &mission.inputPaths);
        }

        QDomNodeList paraOuts = missionElem.elementsByTagName("ParaOut");
        for (int po = 0; po < paraOuts.size(); ++po) {
            WalkResult r;
            walk(paraOuts.at(po).toElement(), paraExcludes, true, false, r);
            applyWalkResult(r, mission, &mission.outputPaths);
        }
        return mission;
    };

    if (missionBlocks.isEmpty()) {
        QDomNodeList missions = root.elementsByTagName("Mission");
        if (!missions.isEmpty()) {
            ParsedMissionBlock virtualBlock;
            virtualBlock.note = QStringLiteral("(根节点)");
            virtualBlock.domElement = root;
            for (int m = 0; m < missions.size(); ++m)
                virtualBlock.missions.append(parseMission(missions.at(m).toElement()));
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

        // 批量处理链接文件：结构判定——文档中无 Mission/Parameter 但含 Link
        // 元素即按链接文件处理，不依赖根元素名（根改名仍可识别）。
        // 独立链接模型，不伪装成 Mission；根元素 relativePath 一并保留
        QDomNodeList linkNodes = root.elementsByTagName("Link");
        if (!linkNodes.isEmpty()) {
            result.isLinkFile = true;
            result.relativePath = root.attribute("relativePath");
            for (int l = 0; l < linkNodes.size(); ++l) {
                QDomElement linkElem = linkNodes.at(l).toElement();
                if (linkElem.isNull()) continue;
                ParsedLink lk;
                lk.path = linkElem.text().trimmed();
                lk.originalPath = lk.path;
                lk.run = (linkElem.attribute("run").compare(
                    QStringLiteral("true"), Qt::CaseInsensitive) == 0);
                lk.originalRun = lk.run;
                lk.favorited = (linkElem.attribute("favorite") == QStringLiteral("true"));
                lk.originalFavorited = lk.favorited;
                lk.domElement = linkElem;
                result.links.append(lk);
            }
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

        for (int m = 0; m < missions.size(); ++m)
            block.missions.append(parseMission(missions.at(m).toElement()));

        result.blocks.append(block);
    }

    if (result.blocks.isEmpty() && !result.isLinkFile)
        result.isUnknown = true; // 成功解析但结构未识别 → 只读查看

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

    // 有未保存修改时先确认，默认取消，防静默丢失
    collectCurrentMissionValues();
    if (hasAnyModification()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("未保存的修改"));
        msgBox.setText(QStringLiteral("打开新文件将丢弃所有未保存的修改，确定继续吗？"));
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        if (msgBox.exec() != QMessageBox::Yes) return;
    }

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
    int totalLinks = 0;
    QStringList skippedFiles;
    for (const QString& path : paths) {
        ParsedFileData fd = parseOneFile(path);
        if (fd.blocks.isEmpty() && !fd.isLinkFile && !fd.isUnknown && QFileInfo(path).exists()) {
            skippedFiles << QFileInfo(path).fileName(); // 无法打开或 XML 解析失败
            continue;
        }
        totalBlocks += fd.blocks.size();
        for (const auto& mb : fd.blocks)
            totalMissions += mb.missions.size();
        totalLinks += fd.links.size();
        m_files.append(fd);
    }

    if (m_files.isEmpty()) {
        QString msg = QStringLiteral("未能加载任何文件");
        if (!skippedFiles.isEmpty())
            msg += QStringLiteral("：%1 个文件无法解析").arg(skippedFiles.size());
        updateStatus(msg);
        if (!skippedFiles.isEmpty())
            QMessageBox::warning(this, QStringLiteral("无法加载"),
                QStringLiteral("以下文件无法打开或 XML 格式不正确，已跳过：\n  ")
                + skippedFiles.join(QStringLiteral("\n  ")));
        return;
    }

    m_currentFileIndex = 0;
    m_filterModifiedOnly = false;
    ui.checkBox_filterMarked->setChecked(false);
    refreshParamTree();
    updateStatus(QStringLiteral("已加载 %1 个文件，%2 个任务块，%3 个Mission%4")
                 .arg(m_files.size()).arg(totalBlocks).arg(totalMissions)
                 .arg(totalLinks > 0 ? QStringLiteral("，%1 条链接").arg(totalLinks)
                                     : QString()));
    if (!skippedFiles.isEmpty())
        QMessageBox::information(this, QStringLiteral("部分文件已跳过"),
            QStringLiteral("以下文件无法打开或 XML 格式不正确，已跳过：\n  ")
            + skippedFiles.join(QStringLiteral("\n  ")));
    updateWindowTitle();
}

bool ParamConfigDialog::loadXmlFile(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        updateStatus(QStringLiteral("XML 文件不存在: %1").arg(path));
        return false;
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
    if (fd.blocks.isEmpty() && fd.links.isEmpty() && !fd.isUnknown) {
        updateStatus(QStringLiteral("未能解析文件: %1").arg(path));
        return false;
    }

    m_files.append(fd);
    m_currentFileIndex = 0;
    m_filterModifiedOnly = false;
    ui.checkBox_filterMarked->setChecked(false);
    refreshParamTree();
    updateStatus(QStringLiteral("已加载: %1").arg(QFileInfo(path).fileName()));
    updateWindowTitle();
    return true;
}

void ParamConfigDialog::onAppendFile()
{
    QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("追加XML配置文件"),
        QString(),
        QStringLiteral("XML文件 (*.xml)"));
    if (path.isEmpty()) return;

    ParsedFileData fd = parseOneFile(path);
    if (fd.blocks.isEmpty() && !fd.isLinkFile && !fd.isUnknown) {
        QMessageBox::warning(this, QStringLiteral("无法追加"),
            QStringLiteral("文件无法打开或 XML 格式不正确，未追加：\n%1").arg(path));
        return;
    }

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
    if (fd.blocks.isEmpty() && !fd.isLinkFile && !fd.isUnknown) return false;
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

QString ParamConfigDialog::paramLeafText(const ParsedParameter& p) const
{
    int sp = findNameSep(p.description);
    QString displayName = p.description.isEmpty() ? p.name
        : p.description.left(sp > 0 ? sp : p.description.length());
    return QStringLiteral("  \xE2\x9A\x99 ") + displayName + QStringLiteral(": ") + p.value;
}

QString ParamConfigDialog::pathLeafText(const ParsedParameter& p) const
{
    QString text = QStringLiteral("  \xF0\x9F\x93\x84 ") + p.value;
    if (!p.tip.isEmpty())
        text += QStringLiteral("  [") + p.tip + QStringLiteral("]");
    return text;
}

QString ParamConfigDialog::linkLeafText(const ParsedLink& l) const
{
    return QStringLiteral("  \xF0\x9F\x94\x97 ") + l.path
        + QStringLiteral("  [") + (l.run ? QStringLiteral("启用") : QStringLiteral("停用"))
        + QStringLiteral("]");
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
        const auto& fd0 = m_files[0];
        if (fd0.isLinkFile) {
            rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x84 ") + QFileInfo(fd0.filePath).fileName()
                + QStringLiteral("  [%1条链接]").arg(fd0.links.size()));
        } else if (fd0.isUnknown) {
            rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x84 ") + QFileInfo(fd0.filePath).fileName()
                + QStringLiteral("  [结构未识别，只读]"));
        } else {
            int totalBlocks = 0, totalMissions = 0;
            for (const auto& mb : fd0.blocks) {
                totalBlocks++;
                totalMissions += mb.missions.size();
            }
            rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x84 ") + QFileInfo(fd0.filePath).fileName()
                + QStringLiteral("  [%1个任务块, %2个Mission]").arg(totalBlocks).arg(totalMissions));
        }
    } else {
        int totalBlocks = 0, totalMissions = 0, totalLinks = 0;
        for (const auto& fd : m_files) {
            totalBlocks += fd.blocks.size();
            for (const auto& mb : fd.blocks)
                totalMissions += mb.missions.size();
            totalLinks += fd.links.size();
        }
        rootItem->setText(0, QStringLiteral("\xF0\x9F\x93\x82 %1 个文件, %2 个任务块, %3 个Mission%4")
            .arg(m_files.size()).arg(totalBlocks).arg(totalMissions)
            .arg(totalLinks > 0 ? QStringLiteral(", %1 条链接").arg(totalLinks) : QString()));
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

        // 链接文件：文件节点（多文件模式）→ "批量处理链接"容器 → 🔗 叶子
        if (fd.isLinkFile) {
            // 常用参数视图：无收藏链接时整组不显示（与右侧表格过滤口径一致）
            int visibleLinkCount = 0;
            for (const auto& lk : fd.links)
                if (!m_showingFavoritesOnly || lk.favorited) ++visibleLinkCount;
            if (m_showingFavoritesOnly && visibleLinkCount == 0)
                continue;
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
            QTreeWidgetItem* linkParent = singleFile ? rootItem
                : rootItem->child(rootItem->childCount() - 1);
            QTreeWidgetItem* linkList = new QTreeWidgetItem();
            linkList->setText(0, QStringLiteral("\xF0\x9F\x94\x97 批量处理链接 [%1]").arg(fd.links.size()));
            linkList->setData(0, Qt::UserRole, fileIdx);
            linkList->setData(1, Qt::UserRole, -1);
            linkList->setData(0, Qt::UserRole + 1, QStringLiteral("linklist"));
            QFont llFont = linkList->font(0);
            llFont.setFamily(QStringLiteral("Segoe UI Emoji"));
            llFont.setBold(true);
            linkList->setFont(0, llFont);
            linkParent->addChild(linkList);

            for (int lIdx = 0; lIdx < fd.links.size(); ++lIdx) {
                const auto& lk = fd.links[lIdx];
                if (m_showingFavoritesOnly && !lk.favorited) continue;
                QTreeWidgetItem* lItem = new QTreeWidgetItem();
                lItem->setText(0, linkLeafText(lk));
                lItem->setData(0, Qt::UserRole, fileIdx);
                lItem->setData(1, Qt::UserRole, lIdx);
                lItem->setData(3, Qt::UserRole, lIdx);
                lItem->setData(0, Qt::UserRole + 1, QStringLiteral("link"));
                QFont lf = lItem->font(0);
                lf.setFamily(QStringLiteral("Segoe UI Emoji"));
                lItem->setFont(0, lf);
                if (lk.modified()) {
                    QFont lf2 = lItem->font(0); lf2.setBold(true); lItem->setFont(0, lf2);
                    lItem->setForeground(0, QColor("#c0392b"));
                }
                linkList->addChild(lItem);
            }
            anyFileShown = true;
            continue;
        }

        // 未识别结构文件：只读查看原始元素树
        if (fd.isUnknown) {
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
            QTreeWidgetItem* rawParent = singleFile ? rootItem
                : rootItem->child(rootItem->childCount() - 1);
            QTreeWidgetItem* rawRoot = new QTreeWidgetItem();
            rawRoot->setText(0, QStringLiteral("\xF0\x9F\x94\x8D 结构未识别 — 只读查看"));
            rawRoot->setData(0, Qt::UserRole, fileIdx);
            rawRoot->setData(0, Qt::UserRole + 1, QStringLiteral("rawroot"));
            QFont rrFont = rawRoot->font(0);
            rrFont.setFamily(QStringLiteral("Segoe UI Emoji"));
            rrFont.setBold(true);
            rawRoot->setFont(0, rrFont);
            rawParent->addChild(rawRoot);

            QTreeWidgetItem* rawElemItem = new QTreeWidgetItem();
            rawElemItem->setText(0, rawNodeText(fd.xmlDoc.documentElement()));
            rawElemItem->setData(0, Qt::UserRole, fileIdx);
            rawElemItem->setData(1, Qt::UserRole, QString());
            rawElemItem->setData(0, Qt::UserRole + 1, QStringLiteral("raw"));
            QFont ref = rawElemItem->font(0);
            ref.setFamily(QStringLiteral("Segoe UI Emoji"));
            rawElemItem->setFont(0, ref);
            rawRoot->addChild(rawElemItem);
            appendRawTree(rawElemItem, fd.xmlDoc.documentElement(), fileIdx, QString());

            anyFileShown = true;
            continue;
        }

        for (int mbIdx = 0; mbIdx < fd.blocks.size(); ++mbIdx) {
            const auto& mb = fd.blocks[mbIdx];

            int mbTotalParams = 0, mbModified = 0;
            for (const auto& ms : mb.missions) {
                mbTotalParams += ms.params.size();
                for (const auto& p : ms.params)
                    if (p.modified()) ++mbModified;
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
                    if (p.modified()) ++missionModified;

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

                    bool modified = param.modified();

                    QTreeWidgetItem* pItem = new QTreeWidgetItem();
                    pItem->setText(0, paramLeafText(param));
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
                        if (p.modified()) ++inPathModified;

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
                        bool mod = pp.modified();
                        QTreeWidgetItem* ipItem = new QTreeWidgetItem();
                        ipItem->setText(0, pathLeafText(pp));
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
                        if (p.modified()) ++outPathModified;

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
                        bool mod = pp.modified();
                        QTreeWidgetItem* opItem = new QTreeWidgetItem();
                        opItem->setText(0, pathLeafText(pp));
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

    // 自动选中第一个有参数的 Mission；没有 Mission 时回退到链接列表节点
    bool autoSelected = false;
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
                autoSelected = true;
                break;
            }
        }
        ++it;
    }
    if (!autoSelected) {
        QTreeWidgetItemIterator it2(tree);
        while (*it2) {
            if ((*it2)->data(0, Qt::UserRole + 1).toString() == "linklist") {
                int fIdx = (*it2)->data(0, Qt::UserRole).toInt();
                if (fIdx >= 0 && fIdx < m_files.size() && m_files[fIdx].isLinkFile) {
                    tree->setCurrentItem(*it2);
                    autoSelected = true;
                    break;
                }
            }
            ++it2;
        }
    }
    if (!autoSelected) {
        QTreeWidgetItemIterator it3(tree);
        while (*it3) {
            if ((*it3)->data(0, Qt::UserRole + 1).toString() == "rawroot") {
                tree->setCurrentItem(*it3);
                break;
            }
            ++it3;
        }
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

    if (itemType == "link") {
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

    if (itemType == "linklist") {
        int fIdx = item->data(0, Qt::UserRole).toInt();
        if (fIdx >= 0 && fIdx < m_files.size() && m_files[fIdx].isLinkFile) {
            m_currentFileIndex = fIdx;
            m_currentMbIndex = -1;
            m_currentMissionIndex = -1;
            m_showingLinks = true;
            m_showingPaths = false;
            populateLinkTable(m_files[fIdx]);
            return;
        }
    }

    if (itemType == "raw" || itemType == "rawroot") {
        int fIdx = item->data(0, Qt::UserRole).toInt();
        QString elemPath = itemType == "raw"
            ? item->data(1, Qt::UserRole).toString() : QString();
        if (fIdx >= 0 && fIdx < m_files.size() && m_files[fIdx].isUnknown) {
            m_currentFileIndex = fIdx;
            m_currentMbIndex = -1;
            m_currentMissionIndex = -1;
            m_showingLinks = false;
            m_showingPaths = false;
            populateRawView(fIdx, elemPath);
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
                if (p.modified()) ++modCount;
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
            if (m_files[fIdx].isUnknown) {
                m_currentFileIndex = fIdx;
                m_currentMbIndex = -1;
                m_currentMissionIndex = -1;
                m_showingLinks = false;
                m_showingPaths = false;
                populateRawView(fIdx, QString());
                return;
            }
            if (m_files[fIdx].isLinkFile) {
                m_currentFileIndex = fIdx;
                m_currentMbIndex = -1;
                m_currentMissionIndex = -1;
                m_showingLinks = true;
                m_showingPaths = false;
                populateLinkTable(m_files[fIdx]);
                return;
            }
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
    setupParamsTableColumns(table);
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
    m_showingLinks = false;
    m_highlightedRow = -1;          // 表格已清空，蓝色高亮行随之失效
    m_updatingTable = false;
}

void ParamConfigDialog::populateTable(const ParsedMission& mission, int searchHighlightRow)
{
    m_updatingTable = true;
    m_showingPaths = false;
    m_showingLinks = false;
    QTableWidget* table = ui.tableWidget_params;
    setupParamsTableColumns(table);
    table->horizontalHeader()->setVisible(true);
    table->verticalHeader()->setVisible(false);

    // 强制清空旧内容（销毁所有旧 cell widget，杜绝 combo 信号残留）
    table->clearContents();
    table->setRowCount(0);

    if (mission.params.isEmpty()) {
        m_updatingTable = false;
        m_highlightedRow = -1;
        updateStatus(QStringLiteral("此 Mission 没有可编辑的参数块"));
        return;
    }

    table->setRowCount(mission.params.size());

    // 【2026-09-11】当前高亮行：搜索命中行优先，其次"点击左侧叶子定位"行
    // （两者互斥，同时最多一个 >= 0）。全表只允许一行是蓝色的。
    const int hlRow = (searchHighlightRow >= 0) ? searchHighlightRow : m_pendingHighlightRow;

    for (int i = 0; i < mission.params.size(); ++i) {
        const ParsedParameter& param = mission.params[i];

        int spaceIdx = findNameSep(param.description);
        QString shortName = param.description.isEmpty() ? param.name
            : param.description.left(spaceIdx > 0 ? spaceIdx : param.description.length());
        QString detail = (spaceIdx > 0) ? param.description.mid(spaceIdx + 1) : QString();
        bool modified = param.modified();

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

        // 参数值 — 布尔参数用下拉框：Is 前缀，或值本身为 true/false
        bool isBool = param.name.startsWith(QStringLiteral("Is"), Qt::CaseInsensitive)
            || param.value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
            || param.value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0;
        if (isBool) {
            QComboBox* combo = new QComboBox();
            combo->addItems({QStringLiteral("true"), QStringLiteral("false")});
            combo->setCurrentIndex(
                param.value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ? 0 : 1);
            combo->setFont(table->font());
            QColor comboBg = (i == hlRow) ? QColor("#29B6F6")
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
                bool mod = p.modified();
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
                        (*it)->setText(0, paramLeafText(p));
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

        // 参数说明 — 有 tip 的行优先显示 tip
        QString descText = param.tip.isEmpty() ? detail : param.tip;
        QString descToolTip = param.tip.isEmpty() ? detail
            : (param.tip + (param.description.isEmpty()
                ? QString() : QStringLiteral("\n(") + param.description + QStringLiteral(")")));
        QTableWidgetItem* descItem = new QTableWidgetItem(descText);
        descItem->setToolTip(descToolTip);
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
        if (i == hlRow)
            rowBg = QColor("#29B6F6");          // 搜索命中/点击叶子定位 → 蓝色
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
    // 记下这一轮的高亮行，供"点右侧别的行时把旧的蓝色关掉"使用
    m_highlightedRow = (scrollRow >= 0 && scrollRow < table->rowCount()) ? scrollRow : -1;
    m_pendingHighlightRow = -1;
}

void ParamConfigDialog::populatePathTable(const QVector<ParsedParameter>& paths, const QString& typeLabel)
{
    m_updatingTable = true;
    m_showingPaths = true;
    m_showingLinks = false;
    QTableWidget* table = ui.tableWidget_params;
    setupParamsTableColumns(table);
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
        bool modified = pp.modified();

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

        QString descText = pp.tip.isEmpty() ? pp.description : pp.tip;
        QString descToolTip = pp.tip.isEmpty() ? pp.description
            : (pp.tip + (pp.description.isEmpty()
                ? QString() : QStringLiteral("\n(") + pp.description + QStringLiteral(")")));
        QTableWidgetItem* descItem = new QTableWidgetItem(descText);
        descItem->setToolTip(descToolTip);
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

    // 定位到点击的路径叶子行，随后立即清零，避免残留高亮泄漏到下一次表格显示
    if (m_pendingHighlightRow >= 0 && m_pendingHighlightRow < table->rowCount())
        table->scrollToItem(table->item(m_pendingHighlightRow, 0), QAbstractItemView::EnsureVisible);
    m_highlightedRow = (m_pendingHighlightRow >= 0 && m_pendingHighlightRow < table->rowCount())
        ? m_pendingHighlightRow : -1;
    m_pendingHighlightRow = -1;

    int modCount = 0;
    for (const auto& p : paths)
        if (p.modified()) ++modCount;
    updateStatus(QStringLiteral("%1路径 — 共 %2 条%3")
        .arg(typeLabel).arg(paths.size())
        .arg(modCount > 0 ? QStringLiteral("，已修改 %1 条").arg(modCount) : QString()));
}

void ParamConfigDialog::populateLinkTable(const ParsedFileData& fd, int searchHighlightRow)
{
    m_updatingTable = true;
    m_showingLinks = true;
    m_showingPaths = false;
    QTableWidget* table = ui.tableWidget_params;
    setupParamsTableColumns(table);
    table->horizontalHeader()->setVisible(true);
    table->verticalHeader()->setVisible(false);
    table->clearContents();
    table->setRowCount(0);

    if (fd.links.isEmpty()) {
        m_updatingTable = false;
        updateStatus(QStringLiteral("此文件没有链接"));
        return;
    }

    table->setRowCount(fd.links.size());

    for (int i = 0; i < fd.links.size(); ++i) {
        const ParsedLink& lk = fd.links[i];
        bool modified = lk.modified();

        QTableWidgetItem* seqItem = new QTableWidgetItem(QString::number(i + 1));
        seqItem->setFlags(seqItem->flags() & ~Qt::ItemIsEditable);
        seqItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(i, 0, seqItem);

        QTableWidgetItem* idItem = new QTableWidgetItem(QStringLiteral("Link"));
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(i, 1, idItem);

        QTableWidgetItem* nameItem = new QTableWidgetItem(QStringLiteral("引用文件"));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(i, 2, nameItem);

        QTableWidgetItem* valItem = new QTableWidgetItem(lk.path);
        table->setItem(i, 3, valItem);

        // 启用/停用下拉（run 属性）
        QComboBox* runCombo = new QComboBox();
        runCombo->addItems({QStringLiteral("启用"), QStringLiteral("停用")});
        runCombo->setCurrentIndex(lk.run ? 0 : 1);
        runCombo->setFont(table->font());
        QColor runBg = (i == searchHighlightRow || i == m_pendingHighlightRow) ? QColor("#29B6F6")
            : modified ? QColor("#fff3cd")
            : (i % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5");
        runCombo->setStyleSheet(
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
            .arg(runBg.name()));
        int fIdx = m_currentFileIndex;
        connect(runCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, i, fIdx, runCombo](int idx) {
            if (m_updatingTable) return;
            if (fIdx < 0 || fIdx >= m_files.size()) return;
            ParsedFileData& cfd = m_files[fIdx];
            if (!cfd.isLinkFile || i >= cfd.links.size()) return;
            ParsedLink& pl = cfd.links[i];
            pl.run = (idx == 0);
            bool mod = pl.modified();
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
            runCombo->setStyleSheet(
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
                if ((*it)->data(0, Qt::UserRole + 1).toString() == "link" &&
                    (*it)->data(0, Qt::UserRole).toInt() == fIdx &&
                    (*it)->data(1, Qt::UserRole).toInt() == i) {
                    (*it)->setText(0, linkLeafText(pl));
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
        table->setCellWidget(i, 4, runCombo);

        // 状态（只读）
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
            lk.favorited ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
        favItem->setFlags(favItem->flags() & ~Qt::ItemIsEditable);
        favItem->setTextAlignment(Qt::AlignCenter);
        favItem->setData(Qt::UserRole, lk.favorited ? 1 : 0);
        favItem->setToolTip(lk.favorited
            ? QStringLiteral("点击取消收藏") : QStringLiteral("点击添加收藏"));
        table->setItem(i, 6, favItem);

        // 整行统一背景色（必须在所有 7 列 setItem 之后）
        QColor rowBg;
        if (i == searchHighlightRow || i == m_pendingHighlightRow)
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

    // 定位到点击的链接叶子行，随后立即清零，避免残留高亮泄漏
    int scrollRow = (searchHighlightRow >= 0) ? searchHighlightRow : m_pendingHighlightRow;
    if (scrollRow >= 0 && scrollRow < table->rowCount())
        table->scrollToItem(table->item(scrollRow, 0), QAbstractItemView::EnsureVisible);
    m_highlightedRow = (scrollRow >= 0 && scrollRow < table->rowCount()) ? scrollRow : -1;
    m_pendingHighlightRow = -1;

    int modCount = 0;
    for (const auto& l : fd.links)
        if (l.modified()) ++modCount;
    updateStatus(QStringLiteral("批量处理链接 — 共 %1 条%2%3")
        .arg(fd.links.size())
        .arg(modCount > 0 ? QStringLiteral("，已修改 %1 条").arg(modCount) : QString())
        .arg(fd.relativePath.isEmpty() ? QString()
            : QStringLiteral("  [relativePath: %1]").arg(fd.relativePath)));
}

void ParamConfigDialog::populateRawView(int fileIdx, const QString& elemPath)
{
    m_updatingTable = true;
    m_showingPaths = false;
    m_showingLinks = false;
    m_highlightedRow = -1;          // 原始视图（2列）没有高亮行概念
    QTableWidget* table = ui.tableWidget_params;
    setupRawTableColumns(table);
    table->horizontalHeader()->setVisible(true);
    table->verticalHeader()->setVisible(false);
    table->clearContents();
    table->setRowCount(0);

    if (fileIdx < 0 || fileIdx >= m_files.size()) {
        m_updatingTable = false;
        clearTable();
        return;
    }
    const ParsedFileData& fd = m_files[fileIdx];

    QDomElement elem = elementAtPath(fd.xmlDoc.documentElement(), elemPath);
    if (elem.isNull())
        elem = fd.xmlDoc.documentElement();
    if (elem.isNull()) {
        m_updatingTable = false;
        clearTable();
        return;
    }

    int row = 0;
    table->setRowCount(1);
    auto addRow = [&](const QString& key, const QString& value) {
        table->setRowCount(row + 1);
        QTableWidgetItem* k = new QTableWidgetItem(key);
        k->setFlags(k->flags() & ~Qt::ItemIsEditable);
        k->setForeground(QColor("#2c3e50"));
        QFont kf = k->font();
        kf.setBold(true);
        k->setFont(kf);
        table->setItem(row, 0, k);
        QTableWidgetItem* v = new QTableWidgetItem(value);
        v->setFlags(v->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 1, v);
        ++row;
    };

    addRow(QStringLiteral("元素"), elem.tagName());
    QDomNamedNodeMap attrs = elem.attributes();
    for (int a = 0; a < attrs.count(); ++a) {
        QDomAttr attr = attrs.item(a).toAttr();
        if (!attr.isNull())
            addRow(QStringLiteral("@%1").arg(attr.name()), attr.value());
    }
    QString directText;
    for (QDomNode n = elem.firstChild(); !n.isNull(); n = n.nextSibling()) {
        if (n.isText())
            directText += n.nodeValue().trimmed();
    }
    if (!directText.isEmpty())
        addRow(QStringLiteral("文本内容"), directText);

    m_updatingTable = false;
    table->clearSpans();
    table->scrollToTop();
    updateStatus(QStringLiteral("只读查看: %1（结构未识别，不可编辑）")
                 .arg(QFileInfo(fd.filePath).fileName()));
}

void ParamConfigDialog::applyTableFilter()
{
    QTableWidget* table = ui.tableWidget_params;
    if (table->columnCount() != 7) return; // 只读原始视图（2列）不参与筛选
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


// 【2026-09-11】右侧表格行 -> 左侧导航同步选中对应的叶子节点。
// 同一张表格控件按视图显示四种内容，各自对应一类树叶子：
//     m_showingLinks                      -> 🔗 链接叶子
//     m_showingPaths + m_showingInputPaths -> 📄 输入路径叶子
//     m_showingPaths + !m_showingInputPaths-> 📄 输出路径叶子
//     其余                                 -> ⚙ 参数叶子
// 四者的行号与该向量的下标一一对应（表格与树都按同一个向量顺序生成），
// 所以直接按行号找节点即可，不需要额外的映射表。
void ParamConfigDialog::syncTreeToTableRow(int row)
{
    if (row < 0) return;
    if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;

    QTreeWidget* tree = ui.treeWidget_checkItems;

    // 先按当前视图确定"这一行对应哪类叶子"，并取该视图的总行数
    QString leafType;
    int leafCount = 0;
    if (m_showingLinks) {
        const auto& fd = m_files[m_currentFileIndex];
        if (!fd.isLinkFile) return;
        leafType = QStringLiteral("link");
        leafCount = fd.links.size();
    } else {
        if (m_currentMbIndex < 0 ||
            m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
        const auto& mb = m_files[m_currentFileIndex].blocks[m_currentMbIndex];
        if (m_currentMissionIndex < 0 ||
            m_currentMissionIndex >= mb.missions.size()) return;
        const auto& ms = mb.missions[m_currentMissionIndex];
        if (m_showingPaths) {
            leafType = m_showingInputPaths ? QStringLiteral("inpath") : QStringLiteral("outpath");
            leafCount = m_showingInputPaths ? ms.inputPaths.size() : ms.outputPaths.size();
        } else {
            leafType = QStringLiteral("param");
            leafCount = ms.params.size();
        }
    }
    if (row >= leafCount) return;

    QTreeWidgetItemIterator it(tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        if (item->data(0, Qt::UserRole + 1).toString() == leafType &&
            item->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
            item->data(3, Qt::UserRole).toInt() == row) {
            // 链接叶子只带文件下标；其余三类叶子还要比对任务块/Mission。
            // 注意链接叶子的 data(1) 存的是链接下标而非任务块下标，不能参与比对。
            const bool bLeafMatch = (leafType == QStringLiteral("link"))
                || (item->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    item->data(2, Qt::UserRole).toInt() == m_currentMissionIndex);
            if (bLeafMatch) {
                QTreeWidgetItem* p = item->parent();
                while (p) { p->setExpanded(true); p = p->parent(); }
                // 必须加守卫：否则 setCurrentItem 会触发 onBlockSelectionChanged，
                // 而该回调对叶子会把选中框弹回父节点 —— 等于选中不到叶子
                m_selectingTreeItem = true;
                tree->setCurrentItem(item);
                m_selectingTreeItem = false;
                tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
                return;
            }
        }
        ++it;
    }

    // 树上找不到对应叶子（"常用参数"视图下未收藏的条目不建节点，但搜索时
    // 表格不按收藏过滤、仍会显示该行）-> 退化为选中上一级节点
    if (leafType == QStringLiteral("link")) {
        QTreeWidgetItemIterator lit(tree);
        while (*lit) {
            if ((*lit)->data(0, Qt::UserRole + 1).toString() == QStringLiteral("linklist") &&
                (*lit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex) {
                QTreeWidgetItem* p = (*lit)->parent();
                while (p) { p->setExpanded(true); p = p->parent(); }
                m_selectingTreeItem = true;
                tree->setCurrentItem(*lit);
                m_selectingTreeItem = false;
                tree->scrollToItem(*lit, QAbstractItemView::PositionAtCenter);
                break;
            }
            ++lit;
        }
    } else {
        selectTreeMission(m_currentFileIndex, m_currentMbIndex, m_currentMissionIndex);
    }
}

// 【2026-09-11】按"是否高亮 + 是否已修改"重绘整行底色。
// 蓝(#29B6F6)=当前高亮行，黄(#fff3cd)=已修改，白/浅灰=普通行。
// 布尔参数的"参数值"是 QComboBox 单元格控件，它的底色由样式表控制，
// 不跟着 item 的 background 走，必须单独设一次，否则那一格颜色会跟同行的其他格不一致。
void ParamConfigDialog::repaintRowBackground(int row, bool highlighted)
{
    QTableWidget* table = ui.tableWidget_params;
    if (table->columnCount() != 7) return;      // 只读原始视图（2列）不参与
    if (row < 0 || row >= table->rowCount()) return;

    QTableWidgetItem* statusItem = table->item(row, 5);
    const bool modified = statusItem && statusItem->text() == QStringLiteral("已修改");
    const QColor bg = highlighted ? QColor("#29B6F6")
        : modified ? QColor("#fff3cd")
        : ((row % 2 == 0) ? QColor(Qt::white) : QColor("#f5f5f5"));

    const bool bOldUpdating = m_updatingTable;
    m_updatingTable = true;
    for (int c = 0; c < 7; ++c) {
        QTableWidgetItem* cell = table->item(row, c);
        if (cell) cell->setBackground(bg);
        // 该格是下拉框（参数表的布尔值是第 3 列、链接表的 run 是第 4 列）时，
        // 它的底色由样式表控制，不跟着 item 的 background 走，需一并改
        if (QComboBox* combo = qobject_cast<QComboBox*>(table->cellWidget(row, c))) {
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
                .arg(bg.name()));
        }
    }
    m_updatingTable = bOldUpdating;
}

void ParamConfigDialog::onTableCellClicked(int row, int col)
{
    if (col != 6) {
        // 【2026-09-11】点右侧表格任一行（收藏列以外的任意列）时：
        //  ① 左侧导航同步选中对应的叶子节点（参数/输入路径/输出路径/链接都走这里）；
        //  ② 把上一行残留的蓝色高亮关掉，只让当前这一行是蓝色的。
        //     左侧点叶子时右侧那行的蓝色是 populateTable 刷的一次性底色，不会自己消失，
        //     不主动清掉的话表格里会同时出现两行"被选中"。
        syncTreeToTableRow(row);
        // 只有表格确实在显示可点选内容时才动高亮色：
        // 清空态（"请点击左侧 Mission 节点"提示行）与只读原始视图下 currentFile/Mb 为 -1，不动
        const bool bHasRows = (m_currentFileIndex >= 0)
            && (m_showingLinks || m_currentMbIndex >= 0);
        if (bHasRows && row != m_highlightedRow) {
            const int nPrevRow = m_highlightedRow;
            m_highlightedRow = row;
            if (nPrevRow >= 0) repaintRowBackground(nPrevRow, false);
            repaintRowBackground(row, true);
        }
        return;
    }

    QTableWidget* table = ui.tableWidget_params;

    // 收藏切换后同步行级"已修改"状态与底色（与 onTableCellChanged 同款样式）
    auto updateFavRowStatus = [&](int r, bool mod) {
        m_updatingTable = true;
        QTableWidgetItem* si = table->item(r, 5);
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
        m_updatingTable = false;
        // 底色统一交给 repaintRowBackground：它认识"当前高亮行"，
        // 不会把蓝色误刷成黄/白（收藏切换的行若正好是高亮行要保住蓝色）
        repaintRowBackground(r, r == m_highlightedRow);
    };

    if (m_showingLinks) {
        if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
        ParsedFileData& fd = m_files[m_currentFileIndex];
        if (!fd.isLinkFile || row < 0 || row >= fd.links.size()) return;
        ParsedLink& lk = fd.links[row];
        lk.favorited = !lk.favorited;
        lk.domElement.setAttribute(QStringLiteral("favorite"),
            lk.favorited ? QStringLiteral("true") : QStringLiteral("false"));

        m_updatingTable = true;
        QTableWidgetItem* fi = table->item(row, 6);
        if (fi) {
            fi->setText(lk.favorited
                ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
            fi->setData(Qt::UserRole, lk.favorited ? 1 : 0);
            fi->setToolTip(lk.favorited
                ? QStringLiteral("点击取消收藏") : QStringLiteral("点击添加收藏"));
        }
        m_updatingTable = false;
        updateFavRowStatus(row, lk.modified());

        // 同步左侧树节点文本与加粗红色样式
        QTreeWidgetItemIterator lit(ui.treeWidget_checkItems);
        while (*lit) {
            if ((*lit)->data(0, Qt::UserRole + 1).toString() == QStringLiteral("link")
                && (*lit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex
                && (*lit)->data(1, Qt::UserRole).toInt() == row) {
                (*lit)->setText(0, linkLeafText(lk));
                QFont lf = (*lit)->font(0);
                lf.setFamily(QStringLiteral("Segoe UI Emoji"));
                lf.setBold(lk.modified());
                (*lit)->setFont(0, lf);
                (*lit)->setForeground(0, lk.modified()
                    ? QColor("#c0392b") : QColor(Qt::black));
                break;
            }
            ++lit;
        }

        updateWindowTitle();
        applyTableFilter();
        // 常用参数视图下收藏切换后重建树，保证左右两侧过滤口径一致
        if (m_showingFavoritesOnly)
            refreshParamTree();
        return;
    }

    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];

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
        updateFavRowStatus(row, pp.modified());

        // 同步左侧树叶子加粗红色样式
        QString treeType = m_showingInputPaths ? QStringLiteral("inpath") : QStringLiteral("outpath");
        QTreeWidgetItemIterator pit(ui.treeWidget_checkItems);
        while (*pit) {
            if ((*pit)->data(0, Qt::UserRole + 1).toString() == treeType &&
                (*pit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                (*pit)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                (*pit)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                (*pit)->data(3, Qt::UserRole).toInt() == row) {
                (*pit)->setText(0, pathLeafText(pp));
                if (pp.modified()) {
                    QFont pf = (*pit)->font(0); pf.setBold(true); (*pit)->setFont(0, pf);
                    (*pit)->setForeground(0, QColor("#c0392b"));
                } else {
                    QFont pf = (*pit)->font(0); pf.setBold(false); (*pit)->setFont(0, pf);
                    (*pit)->setForeground(0, QColor(Qt::black));
                }
                break;
            }
            ++pit;
        }
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
        updateFavRowStatus(row, param.modified());

        // 同步左侧树叶子加粗红色样式
        QTreeWidgetItemIterator pit(ui.treeWidget_checkItems);
        while (*pit) {
            if ((*pit)->data(0, Qt::UserRole + 1).toString() == QStringLiteral("param") &&
                (*pit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                (*pit)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                (*pit)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                (*pit)->data(3, Qt::UserRole).toInt() == row) {
                (*pit)->setText(0, paramLeafText(param));
                if (param.modified()) {
                    QFont pf = (*pit)->font(0); pf.setBold(true); (*pit)->setFont(0, pf);
                    (*pit)->setForeground(0, QColor("#c0392b"));
                } else {
                    QFont pf = (*pit)->font(0); pf.setBold(false); (*pit)->setFont(0, pf);
                    (*pit)->setForeground(0, QColor(Qt::black));
                }
                break;
            }
            ++pit;
        }
    }

    updateWindowTitle();
    applyTableFilter();
    // 常用参数视图下收藏切换后重建树，保证左右两侧过滤口径一致
    if (m_showingFavoritesOnly)
        refreshParamTree();
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

    if (m_showingLinks) {
        if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
        ParsedFileData& fd = m_files[m_currentFileIndex];
        if (!fd.isLinkFile || row < 0 || row >= fd.links.size()) return;
        if (col == 3 && table->item(row, col)) {
            fd.links[row].path = table->item(row, col)->text().trimmed();
            bool modified = fd.links[row].modified();
            updateRowStatus(row, modified);
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == "link" &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == row) {
                    (*it)->setText(0, linkLeafText(fd.links[row]));
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

    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];

    if (m_showingPaths) {
        QVector<ParsedParameter>& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;
        if (row < 0 || row >= pathVec.size()) return;
        ParsedParameter& pp = pathVec[row];

        if (col == 1 && table->item(row, col))
            pp.name = table->item(row, col)->text().remove(QChar(0x200B)).trimmed();

        if (col == 3 && table->item(row, col)) {
            pp.value = table->item(row, col)->text().trimmed();
            bool modified = pp.modified();
            updateRowStatus(row, modified);
            QString treeType = m_showingInputPaths ? "inpath" : "outpath";
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == treeType &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    (*it)->setText(0, pathLeafText(pp));
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
            if (pp.tip.isEmpty())
                pp.description = table->item(row, col)->text().trimmed();
            else
                pp.tip = table->item(row, col)->text().trimmed();
            bool modified = pp.modified();
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
            bool modified = param.modified();
            updateRowStatus(row, modified);
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    (*it)->setText(0, paramLeafText(param));
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
        bool modified = param.modified();
        updateRowStatus(row, modified);
        QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                (*it)->data(3, Qt::UserRole).toInt() == row) {
                (*it)->setText(0, paramLeafText(param));
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
            if (param.tip.isEmpty()) {
                int spaceIdx = findNameSep(param.description);
                QString shortPart = spaceIdx > 0
                    ? param.description.left(spaceIdx)
                    : param.description;
                param.description = newDetail.isEmpty() ? shortPart
                    : shortPart + " " + newDetail;
            } else {
                param.tip = newDetail;
            }
            bool modified = param.modified();
            updateRowStatus(row, modified);
            QTreeWidgetItemIterator it(ui.treeWidget_checkItems);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == "param" &&
                    (*it)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                    (*it)->data(1, Qt::UserRole).toInt() == m_currentMbIndex &&
                    (*it)->data(2, Qt::UserRole).toInt() == m_currentMissionIndex &&
                    (*it)->data(3, Qt::UserRole).toInt() == row) {
                    (*it)->setText(0, paramLeafText(param));
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
    Q_UNUSED(text);
    // 防抖：每次输入仅清空旧结果并重启 150ms 定时器，超时后才全量搜索
    m_searchResults.clear();
    m_currentSearchIndex = -1;
    if (m_searchInfoBar) m_searchInfoBar->setVisible(false);
    if (m_searchTimer) m_searchTimer->start();
}

void ParamConfigDialog::doSearch(const QString& kw)
{
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
        // 没有 Mission 时回退到第一个链接文件
        for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
            if (m_files[fIdx].isLinkFile && !m_files[fIdx].links.isEmpty()) {
                m_currentFileIndex = fIdx;
                m_currentMbIndex = -1;
                m_currentMissionIndex = -1;
                populateLinkTable(m_files[fIdx]);
                updateStatus(QStringLiteral("就绪"));
                return;
            }
        }
        // 再回退到第一个结构未识别文件（只读查看）
        for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
            if (m_files[fIdx].isUnknown) {
                m_currentFileIndex = fIdx;
                m_currentMbIndex = -1;
                m_currentMissionIndex = -1;
                populateRawView(fIdx, QString());
                updateStatus(QStringLiteral("就绪"));
                return;
            }
        }
        clearTable();
        updateStatus(QStringLiteral("就绪"));
        return;
    }

    // 收集全部结果（全局搜索：不受"常用参数"过滤限制，
    // 与 applyTableFilter 搜索激活时全量显示的口径一致）
    // 1) 搜任务 ID
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        for (int mbIdx = 0; mbIdx < m_files[fIdx].blocks.size(); ++mbIdx) {
            for (int mIdx = 0; mIdx < m_files[fIdx].blocks[mbIdx].missions.size(); ++mIdx) {
                const auto& mission = m_files[fIdx].blocks[mbIdx].missions[mIdx];
                if (!mission.id.isEmpty() && mission.id.contains(kw, Qt::CaseInsensitive)) {
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
                    if (param.name.contains(kw, Qt::CaseInsensitive) ||
                        paramName.contains(kw, Qt::CaseInsensitive)) {
                        m_searchResults.append({fIdx, mbIdx, mIdx, pIdx, 2});
                    }
                }
            }
        }
    }

    // 3) 搜链接文件路径
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        const auto& fd = m_files[fIdx];
        if (!fd.isLinkFile) continue;
        for (int lIdx = 0; lIdx < fd.links.size(); ++lIdx) {
            if (fd.links[lIdx].path.contains(kw, Qt::CaseInsensitive))
                m_searchResults.append({fIdx, -1, -1, lIdx, 4});
        }
    }

    // 4) 搜树节点文本
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
        if (m_files[r.fileIdx].isLinkFile && r.mbIdx < 0 && r.mIdx < 0 && r.pIdx >= 0) {
            m_currentFileIndex = r.fileIdx;
            m_currentMbIndex = -1;
            m_currentMissionIndex = -1;
            populateLinkTable(m_files[r.fileIdx], r.pIdx);
            QTreeWidgetItemIterator lit(ui.treeWidget_checkItems);
            while (*lit) {
                if ((*lit)->data(0, Qt::UserRole + 1).toString() == "linklist" &&
                    (*lit)->data(0, Qt::UserRole).toInt() == r.fileIdx) {
                    QTreeWidgetItem* p = (*lit)->parent();
                    while (p) { p->setExpanded(true); p = p->parent(); }
                    m_selectingTreeItem = true;
                    ui.treeWidget_checkItems->setCurrentItem(*lit);
                    m_selectingTreeItem = false;
                    ui.treeWidget_checkItems->scrollToItem(*lit, QAbstractItemView::PositionAtCenter);
                    break;
                }
                ++lit;
            }
        } else if (r.mbIdx >= 0 && r.mIdx >= 0 &&
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
    QTableWidget* table = ui.tableWidget_params;

    if (m_showingLinks) {
        if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
        ParsedFileData& fd = m_files[m_currentFileIndex];
        if (!fd.isLinkFile) return;
        for (int row = 0; row < fd.links.size() && row < table->rowCount(); ++row)
            if (QTableWidgetItem* item = table->item(row, 3))
                fd.links[row].path = item->text().trimmed();
        return;
    }

    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];

    if (m_showingPaths) {
        QVector<ParsedParameter>& pathVec = m_showingInputPaths
            ? mission.inputPaths : mission.outputPaths;

        for (int row = 0; row < pathVec.size() && row < table->rowCount(); ++row) {
            ParsedParameter& pp = pathVec[row];
            if (QTableWidgetItem* item = table->item(row, 1))
                pp.name = item->text().remove(QChar(0x200B)).trimmed();
            if (QTableWidgetItem* item = table->item(row, 3))
                pp.value = item->text().trimmed();
            if (QTableWidgetItem* item = table->item(row, 4)) {
                if (pp.tip.isEmpty())
                    pp.description = item->text().trimmed();
                else
                    pp.tip = item->text().trimmed();
            }
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

    int savedCount = 0;
    int unknownCount = 0;

    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        auto& fd = m_files[fIdx];

        if (fd.isUnknown) {
            ++unknownCount;
            continue;
        }

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
                    if (param.favorited || param.originalFavorited)
                        newElem.setAttribute("favorite", param.favorited ? "true" : "false");
                    if (param.tip.isEmpty())
                        newElem.removeAttribute("tip");
                    else
                        newElem.setAttribute("tip", param.tip);
                    QDomText textNode = doc.createTextNode(param.value);
                    newElem.appendChild(textNode);
                    parent.replaceChild(newElem, param.domElement);
                    param.domElement = newElem;
                    return;
                }
            }
            param.domElement.setAttribute("note", param.description);
            if (param.favorited || param.originalFavorited)
                param.domElement.setAttribute("favorite", param.favorited ? "true" : "false");
            else
                param.domElement.removeAttribute("favorite");
            if (param.tip.isEmpty())
                param.domElement.removeAttribute("tip");
            else
                param.domElement.setAttribute("tip", param.tip);
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
        if (fd.isLinkFile) {
            for (auto& lk : fd.links) {
                if (lk.domElement.isNull()) continue;
                lk.domElement.setAttribute("run", lk.run ? "true" : "false");
                lk.domElement.setAttribute("favorite", lk.favorited ? "true" : "false");
                while (lk.domElement.hasChildNodes())
                    lk.domElement.removeChild(lk.domElement.firstChild());
                lk.domElement.appendChild(fd.xmlDoc.createTextNode(lk.path));
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
        ++savedCount;

        for (auto& mb : fd.blocks)
            for (auto& mission : mb.missions) {
                for (auto& param : mission.params) {
                    param.originalName = param.name;
                    param.originalValue = param.value;
                    param.originalDescription = param.description;
                    param.originalTip = param.tip;
                    param.originalFavorited = param.favorited;
                }
                for (auto& pp : mission.inputPaths) {
                    pp.originalName = pp.name;
                    pp.originalValue = pp.value;
                    pp.originalDescription = pp.description;
                    pp.originalTip = pp.tip;
                    pp.originalFavorited = pp.favorited;
                }
                for (auto& pp : mission.outputPaths) {
                    pp.originalName = pp.name;
                    pp.originalValue = pp.value;
                    pp.originalDescription = pp.description;
                    pp.originalTip = pp.tip;
                    pp.originalFavorited = pp.favorited;
                }
            }
        if (fd.isLinkFile) {
            for (auto& lk : fd.links) {
                lk.originalPath = lk.path;
                lk.originalRun = lk.run;
                lk.originalFavorited = lk.favorited;
            }
        }
    }

    m_pendingHighlightRow = -1;
    refreshParamTree();

    QString msg = QStringLiteral("已保存 %1 个文件").arg(savedCount);
    if (unknownCount > 0)
        msg += QStringLiteral("（跳过 %1 个结构未识别的只读文件）").arg(unknownCount);
    updateStatus(msg);
    updateWindowTitle();
    QMessageBox::information(this, QStringLiteral("保存成功"), msg);
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

    if (fd.isUnknown) {
        QMessageBox::information(this,
            QStringLiteral("另存为"),
            QStringLiteral("该文件结构未识别，为只读查看，没有可编辑内容"));
        return;
    }

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
                if (param.favorited || param.originalFavorited)
                    newElem.setAttribute("favorite", param.favorited ? "true" : "false");
                if (param.tip.isEmpty())
                    newElem.removeAttribute("tip");
                else
                    newElem.setAttribute("tip", param.tip);
                QDomText textNode = doc.createTextNode(param.value);
                newElem.appendChild(textNode);
                parent.replaceChild(newElem, param.domElement);
                param.domElement = newElem;
                return;
            }
        }
        param.domElement.setAttribute("note", param.description);
        if (param.favorited || param.originalFavorited)
            param.domElement.setAttribute("favorite", param.favorited ? "true" : "false");
        else
            param.domElement.removeAttribute("favorite");
        if (param.tip.isEmpty())
            param.domElement.removeAttribute("tip");
        else
            param.domElement.setAttribute("tip", param.tip);
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
    if (fd.isLinkFile) {
        for (auto& lk : fd.links) {
            if (lk.domElement.isNull()) continue;
            lk.domElement.setAttribute("run", lk.run ? "true" : "false");
            lk.domElement.setAttribute("favorite", lk.favorited ? "true" : "false");
            while (lk.domElement.hasChildNodes())
                lk.domElement.removeChild(lk.domElement.firstChild());
            lk.domElement.appendChild(fd.xmlDoc.createTextNode(lk.path));
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
                param.originalName = param.name;
                param.originalValue = param.value;
                param.originalDescription = param.description;
                param.originalTip = param.tip;
                param.originalFavorited = param.favorited;
            }
            for (auto& pp : mission.inputPaths) {
                pp.originalName = pp.name;
                pp.originalValue = pp.value;
                pp.originalDescription = pp.description;
                pp.originalTip = pp.tip;
                pp.originalFavorited = pp.favorited;
            }
            for (auto& pp : mission.outputPaths) {
                pp.originalName = pp.name;
                pp.originalValue = pp.value;
                pp.originalDescription = pp.description;
                pp.originalTip = pp.tip;
                pp.originalFavorited = pp.favorited;
            }
        }
    if (fd.isLinkFile) {
        for (auto& lk : fd.links) {
            lk.originalPath = lk.path;
            lk.originalRun = lk.run;
            lk.originalFavorited = lk.favorited;
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

    collectCurrentMissionValues();
    if (hasAnyModification()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("未保存的修改"));
        msgBox.setText(QStringLiteral("重新加载将丢弃所有修改，确定继续吗？"));
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        if (msgBox.exec() != QMessageBox::Yes) return;
    }

    ui.lineEdit_search->clear();
    m_filterModifiedOnly = false;
    ui.checkBox_filterMarked->setChecked(false);

    // 重新解析所有文件
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
        ParsedFileData fd = parseOneFile(m_files[fIdx].filePath);
        if (!fd.blocks.isEmpty() || fd.isLinkFile || fd.isUnknown)
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
    for (const auto& fd : m_files) {
        for (const auto& mb : fd.blocks)
            for (const auto& ms : mb.missions) {
                for (const auto& p : ms.params)
                    if (p.modified() || p.favorited != p.originalFavorited)
                        return true;
                for (const auto& p : ms.inputPaths)
                    if (p.modified() || p.favorited != p.originalFavorited)
                        return true;
                for (const auto& p : ms.outputPaths)
                    if (p.modified() || p.favorited != p.originalFavorited)
                        return true;
            }
        for (const auto& l : fd.links)
            if (l.modified() || l.favorited != l.originalFavorited)
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
    if (m_showingLinks) {
        if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
        ParsedFileData& fd = m_files[m_currentFileIndex];
        int row = ui.tableWidget_params->currentRow();
        if (row < 0 || row >= fd.links.size()) return;
        ParsedLink& lk = fd.links[row];
        if (!lk.modified()) return;
        collectCurrentMissionValues();
        lk.path = lk.originalPath;
        lk.run = lk.originalRun;
        lk.favorited = lk.originalFavorited;
        if (lk.originalFavorited)
            lk.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
        else
            lk.domElement.removeAttribute(QStringLiteral("favorite"));
        populateLinkTable(fd);
        ui.tableWidget_params->selectRow(row);
        QTreeWidgetItemIterator tit(ui.treeWidget_checkItems);
        while (*tit) {
            if ((*tit)->data(0, Qt::UserRole + 1).toString() == "link" &&
                (*tit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex &&
                (*tit)->data(1, Qt::UserRole).toInt() == row) {
                (*tit)->setText(0, linkLeafText(lk));
                QFont pf = (*tit)->font(0); pf.setBold(false); (*tit)->setFont(0, pf);
                (*tit)->setForeground(0, QColor(Qt::black));
                break;
            }
            ++tit;
        }
        updateWindowTitle();
        updateStatus(QStringLiteral("已重置第 %1 行链接").arg(row + 1));
        return;
    }

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
        if (!pp.modified()) return;
        collectCurrentMissionValues();
        pp.name = pp.originalName;
        pp.value = pp.originalValue;
        pp.description = pp.originalDescription;
        pp.tip = pp.originalTip;
        pp.favorited = pp.originalFavorited;
        if (pp.originalFavorited)
            pp.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
        else
            pp.domElement.removeAttribute(QStringLiteral("favorite"));
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
                (*tit)->setText(0, pathLeafText(pp));
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
    if (!param.modified()) return;

    collectCurrentMissionValues();
    param.name = param.originalName;
    param.value = param.originalValue;
    param.description = param.originalDescription;
    param.tip = param.originalTip;
    param.favorited = param.originalFavorited;
    if (param.originalFavorited)
        param.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
    else
        param.domElement.removeAttribute(QStringLiteral("favorite"));

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
            (*it)->setText(0, paramLeafText(param));
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
    if (m_showingLinks) {
        if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
        ParsedFileData& fd = m_files[m_currentFileIndex];
        bool anyModified = false;
        for (const auto& l : fd.links)
            if (l.modified()) { anyModified = true; break; }
        if (!anyModified) {
            updateStatus(QStringLiteral("当前链接列表无修改，无需重置"));
            return;
        }

        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("重置链接列表"));
        msgBox.setText(QStringLiteral("确定要重置全部链接为初始值吗？\n此操作将丢弃所有未保存的修改。"));
        msgBox.setIcon(QMessageBox::Question);
        QPushButton* btnYes = msgBox.addButton(QStringLiteral("确定"), QMessageBox::YesRole);
        QPushButton* btnNo = msgBox.addButton(QStringLiteral("取消"), QMessageBox::NoRole);
        msgBox.setDefaultButton(btnNo);
        msgBox.exec();
        if (msgBox.clickedButton() != btnYes) return;

        collectCurrentMissionValues();
        for (auto& lk : fd.links) {
            lk.path = lk.originalPath;
            lk.run = lk.originalRun;
            lk.favorited = lk.originalFavorited;
            if (lk.originalFavorited)
                lk.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
            else
                lk.domElement.removeAttribute(QStringLiteral("favorite"));
        }
        populateLinkTable(fd);
        QTreeWidgetItemIterator tit(ui.treeWidget_checkItems);
        while (*tit) {
            QString type = (*tit)->data(0, Qt::UserRole + 1).toString();
            if (type == "link" &&
                (*tit)->data(0, Qt::UserRole).toInt() == m_currentFileIndex) {
                int lIdx = (*tit)->data(1, Qt::UserRole).toInt();
                if (lIdx >= 0 && lIdx < fd.links.size())
                    (*tit)->setText(0, linkLeafText(fd.links[lIdx]));
                QFont pf = (*tit)->font(0);
                pf.setBold(false);
                (*tit)->setFont(0, pf);
                (*tit)->setForeground(0, QColor(Qt::black));
            }
            ++tit;
        }
        updateWindowTitle();
        updateStatus(QStringLiteral("已重置全部链接"));
        return;
    }

    if (m_currentFileIndex < 0 || m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    if (m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex >= m_files[m_currentFileIndex].blocks.size()) return;
    if (m_currentMissionIndex >= m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions.size()) return;

    ParsedMission& mission = m_files[m_currentFileIndex].blocks[m_currentMbIndex].missions[m_currentMissionIndex];

    bool anyModified = false;
    for (const auto& p : mission.params)
        if (p.modified()) { anyModified = true; break; }
    if (!anyModified)
        for (const auto& p : mission.inputPaths)
            if (p.modified()) { anyModified = true; break; }
    if (!anyModified)
        for (const auto& p : mission.outputPaths)
            if (p.modified()) { anyModified = true; break; }
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
        param.name = param.originalName;
        param.value = param.originalValue;
        param.description = param.originalDescription;
        param.tip = param.originalTip;
        param.favorited = param.originalFavorited;
        if (param.originalFavorited)
            param.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
        else
            param.domElement.removeAttribute(QStringLiteral("favorite"));
    }
    for (auto& pp : mission.inputPaths) {
        pp.name = pp.originalName;
        pp.value = pp.originalValue;
        pp.description = pp.originalDescription;
        pp.tip = pp.originalTip;
        pp.favorited = pp.originalFavorited;
        if (pp.originalFavorited)
            pp.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
        else
            pp.domElement.removeAttribute(QStringLiteral("favorite"));
    }
    for (auto& pp : mission.outputPaths) {
        pp.name = pp.originalName;
        pp.value = pp.originalValue;
        pp.description = pp.originalDescription;
        pp.tip = pp.originalTip;
        pp.favorited = pp.originalFavorited;
        if (pp.originalFavorited)
            pp.domElement.setAttribute(QStringLiteral("favorite"), QStringLiteral("true"));
        else
            pp.domElement.removeAttribute(QStringLiteral("favorite"));
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
                (*it)->setText(0, paramLeafText(param));
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
                if (p.modified() || p.favorited != p.originalFavorited)
                    return true;
            for (const auto& p : ms.inputPaths)
                if (p.modified() || p.favorited != p.originalFavorited)
                    return true;
            for (const auto& p : ms.outputPaths)
                if (p.modified() || p.favorited != p.originalFavorited)
                    return true;
        }
    for (const auto& l : fd.links)
        if (l.modified() || l.favorited != l.originalFavorited)
            return true;
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
    if (fileHasModification(fileIdx)) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("未保存的修改"));
        msgBox.setText(QStringLiteral("文件 [%1] 有未保存的修改，确定要移除吗？")
                       .arg(QFileInfo(m_files[fileIdx].filePath).fileName()));
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        if (msgBox.exec() != QMessageBox::Yes) return;
    }
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
    if (m_showingLinks) {
        updateStatus(QStringLiteral("链接列表不支持同步到其他文件"));
        return;
    }
    if (m_currentFileIndex >= 0 && m_currentFileIndex < m_files.size() &&
        m_files[m_currentFileIndex].isUnknown) {
        updateStatus(QStringLiteral("结构未识别的只读文件不支持同步"));
        return;
    }
    if (m_currentFileIndex < 0 || m_currentFileIndex >= m_files.size()) return;
    if (m_currentMbIndex < 0 || m_currentMissionIndex < 0) return;
    const auto& fd = m_files[m_currentFileIndex];
    if (m_currentMbIndex >= fd.blocks.size()) return;
    if (m_currentMissionIndex >= fd.blocks[m_currentMbIndex].missions.size()) return;

    // 收集当前文件所有已修改 Mission 的变更（仅 value/description/tip/name 变化，收藏变化不参与同步）
    collectCurrentMissionValues();
    struct ChangedItem {
        QString key;      // 同步身份键：参数 name:原名#序号；路径 note:机器键#序号 或 pos:位置
        QString display;  // 弹窗节标题展示文本
        QString name;     // 参数当前名（改名同步用）
        QString value;
        QString description;
        QString tip;
    };
    struct MissionChanges {
        int mbIdx; int mIdx; QString id; QString note;
        QString mkey;   // Mission 身份键：id + 同 id 出现序号（note 不参与匹配，见下）
        QVector<ChangedItem> params;
        QVector<ChangedItem> inputs;
        QVector<ChangedItem> outputs;
        int total() const { return params.size() + inputs.size() + outputs.size(); }
    };

    // 参数身份键：原名 + 同名出现序号（改名、同名参数均能稳定匹配）
    auto buildParamKeys = [](const QVector<ParsedParameter>& params, bool useOriginal) {
        QHash<QString, int> occ;
        QVector<QString> keys(params.size());
        for (int i = 0; i < params.size(); ++i) {
            const QString nm = useOriginal ? params[i].originalName : params[i].name;
            int n = occ.value(nm, 0);
            occ[nm] = n + 1;
            keys[i] = QStringLiteral("name:%1#%2").arg(nm).arg(n);
        }
        return keys;
    };
    // 路径身份键：note（机器键，解析时已含父 note 继承）优先；
    // 无 note 按同向量内无 note 路径的出现位置。同名 note 以 #序号 区分。
    auto buildPathKeys = [](const QVector<ParsedParameter>& paths, bool useOriginal) {
        QHash<QString, int> occ;
        int posNoNote = 0;
        QVector<QString> keys(paths.size());
        for (int i = 0; i < paths.size(); ++i) {
            const QString note = useOriginal ? paths[i].originalDescription : paths[i].description;
            if (!note.isEmpty()) {
                int n = occ.value(note, 0);
                occ[note] = n + 1;
                keys[i] = QStringLiteral("note:%1#%2").arg(note).arg(n);
            } else {
                keys[i] = QStringLiteral("pos:%1").arg(posNoNote++);
            }
        }
        return keys;
    };
    // 路径节标题：有值显示值；空值显示（空值）+ 机器键，便于辨认
    auto pathDisplay = [](const ParsedParameter& p) {
        if (!p.value.isEmpty()) return p.value;
        return p.description.isEmpty()
            ? QStringLiteral("(空值)")
            : QStringLiteral("(空值) %1").arg(p.description);
    };

    // Mission 身份键：id（功能类别码，同 id 可达几十个任务）+ 同 id 出现序号。
    // note 是任务名称文本、实测会被改名——参与匹配会导致失配甚至序号串位
    // （note 一改分组就变，后面的任务会错配到相邻任务）；
    // 故 note 不参与匹配，也不随同步传播（任务名是识别标签，不同文件
    // 同位置任务本可不同名，同步改名属"错同步"，2026-09-09 用户拍板移除）。
    auto buildMissionKeys = [](const ParsedFileData& file) {
        QVector<QVector<QString>> keys(file.blocks.size());
        QHash<QString, int> occ;
        for (int bIdx = 0; bIdx < file.blocks.size(); ++bIdx) {
            keys[bIdx].resize(file.blocks[bIdx].missions.size());
            for (int mIdx = 0; mIdx < file.blocks[bIdx].missions.size(); ++mIdx) {
                const QString& id = file.blocks[bIdx].missions[mIdx].id;
                int n = occ.value(id, 0);
                occ[id] = n + 1;
                keys[bIdx][mIdx] = id + QStringLiteral("#") + QString::number(n);
            }
        }
        return keys;
    };

    QVector<QVector<QString>> srcMissionKeys = buildMissionKeys(fd);
    QVector<MissionChanges> allChanges;
    int noIdCount = 0;
    for (int bIdx = 0; bIdx < fd.blocks.size(); ++bIdx) {
        for (int mIdx = 0; mIdx < fd.blocks[bIdx].missions.size(); ++mIdx) {
            const auto& m = fd.blocks[bIdx].missions[mIdx];
            MissionChanges mc;
            mc.mbIdx = bIdx;
            mc.mIdx = mIdx;
            mc.id = m.id;
            mc.note = m.note;
            mc.mkey = srcMissionKeys[bIdx][mIdx];
            QVector<QString> pKeys = buildParamKeys(m.params, true);
            for (int i = 0; i < m.params.size(); ++i) {
                const ParsedParameter& p = m.params[i];
                if (!p.contentModified()) continue;
                mc.params.append({ pKeys[i], p.name, p.name,
                                   p.value, p.description, p.tip });
            }
            QVector<QString> iKeys = buildPathKeys(m.inputPaths, true);
            for (int i = 0; i < m.inputPaths.size(); ++i) {
                const ParsedParameter& p = m.inputPaths[i];
                if (!p.contentModified()) continue;
                mc.inputs.append({ iKeys[i], pathDisplay(p), QString(),
                                   p.value, p.description, p.tip });
            }
            QVector<QString> oKeys = buildPathKeys(m.outputPaths, true);
            for (int i = 0; i < m.outputPaths.size(); ++i) {
                const ParsedParameter& p = m.outputPaths[i];
                if (!p.contentModified()) continue;
                mc.outputs.append({ oKeys[i], pathDisplay(p), QString(),
                                    p.value, p.description, p.tip });
            }
            if (mc.total() == 0) continue;
            if (mc.id.isEmpty()) { ++noIdCount; continue; }
            allChanges.append(mc);
        }
    }
    if (allChanges.isEmpty()) {
        updateStatus(noIdCount > 0
            ? QStringLiteral("有修改的 Mission 均无 ID，无法同步")
            : QStringLiteral("当前文件无修改，无需同步"));
        return;
    }

    // 多个 Mission 有修改时询问同步范围：全部已修改 / 仅当前 Mission
    QVector<MissionChanges> selected;
    bool onlyCurrent = (allChanges.size() == 1
        && allChanges[0].mbIdx == m_currentMbIndex
        && allChanges[0].mIdx == m_currentMissionIndex);
    if (onlyCurrent) {
        selected = allChanges;
    } else {
        bool curModified = false;
        for (const auto& mc : allChanges) {
            if (mc.mbIdx == m_currentMbIndex && mc.mIdx == m_currentMissionIndex) {
                curModified = true;
                break;
            }
        }
        QMessageBox scopeBox(this);
        scopeBox.setWindowTitle(QStringLiteral("同步到其他文件"));
        QString text = QStringLiteral("当前文件共有 %1 个 Mission 发生修改。").arg(allChanges.size());
        if (noIdCount > 0)
            text += QStringLiteral("\n另有 %1 个无 ID 的修改 Mission 无法同步。").arg(noIdCount);
        if (!curModified)
            text += QStringLiteral("\n注意：当前选中的 Mission 没有修改。");
        scopeBox.setText(text + QStringLiteral("\n\n请选择同步范围："));
        QPushButton* btnAll = scopeBox.addButton(QStringLiteral("全部已修改 Mission"), QMessageBox::AcceptRole);
        QPushButton* btnCur = nullptr;
        if (curModified)
            btnCur = scopeBox.addButton(QStringLiteral("仅当前 Mission"), QMessageBox::AcceptRole);
        scopeBox.addButton(QMessageBox::Cancel);
        scopeBox.setDefaultButton(btnAll);
        scopeBox.exec();
        QAbstractButton* clicked = scopeBox.clickedButton();
        if (clicked == btnAll) {
            selected = allChanges;
        } else if (btnCur && clicked == btnCur) {
            for (const auto& mc : allChanges) {
                if (mc.mbIdx == m_currentMbIndex && mc.mIdx == m_currentMissionIndex)
                    selected.append(mc);
            }
        } else {
            return;
        }
    }

    // 每个选中 Mission：在其他文件中按身份键（id+同 id 出现序号）找同一任务，记录匹配项
    struct SyncTarget {
        int fileIdx; int mbIdx; int mIdx; QString fileName;
        QSet<QString> matchedParams;   // 该目标中能匹配上的已修改参数键
        QSet<QString> matchedInputs;   // 该目标中能匹配上的已修改输入路径键
        QSet<QString> matchedOutputs;  // 该目标中能匹配上的已修改输出路径键
    };
    // 每个文件预计算 Mission 身份键（目标侧）
    QVector<QVector<QVector<QString>>> targetMissionKeys(m_files.size());
    for (int fIdx = 0; fIdx < m_files.size(); ++fIdx)
        targetMissionKeys[fIdx] = buildMissionKeys(m_files[fIdx]);

    QVector<QVector<SyncTarget>> targetsPerMission;
    int missionsWithTargets = 0;
    for (const auto& mc : selected) {
        QVector<SyncTarget> validTargets;
        for (int fIdx = 0; fIdx < m_files.size(); ++fIdx) {
            if (fIdx == m_currentFileIndex) continue;
            const auto& ofd = m_files[fIdx];
            for (int bIdx = 0; bIdx < ofd.blocks.size(); ++bIdx) {
                for (int mIdx = 0; mIdx < ofd.blocks[bIdx].missions.size(); ++mIdx) {
                    if (targetMissionKeys[fIdx][bIdx][mIdx] != mc.mkey) continue;

                    const ParsedMission& tm = ofd.blocks[bIdx].missions[mIdx];
                    SyncTarget st;
                    st.fileIdx = fIdx;
                    st.mbIdx = bIdx;
                    st.mIdx = mIdx;
                    st.fileName = QFileInfo(ofd.filePath).fileName();

                    QVector<QString> tParamKeys = buildParamKeys(tm.params, false);
                    QVector<QString> tInputKeys = buildPathKeys(tm.inputPaths, false);
                    QVector<QString> tOutputKeys = buildPathKeys(tm.outputPaths, false);
                    for (const auto& ci : mc.params)
                        if (tParamKeys.contains(ci.key)) st.matchedParams.insert(ci.key);
                    for (const auto& ci : mc.inputs)
                        if (tInputKeys.contains(ci.key)) st.matchedInputs.insert(ci.key);
                    for (const auto& ci : mc.outputs)
                        if (tOutputKeys.contains(ci.key)) st.matchedOutputs.insert(ci.key);

                    if (!st.matchedParams.isEmpty() || !st.matchedInputs.isEmpty()
                        || !st.matchedOutputs.isEmpty())
                        validTargets.append(st);
                }
            }
        }
        targetsPerMission.append(validTargets);
        if (!validTargets.isEmpty()) ++missionsWithTargets;
    }

    if (missionsWithTargets == 0) {
        QString msg;
        if (selected.size() == 1) {
            const MissionChanges& mc = selected[0];
            bool hasSameMission = false;
            for (int fIdx = 0; fIdx < m_files.size() && !hasSameMission; ++fIdx) {
                if (fIdx == m_currentFileIndex) continue;
                for (int bIdx = 0; bIdx < targetMissionKeys[fIdx].size() && !hasSameMission; ++bIdx) {
                    for (int mIdx = 0; mIdx < targetMissionKeys[fIdx][bIdx].size(); ++mIdx) {
                        if (targetMissionKeys[fIdx][bIdx][mIdx] == mc.mkey) {
                            hasSameMission = true; break;
                        }
                    }
                }
            }
            msg = hasSameMission
                ? QStringLiteral("任务 [%1（%2）] 有 %3 项修改，\n"
                    "但其他文件的相同任务中不存在对应项，无法同步。")
                    .arg(mc.id).arg(mc.note).arg(mc.total())
                : QStringLiteral("没有其他文件包含相同任务 [%1（%2）]（按 id 与出现顺序匹配的任务）。")
                    .arg(mc.id).arg(mc.note);
        } else {
            msg = QStringLiteral("共 %1 个 Mission 发生修改，但其他文件中均无匹配目标，无法同步。")
                .arg(selected.size());
        }
        QMessageBox::information(this, QStringLiteral("同步到其他文件"), msg);
        return;
    }

    // 弹窗 — 三列表格：项/文件（树：Mission→分组→文件勾选行）｜旧值｜新值
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("同步到其他文件"));
    QVBoxLayout* dlgLayout = new QVBoxLayout(&dlg);
    dlgLayout->setSpacing(6);

    int totalChanged = 0;
    int totalUnmatched = 0;
    for (int si = 0; si < selected.size(); ++si) {
        totalChanged += selected[si].total();
        const auto& vts = targetsPerMission[si];
        auto itemMatched = [&vts](const ChangedItem& ci, int itemType) {
            for (const auto& st : vts) {
                bool m = itemType == 0 ? st.matchedParams.contains(ci.key)
                        : itemType == 1 ? st.matchedInputs.contains(ci.key)
                        : st.matchedOutputs.contains(ci.key);
                if (m) return true;
            }
            return false;
        };
        for (const auto& ci : selected[si].params)
            if (!itemMatched(ci, 0)) ++totalUnmatched;
        for (const auto& ci : selected[si].inputs)
            if (!itemMatched(ci, 1)) ++totalUnmatched;
        for (const auto& ci : selected[si].outputs)
            if (!itemMatched(ci, 2)) ++totalUnmatched;
    }
    QString hintText = QStringLiteral("共 %1 个任务、%2 项修改").arg(selected.size()).arg(totalChanged);
    if (totalUnmatched > 0)
        hintText += QStringLiteral("，其中 %1 项无对应项（不会同步）").arg(totalUnmatched);
    QLabel* hint = new QLabel(hintText);
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

    // 表格：列0 文件（勾选行）、列1 项、列2 旧值、列3 新值——一行一条完整变更
    //（借鉴 GitHub split diff / Navicat 同步预览的"旧左新右"并排对比）
    QTreeWidget* tree = new QTreeWidget(&dlg);
    tree->setColumnCount(4);
    tree->setHeaderLabels(QStringList()
        << QStringLiteral("文件") << QStringLiteral("项")
        << QStringLiteral("旧值") << QStringLiteral("新值"));
    tree->setRootIsDecorated(true);
    tree->setIndentation(14);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    tree->header()->setStretchLastSection(false);
    tree->header()->setMinimumSectionSize(40);
    dlgLayout->addWidget(tree, 1);

    // 数据结构：每个勾选行对应一个 (Mission, 项, 目标文件)
    struct CheckEntry {
        QTreeWidgetItem* item;
        int fileIdx; int mbIdx; int mIdx;
        int itemType; // 0=param, 1=inputPath, 2=outputPath
        int srcIdx;   // 来源 Mission 在 selected 中的索引
        int itemIdx;  // 该项在 src.params/inputs/outputs 中的索引（itemType=3 时无意义）
    };
    QVector<CheckEntry> allChecks;

    // 值截短显示（路径等长值不撑爆行宽）
    auto shortVal = [](const QString& s, int maxLen = 28) {
        QString t = s.trimmed();
        if (t.isEmpty()) return QStringLiteral("(空)");
        return t.size() > maxLen ? t.left(maxLen) + QStringLiteral("…") : t;
    };
    // 字段行：灰色小标签 + 值（旧值黑 #333 / 新值绿 #1a7f37 / 无对应项灰 #aaa）
    auto fieldLine = [](const QString& label, const QString& value, const QString& color) {
        return QStringLiteral("<span style='color:#999'>%1</span> "
                              "<span style='color:%2'>%3</span>")
            .arg(label.toHtmlEscaped()).arg(color).arg(value.toHtmlEscaped());
    };
    // 旧值/新值格：只列实际变化的字段（值/键名/说明/提示），每字段一行
    auto buildValueCell = [&](const ParsedParameter& oldP, const ChangedItem& ci, bool isParam,
                              bool isNew, const QString& color) {
        QStringList lines;
        if (ci.value != oldP.value)
            lines << fieldLine(QStringLiteral("值"),
                shortVal(isNew ? ci.value : oldP.value), color);
        if (isParam && ci.name != oldP.name)
            lines << fieldLine(QStringLiteral("键名"),
                shortVal(isNew ? ci.name : oldP.name), color);
        if (ci.description != oldP.description)
            lines << fieldLine(QStringLiteral("说明"),
                shortVal(isNew ? ci.description : oldP.description), color);
        if (ci.tip != oldP.tip)
            lines << fieldLine(QStringLiteral("提示"),
                shortVal(isNew ? ci.tip : oldP.tip), color);
        return lines.join(QStringLiteral("<br>"));
    };
    auto setCellWidget = [&tree](QTreeWidgetItem* item, int col, const QString& html) {
        if (html.isEmpty()) return;
        QLabel* lbl = new QLabel(html);
        lbl->setTextFormat(Qt::RichText);
        tree->setItemWidget(item, col, lbl);
    };
    // 按身份键在目标 Mission 中定位对应元素（旧值来源）
    auto findOldItem = [&](const SyncTarget& st, const ChangedItem& ci, int itemType) -> const ParsedParameter* {
        const ParsedMission& tm = m_files[st.fileIdx].blocks[st.mbIdx].missions[st.mIdx];
        if (itemType == 0) {
            QVector<QString> ks = buildParamKeys(tm.params, false);
            int i = ks.indexOf(ci.key);
            return i >= 0 ? &tm.params[i] : nullptr;
        }
        const QVector<ParsedParameter>& paths = itemType == 1 ? tm.inputPaths : tm.outputPaths;
        QVector<QString> ks = buildPathKeys(paths, false);
        int i = ks.indexOf(ci.key);
        return i >= 0 ? &paths[i] : nullptr;
    };

    // 按 Mission 建表：文件为主序（同文件连续行，每行都写文件名），
    // 每行一条完整变更：文件｜项｜旧值｜新值
    for (int si = 0; si < selected.size(); ++si) {
        const MissionChanges& mc = selected[si];
        const QVector<SyncTarget>& validTargets = targetsPerMission[si];
        if (validTargets.isEmpty()) continue; // 无任何目标文件的任务不展示，数量已计入顶部提示

        // 多任务时才显示 📁 任务分组头（单任务时顶部提示已说明）
        QTreeWidgetItem* missionNode = nullptr;
        if (selected.size() > 1) {
            missionNode = new QTreeWidgetItem(tree);
            QString mText = QStringLiteral("\xF0\x9F\x93\x81 [%1]").arg(mc.id);
            if (!mc.note.isEmpty()) mText += QStringLiteral(" %1").arg(mc.note);
            missionNode->setText(0, mText);
            missionNode->setFirstColumnSpanned(true);
            QFont mf = missionNode->font(0);
            mf.setBold(true);
            missionNode->setFont(0, mf);
            missionNode->setExpanded(true);
        }
        auto addRow = [&]() {
            return missionNode ? new QTreeWidgetItem(missionNode) : new QTreeWidgetItem(tree);
        };

        // 每个目标文件一个块：先已匹配行（参数→输入→输出），再无对应项行
        for (int fi = 0; fi < validTargets.size(); ++fi) {
            const SyncTarget& st = validTargets[fi];

            auto rowForItem = [&](const ChangedItem& ci, int itemType, int iIdx, const QString& icon) {
                const ParsedParameter* oldP = findOldItem(st, ci, itemType);
                QTreeWidgetItem* row = addRow();
                row->setText(0, st.fileName);
                row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                row->setCheckState(0, Qt::Unchecked); // 默认全不选，用户勾选后同步
                row->setText(1, icon + ci.display);
                if (oldP) {
                    setCellWidget(row, 2, buildValueCell(*oldP, ci, itemType == 0, false, QStringLiteral("#333")));
                    setCellWidget(row, 3, buildValueCell(*oldP, ci, itemType == 0, true, QStringLiteral("#1a7f37")));
                }
                allChecks.append({row, st.fileIdx, st.mbIdx, st.mIdx, itemType, si, iIdx});
            };

            for (int iIdx = 0; iIdx < mc.params.size(); ++iIdx)
                if (st.matchedParams.contains(mc.params[iIdx].key))
                    rowForItem(mc.params[iIdx], 0, iIdx, QStringLiteral("\xE2\x9A\x99 "));
            for (int iIdx = 0; iIdx < mc.inputs.size(); ++iIdx)
                if (st.matchedInputs.contains(mc.inputs[iIdx].key))
                    rowForItem(mc.inputs[iIdx], 1, iIdx, QStringLiteral("\xF0\x9F\x93\xA5 "));
            for (int iIdx = 0; iIdx < mc.outputs.size(); ++iIdx)
                if (st.matchedOutputs.contains(mc.outputs[iIdx].key))
                    rowForItem(mc.outputs[iIdx], 2, iIdx, QStringLiteral("\xF0\x9F\x93\xA4 "));

            // 该文件没有的项：灰色行，旧值"—"，新值列灰显本应同步的值（始终写全文件名）
            auto rowForMissing = [&](const ChangedItem& ci, const QString& icon) {
                QTreeWidgetItem* row = addRow();
                row->setText(0, QStringLiteral("%1（无对应项）").arg(st.fileName));
                row->setForeground(0, QColor(0xaa, 0xaa, 0xaa));
                row->setForeground(1, QColor(0xaa, 0xaa, 0xaa));
                row->setText(1, icon + ci.display);
                setCellWidget(row, 2, QStringLiteral("<span style='color:#aaa'>—</span>"));
                setCellWidget(row, 3, fieldLine(QStringLiteral("值"), shortVal(ci.value), QStringLiteral("#aaa")));
            };
            for (int iIdx = 0; iIdx < mc.params.size(); ++iIdx)
                if (!st.matchedParams.contains(mc.params[iIdx].key))
                    rowForMissing(mc.params[iIdx], QStringLiteral("\xE2\x9A\x99 "));
            for (int iIdx = 0; iIdx < mc.inputs.size(); ++iIdx)
                if (!st.matchedInputs.contains(mc.inputs[iIdx].key))
                    rowForMissing(mc.inputs[iIdx], QStringLiteral("\xF0\x9F\x93\xA5 "));
            for (int iIdx = 0; iIdx < mc.outputs.size(); ++iIdx)
                if (!st.matchedOutputs.contains(mc.outputs[iIdx].key))
                    rowForMissing(mc.outputs[iIdx], QStringLiteral("\xF0\x9F\x93\xA4 "));
        }
    }

    tree->expandAll();

    // 无可勾选项时全选按钮置灰
    if (allChecks.isEmpty()) {
        btnSelectAll->setEnabled(false);
        btnDeselectAll->setEnabled(false);
    }

    connect(btnSelectAll, &QPushButton::clicked, [&allChecks]() {
        for (auto& e : allChecks) e.item->setCheckState(0, Qt::Checked);
    });
    connect(btnDeselectAll, &QPushButton::clicked, [&allChecks]() {
        for (auto& e : allChecks) e.item->setCheckState(0, Qt::Unchecked);
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

    // 自适应宽高：文件/项列按文本宽、旧值/新值列按内容宽；高度按行数估算，封顶 420 树内滚动
    QFontMetrics fm(tree->font());
    int w0 = 60, w1 = 40, w2 = 40, w3 = 40;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        w0 = qMax(w0, fm.horizontalAdvance((*it)->text(0)));
        w1 = qMax(w1, fm.horizontalAdvance((*it)->text(1)));
        if (QWidget* wdg = tree->itemWidget(*it, 2)) w2 = qMax(w2, wdg->sizeHint().width());
        if (QWidget* wdg = tree->itemWidget(*it, 3)) w3 = qMax(w3, wdg->sizeHint().width());
    }
    tree->setColumnWidth(1, w1 + 16);
    tree->setColumnWidth(2, w2 + 16);
    tree->setColumnWidth(3, w3 + 16);
    int w = qBound(560, w0 + w1 + w2 + w3 + 26 + 16 * 3
        + dlgLayout->contentsMargins().left() + dlgLayout->contentsMargins().right()
        + tree->frameWidth() * 2 + 20, 900);

    int contentH = tree->header()->height();
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->isFirstColumnSpanned()) { contentH += 24; continue; } // 任务分组头
        int rowH = tree->fontMetrics().height() + 4;
        for (int c = 0; c < 4; ++c) {
            if (QWidget* wdg = tree->itemWidget(item, c))
                rowH = qMax(rowH, wdg->sizeHint().height() + 4);
        }
        contentH += rowH;
    }
    int treeH = qBound(80, contentH + 2, 420); // 按内容最优高度，最少 80、封顶 420 树内滚动
    tree->setFixedHeight(treeH);

    int hintH = hint->heightForWidth(w);
    if (hintH <= 0) hintH = hint->sizeHint().height();
    int fixedH = hintH + selRow->sizeHint().height() + btnRow->sizeHint().height()
        + dlgLayout->contentsMargins().top() + dlgLayout->contentsMargins().bottom()
        + dlgLayout->spacing() * 3 + 10;
    dlg.resize(w, qBound(220, fixedH + treeH, 640)); // 高度随内容收放，220~640

    if (dlg.exec() != QDialog::Accepted) return;

    // 同步：遍历已勾选的 (Mission, 项, 文件) 对，按身份键定位目标元素
    int syncedItems = 0;
    QSet<int> syncedFileIndices;
    for (const auto& e : allChecks) {
        if (e.item->checkState(0) != Qt::Checked) continue;
        const MissionChanges& src = selected[e.srcIdx];
        if (e.fileIdx < 0 || e.fileIdx >= m_files.size()) continue;
        ParsedFileData& tfd = m_files[e.fileIdx];
        if (e.mbIdx < 0 || e.mbIdx >= tfd.blocks.size()) continue;
        if (e.mIdx < 0 || e.mIdx >= tfd.blocks[e.mbIdx].missions.size()) continue;
        ParsedMission& target = tfd.blocks[e.mbIdx].missions[e.mIdx];

        const ChangedItem* item = nullptr;
        QVector<ParsedParameter>* tvec = nullptr;
        if (e.itemType == 0) {
            if (e.itemIdx < 0 || e.itemIdx >= src.params.size()) continue;
            item = &src.params[e.itemIdx];
            tvec = &target.params;
        } else if (e.itemType == 1) {
            if (e.itemIdx < 0 || e.itemIdx >= src.inputs.size()) continue;
            item = &src.inputs[e.itemIdx];
            tvec = &target.inputPaths;
        } else {
            if (e.itemIdx < 0 || e.itemIdx >= src.outputs.size()) continue;
            item = &src.outputs[e.itemIdx];
            tvec = &target.outputPaths;
        }
        QVector<QString> tKeys = (e.itemType == 0)
            ? buildParamKeys(*tvec, false)
            : buildPathKeys(*tvec, false);
        int ti = tKeys.indexOf(item->key);
        if (ti < 0) continue;
        ParsedParameter& tp = (*tvec)[ti];
        if (e.itemType == 0) tp.name = item->name;
        tp.value = item->value;
        tp.description = item->description;
        tp.tip = item->tip;
        ++syncedItems;
        syncedFileIndices.insert(e.fileIdx);
    }

    refreshParamTree();
    updateWindowTitle();
    updateStatus(QStringLiteral("已将 %1 项修改同步到 %2 个文件（未保存，点击“保存”写入磁盘）")
        .arg(syncedItems).arg(syncedFileIndices.size()));
}

void ParamConfigDialog::updateStatus(const QString& msg)
{
    ui.label_status->setText(msg);
}
