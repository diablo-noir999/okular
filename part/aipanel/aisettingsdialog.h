#ifndef AISETTINGSDIALOG_H
#define AISETTINGSDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include "aisettings.h"
#include "llmclient.h"
#include "opencodeclient.h"

class AISettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AISettingsDialog(QNetworkAccessManager *nam, QWidget *parent = nullptr);

    AISettings settings() const;

private Q_SLOTS:
    void onProviderChanged(int index);
    void onTestConnection();
    void onTestResult(bool success, const QString &msg);
    void onBrowseObsidianVault();
    void onSave();

private:
    void setupUI();
    void populateFields();

    QNetworkAccessManager *m_nam;
    LLMClient *m_testClient;
    OpenCodeClient *m_openCodeTestClient;

    QComboBox *m_providerCombo;
    QLineEdit *m_apiUrlEdit;
    QLineEdit *m_apiKeyEdit;
    QLineEdit *m_modelEdit;
    QCheckBox *m_autoSummarizeCheck;
    QCheckBox *m_enableWebSearchCheck;
    QLineEdit *m_exaApiKeyEdit;
    QCheckBox *m_enableObsidianSyncCheck;
    QLineEdit *m_obsidianVaultEdit;
    QPushButton *m_browseObsidianBtn;
    QCheckBox *m_enableVisionCheck;
    QLineEdit *m_visionApiUrlEdit;
    QLineEdit *m_visionModelEdit;
    QLineEdit *m_visionApiKeyEdit;
    QSpinBox *m_debounceDelaySpin;
    QSpinBox *m_maxTokensSpin;
    QSpinBox *m_contextWindowSpin;
    QPushButton *m_testBtn;
    QLabel *m_statusLabel;
    QPushButton *m_saveBtn;
    QPushButton *m_cancelBtn;

    AISettings m_settings;
    QString m_lastAutoModel;
};

#endif // AISETTINGSDIALOG_H
