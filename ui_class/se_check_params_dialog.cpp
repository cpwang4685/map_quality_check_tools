#include "se_check_params_dialog.h"

#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

// ================================================================
//  纯文本 XML 小工具
//  说明：不用 QDomDocument 整体回写，避免重排原配置文件、破坏其中的
//  注释（M453/M454、M457 等整段注释必须原样保留），只对目标节点做
//  定点替换。
// ================================================================
namespace {

// XML 注释区间：注释里可能出现 "<MinVertexReduceRatio" 这类未转义文本，
// 正则查找标签时必须跳过，否则会把注释当成元素、写坏文件。
QVector<QPair<int, int>> commentRanges(const QString& text)
{
    QVector<QPair<int, int>> ranges;
    int pos = 0;
    while (true) {
        const int s = text.indexOf("<!--", pos);
        if (s < 0) break;
        const int e = text.indexOf("-->", s + 4);
        if (e < 0) {
            ranges.append(qMakePair(s, text.size()));
            break;
        }
        ranges.append(qMakePair(s, e + 3));
        pos = e + 3;
    }
    return ranges;
}

bool positionInComment(const QVector<QPair<int, int>>& ranges, int pos)
{
    for (const QPair<int, int>& r : ranges) {
        if (pos >= r.first && pos < r.second) return true;
    }
    return false;
}

bool textToDouble(const QString& s, double* out)
{
    bool ok = false;
    const double v = s.trimmed().toDouble(&ok);
    if (ok) *out = v;
    return ok;
}

bool sameNumber(double a, double b)
{
    return qAbs(a - b) <= 1e-9 * qMax(1.0, qMax(qAbs(a), qAbs(b)));
}

// 表单分组标题：占满两列，便于按 M460 七项关注点阅读参数
void addFormSection(QFormLayout* form, QWidget* parent, const QString& title)
{
    QLabel* lbl = new QLabel(title, parent);
    QFont f = lbl->font();
    f.setBold(true);
    lbl->setFont(f);
    lbl->setStyleSheet(QStringLiteral("color:#1F4E79; margin-top:8px;"));
    form->addRow(lbl);
}

int elementStart(const QString& text, const QString& tag)
{
    const QVector<QPair<int, int>> comments = commentRanges(text);
    QRegularExpression re(QString("<%1(?:\\s|>)").arg(tag));
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (!positionInComment(comments, m.capturedStart()))
            return m.capturedStart();
    }
    return -1;
}

int elementEnd(const QString& text, const QString& tag, int from)
{
    const QVector<QPair<int, int>> comments = commentRanges(text);
    const QString close = QString("</%1>").arg(tag);
    int p = text.indexOf(close, from);
    while (p >= 0 && positionInComment(comments, p))
        p = text.indexOf(close, p + close.size());
    return p < 0 ? -1 : p + close.size();
}

// 替换 <Tag ...>旧值</Tag>，保留原有属性（如 note/min/max）；
// 数值未变化时原样保留（如 1.0 不会写成 1），避免无意义改写触发备份。
// 注意：只匹配开标签，再向后找闭标签；不能用一个 .*? 大正则，否则注释里
// 出现 "<Tag" 时整段匹配会被注释开头吞掉，导致漏改或重复插入。
bool setElementText(QString& block, const QString& tag, const QString& value)
{
    const QVector<QPair<int, int>> comments = commentRanges(block);
    const QString closeTag = QString("</%1>").arg(tag);
    QRegularExpression openRe(QString("<%1\\b([^>]*)>").arg(tag));
    QRegularExpressionMatchIterator it = openRe.globalMatch(block);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (positionInComment(comments, m.capturedStart())) continue;
        if (m.captured(1).trimmed().endsWith('/')) continue; // 自闭合节点不走这里

        int closePos = block.indexOf(closeTag, m.capturedEnd());
        while (closePos >= 0 && positionInComment(comments, closePos))
            closePos = block.indexOf(closeTag, closePos + closeTag.size());
        if (closePos < 0) return false;

        double oldV = 0.0, newV = 0.0;
        if (textToDouble(block.mid(m.capturedEnd(), closePos - m.capturedEnd()), &oldV)
            && textToDouble(value, &newV) && sameNumber(oldV, newV))
            return true;

        block.replace(m.capturedStart(), closePos + closeTag.size() - m.capturedStart(),
                      QString("<%1%2>%3</%1>").arg(tag, m.captured(1), value));
        return true;
    }
    return false;
}

