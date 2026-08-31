#ifndef SE_VECTOR_FORMAT_CONVERSION_H
#define SE_VECTOR_FORMAT_CONVERSION_H

#include <QDialog>

#include "ui_vector_format_conversion.h"
#include <QString>
#include "qgisinterface.h"

#include <vector>
#include <string>

using namespace std;

class CSE_VectorFormatConversionDialog : public QDialog
{
	Q_OBJECT

public:
	CSE_VectorFormatConversionDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~CSE_VectorFormatConversionDialog() override;

public:

	// 获取输入数据路径
	QString GetInputDataPath();

	// 获取输出数据路径
	QString GetOutputPath();

	// 获取geojson数据文件列表
	QStringList GetFileNames(const QString& path);

	// 计算并打印总进度
	void CalculateTotalProgress();

	// 检查文件或文件夹是否存在
	bool CheckFileOrDirExist(const QString& qstrFileDirOrPath);
	
	// 获取指定目录的最后一级目录
	void GetFolderNameFromPath(QString qstrPath, QString& qstrFolderName);

	// 获取当前目录下所有子目录
	QStringList GetSubDirPathOfCurrentDir(QString dirPath);

	// 判断目录是否存在
	bool FilePathIsExisted(QString qstrPath);

	// 判断文件是否存在
	bool FileIsExisted(QString qstrFilePath);


public slots:

	// 任务完成
	void onTaskFinished(bool result);

private:

	Ui_SeRasterFormatConversionDialog ui;

	// 恢复保存参数
	void restoreState();

	// 保存路径
	QString m_qstrSavePath;

	// 输入路径
	QString m_qstrInputDataPath;

	// 保存日志文件路径
	QString m_qstrOutputLogPath;

private slots:

	// 打开保存路径
	void Button_Save_clicked();

	// 打开输入路径
	void Button_Open_clicked();

	// 格式转换
	void Button_OK_accepted();

	// 关闭
	void Button_Cancel_rejected();

	void on_lineEdit_InputDataPath_TextChanged(const QString& qstr);

	void on_lineEdit_OutputPath_TextChanged(const QString& qstr);

	// 保存日志数据
	void pushButton_SaveLog_clicked();

};

#endif // SE_VECTOR_FORMAT_CONVERSION_H
