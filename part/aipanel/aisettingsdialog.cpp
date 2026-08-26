#include "aisettingsdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QToolButton>
#include <QFile>

// Brief status of the OCR toolchain shown in the Vision group so users don't
// have to guess why textless pages aren't being read.
static QString visionAvailabilityText()
{
    const QString tesseract = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    if (tesseract.isEmpty()) {
        return QStringLiteral("Tesseract not found - install it with: pacman -S tesseract tesseract-data-eng (or apt/brew equivalent)");
    }
    const QStringList tessdataDirs = {
        QStringLiteral("/usr/share/tessdata"),
        QStringLiteral("/usr/local/share/tessdata"),
        QStringLiteral("/usr/share/tesseract-ocr/5/tessdata"),
        QStringLiteral("/opt/homebrew/share/tessdata"),
        QStringLiteral("/opt/local/share/tessdata"),
    };
    bool hasEng = false;
    for (const QString &dir : tessdataDirs) {
        if (QFile::exists(dir + QStringLiteral("/eng.traineddata"))) {
            hasEng = true;
            break;
        }
    }
    if (!hasEng) {
        return QStringLiteral("Tesseract is installed but the English language data is missing - install tesseract-data-eng. The vision model will be used until then.");
    }
    return QStringLiteral("Tesseract ready (primary OCR). If it fails, a vision server is auto-detected on localhost:8080.");
}

AISettingsDialog::AISettingsDialog(QNetworkAccessManager *nam, QWidget *parent)
    : QDialog(parent)
    , m_nam(nam)
{
    setWindowTitle(QStringLiteral("AI Page Explainer & BYOK Settings"));
    resize(500, 580);

    m_testClient = new LLMClient(m_nam, this);
    connect(m_testClient, &LLMClient::testConnectionResult, this, &AISettingsDialog::onTestResult);
    connect(m_testClient, &LLMClient::retrying, this, [this](int delaySeconds) {
        m_statusLabel->setText(QStringLiteral("Rate limited - retrying in %1s...").arg(delaySeconds));
    });
    m_openCodeTestClient = new OpenCodeClient(this);
    connect(m_openCodeTestClient, &OpenCodeClient::testConnectionResult, this, &AISettingsDialog::onTestResult);

    m_settings = AISettings::load();
    setupUI();
    populateFields();
}

void AISettingsDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *providerGroup = new QGroupBox(QStringLiteral("Model Provider & BYOK"), this);
    auto *formLayout = new QFormLayout(providerGroup);

    m_providerCombo = new QComboBox(this);
    m_providerCombo->addItem(QStringLiteral("opencode CLI (local, anonymous - no key needed)"), static_cast<int>(AISettings::OpenCodeCLI));
    m_providerCombo->addItem(QStringLiteral("OpenCode Zen (BYOK API)"), static_cast<int>(AISettings::OpenCode));
    m_providerCombo->addItem(QStringLiteral("Ollama (Local)"), static_cast<int>(AISettings::Ollama));
    m_providerCombo->addItem(QStringLiteral("OpenAI"), static_cast<int>(AISettings::OpenAI));
    m_providerCombo->addItem(QStringLiteral("Anthropic (Claude)"), static_cast<int>(AISettings::Anthropic));
    m_providerCombo->addItem(QStringLiteral("Custom OpenAI-Compatible"), static_cast<int>(AISettings::Custom));
    connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &AISettingsDialog::onProviderChanged);
    formLayout->addRow(QStringLiteral("Provider:"), m_providerCombo);

    m_apiUrlEdit = new QLineEdit(this);
    formLayout->addRow(QStringLiteral("API Base URL:"), m_apiUrlEdit);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(QStringLiteral("Enter your API Key (BYOK)"));
    auto *apiKeyLayout = new QHBoxLayout();
    auto *apiKeyVisibilityButton = new QToolButton(this);
    apiKeyVisibilityButton->setCheckable(true);
    apiKeyVisibilityButton->setIcon(QIcon::fromTheme(QStringLiteral("visibility")));
    apiKeyVisibilityButton->setToolTip(QStringLiteral("Show or hide API key"));
    connect(apiKeyVisibilityButton, &QToolButton::toggled, this, [this](bool visible) {
        m_apiKeyEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    });
    apiKeyLayout->addWidget(m_apiKeyEdit, 1);
    apiKeyLayout->addWidget(apiKeyVisibilityButton);
    formLayout->addRow(QStringLiteral("API Key:"), apiKeyLayout);

    m_modelEdit = new QLineEdit(this);
    formLayout->addRow(QStringLiteral("Model Name:"), m_modelEdit);

    mainLayout->addWidget(providerGroup);

    auto *featuresGroup = new QGroupBox(QStringLiteral("Behavior & Search"), this);
    auto *featLayout = new QVBoxLayout(featuresGroup);

    m_enableWebSearchCheck = new QCheckBox(QStringLiteral("Enable Web Search Augmentation (powered by Exa AI / fast fallback)"), this);
    featLayout->addWidget(m_enableWebSearchCheck);

    auto *searchKeyForm = new QFormLayout();
    m_exaApiKeyEdit = new QLineEdit(this);
    m_exaApiKeyEdit->setEchoMode(QLineEdit::Password);
    m_exaApiKeyEdit->setPlaceholderText(QStringLiteral("Optional Exa API Key (e.g. for high-quality neural search)"));
    searchKeyForm->addRow(QStringLiteral("Exa API Key:"), m_exaApiKeyEdit);
    featLayout->addLayout(searchKeyForm);

    m_autoSummarizeCheck = new QCheckBox(QStringLiteral("Automatically explain page upon navigation"), this);
    featLayout->addWidget(m_autoSummarizeCheck);

    auto *subForm = new QFormLayout();
    m_debounceDelaySpin = new QSpinBox(this);
    m_debounceDelaySpin->setRange(300, 3000);
    m_debounceDelaySpin->setSuffix(QStringLiteral(" ms"));
    subForm->addRow(QStringLiteral("Page settle delay:"), m_debounceDelaySpin);

    m_maxTokensSpin = new QSpinBox(this);
    m_maxTokensSpin->setRange(100, 65536);
    m_maxTokensSpin->setSingleStep(1024);
    m_maxTokensSpin->setToolTip(QStringLiteral("Maximum output budget. Reasoning models (e.g. hy3-free) count their thinking against this too, so keep it generous enough that the answer isn't starved after reasoning."));
    subForm->addRow(QStringLiteral("Max response tokens:"), m_maxTokensSpin);

    m_contextWindowSpin = new QSpinBox(this);
    m_contextWindowSpin->setRange(4096, 262144);
    m_contextWindowSpin->setSingleStep(4096);
    m_contextWindowSpin->setSuffix(QStringLiteral(" tokens"));
    m_contextWindowSpin->setToolTip(QStringLiteral("Total context window of the model. The request (current page + previous context + web search) is capped to fit this minus the response budget."));
    subForm->addRow(QStringLiteral("Model context window:"), m_contextWindowSpin);
    featLayout->addLayout(subForm);

    mainLayout->addWidget(featuresGroup);

    auto *obsidianGroup = new QGroupBox(QStringLiteral("Obsidian Knowledge Notes"), this);
    auto *obsLayout = new QVBoxLayout(obsidianGroup);

    m_enableObsidianSyncCheck = new QCheckBox(QStringLiteral("Automatically save per-page explanations to Obsidian markdown note"), this);
    obsLayout->addWidget(m_enableObsidianSyncCheck);

    auto *vaultPathLayout = new QHBoxLayout();
    m_obsidianVaultEdit = new QLineEdit(this);
    m_obsidianVaultEdit->setPlaceholderText(QStringLiteral("Default: ~/Documents/Obsidian Vault/Okular Notes"));
    m_browseObsidianBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(m_browseObsidianBtn, &QPushButton::clicked, this, &AISettingsDialog::onBrowseObsidianVault);

    vaultPathLayout->addWidget(m_obsidianVaultEdit, 1);
    vaultPathLayout->addWidget(m_browseObsidianBtn);
    obsLayout->addLayout(vaultPathLayout);

    mainLayout->addWidget(obsidianGroup);

    auto *visionGroup = new QGroupBox(QStringLiteral("Vision (OCR) Fallback"), this);
    auto *visionLayout = new QVBoxLayout(visionGroup);

    m_enableVisionCheck = new QCheckBox(QStringLiteral("Transcribe pages without selectable text automatically (Tesseract OCR, then vision model)"), this);
    m_enableVisionCheck->setToolTip(QStringLiteral("Pages that have no selectable text are rendered and read with Tesseract first; if that fails, a vision model is used. Tesseract needs to be installed (tesseract + tesseract-data-eng). The vision server (e.g. llama.cpp) is auto-detected on localhost:8080."));
    visionLayout->addWidget(m_enableVisionCheck);

    auto *visionForm = new QFormLayout();
    m_visionApiUrlEdit = new QLineEdit(this);
    m_visionApiUrlEdit->setPlaceholderText(QStringLiteral("Auto-detect (defaults to localhost:8080). Leave empty."));
    visionForm->addRow(QStringLiteral("Vision API Base URL:"), m_visionApiUrlEdit);

    m_visionModelEdit = new QLineEdit(this);
    m_visionModelEdit->setPlaceholderText(QStringLiteral("Auto-detect a vision model from the server. Leave empty."));
    visionForm->addRow(QStringLiteral("Vision Model:"), m_visionModelEdit);

    m_visionApiKeyEdit = new QLineEdit(this);
    m_visionApiKeyEdit->setEchoMode(QLineEdit::Password);
    m_visionApiKeyEdit->setPlaceholderText(QStringLiteral("Optional API key for the vision endpoint"));
    visionForm->addRow(QStringLiteral("Vision API Key:"), m_visionApiKeyEdit);
    visionLayout->addLayout(visionForm);

    auto *visionHint = new QLabel(this);
    visionHint->setWordWrap(true);
    visionHint->setStyleSheet(QStringLiteral("color: #666; font-size: 11px;"));
    visionHint->setText(visionAvailabilityText());
    visionLayout->addWidget(visionHint);

    mainLayout->addWidget(visionGroup);

    auto *testLayout = new QHBoxLayout();
    m_testBtn = new QPushButton(QStringLiteral("Test Connection"), this);
    connect(m_testBtn, &QPushButton::clicked, this, &AISettingsDialog::onTestConnection);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    testLayout->addWidget(m_testBtn);
    testLayout->addWidget(m_statusLabel, 1);
    mainLayout->addLayout(testLayout);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveBtn = new QPushButton(QStringLiteral("Save"), this);
    m_saveBtn->setDefault(true);
    connect(m_saveBtn, &QPushButton::clicked, this, &AISettingsDialog::onSave);

    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);
    mainLayout->addLayout(btnLayout);
}

