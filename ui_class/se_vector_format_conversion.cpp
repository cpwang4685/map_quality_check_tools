
/*--------------QGIS---------------*/
#include "qgssettings.h"
#include "qgsgui.h"
#include "qgsstatusbar.h"
#include "qgsapplication.h"

/*--------------QT---------------*/
#include <QFileDialog>
#include <QMessageBox>
#include <qvalidator.h>
#include <qtemporarydir.h>

/*----------------GDAL--------------*/
#include "gdal.h"
#include "gdal_priv.h"
#include "cpl_string.h"
#include "gdal_utils.h"

/*-----------------------------------*/


/*--------------SE---------------*/
#include "se_vector_format_conversion.h"
#include "../ui_task/se_geojson2shp.h"
//#include "../ui_task/se_gpkg2shp.h"
//#include "../ui_task/se_geojson2gpkg.h"
//#include "../ui_task/se_shp2gpkg.h"

#include "ui_fit_helper.h"

/*-------------------------------*/

/*行列索引结构*/
struct FromToIndex
{
	int		iFromRowIndex;			// 每块的行起始索引
	int		iToRowIndex;			// 每块的行终止索引
	int		iFromColIndex;			// 每块的列起始索引
	int		iToColIndex;			// 每块的列终止索引

	FromToIndex()
	{
		iFromRowIndex = 0;
		iToRowIndex = 0;
		iFromColIndex = 0;
		iToColIndex = 0;
	}
};


CSE_VectorFormatConversionDialog::CSE_VectorFormatConversionDialog( QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
{
	ui.setupUi(this);
	QgsGui::enableAutoGeometryRestore(this);
	DialogFitHelper::install(this);

	this->setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);

	connect(ui.Button_Save, &QPushButton::clicked, this, &CSE_VectorFormatConversionDialog::Button_Save_clicked);
	connect(ui.Button_Open, &QPushButton::clicked, this, &CSE_VectorFormatConversionDialog::Button_Open_clicked);
	connect(ui.Button_OK, &QPushButton::clicked, this, &CSE_VectorFormatConversionDialog::Button_OK_accepted);
	connect(ui.Button_Cancel, &QPushButton::clicked, this, &CSE_VectorFormatConversionDialog::Button_Cancel_rejected);

	connect(ui.lineEdit_InputDataPath, &QLineEdit::textChanged, this, &CSE_VectorFormatConversionDialog::on_lineEdit_InputDataPath_TextChanged);
	connect(ui.lineEdit_OutputPath, &QLineEdit::textChanged, this, &CSE_VectorFormatConversionDialog::on_lineEdit_OutputPath_TextChanged);
	connect(ui.Button_SaveLogFilePath, &QPushButton::clicked, this, &CSE_VectorFormatConversionDialog::pushButton_SaveLog_clicked);


	restoreState();

	ui.radioButton_geojson2shp->setChecked(true);

	// 初始化输入路径和输出路径
	ui.lineEdit_InputDataPath->setText(m_qstrInputDataPath);
	ui.lineEdit_OutputPath->setText(m_qstrSavePath);
	ui.lineEdit_OutputLogPath->setText(m_qstrOutputLogPath);
}

CSE_VectorFormatConversionDialog::~CSE_VectorFormatConversionDialog()
{
	// 设置当前保存路径
	QgsSettings settings;
	settings.setValue(QStringLiteral("VectorFormatConversion/InputDataPath"), m_qstrInputDataPath, QgsSettings::Section::Plugins);
	settings.setValue(QStringLiteral("VectorFormatConversion/SavePath"), m_qstrSavePath, QgsSettings::Section::Plugins);
	settings.setValue(QStringLiteral("VectorFormatConversion/OutputLogPath"), m_qstrOutputLogPath, QgsSettings::Section::Plugins);

}


