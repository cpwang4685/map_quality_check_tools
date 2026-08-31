#ifndef SE_DATA_LIST_EXPORT_H
#define SE_DATA_LIST_EXPORT_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QTreeWidgetItem>
#include <QList>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QPoint>
#include <qgsrectangle.h>   // QGIS 3 标准小写头；驼峰 QgsRectangle.h 仅 Windows SDK 有，麒麟无
#include "se_database_connection.h"

class QgisInterface;
class QgsMapCanvas;
class QgsVectorLayer;
class QgsCoordinateReferenceSystem;
class QgsMapTool;
class QgsRubberBand;
class QgisInterface;
class QStandardItem;
class QStandardItemModel;
class QStandardPaths;
class QStorageInfo;
class QAbstractItemView;
class QTreeWidget;
class QTextEdit;
class QPushButton;
class QCheckBox;
class QLineEdit;
class QComboBox;
class QLabel;
class QFormLayout;
class QGroupBox;
class QStackedWidget;
class QDoubleSpinBox;
class QGridLayout;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QValidator;
class QRegularExpression;
class QStackedLayout;
class QGridLayout;
class QRect;
class QMouseEvent;
class QKeyEvent;
class QEvent;
class QShowEvent;
class QResizeEvent;
class QCloseEvent;
class QDropEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDir;
class QFileInfo;
class QTimer;
class QMenu;
class QAction;
class QWidget;
class QHBoxLayout;
class QVBoxLayout;
class QVariant;
class QDateTimeEdit;
class QPointF;
class QMessageBox;

class CMapToolDrawShape;
class QStandardItem;
class QStandardItemModel;
class QStandardPaths;
class QStorageInfo;
class QAbstractItemView;

class QListWidgetItem;
class QDate;
class QTime;
class QSpinBox;
class QDial;
class QToolBar;
class QToolButton;
class QStatusBar;
class QMainWindow;
class QTranslator;
class QSplitter;
class QTabWidget;
class QTabBar;
class QTableWidget;
class QTableWidgetItem;
class QTableView;
class QTreeView;
class QHeaderView;
class QStyledItemDelegate;
class QItemDelegate;
class QProgressBar;
class QProgressDialog;
class QSlider;
class QScrollBar;
class QScrollArea;
class QAbstractSlider;
class QAbstractScrollArea;
class QFrame;
class QLabel;
class QLCDNumber;
class QLineEdit;
class QListView;
class QComboBox;
class QFontComboBox;
class QFont;
class QFontDatabase;
class QFontInfo;
class QFontMetrics;
class QFontMetricsF;
class QGlyphRun;
class QTextLayout;
class QTextLine;
class QTextCursor;
class QTextBlock;
class QTextBlockFormat;
class QTextCharFormat;
class QTextDocumentFragment;
class QTextEdit;
class QTextBrowser;
class QTextDocument;
class QTextFormat;
class QTextLength;
class QTextObject;
class QTextInlineObject;
class QTextOption;
class QTextBoundaryFinder;
class QTextCodec;
class QTextDecoder;
class QTextEncoder;
class QTextStream;