// 替换自闭合阈值节点上的 default="..."，保留 note/min/max；数值未变化时不动
bool setDefaultAttribute(QString& block, const QString& tag, const QString& value)
{
    const QVector<QPair<int, int>> comments = commentRanges(block);
    QRegularExpression tagRe(QString("<%1\\b[^>]*>").arg(tag));
    QRegularExpressionMatchIterator it = tagRe.globalMatch(block);
    QRegularExpressionMatch m;
    while (it.hasNext()) {
        const QRegularExpressionMatch candidate = it.next();
        if (!positionInComment(comments, candidate.capturedStart())) {
            m = candidate;
            break;
        }
    }
    if (!m.hasMatch()) return false;

    const QString tagText = m.captured(0);
    QRegularExpression attrRe(QString("\\bdefault\\s*=\\s*\"([^\"]*)\""));
    const QRegularExpressionMatch attrMatch = attrRe.match(tagText);
    double oldV = 0.0, newV = 0.0;
    if (attrMatch.hasMatch() && textToDouble(attrMatch.captured(1), &oldV)
        && textToDouble(value, &newV) && sameNumber(oldV, newV))
        return true;

    QString newTag = tagText;
    if (attrMatch.hasMatch()) {
        newTag.replace(attrRe, QString("default=\"%1\"").arg(value));
    } else if (newTag.endsWith("/>")) {
        newTag.insert(newTag.size() - 2, QString(" default=\"%1\"").arg(value));
    } else {
        newTag.insert(newTag.size() - 1, QString(" default=\"%1\"").arg(value));
    }
    block.replace(m.capturedStart(), m.capturedLength(), newTag);
    return true;
}

void insertBeforeClose(QString& block, const QString& tag, const QString& snippet)
{
    const QString close = QString("</%1>").arg(tag);
    const int p = block.lastIndexOf(close);
    if (p >= 0) block.insert(p, snippet);
}

QDomElement mission458Of(const QDomDocument& doc)
{
    const QDomNodeList missions = doc.documentElement().elementsByTagName("Mission");
    for (int i = 0; i < missions.count(); ++i) {
        const QDomElement m = missions.at(i).toElement();
        if (m.attribute("id").toInt() == 458) return m;
    }
    return QDomElement();
}

} // namespace

// ================================================================
//  构造 / 界面
// ================================================================
CSE_CheckParamsDialog::CSE_CheckParamsDialog(const QString& missionXmlPath,
                                             const QHash<QString, double>& overrides,
                                             QWidget* parent)
    : QDialog(parent)
    , m_missionXmlPath(missionXmlPath)
{
    m_matchParamPath = QFileInfo(missionXmlPath).absolutePath() + "/matchParameter.xml";
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);   // 去掉标题栏“?”帮助按钮
    buildUi();
    loadValues(overrides);
    resize(760, 640);
}