void CSE_VectorFormatConversionDialog::Button_Save_clicked()
{
	// 如果是保存到文件夹gpkg转shp、geojson转shp，选择保存到文件夹
	if (ui.radioButton_gpkg2shp->isChecked()
		|| ui.radioButton_geojson2shp->isChecked())
	{
		QString curPath = QCoreApplication::applicationDirPath();
		QString dlgTile = tr("请选择转换后的数据存储路径");
		QString selectedDir = QFileDialog::getExistingDirectory(
			this,
			dlgTile,
			curPath,
			QFileDialog::ShowDirsOnly);

		if (!selectedDir.isEmpty())
		{
			ui.lineEdit_OutputPath->setText(selectedDir);
			m_qstrSavePath = selectedDir;
		}
	}

	// 如果是shp转gpkg、geojson转gpkg
	else if (ui.radioButton_geojson2gpkg->isChecked()
		|| ui.radioButton_shp2gpkg->isChecked())
	{
		// 配置文件默认保存在当前运行环境下
		QString curPath = QCoreApplication::applicationDirPath();
		QString dlgTitle = tr("保存gpkg文件");
		QString filter = tr("gpkg 文件(*.gpkg)");
		QString strFileName = QFileDialog::getSaveFileName(this,
			dlgTitle, curPath, filter);
		if (!strFileName.isEmpty())
		{
			ui.lineEdit_OutputPath->setText(strFileName);
			m_qstrSavePath = strFileName;
		}
	}





}


void CSE_VectorFormatConversionDialog::Button_Open_clicked()
{
	// 如果选择geojson2gpkg或geojson2shp
	if (ui.radioButton_geojson2gpkg->isChecked()
		|| ui.radioButton_geojson2shp->isChecked())
	{
		QString curPath = QCoreApplication::applicationDirPath();
		QString dlgTile = tr("请选择geojson数据所在目录");
		QString directory = QFileDialog::getExistingDirectory(this,
			dlgTile,
			curPath, // 默认路径
			QFileDialog::ShowDirsOnly);

		if (!directory.isEmpty())
		{
			m_qstrInputDataPath = directory;
		}
		else
		{
			return;
		}
		ui.lineEdit_InputDataPath->setText(directory);
	}

	// 如果选择shp2gpkg
	else if (ui.radioButton_shp2gpkg->isChecked())
	{
		QString curPath = QCoreApplication::applicationDirPath();
		QString dlgTile = tr("请选择shp数据所在目录");
		QString directory = QFileDialog::getExistingDirectory(this,
			dlgTile,
			curPath, // 默认路径
			QFileDialog::ShowDirsOnly);

		if (!directory.isEmpty())
		{
			m_qstrInputDataPath = directory;
			ui.lineEdit_InputDataPath->setText(directory);
		}
		else
		{
			return;
		}
		
	}

	// 如果选择gpkg2shp
	else if (ui.radioButton_gpkg2shp->isChecked())
	{
		QString curPath = QCoreApplication::applicationDirPath();
		QString dlgTitle = tr("选择gpkg文件");
		QString filter = tr("gpkg 文件(*.gpkg)");
		QString strFileName = QFileDialog::getOpenFileName(this,
			dlgTitle, curPath, filter);
		if (!strFileName.isEmpty())
		{
			m_qstrInputDataPath = strFileName;
			ui.lineEdit_InputDataPath->setText(strFileName);
		}
		else
		{
			return;
		}
		
	}
}

/*获取在指定目录下的目录的路径*/
QStringList CSE_VectorFormatConversionDialog::GetSubDirPathOfCurrentDir(QString dirPath)
{
	QStringList dirPaths;
	QDir splDir(dirPath);
	QFileInfoList fileInfoListInSplDir = splDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
	QFileInfo tempFileInfo;
	foreach(tempFileInfo, fileInfoListInSplDir) {
		dirPaths << tempFileInfo.absoluteFilePath();
	}
	return dirPaths;
}