class QImage;
class QImageReader;
class QImageWriter;
class QImageIOHandler;
class QImageIOPlugin;
class QImageTextHandler;
class QPixmap;
class QPixmapCache;
class QPixmapFilter;
class QBitmap;
class QIcon;
class QIconEngine;
class QPicture;
class QPictureIO;
class QMovie;
class QPainter;
class QPainterPath;
class QPainterPathStroker;
class QPaintEngine;
class QPaintDevice;
class QPaintEngineState;
class QPaintEvent;
class QRegion;
class QPolygon;
class QPolygonF;
class QRectF;
class QSize;
class QSizeF;
class QTransform;
class QMatrix;
class QMatrix4x4;
class QVector2D;
class QVector3D;
class QVector4D;
class QQuaternion;
class QGenericMatrix;
class QGenericVector;
class QGenericArgument;
class QGenericReturnArgument;
class QMetaMethod;
class QMetaProperty;
class QMetaType;
class QMetaObject;
class QMetaEnum;
class QMetaClassInfo;
class QObject;
class QSignalMapper;
class QSignalBlocker;
class QSocketNotifier;

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QAuthenticator;
class QNetworkProxy;
class QNetworkProxyFactory;
class QNetworkProxyQuery;
class QNetworkCacheMetaData;
class QAbstractNetworkCache;
class QNetworkDiskCache;
class QNetworkCookie;
class QNetworkCookieJar;
class QNetworkConfiguration;
class QNetworkConfigurationManager;
class QNetworkSession;
class QNetworkInterface;
class QNetworkAddressEntry;
class QHostAddress;
class QHostInfo;
class QDnsLookup;
class QAbstractSocket;
class QTcpSocket;
class QSslSocket;

class QLocale;
class QTimeZone;
class QCalendar;
class QGregorianCalendar;
class QJulianCalendar;
class QMilankovicCalendar;
class QRomanCalendar;

class QUrl;
class QUrlQuery;
class QUrlIdna;

class QXmlStreamAttribute;
class QXmlStreamAttributes;
class QXmlStreamReader;
class QXmlStreamWriter;

class QJsonDocument;
class QJsonObject;
class QJsonArray;
class QJsonValue;
class QJsonParseError;

class QSqlDatabase;
class QSqlDriver;
class QSqlError;
class QSqlField;
class QSqlIndex;
class QSqlQuery;
class QSqlQueryModel;
class QSqlRecord;
class QSqlRelation;
class QSqlTableModel;
class QSqlResult;

class QThread;
class QMutex;
class QMutexLocker;
class QWaitCondition;

class QUuid;
class QByteArray;
class QBitArray;

// "按范围导出"绘制形状
enum DrawShapeType
{
	DrawRect = 0,
	DrawCircle = 1,
	DrawPolygon = 2
};

// 导出类型枚举（与执行流程分支对应）
enum ExportType
{
	ExportBatch = 0,
	ExportCondition = 1,
	ExportRange = 2,
	ExportMainArea = 3
};

// “按范围导出”范围获取方式
enum RangeMode
{
	RangeInputCoord = 0, // 输入坐标
	RangeManualDraw = 1, // 手动绘制
	RangeSelectFeature = 2 // 选择要素
};

// 数据列表节点类型（用于在 QTreeWidgetItem 的 UserRole+1 中保存枚举值）
enum NodeType
{
	NodeDir = 0,         // 本地目录
	NodeLocalFile = 1,   // 单个本地文件
	NodeDbRoot = 2,      // 数据库连接根
	NodeDbTable = 3,     // 数据库表（PG/SQLite）
	NodeGdbRoot = 4,     // GDB 文件根
	NodeGdbLayer = 5,    // GDB 图层
	NodeMapLayerRoot = 6,// 当前 QGIS 工程中地图图层根
	NodeMapLayer = 7     // 单个地图图层
};

// ============================================================
// 在地图上绘制矩形/圆/多边形等形状的 QObject 工具（moc 必须看到完整定义）
// ============================================================
class CMapToolDrawShape : public QObject
{
	Q_OBJECT
public:
	CMapToolDrawShape(QgsMapCanvas* canvas, DrawShapeType type);
	~CMapToolDrawShape() override;

	void activate();
	void deactivate();
	void finish();

signals:
	void shapeFinished(double minX, double minY, double maxX, double maxY);
	void drawCancelled();

private:
	QgsMapCanvas* m_pCanvas = nullptr;
	DrawShapeType m_drawType = DrawRect;
	// 实际绘图工具（实现细节：cpp 内部 QDrawShapeMapTool 类型）
	// 头文件中仅以 void* 暴露，避免 moc/qmake 需要解析 QDrawShapeMapTool 的全部定义
	void* m_pMapTool = nullptr;
};