void CSE_CheckParamsDialog::buildUi()
{
    setWindowTitle("检查项参数设置");
    setObjectName("CSE_CheckParamsDialog");
    setSizeGripEnabled(true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 10);
    root->setSpacing(8);

    QTabWidget* tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    // 创建一个 Tab 页：顶部说明 + 表单，返回表单布局与页面控件
    const auto addPage = [&](const QString& title, const QString& intro)
        -> QPair<QFormLayout*, QWidget*> {
        QWidget* page = new QWidget(tabs);
        QVBoxLayout* lay = new QVBoxLayout(page);
        lay->setContentsMargins(14, 12, 14, 10);
        lay->setSpacing(10);

        QLabel* tip = new QLabel(intro, page);
        tip->setWordWrap(true);
        lay->addWidget(tip);

        QFormLayout* form = new QFormLayout();
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(9);
        lay->addLayout(form);
        lay->addStretch();

        // 参数较多（尤其 M460）时纵向可滚动，避免底部参数/保存按钮被挤出窗口
        QScrollArea* scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        tabs->addTab(scroll, title);
        return qMakePair(form, page);
    };

    // ---------------- M459 图形规范 ----------------
    {
        QFormLayout* form = addPage(
            "图形规范性检查",
            "图形规范性检查参数。用于判定尖锐角、碎面、狭长面、小面积及节点密度等"
            "图形问题；系统默认参数已按相关技术规范配置，可直接使用。").first;
        addField(form, "AcuteAngle", "尖锐角 (°)：",
                 0.1, 180.0, 1, 0.5, 5.0,
                 "面的夹角小于该值视为尖锐角。");
        addField(form, "SliverArea", "碎面面积：",
                 0.0, 100000.0, 2, 1.0, 1.0,
                 "面积小于该值的面视为碎面。");
        addField(form, "NarrowWidth", "狭长面宽度 (米)：",
                 0.0, 1000.0, 2, 0.1, 0.5,
                 "最小外接矩形短边小于该值的面视为狭长面。");
        addField(form, "MinArea", "小面积：",
                 0.0, 100000.0, 2, 1.0, 1.0,
                 "面积小于该值的面视为小面积面。");
        addField(form, "MinNodeDistance", "节点最小距离 (米)：",
                 0.0, 100.0, 6, 0.001, 0.001,
                 "相邻节点距离小于该值视为节点过密。");
        addField(form, "AvgNodeDensityUpper", "节点平均密度上限 (个/米)：",
                 0.0, 1000.0, 4, 0.01, 0.0,
                 "边界节点数/边界长度超过该值报警；0=不启用上限。");
        addField(form, "AvgNodeDensityLower", "节点平均密度下限 (个/米)：",
                 0.0, 1000.0, 4, 0.01, 0.0,
                 "边界节点数/边界长度低于该值报警；0=不启用下限。");
        addField(form, "NodeDensityUpper", "节点密度上限 (个/要素)：",
                 0.0, 1000.0, 4, 1.0, 0.0,
                 "单个要素节点数超过该值报警；0=不启用上限。");
        addField(form, "NodeDensityLower", "节点密度下限 (个/要素)：",
                 0.0, 1000.0, 4, 1.0, 0.0,
                 "单个要素节点数低于该值报警；0=不启用下限。");
    }

    // ---------------- M460 综合前后拓扑 ----------------
    {
        auto pageRef = addPage(
            "综合前后拓扑检查",
            "综合前后拓扑检查参数。系统将成果要素与原始要素自动配对，检查居民点与道路、"
            "道路断口、道路变形、偏移、拉直及属性一致性等问题；"
            "默认参数已按相关技术规范配置，可直接使用。");
        QFormLayout* form = pageRef.first;
        QWidget* page = pageRef.second;

        // 2026-10-30：“综合前后要素配对”（BufferDistance/MinOverlapRatio）属
        // “综合前后匹配”M458，已从本页删除，避免与 M460 检查项混在一起。
        addFormSection(form, this, "① 居民点与道路拓扑");
        addField(form, "EndpointBuffer", "端点连接容差 (米)：",
                 0.0, 1000.0, 2, 1.0, 50.0,
                 "村道/乡道折线端点距其他道路或居民点≤该值视为已连接/已压盖；"
                 "0=必须严格相接。");
        addField(form, "BoundaryTolerance", "边界端点排除距离 (米)：",
                 0.0, 100000.0, 2, 10.0, 50.0,
                 "端点位于数据外框或县界/市界/省界附近≤该值时，视为数据范围截断，"
                 "不报“居民点与道路拓扑”断点；0=不排除。");
        addField(form, "PointSideTolerance", "居民点换边容差 (米)：",
                 0.0, 100000.0, 2, 10.0, 100.0,
                 "只比较距最近道路≤该值的居民点是否左右换边；0=不判定换边。");

        addFormSection(form, this, "②③ 道路长距离连接 / 道路·河流断裂");
        addField(form, "LongGapThreshold", "长断口阈值 (米)：",
                 0.0, 1000000.0, 2, 10.0, 100.0,
                 "综合后断口大于该值报“道路长距离连接”，否则按短断口处理。");
        addField(form, "ConnectTolerance", "连通判定容差 (米)：",
                 0.0, 100000.0, 4, 0.5, 1.0,
                 "综合前后端点间距小于该值视为相接。");
        addField(form, "MinBreakGap", "短断口报错下限 (米)：",
                 0.0, 1000000.0, 2, 10.0, 50.0,
                 "综合后断口≤该值视为可接受、不报；>该值且≤长断口阈值报“道路·河流断裂”；"
                 "默认50米。");
        addField(form, "TunnelPortalTolerance", "隧道口容差 (米)：",
                 0.0, 1000.0, 2, 1.0, 5.0,
                 "成果道路端点距隧道线端点（隧道口）≤该值时，视为经隧道连通，"
                 "不报“道路长距离连接/道路·河流断裂”；隧道按 CLASID(4506xx)/"
                 "TYPE(含“隧道”)识别；0=关闭隧道豁免。");

        addFormSection(form, this, "④ 道路变形异常");
        addField(form, "MaxChangeRatio", "最大变化率 (0~1)：",
                 0.0, 10.0, 4, 0.05, 0.6,
                 "面积/长度变化率超过该值视为显著变形。");
        addField(form, "MaxVertexChange", "最大顶点变化数 (个)：",
                 0.0, 1000000.0, 0, 10.0, 100.0,
                 "顶点数变化超过该值视为显著变形。");

        addFormSection(form, this, "⑤ 道路过度偏移");
        addField(form, "MaxDisplacement", "偏移缓冲半径 (米)：",
                 0.0, 1000000.0, 2, 10.0, 100.0,
                 "以同路原始线按该半径生成缓冲，成果线落在缓冲外的部分视为偏移；"
                 "不再使用中心/端点位移判定，成果线合并、截短不会误报。");

        addFormSection(form, this, "⑥ 道路拉直异常");
        addField(form, "MaxSinuosityDrop", "最大弯曲度下降：",
                 0.0, 10.0, 4, 0.05, 0.5,
                 "弯曲度（线长/首尾直线距离）下降超过该值报“道路拉直异常”（默认0.5）。");

        // ---- 高级参数：默认隐藏，避免界面参数过多 ----
        QWidget* advHost = new QWidget(page);
        QVBoxLayout* advLay = new QVBoxLayout(advHost);
        advLay->setContentsMargins(0, 8, 0, 0);
        advLay->setSpacing(6);

        QPushButton* advToggle = new QPushButton("显示高级参数", advHost);
        advToggle->setCheckable(true);
        advToggle->setAutoDefault(false);
        advToggle->setCursor(Qt::PointingHandCursor);
        advToggle->setStyleSheet(
            QStringLiteral("text-align:left; border:none; color:#1F4E79; padding:4px 0;"));
        advLay->addWidget(advToggle);

        QWidget* advBody = new QWidget(advHost);
        advBody->setVisible(false);
        QFormLayout* advForm = new QFormLayout();
        advForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        advForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        advForm->setHorizontalSpacing(12);
        advForm->setVerticalSpacing(9);
        advBody->setLayout(advForm);
        advLay->addWidget(advBody);

        QObject::connect(advToggle, &QPushButton::toggled, advBody,
                         [advToggle, advBody](bool on) {
            advBody->setVisible(on);
            advToggle->setText(on ? QStringLiteral("收起高级参数")
                                  : QStringLiteral("显示高级参数"));
        });

        addFormSection(advForm, advBody, "⑤ 偏移（高级）");
        addField(advForm, "MaxOutsideRatio", "缓冲外长度占比 (0~1)：",
                 0.0, 1.0, 3, 0.01, 0.2,
                 "成果线落在同路原始线缓冲外的长度占成果线长的比例超过该值，"
                 "判为“道路过度偏移”；默认0.2（20%），另需缓冲外长度大于50米。");

        addFormSection(advForm, advBody, "④ 变形与化简（高级）");
        addField(advForm, "MaxLocalBulge", "局部鼓包弦高 (米)：",
                 0.0, 100000.0, 2, 5.0, 20.0,
                 "局部鼓包弦高超过该值且距原始线超过最大局部偏离时报警；0=关闭该项。");
        addField(advForm, "MaxLocalDeviation", "最大局部偏离 (米)：",
                 0.0, 100000.0, 2, 5.0, 20.0,
                 "排除原始道路本身急弯的偏离阈值，与局部鼓包弦高配合使用。");
        addField(advForm, "MinSimplifyVertexCount", "化简最少原始顶点数 (个)：",
                 0.0, 1000000.0, 0, 10.0, 20.0,
                 "原始顶点数达到该值才检查是否化简；0=关闭化简检查。");
        addField(advForm, "MinVertexReduceRatio", "化简最少顶点减少比例 (0~1)：",
                 0.0, 1.0, 4, 0.01, 0.05,
                 "顶点减少比例低于该值视为未化简/化简不足；0=关闭化简检查。");

        addFormSection(advForm, advBody, "⑥ 直角拉直重点（高级）");
        addField(advForm, "RightAngleMinAngle", "直角拉直最小转角 (°)：",
                 0.0, 180.0, 1, 5.0, 70.0,
                 "成果线拐点转角（0=直行，90=直角）达到该值且不超过最大值时，"
                 "作为“道路直角拉直（重点）”候选。");
        addField(advForm, "RightAngleMaxAngle", "直角拉直最大转角 (°)：",
                 0.0, 180.0, 1, 5.0, 110.0,
                 "与最小转角配合使用；默认 70~110°（直角±20°）。");
        addField(advForm, "RightAngleOrigMaxTurn", "原始同位置最大转角 (°)：",
                 0.0, 180.0, 1, 5.0, 30.0,
                 "原始同名线在成果拐点附近的转角≤该值时，才判为综合后新增直角；"
                 "原始本来就有直角则不报。");
        addField(advForm, "RightAngleMinSegment", "直角拐点最短邻边 (米)：",
                 0.0, 1000.0, 2, 1.0, 5.0,
                 "拐点两侧线段长度均≥该值才参与直角判定，避免小折点误报。");
        addField(advForm, "RightAngleSearchRadius", "原始线搜索半径 (米)：",
                 0.0, 10000.0, 2, 5.0, 60.0,
                 "在原始同名线上搜索对应拐点的半径。");

        form->addRow(advHost);
    }

    // ---------------- 底部说明与按钮 ----------------
    QLabel* note = new QLabel(
        "参数保存后立即生效，无需重启软件。系统会自动备份原配置文件，"
        "检查项的选择状态不受影响。", this);
    note->setWordWrap(true);
    root->addWidget(note);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* btnSave = new QPushButton("保存", this);
    btnSave->setObjectName("btnSaveParams");
    btnSave->setDefault(true);
    btnSave->setMinimumWidth(88);
    QPushButton* btnCancel = new QPushButton("取消", this);
    btnCancel->setObjectName("btnCancelParams");
    btnCancel->setMinimumWidth(88);
    btnRow->addWidget(btnSave);
    btnRow->addWidget(btnCancel);
    root->addLayout(btnRow);

    connect(btnSave, &QPushButton::clicked, this, &CSE_CheckParamsDialog::onSave);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
}

