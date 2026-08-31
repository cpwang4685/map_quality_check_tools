#ifndef SE_METADATA_VIEWER_H
#define SE_METADATA_VIEWER_H

#include <QDialog>
#include <QString>

class QTableView;
class QSqlTableModel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QSortFilterProxyModel;

// ================================================================
//  元数据查看/编辑对话框
//  ================================================================
//  功能：以表格形式展示 gis_metadata 元数据表，支持：
//    - 浏览所有入库数据的元数据记录
//    - 双击单元格直接编辑
//    - 按列筛选（列名下拉 + 关键词输入）
//    - 删除选中行
//    - 保存修改到数据库
//  ================================================================

class CSEMetadataViewerDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param host     数据库主机
    /// @param port     数据库端口
    /// @param database 数据库名
    /// @param username 用户名
    /// @param password 密码
    /// @param schema   元数据表所在 schema
    /// @param parent   父窗口
    explicit CSEMetadataViewerDialog(const QString& host,
                                     int port,
                                     const QString& database,
                                     const QString& username,
                                     const QString& password,
                                     const QString& schema,
                                     QWidget* parent = nullptr);
    ~CSEMetadataViewerDialog() override;

private:
    void InitUI();
    void OpenDatabase();
    void LoadTableData();

    // ========== 数据库连接 ==========
    QString  m_host;
    int      m_port;
    QString  m_database;
    QString  m_username;
    QString  m_password;
    QString  m_schema;
    QString  m_connName;       // QSqlDatabase 连接名（唯一标识）

    // ========== UI 控件 ==========
    QTableView*            m_pTableView;
    QSqlTableModel*        m_pModel;
    QSortFilterProxyModel* m_pProxyModel;

    QLabel*       m_pLabelFilterCol;
    QComboBox*    m_pComboFilterCol;
    QLabel*       m_pLabelFilter;
    QLineEdit*    m_pEditFilter;
    QPushButton*  m_pBtnRefresh;
    QPushButton*  m_pBtnSave;
    QPushButton*  m_pBtnDelete;
    QPushButton*  m_pBtnClose;

    QLabel*       m_pLabelRowCount;

private slots:
    void slotRefresh();
    void slotSave();
    void slotDeleteRow();
    void slotApplyFilter(const QString& text);
};

#endif // SE_METADATA_VIEWER_H