class CSE_DataListExportDialog : public QDialog
{
	Q_OBJECT

public:
	explicit CSE_DataListExportDialog(QWidget* parent = nullptr);
	~CSE_DataListExportDialog() override;

	void setQgisInterface(QgisInterface* iface);

	// 主界面"导出后自动加载到地图"开关（影响批量/条件/范围三种导出）
	bool autoLoadAfterExport() const;

public slots:
	void on_Button_BrowseRoot_clicked();
	void on_Button_Refresh_clicked();
	void on_Button_ExpandAll_clicked();
	void on_Button_CollapseAll_clicked();
	void on_treeWidgetDataList_customContextMenuRequested(const QPoint& pos);
	void reject() override;

private:
	QgisInterface* m_pQgisIface = nullptr;
	QgsMapCanvas* m_pMapCanvas = nullptr;

	// 手动绘制返回的"原始画布 CRS 下的范围"，用于 exportFiles() 做真实空间过滤。
	// 对话框 spinbox 显示的是经纬度(EPSG:4326)副本，导出时必须用原始 CRS 的 m_drawnRectMap。
	QgsRectangle m_drawnRectMap;

	QString m_strRootDir;
	QString m_strOutputDir;

	QTreeWidget* m_pTree = nullptr;
	QTextEdit* m_pLog = nullptr;
	QPushButton* m_btnConnectDb = nullptr;
	QPushButton* m_btnRefreshLayers = nullptr;
	QCheckBox* m_chkAutoLoadAfterExport = nullptr; // 主界面"导出后自动加载到地图"开关

	// 已保存的数据库连接列表（连接名称 + 参数），生命周期内有效
	QList<DatabaseConnectionInfo> m_dbConns;

private:
	void populateDataList();
	void addDirNode(QTreeWidgetItem* parent, const QString& dirPath);
	void populateDatabaseRoot();
	void populateMapLayersRoot();
	static bool isDataFile(const QString& filePath);
	static bool isRasterPath(const QString& filePath);
	static bool isGdbPath(const QString& filePath);
	QStringList collectDataFiles(const QString& dirPath, bool bIncludeSubDirs) const;
	QStringList collectFromFile(const QString& filePath) const;

	int exportFiles(const QStringList& files, const QString& outDir,
		const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
		const QString& strNameFilter,
		const QString& strUserFilter, const QString& strTypeFilter,
		const QDateTime& dtStart, const QDateTime& dtEnd,
		bool bUseTime, QString& errMsg, QStringList* writtenPaths = nullptr);
	bool exportSingleFile(const QString& srcPath, const QString& outDir,
		const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
		const QString& strNameFilter,
		const QString& strUserFilter, const QString& strTypeFilter,
		const QDateTime& dtStart, const QDateTime& dtEnd,
		bool bUseTime, QString& errMsg, QStringList* writtenPaths = nullptr);
	// 单数据源导出到用户选择的单个文件（按源类型补全 .shp/.tif 扩展名），返回实际输出路径
	bool exportSingleToFile(const QString& srcPath, const QString& pickPath,
		const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
		QString& dstPathOut, QString& errMsg);
	bool exportVectorFile(const QString& srcPath, const QString& dstPath,
		const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
		const QString& strNameFilter,
		const QString& strUserFilter, const QString& strTypeFilter,
		const QDateTime& dtStart, const QDateTime& dtEnd,
		bool bUseTime, QString& errMsg);
	bool exportRasterFile(const QString& srcPath, const QString& dstPath,
		const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
		const QString& strNameFilter,
		const QString& strUserFilter, const QString& strTypeFilter,
		const QDateTime& dtStart, const QDateTime& dtEnd,
		bool bUseTime, QString& errMsg);