void AISettingsDialog::populateFields()
{
    int idx = m_providerCombo->findData(static_cast<int>(m_settings.provider));
    if (idx != -1) m_providerCombo->setCurrentIndex(idx);

    m_apiUrlEdit->setText(m_settings.apiUrl);
    m_apiKeyEdit->setText(m_settings.apiKey);
    m_modelEdit->setText(m_settings.model);
    m_enableWebSearchCheck->setChecked(m_settings.enableWebSearch);
    m_exaApiKeyEdit->setText(m_settings.exaApiKey);
    m_autoSummarizeCheck->setChecked(m_settings.autoSummarize);
    m_enableObsidianSyncCheck->setChecked(m_settings.enableObsidianSync);
    m_obsidianVaultEdit->setText(m_settings.obsidianVaultPath);
    m_debounceDelaySpin->setValue(m_settings.debounceDelayMs);
    m_maxTokensSpin->setValue(m_settings.maxTokens);
    m_contextWindowSpin->setValue(m_settings.contextWindowTokens);
    m_enableVisionCheck->setChecked(m_settings.enableVisionFallback);
    m_visionApiUrlEdit->setText(m_settings.visionApiUrl);
    m_visionModelEdit->setText(m_settings.visionModel);
    m_visionApiKeyEdit->setText(m_settings.visionApiKey);
}

static QString defaultModelForProvider(AISettings::Provider provider)
{
    switch (provider) {
    case AISettings::OpenCodeCLI:
        return QStringLiteral("opencode/mimo-v2.5-free"); // anonymous free model via the opencode CLI
    case AISettings::OpenCode:
        return QStringLiteral("mimo-v2.5-free"); // free model on OpenCode Zen
    case AISettings::Ollama:
        return QStringLiteral("llama3.2");
    case AISettings::OpenAI:
        return QStringLiteral("gpt-5-nano");
    case AISettings::Anthropic:
        return QStringLiteral("claude-sonnet-4-5");
    case AISettings::Custom:
        return QString();
    }
    return QString();
}