void CSE_CheckParamsDialog::addField(QFormLayout* form, const QString& key,
                                     const QString& label, double minV, double maxV,
                                     int decimals, double step, double defaultV,
                                     const QString& hint)
{
    QDoubleSpinBox* box = new QDoubleSpinBox(this);
    box->setObjectName("param_" + key);
    box->setAccessibleName(label);
    box->setRange(minV, maxV);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setValue(defaultV);
    box->setKeyboardTracking(false);
    box->setAlignment(Qt::AlignRight);
    box->setMinimumWidth(160);
    if (!hint.isEmpty()) box->setToolTip(hint);

    QLabel* lbl = new QLabel(label, this);
    if (!hint.isEmpty()) lbl->setToolTip(hint);

    form->addRow(lbl, box);

    Field f;
    f.editor = box;
    f.decimals = decimals;
    m_fields.insert(key, f);
}

void CSE_CheckParamsDialog::addTextField(QFormLayout* form, const QString& key,
                                         const QString& label, const QString& defaultV,
                                         const QString& hint)
{
    QLineEdit* edit = new QLineEdit(this);
    edit->setObjectName("param_" + key);
    edit->setAccessibleName(label);
    edit->setText(defaultV);
    edit->setClearButtonEnabled(true);
    edit->setMinimumWidth(360);
    if (!hint.isEmpty()) edit->setToolTip(hint);

    QLabel* lbl = new QLabel(label, this);
    if (!hint.isEmpty()) lbl->setToolTip(hint);

    form->addRow(lbl, edit);
    m_textFields.insert(key, edit);
}