void CSE_VectorFormatConversionDialog::Button_OK_accepted()
{

	QMessageBox msgBox;
	msgBox.setWindowTitle(tr("数据包定制及标准化封装工具"));
	msgBox.setText(tr("测试插件程序！").arg(ui.lineEdit_OutputLogPath->text()));
	msgBox.setStandardButtons(QMessageBox::Yes);
	msgBox.setDefaultButton(QMessageBox::Yes);
	msgBox.exec();
	return;












	// 日志级别
	// 0——错误；1——信息；2——调试
	int iLogLevel = ui.comboBox_LogLevel->currentIndex();

	// 日志结果保存路径
	string strOutputLogPath = ui.lineEdit_OutputLogPath->text().toLocal8Bit().constData();

	// 如果不存在
	if (!CheckFileOrDirExist(ui.lineEdit_OutputLogPath->text()))
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle(tr("数据包定制及标准化封装工具"));
		msgBox.setText(tr("文件夹：%1不存在！").arg(ui.lineEdit_OutputLogPath->text()));
		msgBox.setStandardButtons(QMessageBox::Yes);
		msgBox.setDefaultButton(QMessageBox::Yes);
		msgBox.exec();
		return;
	}


	// 如果文件路径不存在
	if (!CheckFileOrDirExist(ui.lineEdit_InputDataPath->text()))
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle(tr("数据包定制及标准化封装工具"));
		msgBox.setText(tr("文件路径：%1不存在！").arg(ui.lineEdit_InputDataPath->text()));
		msgBox.setStandardButtons(QMessageBox::Yes);
		msgBox.setDefaultButton(QMessageBox::Yes);
		msgBox.exec();
		return;
	}


	// 检查文件夹是否存在
	if (ui.radioButton_geojson2shp->isChecked() ||
		ui.radioButton_gpkg2shp->isChecked())
	{
		// 如果不存在
		if (!CheckFileOrDirExist(ui.lineEdit_OutputPath->text()))
		{
			QMessageBox msgBox;
			msgBox.setWindowTitle(tr("数据包定制及标准化封装工具"));
			msgBox.setText(tr("文件夹：%1不存在！").arg(ui.lineEdit_OutputPath->text()));
			msgBox.setStandardButtons(QMessageBox::Yes);
			msgBox.setDefaultButton(QMessageBox::Yes);
			msgBox.exec();
			return;
		}
	}

	ui.progressBar->reset();

	// geojson转shp
	if(ui.radioButton_geojson2shp->isChecked())
	{
		// 输入数据路径
		string strInputPath = ui.lineEdit_InputDataPath->text().toLocal8Bit().constData();

		// 分析结果保存路径
		string strOutputPath = ui.lineEdit_OutputPath->text().toLocal8Bit().constData();

		// 创建任务
		SE_Geojson2ShpTask* task = new SE_Geojson2ShpTask(
			tr("geojson转shp"),
			strInputPath,
			strOutputPath,
			iLogLevel,
			strOutputLogPath);
		QgsApplication::taskManager()->addTask(task);

		connect(task, &SE_Geojson2ShpTask::taskFinished, this, &CSE_VectorFormatConversionDialog::onTaskFinished);

	}

	// gpkg转shp
	else if (ui.radioButton_gpkg2shp->isChecked())
	{
		// 输入数据路径
		string strInputPath = ui.lineEdit_InputDataPath->text().toLocal8Bit().constData();

		// 分析结果保存路径
		string strOutputPath = ui.lineEdit_OutputPath->text().toLocal8Bit().constData();

		//// 创建任务
		//SE_Gpkg2ShpTask* task = new SE_Gpkg2ShpTask(
		//	tr("gpkg转shp"),
		//	strInputPath,
		//	strOutputPath,
		//	iLogLevel,
		//	strOutputLogPath);
		//QgsApplication::taskManager()->addTask(task);

		//connect(task, &SE_Gpkg2ShpTask::taskFinished, this, &CSE_VectorFormatConversionDialog::onTaskFinished);

	}

	// geojson转gpkg
	else if (ui.radioButton_geojson2gpkg->isChecked())
	{
		// 输入数据路径
		string strInputPath = ui.lineEdit_InputDataPath->text().toLocal8Bit().constData();

		// 分析结果保存路径
		string strOutputPath = ui.lineEdit_OutputPath->text().toLocal8Bit().constData();

	//	// 创建任务
	//	SE_Geojson2GpkgTask* task = new SE_Geojson2GpkgTask(
	//		tr("geojson转gpkg"),
	//		strInputPath,
	//		strOutputPath,
	//		iLogLevel,
	//		strOutputLogPath);
	//	QgsApplication::taskManager()->addTask(task);

	//	connect(task, &SE_Geojson2GpkgTask::taskFinished, this, &CSE_VectorFormatConversionDialog::onTaskFinished);
	//
	}
	
	// shp转gpkg
	else if (ui.radioButton_shp2gpkg->isChecked())
	{
		// 输入数据路径
		string strInputPath = ui.lineEdit_InputDataPath->text().toLocal8Bit().constData();

		// 分析结果保存路径
		string strOutputPath = ui.lineEdit_OutputPath->text().toLocal8Bit().constData();

		// 创建任务
/*		SE_Shp2GpkgTask* task = new SE_Shp2GpkgTask(
			tr("shp转gpkg"),
			strInputPath,
			strOutputPath,
			iLogLevel,
			strOutputLogPath);
		QgsApplication::taskManager()->addTask(task);

		connect(task, &SE_Shp2GpkgTask::taskFinished, this, &CSE_VectorFormatConversionDialog::onTaskFinished);
	*/
	}

	// 设置当前保存路径
	QgsSettings settings;
	settings.setValue(QStringLiteral("VectorFormatConversion/InputDataPath"), m_qstrInputDataPath, QgsSettings::Section::Plugins);
	settings.setValue(QStringLiteral("VectorFormatConversion/SavePath"), m_qstrSavePath, QgsSettings::Section::Plugins);
	settings.setValue(QStringLiteral("VectorFormatConversion/OutputLogPath"), m_qstrOutputLogPath, QgsSettings::Section::Plugins);

}

