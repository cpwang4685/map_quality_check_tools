#ifndef SE_DATA_IMPORT_H
#define SE_DATA_IMPORT_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QList>

#include <QProcess>

// ============== 元数据字段定义（从XML配置动态读取）==============
// 每个 SeMetadataFieldDef 对应 XML 中一个 <field> 节点，
// 用于动态生成 CREATE TABLE 和 INSERT 语句，避免硬编码表结构。
struct SeMetadataFieldDef {
    QString name;        // 字段名
    QString type;        // PostgreSQL 数据类型
    QString defaultVal;  // 默认值（空字符串表示无默认值）
    bool    notNull;     // 是否 NOT NULL
    bool    primaryKey;  // 是否主键
    QString value;       // 导入时的值填充令牌（空字符串表示跳过，留 NULL）
};

// ============== 元数据上下文（WriteMetadata 时收集的所有值）==============
// 将所有可计算的值聚合成一个结构体，供 ResolveMetadataValue() 统一替换令牌。
struct MetadataContext {
    // 文件信息
    QString  fileName;
    QString  filePath;
    double   fileSizeMB = 0.0;
    QString  tableName;
    QString  schema;

    // 数据类型
    QString  dataType;       // SHP / GeoJSON / GPKG / GDB / TIF
    QString  dataFormat;     // 同 dataType，用于 data_format 列
    QString  metaDataType;   // "vector" 或 "raster"
    int      srid = 0;
    QString  encoding;
    QString  dataSource;
    QString  description;
    QString  uploadUser;

    // 矢量 — 来自 PostGIS 查询
    int      featureCount = 0;
    QString  geometryType;
    double   westLongitude  = 0.0;
    double   eastLongitude  = 0.0;
    double   southLatitude  = 0.0;
    double   northLatitude  = 0.0;
    int      fieldCount = 0;
    QString  attributeFields;   // JSON 数组字符串
    bool     hasZValue = false;
    bool     hasMValue = false;
    QString  shpType;

    // 栅格 — 来自 gdalinfo
    bool     rasterValid = false;
    int      bandCount = 0;
    double   resolutionX = 0.0;
    double   resolutionY = 0.0;
    int      colCount = 0;
    int      rowCount = 0;
    QString  pixelType;
    int      pixelDepth = 0;
    QString  compressionType;
    QString  colorInterpretation;
    double   noDataValue = 0.0;
    bool     hasNoData = false;
    QString  geotransform;      // JSON 数组字符串
    QString  projectionWkt;
    QString  bandInfo;          // JSON 数组字符串
    bool     hasOverviews = false;
};

class QGroupBox;
class QLineEdit;
class QSpinBox;
class QRadioButton;
class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;
class QComboBox;
class QListWidget;

class CSE_DataImportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CSE_DataImportDialog(QWidget* parent = nullptr,
                                  Qt::WindowFlags fl = Qt::WindowFlags());
    ~CSE_DataImportDialog() override;

