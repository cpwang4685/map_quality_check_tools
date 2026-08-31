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
