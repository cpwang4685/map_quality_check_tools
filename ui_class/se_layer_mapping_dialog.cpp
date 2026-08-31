#include "se_layer_mapping_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QRegularExpression>
#include <QApplication>

#include "ui_fit_helper.h"

// ====== 构造 ======
SeLayerMappingDialog::SeLayerMappingDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("图层映射配置");
    setMinimumSize(900, 600);
    resize(960, 700);
    DialogFitHelper::install(this);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    // ---- 映射表格 ----
    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({
        "源图层名称", "源图层代码", "→", "标准成果图层", "数据图层(SHP)", "几何类型", "备注"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(2, 30);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table);

    // ---- 状态标签 ----
    m_statusLabel = new QLabel("就绪", this);
    m_statusLabel->setStyleSheet("QLabel { color: #888; font-size: 11px; padding: 2px 4px; }");
    mainLayout->addWidget(m_statusLabel);

    // ---- 底部按钮 ----
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    auto* btnImport = new QPushButton("从CSV导入...", this);
    auto* btnExport = new QPushButton("导出CSV...", this);
    auto* btnAuto   = new QPushButton("自动匹配", this);
    auto* btnSave   = new QPushButton("保存", this);
    auto* btnOk     = new QPushButton("确定", this);

    btnImport->setStyleSheet("QPushButton { padding: 6px 14px; border-radius: 4px; }");
    btnExport->setStyleSheet("QPushButton { padding: 6px 14px; border-radius: 4px; }");
    btnAuto->setStyleSheet("QPushButton { color: #1890ff; border: 1px solid #1890ff; "
        "background: #fff; padding: 6px 16px; border-radius: 4px; }");
    btnOk->setStyleSheet("QPushButton { background: #1890ff; color: #fff; "
        "padding: 6px 20px; border-radius: 4px; border: none; }");
    btnSave->setStyleSheet("QPushButton { padding: 6px 16px; border-radius: 4px; }");

    btnLayout->addWidget(btnImport);
    btnLayout->addWidget(btnExport);
    btnLayout->addSpacing(20);
    btnLayout->addWidget(btnAuto);
    btnLayout->addWidget(btnSave);
    btnLayout->addWidget(btnOk);
    mainLayout->addLayout(btnLayout);

    connect(btnImport, &QPushButton::clicked, this, &SeLayerMappingDialog::onImportCsv);
    connect(btnExport, &QPushButton::clicked, this, &SeLayerMappingDialog::onExportCsv);
    connect(btnAuto,   &QPushButton::clicked, this, &SeLayerMappingDialog::onAutoMatch);
    connect(btnSave,   &QPushButton::clicked, this, &SeLayerMappingDialog::onSave);
    connect(btnOk,     &QPushButton::clicked, this, &QDialog::accept);
}

SeLayerMappingDialog::~SeLayerMappingDialog() {}

// ====== 设置标准图层列表 ======
void SeLayerMappingDialog::setStandardLayers(const QList<LayerMappingItem>& items)
{
    m_items.clear();
    for (const auto& src : items) {
        LayerMappingItem item;
        item.sourceCode = src.sourceCode;
        item.sourceDesc = src.sourceDesc;
        item.stdName    = src.stdName;
        item.actualShp  = src.actualShp;
        item.geomType   = src.geomType;
        item.note       = src.note;
        item.autoMatched = false;
        item.comboBox   = nullptr;
        m_items.append(item);
    }
    buildTable();
}