void AISettingsDialog::onProviderChanged(int index)
{
    AISettings::Provider p = static_cast<AISettings::Provider>(m_providerCombo->itemData(index).toInt());
    const bool isCli = (p == AISettings::OpenCodeCLI);
    m_apiUrlEdit->setEnabled(!isCli);
    m_apiKeyEdit->setEnabled(!isCli);

    switch (p) {
    case AISettings::OpenCodeCLI:
        m_modelEdit->setPlaceholderText(QStringLiteral("opencode/mimo-v2.5-free (or any opencode provider/model, e.g. opencode/hy3-free)"));
        break;
    case AISettings::OpenCode:
        m_apiUrlEdit->setText(QStringLiteral("https://opencode.ai/zen"));
        m_modelEdit->setPlaceholderText(QStringLiteral("Free: big-pickle, mimo-v2.5-free, hy3-free, muse-spark-1.2-contributor-free"));
        break;
    case AISettings::Ollama:
        m_apiUrlEdit->setText(QStringLiteral("http://localhost:11434"));
        m_modelEdit->setPlaceholderText(QStringLiteral("e.g. llama3.2"));
        break;
    case AISettings::OpenAI:
        m_apiUrlEdit->setText(QStringLiteral("https://api.openai.com"));
        m_modelEdit->setPlaceholderText(QStringLiteral("e.g. gpt-5-nano"));
        break;
    case AISettings::Anthropic:
        m_apiUrlEdit->setText(QStringLiteral("https://api.anthropic.com"));
        m_modelEdit->setPlaceholderText(QStringLiteral("e.g. claude-sonnet-4-5"));
        break;
    case AISettings::Custom:
        m_modelEdit->setPlaceholderText(QStringLiteral("Model name for your endpoint"));
        break;
    }

    // Only auto-fill the model when the user hasn't customized it (empty or still the
    // previous provider's default), so switching providers doesn't clobber a BYOK model.
    const QString currentModel = m_modelEdit->text().trimmed();
    const QString newDefault = defaultModelForProvider(p);
    if (currentModel.isEmpty() || currentModel == m_lastAutoModel) {
        m_modelEdit->setText(newDefault);
    }
    m_lastAutoModel = newDefault;
}

void AISettingsDialog::onTestConnection()
{
    m_statusLabel->setText(QStringLiteral("Testing connection..."));
    m_testBtn->setEnabled(false);

    AISettings temp;
    temp.provider = static_cast<AISettings::Provider>(m_providerCombo->currentData().toInt());
    temp.apiUrl = m_apiUrlEdit->text().trimmed();
    temp.apiKey = m_apiKeyEdit->text().trimmed();
    temp.model = m_modelEdit->text().trimmed();

    if (temp.provider == AISettings::OpenCodeCLI) {
        m_openCodeTestClient->setWorkingDirectory(QDir::homePath());
        m_openCodeTestClient->testConnection(temp);
    } else {
        m_testClient->testConnection(temp);
    }
}

void AISettingsDialog::onTestResult(bool success, const QString &msg)
{
    m_testBtn->setEnabled(true);
    if (success) {
        m_statusLabel->setText(QStringLiteral("<font color='green'>✓ %1</font>").arg(msg));
    } else {
        m_statusLabel->setText(QStringLiteral("<font color='red'>✗ %1</font>").arg(msg));
    }
}

void AISettingsDialog::onBrowseObsidianVault()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Obsidian Vault or Notes Folder"),
        m_obsidianVaultEdit->text().isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            : m_obsidianVaultEdit->text()
    );
    if (!dir.isEmpty()) {
        m_obsidianVaultEdit->setText(dir);
    }
}

void AISettingsDialog::onSave()
{
    m_settings.provider = static_cast<AISettings::Provider>(m_providerCombo->currentData().toInt());
    m_settings.apiUrl = m_apiUrlEdit->text().trimmed();
    m_settings.apiKey = m_apiKeyEdit->text().trimmed();
    m_settings.model = m_modelEdit->text().trimmed();
    m_settings.enableWebSearch = m_enableWebSearchCheck->isChecked();
    m_settings.exaApiKey = m_exaApiKeyEdit->text().trimmed();
    m_settings.autoSummarize = m_autoSummarizeCheck->isChecked();
    m_settings.enableObsidianSync = m_enableObsidianSyncCheck->isChecked();
    m_settings.obsidianVaultPath = m_obsidianVaultEdit->text().trimmed();
    m_settings.debounceDelayMs = m_debounceDelaySpin->value();
    m_settings.maxTokens = m_maxTokensSpin->value();
    m_settings.contextWindowTokens = m_contextWindowSpin->value();
    m_settings.enableVisionFallback = m_enableVisionCheck->isChecked();
    m_settings.visionApiUrl = m_visionApiUrlEdit->text().trimmed();
    m_settings.visionModel = m_visionModelEdit->text().trimmed();
    m_settings.visionApiKey = m_visionApiKeyEdit->text().trimmed();

    m_settings.save();
    accept();
}

AISettings AISettingsDialog::settings() const
{
    return m_settings;
}