// ================================================================
//  读取当前值（XML → 控件；主界面已改但未保存的值优先）
// ================================================================
void CSE_CheckParamsDialog::loadValues(const QHash<QString, double>& overrides)
{
    QHash<QString, double> vals;
    for (auto it = m_fields.constBegin(); it != m_fields.constEnd(); ++it)
        vals.insert(it.key(), it.value().editor->value());

    // ---- mission_config.xml ----
    QFile mf(m_missionXmlPath);
    if (mf.open(QIODevice::ReadOnly)) {
        QDomDocument doc;
        if (doc.setContent(&mf)) {
            const QDomElement root = doc.documentElement();

            const QDomElement th = root.firstChildElement("Thresholds");
            if (!th.isNull()) {
                const char* thresholdKeys[] = {
                    "AcuteAngle", "SliverArea", "NarrowWidth", "MinArea",
                    "MinNodeDistance", "AvgNodeDensityUpper", "AvgNodeDensityLower",
                    "NodeDensityUpper", "NodeDensityLower"
                };
                for (const char* key : thresholdKeys) {
                    const QString tag = QString::fromLatin1(key);
                    const QDomElement e = th.firstChildElement(tag);
                    if (e.isNull()) continue;
                    bool ok = false;
                    const double v = e.attribute("default").toDouble(&ok);
                    if (ok) vals[tag] = v;
                }
            }
        }
        mf.close();
    }

    // ---- matchParameter.xml ----
    QFile pf(m_matchParamPath);
    if (pf.open(QIODevice::ReadOnly)) {
        QDomDocument doc;
        if (doc.setContent(&pf)) {
            const QDomElement m = mission458Of(doc);
            const QDomElement param = m.isNull() ? QDomElement()
                                                 : m.firstChildElement("Parameter");
            if (!param.isNull()) {
                const char* matchKeys[] = {
                    "BufferDistance", "MinOverlapRatio", "MaxChangeRatio", "MaxVertexChange",
                    "LongGapThreshold", "ConnectTolerance", "MinBreakGap", "MaxDisplacement",
                    "MaxOutsideRatio", "MaxSinuosityDrop", "TunnelPortalTolerance",
                    "RightAngleMinAngle", "RightAngleMaxAngle", "RightAngleOrigMaxTurn",
                    "RightAngleMinSegment", "RightAngleSearchRadius",
                    "EndpointBuffer", "BoundaryTolerance",
                    "PointSideTolerance", "MinSimplifyVertexCount", "MinVertexReduceRatio",
                    "MaxLocalBulge", "MaxLocalDeviation"
                };
                for (const char* key : matchKeys) {
                    const QString tag = QString::fromLatin1(key);
                    const QDomElement e = param.firstChildElement(tag);
                    double v = 0.0;
                    if (!e.isNull() && textToDouble(e.text(), &v))
                        vals[tag] = v;
                }

                const char* textKeys[] = {
                    "RoadAttrFields", "BoundaryAttrFields", "WaterAttrFields",
                    "RoadLayerKeywords", "RiverLayerKeywords", "BoundaryLayerKeywords"
                };
                for (const char* key : textKeys) {
                    const QString tag = QString::fromLatin1(key);
                    const QDomElement e = param.firstChildElement(tag);
                    QLineEdit* edit = m_textFields.value(tag, nullptr);
                    if (edit && !e.isNull())
                        edit->setText(e.text().trimmed());
                }
            }
        }
        pf.close();
    }

    // ---- 主界面当前值（用户可能刚改过上方阈值框） ----
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
        if (m_fields.contains(it.key()))
            vals[it.key()] = it.value();
    }

    for (auto it = m_fields.constBegin(); it != m_fields.constEnd(); ++it)
        it.value().editor->setValue(vals.value(it.key(), it.value().editor->value()));
}