// ====== 设置数据目录SHP文件列表 ======
void SeLayerMappingDialog::setDataShpFiles(const QStringList& shpFiles)
{
    m_dataShpFiles = shpFiles;
    // 重新填充所有下拉框
    for (int i = 0; i < m_items.size(); i++) {
        auto& item = m_items[i];
        if (!item.comboBox) continue;
        QString prev = item.comboBox->currentText();
        item.comboBox->blockSignals(true);
        item.comboBox->clear();
        item.comboBox->addItem("- 不映射 -");
        item.comboBox->addItems(m_dataShpFiles);

        // 尝试恢复之前的选择，或自动匹配
        if (!prev.isEmpty() && m_dataShpFiles.contains(prev)) {
            item.comboBox->setCurrentText(prev);
        } else {
            QString matched = autoMatchShp(item.stdName);
            if (!matched.isEmpty()) {
                item.comboBox->setCurrentText(matched);
                item.actualShp = matched;
                item.autoMatched = true;
            } else {
                item.comboBox->setCurrentIndex(0); // "不映射"
            }
        }
        item.comboBox->blockSignals(false);
        updateGeomTypeForRow(i);
    }
}

// ====== 设置可选源图层代码列表 ======
void SeLayerMappingDialog::setSourceCodes(const QStringList& codes)
{
    m_sourceCodes = codes;
}

// ====== 构建表格 ======
void SeLayerMappingDialog::buildTable()
{
    m_table->setRowCount(m_items.size());

    for (int i = 0; i < m_items.size(); i++) {
        auto& item = m_items[i];

        // 列0：源图层名称
        auto* descItem = new QTableWidgetItem(item.sourceDesc);
        descItem->setFlags(descItem->flags() & ~Qt::ItemIsEditable);
        descItem->setToolTip(item.sourceDesc);
        m_table->setItem(i, 0, descItem);

        // 列1：源图层代码
        auto* codeItem = new QTableWidgetItem(item.sourceCode);
        codeItem->setFlags(codeItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 1, codeItem);

        // 列2：箭头
        auto* arrowItem = new QTableWidgetItem("→");
        arrowItem->setFlags(arrowItem->flags() & ~Qt::ItemIsEditable);
        arrowItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 2, arrowItem);

        // 列3：标准成果图层
        auto* nameItem = new QTableWidgetItem(item.stdName);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 3, nameItem);

        // 列4：数据图层下拉框
        item.comboBox = new QComboBox(m_table);
        item.comboBox->addItem("- 不映射 -");
        item.comboBox->addItems(m_dataShpFiles);

        // 自动匹配
        if (!item.actualShp.isEmpty() && m_dataShpFiles.contains(item.actualShp)) {
            item.comboBox->setCurrentText(item.actualShp);
            item.autoMatched = true;
        } else {
            QString matched = autoMatchShp(item.stdName);
            if (!matched.isEmpty()) {
                item.comboBox->setCurrentText(matched);
                item.actualShp = matched;
                item.autoMatched = true;
            } else {
                item.comboBox->setCurrentIndex(0);
            }
        }

        connect(item.comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, i](int) {
                auto& it = m_items[i];
                QString selected = it.comboBox->currentText();
                if (selected == "- 不映射 -" || selected.isEmpty()) {
                    it.actualShp.clear();
                    it.autoMatched = false;
                } else {
                    it.actualShp = selected;
                }
                updateGeomTypeForRow(i);
            });

        m_table->setCellWidget(i, 4, item.comboBox);

        // 列5：几何类型
        updateGeomTypeForRow(i);
        auto* geomItem = new QTableWidgetItem(item.geomType);
        geomItem->setFlags(geomItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 5, geomItem);

        // 列6：备注
        auto* noteItem = new QTableWidgetItem(item.note);
        noteItem->setFlags(noteItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 6, noteItem);
    }

    // 更新计数
    int matched = 0;
    for (const auto& it : m_items) {
        if (!it.actualShp.isEmpty()) matched++;
    }
    m_statusLabel->setText(
        QString("共 %1 个标准图层，已匹配 %2 个，待匹配 %3 个")
            .arg(m_items.size()).arg(matched).arg(m_items.size() - matched));
}

// ====== 更新某行的几何类型列 ======
void SeLayerMappingDialog::updateGeomTypeForRow(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    auto& item = m_items[row];
    if (!item.actualShp.isEmpty()) {
        item.geomType = guessGeomType(item.actualShp);
    } else {
        item.geomType.clear();
    }
    auto* geomItem = m_table->item(row, 5);
    if (geomItem) {
        geomItem->setText(item.geomType);
        // 着色
        if (item.geomType == "面")       geomItem->setForeground(QColor("#52c41a"));
        else if (item.geomType == "线")  geomItem->setForeground(QColor("#1890ff"));
        else if (item.geomType == "点")  geomItem->setForeground(QColor("#fa8c16"));
        else                             geomItem->setForeground(QColor("#999"));
    }
}