void CSE_VectorFormatConversionDialog::Button_Cancel_rejected()
{
	reject();
}

void CSE_VectorFormatConversionDialog::on_lineEdit_InputDataPath_TextChanged(const QString& qstr)
{
	ui.progressBar->reset();
}

void CSE_VectorFormatConversionDialog::on_lineEdit_OutputPath_TextChanged(const QString& qstr)
{
	ui.progressBar->reset();
}

void CSE_VectorFormatConversionDialog::pushButton_SaveLog_clicked()
{
	// 选择文件夹
	QString curPath = m_qstrOutputLogPath;
	QString dlgTile = tr("请选择日志文件保存路径");
	QString selectedDir = QFileDialog::getExistingDirectory(
		this,
		dlgTile,
		curPath,
		QFileDialog::ShowDirsOnly);

	if (!selectedDir.isEmpty())
	{
		m_qstrOutputLogPath = selectedDir;
		ui.lineEdit_OutputLogPath->setText(m_qstrOutputLogPath);
	}
}


void CSE_VectorFormatConversionDialog::restoreState()
{
	const QgsSettings settings;
	m_qstrInputDataPath = settings.value(QStringLiteral("VectorFormatConversion/InputDataPath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
	m_qstrSavePath = settings.value(QStringLiteral("VectorFormatConversion/SavePath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
	m_qstrOutputLogPath = settings.value(QStringLiteral("VectorFormatConversion/OutputLogPath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();


}

// 判断目录是否存在
bool CSE_VectorFormatConversionDialog::FilePathIsExisted(QString qstrPath)
{
	QDir dir(qstrPath);
	return dir.exists();
}

// 判断文件是否存在
bool CSE_VectorFormatConversionDialog::FileIsExisted(QString qstrFilePath)
{
	return QFile::exists(qstrFilePath);
}

QString CSE_VectorFormatConversionDialog::GetInputDataPath()
{
	return ui.lineEdit_InputDataPath->text();
}

QString CSE_VectorFormatConversionDialog::GetOutputPath()
{
	return ui.lineEdit_OutputPath->text();
}




QStringList CSE_VectorFormatConversionDialog::GetFileNames(const QString& path)
{
	QDir dir(path);
	QStringList nameFilters;
	nameFilters << "*.geojson" << "*.GEOJSON";
	QStringList files = dir.entryList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
	return files;
}


void CSE_VectorFormatConversionDialog::onTaskFinished(bool result)
{
	// 计算并打印总进度
	CalculateTotalProgress();
}

void CSE_VectorFormatConversionDialog::CalculateTotalProgress()
{

	int totalProgress = 0;
	for (const auto& task : QgsApplication::taskManager()->tasks())
	{
		if (task->status() == QgsTask::Complete)
		{
			totalProgress += 100; // 完成的任务进度加100
		}
		else
		{
			totalProgress += task->progress(); // 当前任务进度
		}
	}
	totalProgress /= QgsApplication::taskManager()->count(); // 计算平均进度


	ui.progressBar->setValue(totalProgress);

}


#pragma region  "检查文件或文件夹是否存在"

bool CSE_VectorFormatConversionDialog::CheckFileOrDirExist(const QString& qstrFileDirOrPath)
{
	QFileInfo objFileInfo(qstrFileDirOrPath);
	return objFileInfo.exists(); // 直接检查文件或目录是否存在
}

#pragma endregion