private:
    // ============ UI构建 ============
    void InitUI();
    void LoadSettings();
    void SaveSettings();

    // ============ 数据库连接控件 ============
    QLabel*       m_pLabelHost;
    QLineEdit*    m_pEditHost;
    QLabel*       m_pLabelPort;
    QSpinBox*     m_pSpinPort;
    QLabel*       m_pLabelDatabase;
    QLineEdit*    m_pEditDatabase;
    QLabel*       m_pLabelSchema;
    QLineEdit*    m_pEditSchema;
    QLabel*       m_pLabelUsername;
    QLineEdit*    m_pEditUsername;
    QLabel*       m_pLabelPassword;
    QLineEdit*    m_pEditPassword;
    QPushButton*  m_pBtnTestConn;

    // ============ 数据类型控件 ============
    QRadioButton* m_pRadioShp;
    QRadioButton* m_pRadioGeoJson;
    QRadioButton* m_pRadioGpkg;
    QRadioButton* m_pRadioGdb;
    QRadioButton* m_pRadioTif;
    QRadioButton* m_pRadioFolder;

    // ============ 导入选项控件 ============
    QLabel*       m_pLabelSrid;
    QComboBox*    m_pComboSrid;
    QLabel*       m_pLabelEncoding;
    QComboBox*    m_pComboEncoding;
    QLabel*       m_pLabelTileSize;
    QSpinBox*     m_pSpinTileSize;
    QCheckBox*    m_pChkSpatialIndex;
    QCheckBox*    m_pChkWriteMetadata;

    // ============ 输入设置控件 ============
    QLabel*       m_pLabelDataPath;
    QLineEdit*    m_pEditDataPath;
    QPushButton*  m_pBtnBrowse;
    QLabel*       m_pLabelTableName;
    QLineEdit*    m_pEditTableName;
    QCheckBox*    m_pChkOverwrite;
    QCheckBox*    m_pChkLoadAfter;

    // ============ GDB图层列表（仅GDB时显示）============
    QGroupBox*    m_pGrpGdbLayers;
    QLabel*       m_pLabelGdbLayers;
    QListWidget*  m_pListGdbLayers;
    QPushButton*  m_pBtnRefreshLayers;

    // ============ 元数据信息 ============
    QLabel*       m_pLabelDataSource;
    QLineEdit*    m_pEditDataSource;
    QLabel*       m_pLabelDescription;
    QLineEdit*    m_pEditDescription;

    // ============ 进度与操作 ============
    QProgressBar* m_pProgressBar;
    QLabel*       m_pLabelStatus;
    QPushButton*  m_pBtnImport;
    QPushButton*  m_pBtnCancel;   // 批量导入时显示的停止按钮
    QPushButton*  m_pBtnMetadataViewer;  // 元数据管理
    QPushButton*  m_pBtnClose;

    // ============ QProcess ============
    QProcess*     m_pProcess;

    // ============ 导入状态追踪 ============
    int           m_iTotalItems;
    int           m_iSuccessCount;
    int           m_iFailCount;
    bool          m_bCancelBatchImport;  // 批量导入取消标志
    QString       m_qstrCurrentTableName;  // 当前正在导入的表名（元数据用）

    // ============ 辅助 ============
    /// 拼接PostgreSQL连接字符串 "PG:host=... port=... dbname=... user=... password=..."
    QString BuildPGConnString() const;

    /// 从SRID下拉框获取当前EPSG编码（预设项取data，手动输入解析文本）
    int GetSrid() const;

    /// 导入专用：连接串中附加 client_encoding，让 PG 将源编码转为 UTF-8 存储
    QString BuildPGConnStringForImport() const;

    /// QGIS 加载图层专用：用 QgsDataSourceUri 构建合法的 PostGIS 图层 URI
    QString BuildDataSourceUri(const QString& schema, const QString& tableName) const;

    /// 拼接psql连接参数 "-h host -p port -d dbname -U user"
    QStringList BuildPsqlArgs() const;

    /// 拼接psql环境变量（PGPASSWORD）
    void SetPsqlPassword() const;

    /// 为 QProcess 设置 PGPASSWORD / PGCLIENTENCODING 环境变量
    /// clientEncoding: ogr2ogr 用源数据编码让PG转码; psql 传空不设
    void SetupProcessEnv(QProcess& proc, const QString& clientEncoding = QString()) const;

    /// 获取当前选中数据类型对应的文件过滤器
    QString GetFileFilter() const;

    /// 获取当前选中数据类型的名称
    QString GetDataTypeName() const;

    /// 获取数据类型标识符 (shp/geojson/gpkg/gdb/tif)
    QString GetDataTypeId() const;

    /// 扫描目录下所有支持的文件
    QStringList ScanSupportedFiles(const QString& dirPath) const;

    /// 从文件路径提取默认表名 (去除扩展名、替换特殊字符)
    QString DefaultTableName(const QString& filePath) const;

    /// 拼接完整 ogr2ogr 参数列表
    /// @param dateFields 日期字段名列表（用于 -lco COLUMN_TYPES，避免 0000/00/00 报错）
    QStringList BuildOgr2ogrArgs(const QString& inputPath, const QString& tableName,
                                 const QStringList& dateFields = QStringList()) const;

    /// 拼接 raster2pgsql 参数列表
    QStringList BuildRaster2pgsqlArgs(const QString& inputPath, const QString& tableName) const;

    /// 列出GDB中所有图层名
    QStringList ListGdbLayers(const QString& gdbPath) const;

    /// 扫描数据源中的 Date/DateTime 字段名
    /// @param filePath  数据源路径（SHP/GDB/GPKG等）
    /// @param layerName GDB图层名（非GDB可留空）
    /// @return Date或DateTime类型的字段名列表
    QStringList ScanDateFields(const QString& filePath, const QString& layerName = QString()) const;

    /// 导入后将日期字段从 TEXT 转回 DATE/TIMESTAMP（处理 0000/00/00 → NULL）
    /// @param schema    目标PostgreSQL schema
    /// @param tableName 目标表名
    /// @param filePath  源数据路径（用于 ogrinfo 回查字段类型）
    /// @param layerName GDB图层名（非GDB可留空）
    /// @return 全部转换成功返回true
    bool FixDateColumns(const QString& schema, const QString& tableName,
                        const QString& filePath, const QString& layerName = QString());

    /// 从XML配置文件动态加载元数据表字段定义
    /// @param outFields 输出：解析出的所有字段定义列表
    /// @return 成功返回true，XML文件缺失或解析失败返回false
    bool LoadMetadataConfig(QList<SeMetadataFieldDef>& outFields);

    /// 确保元数据表存在（如不存在则根据XML配置动态创建）
    bool EnsureMetadataTable();

    /// 将 XML 中的 value 令牌替换为实际值
    /// @param token  XML 中 field 的 value 属性值（如 "{file_name}"）
    /// @param ctx    当前导入的上下文数据
    /// @return 替换后的 SQL 值表达式，如 "$$roads.shp$$"；若无法解析则返回空字符串
    QString ResolveMetadataValue(const QString& token, const MetadataContext& ctx) const;

    /// 写入一条元数据记录（完全由XML配置驱动）
    /// 1) 加载 XML → 获取字段定义及 value 令牌
    /// 2) 构建 MetadataContext 上下文
    /// 3) 对每个有 value 的字段调用 ResolveMetadataValue() 替换令牌
    /// 4) 动态拼接 INSERT 语句执行
    bool WriteMetadata(const QString& tableName, const QString& originalFile,
                       const QString& dataType, qint64 fileSizeBytes);

    /// 获取PostGIS表的要素数和几何类型
    bool GetTableInfo(const QString& tableName, int& featureCount,
                      QString& geometryType, QString& bboxWkt);

    /// 更新状态栏文字
    void UpdateStatus(const QString& message);

    /// 更新进度条
    void UpdateProgress(int current, int total);

    /// 执行SQL（通过psql）
    bool ExecuteSql(const QString& sql);

    /// 显示/隐藏数据类型相关控件
    void UpdateControlVisibility();

private slots:
    void slotBrowse();
    void slotTestConnection();
    void slotStartImport();
    void slotClose();
    void slotProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotProcessError(QProcess::ProcessError error);
    void slotUpdateDataType();
    void slotRefreshGdbLayers();
    void slotCancelBatchImport();
    void slotOpenMetadataViewer();
};

#endif // SE_DATA_IMPORT_H
