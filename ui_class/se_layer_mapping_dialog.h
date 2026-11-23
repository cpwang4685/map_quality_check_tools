#ifndef SE_LAYER_MAPPING_DIALOG_H
#define SE_LAYER_MAPPING_DIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QString>
#include <QStringList>
#include <QList>
#include <QHash>

// ---- 单个图层映射项 ----
struct LayerMappingItem {
    QString sourceCode;     // 源图层代码（如 LRDL、BOUL、HYDL_HL）
    QString sourceDesc;     // 源图层描述（如 "公路"、"行政境界"、"河流"）
    QString stdName;        // 标准成果图层名（如 "国道"、"省界"、"一级河流"）
    QString actualShp;      // 实际SHP文件名（如 "国道.shp"）
    QString geomType;       // 几何类型：point / line / polygon / unknown
    QString note;           // 备注说明
    bool    autoMatched;    // 是否自动匹配
    QComboBox* comboBox;    // UI控件指针

    LayerMappingItem() : autoMatched(false), comboBox(nullptr) {}
};

// ---- 图层映射CSV格式兼容（主界面加载 / 映射弹窗导入导出共用） ----
// 收到的工作表数据行是“源图层代码,源图层名称,标准成果图层,...”（代码在前），
// 但表头文字写的是“源图层名称,源图层代码,...”（名称在前）；本工程旧配置又是
// 名称在前。程序不靠表头判断，而是按每行前两列的内容自动识别：
//   - 第1列像图层代码（ASCII 字母/数字/下划线）、第2列不像代码 → 代码在前
//   - 否则 → 名称在前（第1列允许为空，如 ",,0A级景区_选取,..."）
// 旧4列格式（源图层代码,源图层名称,成果图层名称,说明）继续兼容。
namespace LayerMappingCsv {

inline bool looksLikeSourceCode(const QString& s)
{
    const QString t = s.trimmed();
    if (t.isEmpty()) return false;
    for (const QChar c : t) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
            || (u >= '0' && u <= '9') || u == '_';
        if (!ok) return false;
    }
    return true;
}

inline bool isCodeFirst(const QStringList& parts)
{
    if (parts.size() < 6) return false;
    const QString c0 = parts.value(0).trimmed();
    const QString c1 = parts.value(1).trimmed();
    if (!looksLikeSourceCode(c0)) return false;
    // 第2列也是代码时无法判断（正常名称列不会全为ASCII代码），按名称在前处理
    if (!c1.isEmpty() && looksLikeSourceCode(c1)) return false;
    return true;
}

// 解析一行数据（不含表头/空行）。oldFormat 输出是否为旧4列格式。
inline void parseRow(const QStringList& parts, LayerMappingItem& item, bool& oldFormat)
{
    oldFormat = (parts.size() < 5);
    if (oldFormat) {
        item.sourceCode = parts.value(0).trimmed();
        item.sourceDesc = parts.value(1).trimmed();
        item.stdName    = parts.value(2).trimmed();
        item.note       = parts.value(3).trimmed();
        return;
    }
    if (isCodeFirst(parts)) {
        item.sourceCode = parts.value(0).trimmed();
        item.sourceDesc = parts.value(1).trimmed();
    } else {
        item.sourceDesc = parts.value(0).trimmed();
        item.sourceCode = parts.value(1).trimmed();
    }
    item.stdName   = parts.value(2).trimmed();
    item.actualShp = parts.value(3).trimmed();
    item.geomType  = parts.value(4).trimmed();
    item.note      = parts.value(5).trimmed();
}

// 导出/保存统一按收到的工作表列序：代码,名称,标准图层,实际SHP,几何类型,备注
inline QStringList codeFirstFields(const LayerMappingItem& item)
{
    QStringList f;
    f << item.sourceCode << item.sourceDesc << item.stdName
      << item.actualShp << item.geomType << item.note;
    return f;
}

// 表头保持与收到的工作表一致（表头只是注释行，程序不读取）
inline QString headerLine()
{
    return QStringLiteral("# 源图层名称,源图层代码,标准成果图层,实际数据SHP,几何类型,备注");
}

} // namespace LayerMappingCsv

class QTableWidget;
class QPushButton;
class QLabel;

// ====== 图层映射配置弹窗 ======
// 左侧：标准成果图层名（从CSV/Excel/知识库读取）
// 右侧：下拉框选择实际数据中的SHP文件
class SeLayerMappingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SeLayerMappingDialog(QWidget* parent = nullptr);
    ~SeLayerMappingDialog() override;

    // 设置标准图层列表（从CSV/配置中读取）
    void setStandardLayers(const QList<LayerMappingItem>& items);

    // 设置数据目录中的SHP文件列表
    void setDataShpFiles(const QStringList& shpFiles);

    // 设置可选的源图层代码列表（供下拉筛选用）
    void setSourceCodes(const QStringList& codes);

    // 获取最终映射结果
    QList<LayerMappingItem> getMappingResult() const;

    // 获取有效映射（排除未匹配的项）：stdName → actualShp
    QHash<QString, QString> getActiveMapping() const;

    // 获取按源图层代码分组的映射：sourceCode → stdName
    QHash<QString, QStringList> getSourceToStdMapping() const;

    // 获取按几何类型分组的SHP列表
    QHash<QString, QStringList> getTypedShpFiles() const;

private slots:
    void onAutoMatch();
    void onImportCsv();
    void onExportCsv();
    void onSave();

private:
    void buildTable();
    void updateGeomTypeForRow(int row);
    QString autoMatchShp(const QString& stdName) const;
    QString guessGeomType(const QString& shpName) const;

    QTableWidget* m_table = nullptr;
    QLabel* m_statusLabel = nullptr;
    QList<LayerMappingItem> m_items;
    QStringList m_dataShpFiles;
    QStringList m_sourceCodes;
};

#endif // SE_LAYER_MAPPING_DIALOG_H
