#pragma once

#include <QDialog>
#include <QString>

struct DataCatalogConnectionParameters
{
    QString host;
    QString port;
    QString database;
    QString schema;
    QString user;
    QString password;
};

class QCheckBox;
class QLineEdit;

class DataCatalogConnectionDialog final : public QDialog
{
public:
    explicit DataCatalogConnectionDialog(QWidget* parent = nullptr);

    DataCatalogConnectionParameters parameters() const;
    bool rememberConnection() const;
    void saveRememberedConnection() const;
    void clearRememberedConnection() const;

private:
    static QString settingsPath();
    void loadRememberedConnection();

    QLineEdit* m_hostEdit = nullptr;
    QLineEdit* m_portEdit = nullptr;
    QLineEdit* m_databaseEdit = nullptr;
    QLineEdit* m_schemaEdit = nullptr;
    QLineEdit* m_userEdit = nullptr;
    QLineEdit* m_passwordEdit = nullptr;
    QCheckBox* m_rememberCheck = nullptr;
};