// ================================================================
//  保存
// ================================================================
void CSE_CheckParamsDialog::onSave()
{
    m_values.clear();
    for (auto it = m_fields.constBegin(); it != m_fields.constEnd(); ++it)
        m_values.insert(it.key(), it.value().editor->value());

    QString error;
    if (!saveToFiles(&error)) {
        QMessageBox::critical(this, "保存失败", error);
        return;
    }
    accept();
}

bool CSE_CheckParamsDialog::saveToFiles(QString* error)
{
    QHash<QString, int> decimals;
    for (auto it = m_fields.constBegin(); it != m_fields.constEnd(); ++it)
        decimals.insert(it.key(), it.value().decimals);

    QHash<QString, QString> textValues;
    for (auto it = m_textFields.constBegin(); it != m_textFields.constEnd(); ++it)
        textValues.insert(it.key(), it.value()->text());

    return saveValuesToFiles(m_missionXmlPath, m_values, decimals, textValues, error);
}

bool CSE_CheckParamsDialog::saveValuesToFiles(const QString& missionXmlPath,
                                              const QHash<QString, double>& values,
                                              const QHash<QString, int>& decimals,
                                              const QHash<QString, QString>& textValues,
                                              QString* error)
{
    const QString matchParamPath =
        QFileInfo(missionXmlPath).absolutePath() + "/matchParameter.xml";

    QFile mf(missionXmlPath);
    if (!mf.open(QIODevice::ReadOnly)) {
        *error = QString("无法读取配置文件：\n%1\n%2")
                     .arg(QDir::toNativeSeparators(missionXmlPath), mf.errorString());
        return false;
    }
    const QString missionSource = QString::fromUtf8(mf.readAll());
    mf.close();

    const bool matchExists = QFileInfo::exists(matchParamPath);
    QString matchSource;
    if (matchExists) {
        QFile pf(matchParamPath);
        if (!pf.open(QIODevice::ReadOnly)) {
            *error = QString("无法读取配置文件：\n%1\n%2")
                         .arg(QDir::toNativeSeparators(matchParamPath), pf.errorString());
            return false;
        }
        matchSource = QString::fromUtf8(pf.readAll());
        pf.close();
    }

    const QString missionNew = buildMissionConfigText(missionSource, values, decimals, error);
    if (missionNew.isEmpty()) return false;
    const QString matchNew = buildMatchParamText(matchSource, matchExists, values, decimals,
                                                 textValues, error);
    if (matchNew.isEmpty()) return false;

    // 写回前先校验生成的 XML 结构，避免写坏配置
    {
        QDomDocument doc;
        QString parseErr;
        if (!doc.setContent(missionNew, &parseErr)) {
            *error = QString("生成的 mission_config.xml 格式无效：%1").arg(parseErr);
            return false;
        }
    }
    {
        QDomDocument doc;
        QString parseErr;
        if (!doc.setContent(matchNew, &parseErr)) {
            *error = QString("生成的 matchParameter.xml 格式无效：%1").arg(parseErr);
            return false;
        }
    }

    if (missionNew != missionSource) {
        if (!backupFile(missionXmlPath, error)) return false;
        if (!writeFileAtomic(missionXmlPath, missionNew, error)) return false;
    }
    if (!matchExists || matchNew != matchSource) {
        if (matchExists && !backupFile(matchParamPath, error)) return false;
        if (!writeFileAtomic(matchParamPath, matchNew, error)) return false;
    }
    return true;
}

