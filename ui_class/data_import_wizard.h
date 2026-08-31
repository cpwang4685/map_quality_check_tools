#ifndef DATA_IMPORT_WIZARD_H
#define DATA_IMPORT_WIZARD_H

#include <QDialog>
#include <QStackedWidget>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QMap>
#include <QVector>
#include <QSet>
#include <QString>

#include "database/product_metadata.h"
#include "database/product_dao.h"
#include "core/directory_helper.h"

class DataImporter;
class ProductDAO;

/**
 * 单条元数据字段定义（从 CSV 解析）
 */
struct MetadataFieldDef
{
	QString name;   // 数据元素名称（中文显示名）
	QString key;    // 数据元素简称（英文字段标识）
	QString type;   // 数据类型
	QString length; // 长度
	QString remark; // 备注
};

/**
 * 入库数据类型枚举
 */
enum class ImportDataType
{
	Vector = 0,   // 矢量文件 (.shp .geojson .kml .gdb .mdb)
	Raster = 1,   // 栅格文件 (.tif .tiff .img .jpg .jp2)
	Other  = 2,   // 制图文件 (.pdf .ai .cdr .dwg .dxf)
	All    = 3    // 其它类型文件 (文档、压缩包、无法识别格式等 *.*)
};

/**
 * 数据导入向导对话框
 *
 * - 第一页：选择入库数据类型
 * - 第二页：设置数据目录 + 填写元数据 + 入库日志 + 入库进度
 * - 顶部：数据库用户与权限信息
 */
class DataImportWizard : public QDialog
{
	Q_OBJECT

public:
	explicit DataImportWizard(QWidget* parent = nullptr);
	~DataImportWizard() override;

private slots:
	void onNextPage();
	void onPrevPage();
	void onBrowseDir();
	void onStartImport();
	void onImportProgress(int percent, const QString& message);
	void onImportFinished(const QString& tableName, int featureCount);
	void onImportFailed(const QString& tableName, const QString& error);
	void onToggleLog();

private:
	void setupUi();
	void loadMetadataFields(ImportDataType dataType);
	void updateImportButton();
	QString dataTypeFilter() const;
	ImportDataType selectedDataType() const;
	QStringList scanFiles(const QString& dirPath) const;

	/** @brief 向日志窗口追加一条消息 */
	void appendLog(const QString& message, const QString& color = "#2c3e50");

	/** @brief 根据产品名查找数据库中已存在的同名产品（返回 first match） */
	ProductMetadata findExistingProductByName(const QString& productName);

	// ---- 页面容器 ----
	QStackedWidget* mStackedWidget;

	// ======== Page 0: 选择数据类型 ========
	QWidget*                mPageType;
	QButtonGroup*           mTypeGroup;
	QRadioButton*           mRadioVector;
	QRadioButton*           mRadioRaster;
	QRadioButton*           mRadioOther;
	QRadioButton*           mRadioAll;

	// ======== Page 1: 数据目录 + 元数据 + 日志 + 入库 ========
	QWidget*                mPageImport;
	QLineEdit*              mDirEdit;
	QPushButton*            mBtnBrowse;
	QLabel*                 mDirInfoLabel;

	// 栅格子类别选择（仅栅格类型显示）
	QLabel*                 mRasterSubcategoryLabel;
	QComboBox*              mRasterSubcategoryCombo;

	// 地图成果目录挂载（数据导入时自动归类）
	ProductDAO              mDirDao;
	DirectoryHelper::FixedDirectoryIds mDirIds;
	bool                    mIsSingleVectorSource = false;
	QSet<int>               mImportUsedDirs;      // 本次导入动态使用的命名目录 id
	QSet<int>               mImportPopulatedDirs; // 本次导入成功挂载产品的目录 id

	// 元数据动态表单容器
	QScrollArea*            mMetadataScroll;
	QGroupBox*              mBasicMetaGroupBox;        // "基础元数据" 分组
	QFormLayout*            mBasicMetaFormLayout;
	QGroupBox*              mSpecificMetaGroupBox;     // "专有元数据" 分组
	QFormLayout*            mSpecificMetaFormLayout;
	QMap<QString, QLineEdit*> mMetadataLineEdits;

	// 入库日志（可收缩）
	QGroupBox*              mLogGroupBox;
	QPushButton*            mBtnToggleLog;
	QPlainTextEdit*         mLogTextEdit;
	bool                    mLogExpanded;

	// 进度与按钮
	QProgressBar*           mProgressBar;
	QLabel*                 mProgressLabel;
	QPushButton*            mBtnImport;

	// 导航按钮
	QPushButton*            mBtnBack;
	QPushButton*            mBtnNext;

	// 用户信息
	QLabel*                 mUserInfoLabel;

	// 状态
	QVector<MetadataFieldDef> mCurrentFields;
	DataImporter*             mImporter;
};

#endif // DATA_IMPORT_WIZARD_H
