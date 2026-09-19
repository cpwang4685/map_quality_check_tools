#include "DataCatalogConnectionDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>

namespace
{
    QStringList settingsPathCandidates()
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        return {
            QDir(appDir).absoluteFilePath(QStringLiteral("config/data_catalog_connection.ini")),
            QDir(appDir).absoluteFilePath(QStringLiteral("../config/data_catalog_connection.ini")),
            QDir(appDir).absoluteFilePath(QStringLiteral("../../config/data_catalog_connection.ini")),
            QDir::current().absoluteFilePath(QStringLiteral("config/data_catalog_connection.ini"))
        };
    }

    void configureLineEdit(QLineEdit* edit, const QString& objectName, const QString& placeholder)
    {
        edit->setObjectName(objectName);
        edit->setMinimumHeight(30);
        edit->setPlaceholderText(placeholder);
        edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
}

DataCatalogConnectionDialog::DataCatalogConnectionDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("dataCatalogConnectionDialog"));
    setWindowTitle(QStringLiteral("连接数据库"));
    setModal(true);
    setMinimumSize(480, 340);
    resize(560, 380);
    setSizeGripEnabled(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QLabel* titleLabel = new QLabel(QStringLiteral("请输入数据库连接信息"), this);
    titleLabel->setObjectName(QStringLiteral("dataCatalogConnectionTitleLabel"));
    titleLabel->setWordWrap(true);

    m_hostEdit = new QLineEdit(this);
    configureLineEdit(
        m_hostEdit,
        QStringLiteral("dataCatalogConnectionHostEdit"),
        QStringLiteral("例如：127.0.0.1"));

    m_portEdit = new QLineEdit(this);
    configureLineEdit(
        m_portEdit,
        QStringLiteral("dataCatalogConnectionPortEdit"),
        QStringLiteral("默认：5432"));
    m_portEdit->setText(QStringLiteral("5432"));

    m_databaseEdit = new QLineEdit(this);
    configureLineEdit(
        m_databaseEdit,
        QStringLiteral("dataCatalogConnectionDatabaseEdit"),
        QStringLiteral("请输入数据库名称"));

    m_schemaEdit = new QLineEdit(this);
    configureLineEdit(
        m_schemaEdit,
        QStringLiteral("dataCatalogConnectionSchemaEdit"),
        QStringLiteral("默认：public"));
    m_schemaEdit->setText(QStringLiteral("public"));

    m_userEdit = new QLineEdit(this);
    configureLineEdit(
        m_userEdit,
        QStringLiteral("dataCatalogConnectionUserEdit"),
        QStringLiteral("请输入数据库用户名"));

    m_passwordEdit = new QLineEdit(this);
    configureLineEdit(
        m_passwordEdit,
        QStringLiteral("dataCatalogConnectionPasswordEdit"),
        QStringLiteral("请输入数据库密码"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_rememberCheck = new QCheckBox(QStringLiteral("记住连接信息"), this);
    m_rememberCheck->setObjectName(QStringLiteral("dataCatalogConnectionRememberCheck"));
    m_rememberCheck->setToolTip(QStringLiteral("仅保存主机、端口、数据库、Schema 和用户名，不保存密码。"));

    QFormLayout* formLayout = new QFormLayout();
    formLayout->setObjectName(QStringLiteral("dataCatalogConnectionFormLayout"));
    formLayout->setHorizontalSpacing(12);
    formLayout->setVerticalSpacing(10);
    formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    formLayout->addRow(QStringLiteral("主机"), m_hostEdit);
    formLayout->addRow(QStringLiteral("端口"), m_portEdit);
    formLayout->addRow(QStringLiteral("数据库"), m_databaseEdit);
    formLayout->addRow(QStringLiteral("Schema"), m_schemaEdit);
    formLayout->addRow(QStringLiteral("用户名"), m_userEdit);
    formLayout->addRow(QStringLiteral("密码"), m_passwordEdit);

    QPushButton* connectButton = new QPushButton(QStringLiteral("连接"), this);
    connectButton->setObjectName(QStringLiteral("dataCatalogConnectionConnectButton"));
    connectButton->setDefault(true);
    connectButton->setMinimumHeight(32);

    QPushButton* cancelButton = new QPushButton(QStringLiteral("取消"), this);
    cancelButton->setObjectName(QStringLiteral("dataCatalogConnectionCancelButton"));
    cancelButton->setMinimumHeight(32);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(connectButton);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(28, 24, 28, 22);
    mainLayout->setSpacing(14);
    mainLayout->addWidget(titleLabel);
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(m_rememberCheck);
    mainLayout->addStretch(1);
    mainLayout->addLayout(buttonLayout);

    connect(connectButton, &QPushButton::clicked, this, [this]() {
        accept();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this]() {
        reject();
    });

    loadRememberedConnection();
}

DataCatalogConnectionParameters DataCatalogConnectionDialog::parameters() const
{
    DataCatalogConnectionParameters result;
    result.host = m_hostEdit ? m_hostEdit->text().trimmed() : QString();
    result.port = m_portEdit ? m_portEdit->text().trimmed() : QString();
    result.database = m_databaseEdit ? m_databaseEdit->text().trimmed() : QString();
    result.schema = m_schemaEdit ? m_schemaEdit->text().trimmed() : QString();
    result.user = m_userEdit ? m_userEdit->text().trimmed() : QString();
    result.password = m_passwordEdit ? m_passwordEdit->text() : QString();
    return result;
}

bool DataCatalogConnectionDialog::rememberConnection() const
{
    return m_rememberCheck && m_rememberCheck->isChecked();
}

void DataCatalogConnectionDialog::saveRememberedConnection() const
{
    const DataCatalogConnectionParameters values = parameters();
    const QString path = settingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.remove(QStringLiteral("connection"));
    settings.setValue(QStringLiteral("connection/remembered"), true);
    settings.setValue(QStringLiteral("connection/host"), values.host);
    settings.setValue(QStringLiteral("connection/port"), values.port);
    settings.setValue(QStringLiteral("connection/database"), values.database);
    settings.setValue(QStringLiteral("connection/schema"), values.schema);
    settings.setValue(QStringLiteral("connection/user"), values.user);
    settings.sync();
}

void DataCatalogConnectionDialog::clearRememberedConnection() const
{
    const QString path = settingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.remove(QStringLiteral("connection"));
    settings.sync();
}

QString DataCatalogConnectionDialog::settingsPath()
{
    const QStringList candidates = settingsPathCandidates();
    for (const QString& candidate : candidates)
    {
        if (QFileInfo::exists(candidate))
        {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    return QFileInfo(candidates.first()).absoluteFilePath();
}

void DataCatalogConnectionDialog::loadRememberedConnection()
{
    const QString path = settingsPath();
    if (!QFileInfo::exists(path))
    {
        return;
    }

    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    if (!settings.value(QStringLiteral("connection/remembered"), false).toBool())
    {
        return;
    }

    m_hostEdit->setText(settings.value(QStringLiteral("connection/host")).toString());
    m_portEdit->setText(settings.value(QStringLiteral("connection/port"), QStringLiteral("5432")).toString());
    m_databaseEdit->setText(settings.value(QStringLiteral("connection/database")).toString());
    m_schemaEdit->setText(settings.value(QStringLiteral("connection/schema"), QStringLiteral("public")).toString());
    m_userEdit->setText(settings.value(QStringLiteral("connection/user")).toString());
    m_rememberCheck->setChecked(true);
}