QString CSE_CheckParamsDialog::buildMissionConfigText(const QString& source,
                                                      const QHash<QString, double>& values,
                                                      const QHash<QString, int>& decimals,
                                                      QString* error)
{
    QString text = source;

    // ---- GlobalParams：比例尺、结点容差（主窗口「阈值参数」两项） ----
    const int gs = elementStart(text, "GlobalParams");
    const int ge = elementEnd(text, "GlobalParams", gs);
    if (gs >= 0 && ge >= 0) {
        QString block = text.mid(gs, ge - gs);
        const char* globalKeys[] = {"Scale", "FuzzyTolerance"};
        for (const char* key : globalKeys) {
            const QString tag = QString::fromLatin1(key);
            if (!values.contains(tag)) continue;
            const QString value = formatNumber(values.value(tag), decimals.value(tag, 2));
            if (!setElementText(block, tag, value))
                insertBeforeClose(block, "GlobalParams",
                                  QString("\n    <%1>%2</%1>").arg(tag, value));
        }
        text.replace(gs, ge - gs, block);
    }

    // ---- Thresholds：M459/M455 阈值的 default 属性 ----
    const int ts = elementStart(text, "Thresholds");
    const int te = elementEnd(text, "Thresholds", ts);
    if (ts < 0 || te < 0) {
        *error = "mission_config.xml 缺少 <Thresholds> 节点，无法保存图形规范参数。";
        return QString();
    }
    {
        QString block = text.mid(ts, te - ts);
        const char* thresholdKeys[] = {
            "AcuteAngle", "SliverArea", "NarrowWidth", "MinArea",
            "MinNodeDistance", "ResidentCoverTolerance",
            "AvgNodeDensityUpper", "AvgNodeDensityLower",
            "NodeDensityUpper", "NodeDensityLower"
        };
        for (const char* key : thresholdKeys) {
            const QString tag = QString::fromLatin1(key);
            if (!values.contains(tag)) continue;
            const QString value = formatNumber(values.value(tag), decimals.value(tag, 2));
            if (!setDefaultAttribute(block, tag, value))
                insertBeforeClose(block, "Thresholds",
                                  QString("\n    <%1 default=\"%2\"/>").arg(tag, value));
        }
        text.replace(ts, te - ts, block);
    }

    return text;
}