// ====== 根据SHP名猜测几何类型 ======
QString SeLayerMappingDialog::guessGeomType(const QString& shpName) const
{
    QString base = QFileInfo(shpName).baseName();
    // 面状要素关键词
    if (base.contains("面") || base.contains("普色") || base.contains("注记")
        || base.contains("晕带") || base.contains("湖泊水库") && !base.contains("_点")
        || base.contains("街区") || base.contains("水库") || base.contains("公园")
        || base.contains("行政区划") || base.contains("国家公园界"))
        return "面";
    // 线状要素关键词
    if (base.contains("界") || base.contains("道") || base.contains("路")
        || base.contains("河") || base.contains("渠") || base.contains("线")
        || base.contains("铁路") || base.contains("高速") || base.contains("隧道")
        || base.contains("长城") || base.contains("岸"))
        return "线";
    // 点状要素关键词
    if (base.contains("点") || base.contains("站") || base.contains("驻地")
        || base.contains("山峰") || base.contains("泉") || base.contains("出入口")
        || base.contains("名称") || base.contains("注记") || base.contains("机场")
        || base.contains("景区") && !base.contains("界"))
        return "点";

    return "unknown";
}

// ====== 自动匹配：模糊查找SHP文件名 ======
QString SeLayerMappingDialog::autoMatchShp(const QString& stdName) const
{
    if (stdName.isEmpty()) return QString();

    // 1. 精确匹配（去掉.shp后缀）
    for (const auto& sf : m_dataShpFiles) {
        QString base = QFileInfo(sf).baseName();
        if (base == stdName)
            return sf;
    }

    // 2. 包含匹配
    for (const auto& sf : m_dataShpFiles) {
        QString base = QFileInfo(sf).baseName();
        if (base.contains(stdName) || stdName.contains(base))
            return sf;
    }

    // 3. 常见别名映射
    QHash<QString, QStringList> aliases;
    aliases["乡道（专用道）"] << "乡道" << "专用道";
    aliases["高速_名称"] << "高速公路" << "高速";
    aliases["湖泊水库_名称"] << "湖泊水库";
    aliases["湖泊水库_点"] << "湖泊水库";
    aliases["干渠_名称"] << "干渠";
    aliases["支渠_名称"] << "支渠";
    aliases["总干渠_名称"] << "总干渠";
    aliases["县级行政区划面普色"] << "县级行政区划" << "县";
    aliases["市级行政区划面普色"] << "市级行政区划" << "市";
    aliases["省级行政区划面普色"] << "省级行政区划" << "省";
    aliases["邻区普染色"] << "邻区";
    aliases["0A级景区_选取"] << "A级景区";
    aliases["0行政村_选取"] << "行政村";
    aliases["省级表面注记"] << "省级" << "表面注记";
    aliases["市级表面注记"] << "市级" << "表面注记";
    aliases["县级表面注记"] << "县级" << "表面注记";
    aliases["乡镇级表面注记"] << "乡镇" << "表面注记";
    aliases["省级表面注记_飞地及小面"] << "省级" << "飞地";
    aliases["市级表面注记_飞地及小面"] << "市级" << "飞地";
    aliases["县级表面注记_飞地及小面"] << "县级" << "飞地";
    aliases["乡镇级表面注记_飞地及小面"] << "乡镇" << "飞地";
    aliases["一级河流_名称"] << "一级河流";
    aliases["二级河流_名称"] << "二级河流";
    aliases["三级河流_名称"] << "三级河流";
    aliases["四级河流_名称"] << "四级河流";
    aliases["五级河流_名称"] << "五级河流";
    aliases["六级河流_名称"] << "六级河流";
    aliases["等外河流_名称"] << "等外河流";
    aliases["高速代码"] << "高速";
    aliases["国道代码"] << "国道";
    aliases["省道代码"] << "省道";
    aliases["县道代码"] << "县道";
    aliases["高速出入口"] << "高速" << "出入口";
    aliases["水渠_方向点"] << "水渠" << "方向点";
    aliases["国家公园注记"] << "国家公园";
    aliases["二级山脉_名称"] << "山脉";
    aliases["三级山脉_名称"] << "山脉";
    aliases["火车隧道口"] << "隧道";
    aliases["汽车隧道口"] << "隧道";
    aliases["铁路休止符"] << "铁路";
    aliases["普通车站"] << "车站" << "火车站";
    aliases["高铁车站"] << "车站" << "高铁" << "火车站";
    aliases["在建高速公路"] << "高速" << "在建";
    aliases["在建普通铁路"] << "铁路" << "在建";
    aliases["世界级古遗址"] << "文物古迹" << "古遗址";
    aliases["地沟名"] << "地理名称";
    aliases["未定省界"] << "省界";
    aliases["省级驻地"] << "驻地" << "省级";
    aliases["市级驻地"] << "驻地" << "市级";
    aliases["县级驻地"] << "驻地" << "县级";
    aliases["乡镇级驻地"] << "驻地" << "乡镇";
    aliases["高速公路"] << "高速";
    aliases["高速铁路"] << "高铁" << "铁路";
    aliases["普通铁路"] << "铁路";

    if (aliases.contains(stdName)) {
        for (const auto& alias : aliases[stdName]) {
            for (const auto& sf : m_dataShpFiles) {
                QString base = QFileInfo(sf).baseName();
                if (base.contains(alias) || alias.contains(base))
                    return sf;
            }
        }
    }

    // 4. 去掉括号内容和常见后缀后重试
    QString cleanName = stdName;
    cleanName.remove(QRegularExpression("[（(][^)）]*[)）]"));  // 去括号
    cleanName.remove(QRegularExpression("_[^_]*$"));             // 去_后缀
    cleanName.remove(QRegularExpression("选取$"));               // 去"选取"
    if (cleanName != stdName && !cleanName.isEmpty()) {
        for (const auto& sf : m_dataShpFiles) {
            QString base = QFileInfo(sf).baseName();
            if (base.contains(cleanName) || cleanName.contains(base))
                return sf;
        }
    }

    return QString();
}

