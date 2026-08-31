#pragma once

#include <QDialog>
#include <QStringList>

namespace Ui {
class GDBLayerSelectorDialog;
}

class GDBLayerSelectorDialog : public QDialog
{
	Q_OBJECT

public:
	explicit GDBLayerSelectorDialog(const QString& gdbPath, QWidget* parent = nullptr);
	~GDBLayerSelectorDialog();

	/// 返回用户勾选的图层名称列表
	QStringList selectedLayers() const;

private slots:
	void onSelectAll();
	void onDeselectAll();
	void onOk();

private:
	void populateLayers(const QString& gdbPath);

	Ui::GDBLayerSelectorDialog* ui;
	QStringList mAllLayerNames;
};