	void appendLog(const QString& msg);
	void showResult(bool bOk, const QString& title, const QString& successMsg, const QString& failMsg);
	// 交互式在地图上绘制范围（非模态环境下阻塞等待绘制完成）。返回 true 表示成功取得矩形范围。
	// shape 用于指定绘制形状（矩形/圆/多边形），从 CSE_RangeExportDialog 透传过来。
	bool interactiveDrawExtent(QgsRectangle& outRect, DrawShapeType shape);
	void doBatchExport(const QString& dirPath);
	void doConditionExport(const QString& dirPath);
	void doRangeExport(const QString& dirPath);
	void loadDataFileToMap(const QString& filePath);
	QString browseOutputDir();
	static bool matchTime(const QFileInfo& fi, const QDateTime& dtStart, const QDateTime& dtEnd);

	// 连接数据库
	void on_Button_ConnectDb_clicked();
	// 刷新列表中的数据库与地图图层节点
	void on_Button_RefreshLayers_clicked();

	// 处理数据库/地图图层节点的导出与显示（依据 item 携带的 NodeType）
	void doExportForItem(QTreeWidgetItem* item);
	void loadItemToMap(QTreeWidgetItem* item);
	// 根据表项信息构造 PG 矢量 URI 或栅格驱动路径，供导出/加载使用
	QString buildDbTableUri(const DatabaseConnectionInfo& conn, const QString& schema,
		const QString& tableName, bool bRaster) const;
	// 导出完成后自动加载到地图的辅助方法
	void autoLoadToMap(const QString& filePath);          // 加载单文件
	void autoLoadDirToMap(const QString& outDir);         // 批量加载目录下所有 shapefile/栅格
	// 读取主区矢量中用户所选要素的合并外包矩形，作为"按主区裁切导出"裁剪范围
	// outCrs（可选）：返回该矩形所在的主区 layer 原始 CRS，供导出时把裁剪范围正确转换到源数据 CRS
	QgsRectangle computeMainAreaClipRect(const QString& mainAreaPath,
		const QString& fieldName, const QStringList& featureIds,
		QgsCoordinateReferenceSystem* outCrs = nullptr) const;
};

// ==================== 按条件导出对话框 ====================
class CSE_ConditionExportDialog : public QDialog
{
	Q_OBJECT

public:
	explicit CSE_ConditionExportDialog(QWidget* parent = nullptr);
	~CSE_ConditionExportDialog() override;

	QString dataName() const;
	QString userName() const;
	QString dataType() const;
	QDateTime startTime() const;
	QDateTime endTime() const;
	QString outputPath() const;

private slots:
	void on_Button_BrowseOutput_clicked();
	void on_Button_OK_clicked();
	void on_Button_Cancel_clicked();

private:
	class Private;
	Private* d;
};

// ==================== 按范围导出对话框 ====================
class CSE_RangeExportDialog : public QDialog
{
	Q_OBJECT

public:
	CSE_RangeExportDialog(QgsMapCanvas* canvas, QgisInterface* iface, QWidget* parent = nullptr);
	~CSE_RangeExportDialog() override;

	QgsRectangle exportExtent() const;
	bool hasExtent() const;
	QString outputPath() const;

	// 预先填充已绘制的范围（重新弹出对话框时用）
	void setExtent(const QgsRectangle& rect);
	// 用户是否点击了"开始绘制"（若为 true，调用方应在对话框关闭后激活绘制工具再重新打开对话框）
	bool drawRequested() const;
	// 复位"开始绘制"标记（对话框复用场景下，每次 exec() 前必须调用，避免上一轮的绘制请求被误判）
	void resetDrawRequested();
	// 用户在"手动绘制"页选中的绘制类型（矩形/圆/多边形）
	DrawShapeType selectedDrawType() const;
	// 记住并恢复用户在"范围获取方式"中的当前选择（输入坐标 / 手动绘制 / 选择要素）
	// 用于 doRangeExport 中重新弹出对话框后保持上下文一致
	void setModeIndex(int idx);
	int modeIndex() const;
	// 记住并恢复用户在"绘制类型"中的当前选择（矩形 / 圆 / 多边形）
	void setDrawTypeIndex(int idx);
	int drawTypeIndex() const;
	// 是否在导出后自动加载结果到地图
	bool autoLoadAfterExport() const;

private slots:
	void on_Button_BrowseOutput_clicked();
	void on_Button_OK_clicked();
	void on_Button_Cancel_clicked();
	void onShapeFinished(double minX, double minY, double maxX, double maxY);
	void onDrawCancelled();
	void onModeChanged(int index);
	void on_Button_LoadFeatures_clicked();

private:
	class Private;
	Private* d;
};