// ====== 全部自动匹配 ======
void SeLayerMappingDialog::onAutoMatch()
{
    int matched = 0;
    for (auto& item : m_items) {
        if (!item.comboBox) continue;
        QString m = autoMatchShp(item.stdName);
        if (!m.isEmpty()) {
            item.comboBox->setCurrentText(m);
            item.actualShp = m;
            item.autoMatched = true;
            matched++;
        } else {
            item.comboBox->setCurrentIndex(0);
            item.actualShp.clear();
            item.autoMatched = false;
        }
    }

    // 刷新几何类型
    for (int i = 0; i < m_items.size(); i++)
        updateGeomTypeForRow(i);

    m_statusLabel->setText(
        QString("自动匹配完成：共 %1 个图层，成功匹配 %2 个")
            .arg(m_items.size()).arg(matched));
}

// ====== 从CSV导入标准图层列表 ======
void SeLayerMappingDialog::onImportCsv()
{
    QString path = QFileDialog::getOpenFileName(this,
        "导入图层映射CSV", QString(),
        "CSV文件 (*.csv);;所有文件 (*.*)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "导入失败", "无法打开文件：" + path);
        return;
    }

    QList<LayerMappingItem> imported;
    QTextStream ts(&file);
    ts.setCodec("UTF-8");

    int lineNo = 0;
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        lineNo++;
        if (line.isEmpty() || line.startsWith("#")) continue;

        QStringList parts = line.split(",");
        // 格式：源图层名称,源图层代码,标准成果图层,实际数据SHP,几何类型,备注
        if (parts.size() < 3) continue;

        LayerMappingItem item;
        item.sourceDesc = parts.value(0).trimmed();
        item.sourceCode = parts.value(1).trimmed();
        item.stdName    = parts.value(2).trimmed();
        item.actualShp  = parts.value(3).trimmed();
        item.geomType   = parts.value(4).trimmed();
        item.note       = parts.value(5).trimmed();
        if (!item.stdName.isEmpty())
            imported.append(item);
    }
    file.close();

    if (imported.isEmpty()) {
        QMessageBox::information(this, "导入结果", "未从文件中读取到有效图层映射项。");
        return;
    }

    // 用导入的数据替换当前列表
    m_items.clear();
    for (const auto& src : imported) {
        LayerMappingItem item;
        item.sourceCode = src.sourceCode;
        item.sourceDesc = src.sourceDesc;
        item.stdName    = src.stdName;
        item.note       = src.note;
        item.autoMatched = false;
        item.comboBox   = nullptr;
        m_items.append(item);
    }
    buildTable();
    onAutoMatch(); // 导入后自动匹配

    m_statusLabel->setText(
        QString("从 %1 导入了 %2 个标准图层").arg(QFileInfo(path).fileName()).arg(imported.size()));
}