QString CSE_CheckParamsDialog::buildMatchParamText(const QString& source, bool fileExists,
                                                   const QHash<QString, double>& values,
                                                   const QHash<QString, int>& decimals,
                                                   const QHash<QString, QString>& textValues,
                                                   QString* error)
{
    QString text = source;
    if (!fileExists || text.trimmed().isEmpty()) {
        text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<MapGeneBatchProcessing>\n"
               "    <Mission id=\"458\">\n"
               "        <Parameter>\n"
               "        </Parameter>\n"
               "    </Mission>\n"
               "</MapGeneBatchProcessing>\n";
    }

    // 定位 <Mission id="458">...</Mission>
    QRegularExpression missionRe("<Mission\\s+id\\s*=\\s*\"458\"");
    const QRegularExpressionMatch mm = missionRe.match(text);
    if (!mm.hasMatch()) {
        *error = "matchParameter.xml 中未找到 <Mission id=\"458\"> 节点。";
        return QString();
    }
    const int ms = mm.capturedStart();
    int me = text.indexOf("</Mission>", ms);
    if (me < 0) {
        *error = "matchParameter.xml 中 <Mission id=\"458\"> 节点未闭合。";
        return QString();
    }
    me += QString("</Mission>").size();
    QString missionBlock = text.mid(ms, me - ms);

    const int ps = elementStart(missionBlock, "Parameter");
    const int pe = elementEnd(missionBlock, "Parameter", ps);
    if (ps < 0 || pe < 0) {
        *error = "matchParameter.xml 中 <Mission id=\"458\"> 缺少 <Parameter> 节点。";
        return QString();
    }
    QString paramBlock = missionBlock.mid(ps, pe - ps);

    const char* matchKeys[] = {
        "BufferDistance", "MinOverlapRatio", "MaxChangeRatio", "MaxVertexChange",
        "LongGapThreshold", "ConnectTolerance", "MinBreakGap", "MaxDisplacement",
        "MaxOutsideRatio", "MaxSinuosityDrop", "TunnelPortalTolerance",
        "RightAngleMinAngle", "RightAngleMaxAngle", "RightAngleOrigMaxTurn",
        "RightAngleMinSegment", "RightAngleSearchRadius",
        "EndpointBuffer", "BoundaryTolerance", "PointSideTolerance",
        "MinSimplifyVertexCount", "MinVertexReduceRatio",
        "MaxLocalBulge", "MaxLocalDeviation"
    };
    for (const char* key : matchKeys) {
        const QString tag = QString::fromLatin1(key);
        if (!values.contains(tag)) continue;
        const QString value = formatNumber(values.value(tag), decimals.value(tag, 2));
        if (!setElementText(paramBlock, tag, value))
            insertBeforeClose(paramBlock, "Parameter",
                              QString("\n            <%1>%2</%1>").arg(tag, value));
    }

    const char* textKeys[] = {
        "RoadAttrFields", "BoundaryAttrFields", "WaterAttrFields",
        "RoadLayerKeywords", "RiverLayerKeywords", "BoundaryLayerKeywords"
    };
    for (const char* key : textKeys) {
        const QString tag = QString::fromLatin1(key);
        if (!textValues.contains(tag)) continue;
        const QString value = textValues.value(tag).trimmed().toHtmlEscaped();
        if (!setElementText(paramBlock, tag, value))
            insertBeforeClose(paramBlock, "Parameter",
                              QString("\n            <%1>%2</%1>").arg(tag, value));
    }

    missionBlock.replace(ps, pe - ps, paramBlock);
    text.replace(ms, me - ms, missionBlock);
    return text;
}

// ================================================================
//  文件工具
// ================================================================
QString CSE_CheckParamsDialog::formatNumber(double v, int decimals)
{
    QString s = QString::number(v, 'f', qBound(0, decimals, 8));
    if (s.contains('.')) {
        while (s.endsWith('0')) s.chop(1);
        if (s.endsWith('.')) s.chop(1);
    }
    if (s == "-0") s = "0";
    return s;
}

bool CSE_CheckParamsDialog::backupFile(const QString& path, QString* error)
{
    if (!QFileInfo::exists(path)) return true;

    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString bak = path + ".bak_" + stamp;
    int n = 1;
    while (QFileInfo::exists(bak))
        bak = path + ".bak_" + stamp + "_" + QString::number(n++);

    if (!QFile::copy(path, bak)) {
        *error = QString("无法备份配置文件：\n%1\n"
                         "请检查文件是否只读或目录是否有写入权限。")
                     .arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

bool CSE_CheckParamsDialog::writeFileAtomic(const QString& path, const QString& text,
                                            QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = QString("无法写入配置文件：\n%1\n%2")
                     .arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    const QByteArray data = text.toUtf8();
    if (f.write(data) != data.size()) {
        *error = QString("写入配置文件失败：\n%1\n%2")
                     .arg(QDir::toNativeSeparators(path), f.errorString());
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        *error = QString("保存配置文件失败：\n%1\n%2")
                     .arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    return true;
}