// ==================== 按主区裁切导出对话框（精简版） ====================
// 与数据管理对话框不同：本对话框隐藏了所有"其它裁切方式/GDB/数据库/PostGIS"等 tab，
// 只暴露"主区数据 -> 标识字段 -> 选要素 -> 输出 -> 纸张 -> 比例尺 -> 内图廓"这一条主区流程，
// 是"右键数据列表 -> 按主区裁切导出"的一级入口
class CSE_MainAreaExportDialog : public QDialog
{
	Q_OBJECT

public:
	explicit CSE_MainAreaExportDialog(QWidget* parent = nullptr);
	~CSE_MainAreaExportDialog() override;

	// 设置或获取当前数据列表中右键选中的源数据路径（可指向单文件或目录）
	void   setSourcePath(const QString& path) { d_srcPath = path; }
	QString sourcePath() const { return d_srcPath; }

	// 主区数据
	QString mainAreaPath()   const;
	QString mainAreaField()  const;
	QStringList selectedFeatureIds() const;

	// 输出设置
	QString outputFilePath() const;
	QString paperSize()      const;     // "A2" / "自定义" 等
	QString paperOrient()    const;     // "自动" / "纵向" / "横向"
	QString standardScale()  const;     // 字符串，如 "10000"
	bool    useCustomScale() const;
	double  mapScaleDenominator() const;
	double  innerMapFrameWidthMm()  const;
	double  innerMapFrameHeightMm() const;
	bool    enableInnerFrame() const;
	// 当前生效的纸张宽度/高度（mm），A0~A4 或自定义
	double  paperWidthMm()   const;
	double  paperHeightMm()  const;
	// 是否在导出后自动加载结果到地图
	bool    autoLoadAfterExport() const;

private slots:
	void on_Button_BrowseMainArea_clicked();
	void on_Button_BrowseOutput_clicked();
	void onPaperSizeChanged(int index);
	void onBtnComputeScale();          // "自动计算"比例尺
	void onBtnComputePaper();          // "计算"：根据主区范围自动确定纸张方向与大小
	void onCheckCustomScaleToggled(bool); // "使用自定义比例尺"勾选联动
	void onCheckEnableInnerToggled(bool); // "启用内图廓"勾选联动
	void on_Button_OK_clicked();
	void on_Button_Cancel_clicked();

private:
	void buildUi();
	void loadMainAreaFields();
	// 根据当前 comboField 选择重新填充要素列表（comboField 切换时调用）
	void reloadFeatures();
	void applyPaperSizeVisibility();
	// 内图廓宽/高输入框根据"启用内图廓"勾选置灰/启用
	void applyInnerFrameEnabled(bool enabled);
	// 当前已选要素的合并外包矩形（在主区 layer 的原始 CRS 下）
	QgsRectangle selectedMainAreaRect() const;
	// 按给定纸张宽高(mm) + 主区外包矩形，计算一个与纸幅匹配的"整比例尺"并写回标准下拉框
	void computeScaleFromExtent();

	class Private;
	Private* d;

	QString d_srcPath;
	// 缓存主区 layer，以便 comboField 切换时无须重新读取整个 shp
	QgsVectorLayer* m_pMainAreaLayer = nullptr;
};

#endif // SE_DATA_LIST_EXPORT_H