// ====== 导出映射结果为CSV ======
void SeLayerMappingDialog::onExportCsv()
{
    QString path = QFileDialog::getSaveFileName(this,
        "导出国层映射CSV", "layer_mapping.csv",
        "CSV文件 (*.csv)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "导出失败", "无法写入文件：" + path);
        return;
    }

    QTextStream ts(&file);
    ts.setCodec("UTF-8");
    ts << "# 源图层名称,源图层代码,标准成果图层,实际数据SHP,几何类型,备注\n";

    for (const auto& item : m_items) {
        ts << item.sourceDesc << ","
           << item.sourceCode << ","
           << item.stdName << ","
           << item.actualShp << ","
           << item.geomType << ","
           << item.note << "\n";
    }
    file.close();

    m_statusLabel->setText(
        QString("已导出 %1 条映射记录到 %2").arg(m_items.size()).arg(QFileInfo(path).fileName()));
}

// ====== 保存并关闭 ======
void SeLayerMappingDialog::onSave()
{
    int unmatched = 0;
    for (const auto& item : m_items) {
        if (item.actualShp.isEmpty()) unmatched++;
    }
    if (unmatched > 0) {
        QMessageBox::StandardButton ret = QMessageBox::question(this, "确认保存",
            QString("还有 %1 个图层未映射，确定保存吗？\n未映射的图层在质检时将被跳过。").arg(unmatched));
        if (ret != QMessageBox::Yes) return;
    }
    accept();
}

// ====== 获取映射结果 ======
QList<LayerMappingItem> SeLayerMappingDialog::getMappingResult() const
{
    return m_items;
}

QHash<QString, QString> SeLayerMappingDialog::getActiveMapping() const
{
    QHash<QString, QString> mapping;
    for (const auto& item : m_items) {
        if (!item.actualShp.isEmpty())
            mapping[item.stdName] = item.actualShp;
    }
    return mapping;
}

QHash<QString, QStringList> SeLayerMappingDialog::getSourceToStdMapping() const
{
    QHash<QString, QStringList> mapping;
    for (const auto& item : m_items) {
        if (!item.sourceCode.isEmpty())
            mapping[item.sourceCode].append(item.stdName);
    }
    return mapping;
}

QHash<QString, QStringList> SeLayerMappingDialog::getTypedShpFiles() const
{
    QHash<QString, QStringList> typed;
    for (const auto& item : m_items) {
        if (item.actualShp.isEmpty()) continue;
        QString type = item.geomType;
        if (type == "unknown") {
            // 根据名称再次猜测
            typed["line"].append(item.actualShp);  // 默认按线处理
        }
        if (type == "面") typed["polygon"].append(item.actualShp);
        else if (type == "线") typed["line"].append(item.actualShp);
        else if (type == "点") typed["point"].append(item.actualShp);
    }
    return typed;
}
