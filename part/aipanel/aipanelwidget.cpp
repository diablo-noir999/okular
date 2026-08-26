#include "aipanelwidget.h"
#include "aisettingsdialog.h"
#include "core/document.h"
#include "core/generator.h"
#include "core/page.h"
#include "gui/pagepainter.h"
#include "opencodeclient.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QScrollBar>
#include <QBuffer>
#include <QPainter>
#include <QProcess>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocument>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
/** Renders a markdown string as an HTML fragment (for the chat browser). */
QString markdownToBodyHtml(const QString &markdown)
{
    if (markdown.trimmed().isEmpty()) {
        return QString();
    }
    QTextDocument doc;
    doc.setMarkdown(markdown);
    const QString fullHtml = doc.toHtml();
    const int bodyStart = fullHtml.indexOf(QStringLiteral("<body"));
    const int bodyEnd = fullHtml.lastIndexOf(QStringLiteral("</body>"));
    if (bodyStart < 0 || bodyEnd < 0) {
        return markdown.toHtmlEscaped();
    }
    const int contentStart = fullHtml.indexOf(QLatin1Char('>'), bodyStart) + 1;
    return fullHtml.mid(contentStart, bodyEnd - contentStart);
}
}

AIPanelWidget::AIPanelWidget(Okular::Document *document, QWidget *parent)
    : QWidget(parent)
    , m_document(document)
{
    m_nam = new QNetworkAccessManager(this);
    m_llmClient = new LLMClient(m_nam, this);
    m_openCodeClient = new OpenCodeClient(this);
    connect(m_openCodeClient, &OpenCodeClient::chunkReceived, this, &AIPanelWidget::onChunkReceived);
    connect(m_openCodeClient, &OpenCodeClient::finished, this, &AIPanelWidget::onStreamFinished);
    connect(m_openCodeClient, &OpenCodeClient::errorOccurred, this, &AIPanelWidget::onErrorOccurred);
    m_searchHelper = new SearchHelper(m_nam, this);
    m_settings = AISettings::load();

    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    connect(m_debounceTimer, &QTimer::timeout, this, &AIPanelWidget::onDebounceTimeout);

    connect(m_searchHelper, &SearchHelper::searchCompleted, this, &AIPanelWidget::onSearchCompleted);
    connect(m_llmClient, &LLMClient::chunkReceived, this, &AIPanelWidget::onChunkReceived);
    connect(m_llmClient, &LLMClient::finished, this, &AIPanelWidget::onStreamFinished);
    connect(m_llmClient, &LLMClient::errorOccurred, this, &AIPanelWidget::onErrorOccurred);
    connect(m_llmClient, &LLMClient::transcriptionFinished, this, &AIPanelWidget::onTranscriptionFinished);
    connect(m_llmClient, &LLMClient::retrying, this, [this](int delaySeconds) {
        qDebug() << "[AIPanel] Rate limited - retrying in" << delaySeconds << "s";
        m_statusLabel->setText(QStringLiteral("Rate limited - retrying in %1s...").arg(delaySeconds));
        if (!m_isAskingQuestion && m_pageCache.contains(m_currentPage)) {
            // Discard any partial stream from the failed attempt so the retry starts clean
            m_pageCache[m_currentPage].explanationMarkdown.clear();
            m_summaryBrowser->clear();
        }
    });
    // Reasoning models stream chain-of-thought silently before producing any visible
    // text; surface it so the panel doesn't look frozen.
    connect(m_llmClient, &LLMClient::reasoningProgress, this, [this](int chars) {
        if (m_llmRequestPage != m_currentPage) {
            return;
        }
        if (m_isAskingQuestion) {
            m_statusLabel->setText(QStringLiteral("Answering question... model is thinking (%1 chars of reasoning)").arg(chars));
        } else {
            m_statusLabel->setText(QStringLiteral("Generating explanation... model is thinking (%1 chars of reasoning)").arg(chars));
        }
    });

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(1000);
    connect(m_elapsedTimer, &QTimer::timeout, this, &AIPanelWidget::onElapsedTick);

    m_visionTimer = new QTimer(this);
    m_visionTimer->setSingleShot(true);
    connect(m_visionTimer, &QTimer::timeout, this, &AIPanelWidget::onVisionTimedOut);

    setupUI();
}

void AIPanelWidget::abortOcr()
{
    if (m_tesseractProcess) {
        m_tesseractProcess->disconnect(this);
        m_tesseractProcess->kill();
        m_tesseractProcess->deleteLater();
        m_tesseractProcess = nullptr;
    }
    m_ocrPage = -1;
    m_pendingVisionPng.clear();
    m_contextBackfill = false;
    m_contextOcrQueue.clear();
}

AIPanelWidget::~AIPanelWidget()
{
    if (m_document) {
        m_document->removeObserver(this);
    }
    m_debounceTimer->stop();
    m_visionTimer->stop();
    abortOcr();
    m_searchHelper->cancel();
    m_llmClient->abort();
    m_openCodeClient->abort();
}

bool AIPanelWidget::usingOpenCodeCli() const
{
    return m_settings.provider == AISettings::OpenCodeCLI;
}

QString AIPanelWidget::composeExplainMessage(int pageNum, const QString &pageText, const QString &prevContext) const
{
    QString msg = QStringLiteral(
        "You are an expert tutor embedded in a PDF viewer, helping a learner understand a document page by page.\n"
        "Explain and summarize page %1 (text below):\n"
        "1. Write a concise plain-English summary (3-6 bullet points).\n"
        "2. Explain the key terms and concepts clearly, including building blocks a beginner might be missing.\n"
        "3. Connect the page to what we discussed earlier in this session, if relevant.\n"
        "4. Be honest if the page is mostly figures, tables of contents, or references.\n"
        "5. End with one short question that moves the learner forward.\n"
        "Style: warm, direct, specific; no emoji, no cheerleading. For graded coursework, explain concepts and method rather than giving answers that could be submitted.\n"
        "\nPage %1 text:\n---\n%2\n---\n\nPlease explain and summarize page %1.")
                     .arg(QString::number(pageNum), pageText);
    if (!prevContext.trimmed().isEmpty()) {
        msg = QStringLiteral("Context from earlier pages of this document (for reference):\n---\n%1\n---\n\n").arg(prevContext) + msg;
    }
    return msg;
}

QString AIPanelWidget::composeQuestionMessage(int pageNum, const QString &pageText, const QString &question)
{
    // If this page hasn't been explained in the session yet, give the model the page
    // text so it can answer; otherwise the session already holds the context.
    QString msg;
    if (!pageText.trimmed().isEmpty() && !m_explainedPagesInSession.contains(pageNum)) {
        msg += QStringLiteral("Context (page %1 text):\n---\n%2\n---\n\n").arg(QString::number(pageNum), pageText);
    }
    msg += question;
    return msg;
}



void AIPanelWidget::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    // Header Toolbar
    auto *headerLayout = new QHBoxLayout();
    m_pageLabel = new QLabel(QStringLiteral("<b>Page -</b>"), this);
    headerLayout->addWidget(m_pageLabel);
    headerLayout->addStretch();

    m_searchToggleBtn = new QPushButton(this);
    m_searchToggleBtn->setText(m_settings.enableWebSearch ? QStringLiteral("Search: ON") : QStringLiteral("Search: OFF"));
    m_searchToggleBtn->setToolTip(QStringLiteral("Toggle Web Search Augmentation"));
    connect(m_searchToggleBtn, &QPushButton::clicked, this, &AIPanelWidget::onToggleSearch);
    headerLayout->addWidget(m_searchToggleBtn);

    m_autoSummarizeBtn = new QPushButton(this);
    m_autoSummarizeBtn->setText(m_settings.autoSummarize ? QStringLiteral("Auto: ON") : QStringLiteral("Auto: OFF"));
    m_autoSummarizeBtn->setToolTip(QStringLiteral("Toggle automatic page explanations"));
    connect(m_autoSummarizeBtn, &QPushButton::clicked, this, &AIPanelWidget::onToggleAutoSummarize);
    headerLayout->addWidget(m_autoSummarizeBtn);

    m_refreshBtn = new QPushButton(this);
    m_refreshBtn->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_refreshBtn->setToolTip(QStringLiteral("Re-explain current page"));
    connect(m_refreshBtn, &QPushButton::clicked, this, &AIPanelWidget::onRefreshPage);
    headerLayout->addWidget(m_refreshBtn);

    m_settingsBtn = new QPushButton(this);
    m_settingsBtn->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
    m_settingsBtn->setToolTip(QStringLiteral("Configure BYOK AI & Providers"));
    connect(m_settingsBtn, &QPushButton::clicked, this, &AIPanelWidget::onOpenSettings);
    headerLayout->addWidget(m_settingsBtn);

    m_closeBtn = new QPushButton(this);
    m_closeBtn->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    m_closeBtn->setToolTip(QStringLiteral("Hide AI Explainer"));
    connect(m_closeBtn, &QPushButton::clicked, this, &AIPanelWidget::onClosePanel);
    headerLayout->addWidget(m_closeBtn);

    mainLayout->addLayout(headerLayout);

    // Status Label
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 11px;"));
    m_statusLabel->setText(QStringLiteral("Ready."));
    mainLayout->addWidget(m_statusLabel);

    // Splitter between Explanation and Q&A Chat
    auto *splitter = new QSplitter(Qt::Vertical, this);

    // Summary View
    auto *summaryContainer = new QWidget(this);
    auto *summaryLayout = new QVBoxLayout(summaryContainer);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    auto *summaryTitleRow = new QHBoxLayout();
    auto *summaryTitle = new QLabel(QStringLiteral("<b>Page Summary & Explanation</b>"), this);
    m_timerLabel = new QLabel(this);
    m_timerLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 11px;"));
    m_timerLabel->setText(QString());
    summaryTitleRow->addWidget(summaryTitle);
    summaryTitleRow->addStretch();
    summaryTitleRow->addWidget(m_timerLabel);

    m_summaryBrowser = new QTextBrowser(this);
    m_summaryBrowser->setOpenExternalLinks(true);
    m_summaryBrowser->setPlaceholderText(QStringLiteral("Explanation for the current page will appear here..."));
    summaryLayout->addLayout(summaryTitleRow);
    summaryLayout->addWidget(m_summaryBrowser);
    splitter->addWidget(summaryContainer);

    // Chat View
    auto *chatContainer = new QWidget(this);
    auto *chatLayout = new QVBoxLayout(chatContainer);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    auto *chatTitle = new QLabel(QStringLiteral("<b>Page Q&A</b>"), this);
    m_chatBrowser = new QTextBrowser(this);
    m_chatBrowser->setOpenExternalLinks(true);
    m_chatBrowser->setPlaceholderText(QStringLiteral("Ask follow-up questions about this page below..."));
    chatLayout->addWidget(chatTitle);
    chatLayout->addWidget(m_chatBrowser);
    splitter->addWidget(chatContainer);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter, 1);

    // Input Area
    auto *inputLayout = new QHBoxLayout();
    m_questionInput = new QLineEdit(this);
    m_questionInput->setPlaceholderText(QStringLiteral("Ask a question about this page..."));
    connect(m_questionInput, &QLineEdit::returnPressed, this, &AIPanelWidget::onSendQuestion);

    m_sendBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("mail-send")), QStringLiteral("Send"), this);
    connect(m_sendBtn, &QPushButton::clicked, this, &AIPanelWidget::onSendQuestion);

    inputLayout->addWidget(m_questionInput, 1);
    inputLayout->addWidget(m_sendBtn);
    mainLayout->addLayout(inputLayout);
}

QString AIPanelWidget::extractPageText(int pageNum) const
{
    if (!m_document) return {};
    const Okular::Page *p = m_document->page(pageNum);
    if (!p) return {};

    // Cap the text sent to the LLM so large/dense pages don't blow up the context window
    static const int MaxPageTextChars = 12000;
    QString text = p->text(nullptr);
    if (text.size() > MaxPageTextChars) {
        text = text.left(MaxPageTextChars) + QStringLiteral("\n[... page text truncated ...]");
    }
    return text;
}

QString AIPanelWidget::getObsidianNoteFilePath() const
{
    if (!m_document) return {};

    QString baseName = QFileInfo(m_document->currentDocument().toLocalFile()).completeBaseName();
    if (baseName.isEmpty()) baseName = QStringLiteral("Okular_Document");

    QString vaultDir = m_settings.obsidianVaultPath.trimmed();
    if (vaultDir.isEmpty()) {
        vaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Obsidian Vault/Okular Notes");
    }
    return QDir(vaultDir).filePath(baseName + QStringLiteral("_AI_Notes.md"));
}

QString AIPanelWidget::getObsidianQnAFilePath() const
{
    if (!m_document) return {};

    QString baseName = QFileInfo(m_document->currentDocument().toLocalFile()).completeBaseName();
    if (baseName.isEmpty()) baseName = QStringLiteral("Okular_Document");

    QString vaultDir = m_settings.obsidianVaultPath.trimmed();
    if (vaultDir.isEmpty()) {
        vaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Obsidian Vault/Okular Notes");
    }
    return QDir(vaultDir).filePath(baseName + QStringLiteral("_AI_QnA.md"));
}

void AIPanelWidget::saveExplanationToObsidian(int pageNum, const QString &explanation)
{
    if (!m_settings.enableObsidianSync || explanation.trimmed().isEmpty()) return;

    const QString notePath = getObsidianNoteFilePath();
    const QFileInfo noteInfo(notePath);
    QDir noteDir(noteInfo.absolutePath());
    if (!noteDir.exists() && !noteDir.mkpath(QStringLiteral("."))) return;

    QFile file(notePath);
    QString content;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        content = QString::fromUtf8(file.readAll());
        file.close();
    }
    if (content.isEmpty()) {
        content = QStringLiteral("# AI Reading Notes\n\n*Generated by Okular AI Page Explainer*\n");
    }

    const QString escapedPage = QRegularExpression::escape(QString::number(pageNum));
    const QRegularExpression headerRegex(QStringLiteral("^## Page %1\\s*$").arg(escapedPage), QRegularExpression::MultilineOption);
    const QString section = QStringLiteral("## Page %1\n\n%2\n").arg(QString::number(pageNum), explanation.trimmed());
    const QRegularExpressionMatch header = headerRegex.match(content);
    if (header.hasMatch()) {
        const QRegularExpressionMatch next = QRegularExpression(QStringLiteral("^## Page \\d+\\s*$"), QRegularExpression::MultilineOption).match(content, header.capturedEnd());
        content.replace(header.capturedStart(), (next.hasMatch() ? next.capturedStart() : content.size()) - header.capturedStart(), section);
    } else {
        content += QStringLiteral("\n\n") + section;
    }

    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(content.toUtf8());
        file.close();
    }
}

void AIPanelWidget::saveChatHistoryToObsidian(int pageNum, const QList<QPair<QString, QString>> &history)
{
    if (!m_settings.enableObsidianSync || history.isEmpty()) return;

    const QString qnaPath = getObsidianQnAFilePath();
    const QFileInfo qnaInfo(qnaPath);
    QDir qnaDir(qnaInfo.absolutePath());
    if (!qnaDir.exists() && !qnaDir.mkpath(QStringLiteral("."))) return;

    // Serialize the history as a JSON array of {"q": ..., "a": ...} objects.
    QJsonArray array;
    for (const auto &pair : history) {
        QJsonObject obj;
        obj.insert(QStringLiteral("q"), pair.first);
        obj.insert(QStringLiteral("a"), pair.second);
        array.append(obj);
    }
    const QJsonDocument doc(array);
    const QString json = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));

    QFile file(qnaPath);
    QString content;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        content = QString::fromUtf8(file.readAll());
        file.close();
    }
    if (content.isEmpty()) {
        content = QStringLiteral("# AI Q&A History\n\n*Generated by Okular AI Page Explainer*\n");
    }

    const QString escapedPage = QRegularExpression::escape(QString::number(pageNum));
    const QRegularExpression headerRegex(QStringLiteral("^## Page %1\\s*$").arg(escapedPage), QRegularExpression::MultilineOption);
    const QString section = QStringLiteral("## Page %1\n\n%2\n").arg(QString::number(pageNum), json.trimmed());
    const QRegularExpressionMatch header = headerRegex.match(content);
    if (header.hasMatch()) {
        const QRegularExpressionMatch next = QRegularExpression(QStringLiteral("^## Page \\d+\\s*$"), QRegularExpression::MultilineOption).match(content, header.capturedEnd());
        content.replace(header.capturedStart(), (next.hasMatch() ? next.capturedStart() : content.size()) - header.capturedStart(), section);
    } else {
        content += QStringLiteral("\n\n") + section;
    }

    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(content.toUtf8());
        file.close();
    }
}

QList<QPair<QString, QString>> AIPanelWidget::loadChatHistoryFromObsidian(int pageNum) const
{
    QList<QPair<QString, QString>> history;
    if (!m_document || pageNum <= 0) return history;
    QFile file(getObsidianQnAFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return history;
    const QString content = QString::fromUtf8(file.readAll());

    const QString escapedPage = QRegularExpression::escape(QString::number(pageNum));
    const QRegularExpression headerRegex(QStringLiteral("^## Page %1\\s*$").arg(escapedPage), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch header = headerRegex.match(content);
    if (!header.hasMatch()) return history;
    const QRegularExpression nextHeader(QStringLiteral("^## Page \\d+\\s*$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch next = nextHeader.match(content, header.capturedEnd());
    const int end = next.hasMatch() ? next.capturedStart() : content.size();
    const QByteArray body = content.mid(header.capturedEnd(), end - header.capturedEnd()).trimmed().toUtf8();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) return history;
    const QJsonArray array = doc.array();
    for (const auto &value : array) {
        const QJsonObject obj = value.toObject();
        history.append(qMakePair(obj.value(QLatin1String("q")).toString(), obj.value(QLatin1String("a")).toString()));
    }
    return history;
}

QString AIPanelWidget::loadPreviousContextFromObsidian(int maxLookback) const
{
    if (!m_document || m_currentPage <= 0) return {};
    QFile file(getObsidianNoteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString content = QString::fromUtf8(file.readAll());
    QStringList parts;
    const int startPage = qMax(0, m_currentPage - maxLookback);
    const QRegularExpression pageRegex(QStringLiteral("^## Page (\\d+)\\s*$"), QRegularExpression::MultilineOption);
    auto it = pageRegex.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int page = match.captured(1).toInt();
        if (page < startPage + 1 || page > m_currentPage) continue;
        const QRegularExpressionMatch next = it.hasNext() ? it.peekNext() : QRegularExpressionMatch();
        const int end = next.hasMatch() ? next.capturedStart() : content.size();
        QString body = content.mid(match.capturedEnd(), end - match.capturedEnd()).trimmed();
        if (page < m_currentPage + 1 && !body.isEmpty()) {
            if (body.size() > 2500) body = body.left(2500) + QStringLiteral("...");
            parts.append(QStringLiteral("[Obsidian Note for Page %1]:\n%2").arg(page).arg(body));
        }
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString AIPanelWidget::loadExplanationFromObsidian(int pageNum) const
{
    if (!m_document || pageNum <= 0) return {};
    QFile file(getObsidianNoteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString content = QString::fromUtf8(file.readAll());

    // Find the "## Page N" section and extract its body up to the next page header.
    const QString escapedPage = QRegularExpression::escape(QString::number(pageNum));
    const QRegularExpression headerRegex(QStringLiteral("^## Page %1\\s*$").arg(escapedPage), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch header = headerRegex.match(content);
    if (!header.hasMatch()) return {};
    const QRegularExpression nextHeader(QStringLiteral("^## Page \\d+\\s*$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch next = nextHeader.match(content, header.capturedEnd());
    const int end = next.hasMatch() ? next.capturedStart() : content.size();
    return content.mid(header.capturedEnd(), end - header.capturedEnd()).trimmed();
}

bool AIPanelWidget::tryRestoreSavedExplanation(int pageNum)
{
    if (!m_document || !m_settings.enableObsidianSync || pageNum < 0) {
        return false;
    }
    PageAIData data = m_pageCache.value(pageNum);
    data.pageNumber = pageNum;
    data.pageText = extractPageText(pageNum);

    const QString saved = loadExplanationFromObsidian(pageNum + 1);
    if (!saved.isEmpty()) {
        data.explanationMarkdown = saved;
        data.isComplete = true;
    }
    const QList<QPair<QString, QString>> chat = loadChatHistoryFromObsidian(pageNum + 1);
    if (!chat.isEmpty()) {
        data.chatHistory = chat;
    }

    if (saved.isEmpty() && chat.isEmpty()) {
        return false;
    }
    m_pageCache[pageNum] = data;
    qDebug() << "[AIPanel] Restored saved explanation (" << saved.size() << " chars) and chat (" << chat.size() << " turns) for page" << (pageNum + 1);
    return true;
}

QString AIPanelWidget::getPreviousPagesContext(int maxLookback) const
{
    if (!m_document || m_currentPage <= 0) return {};

    if (m_settings.enableObsidianSync) {
        const QString obsidianContext = loadPreviousContextFromObsidian(maxLookback);
        if (!obsidianContext.isEmpty()) return obsidianContext;
    }

    QStringList contextParts;
    int startPage = qMax(0, m_currentPage - maxLookback);
    for (int p = startPage; p < m_currentPage; ++p) {
        if (m_pageCache.contains(p) && !m_pageCache[p].explanationMarkdown.isEmpty()) {
            QString summary = m_pageCache[p].explanationMarkdown;
            if (summary.length() > 2500) {
                summary = summary.left(2500) + QStringLiteral("...");
            }
            contextParts.append(QStringLiteral("[Summary of Page %1]:\n%2").arg(p + 1).arg(summary));
        } else {
            // Prefer cached text (which includes OCR'd transcriptions) over a fresh
            // extractPageText() call: scanned pages have no text layer, so the cache
            // is the only source for them.
            QString rawText;
            if (m_pageCache.contains(p) && !m_pageCache[p].pageText.trimmed().isEmpty()) {
                rawText = m_pageCache[p].pageText;
            } else {
                rawText = extractPageText(p);
            }
            if (!rawText.isEmpty()) {
                QString snippet = rawText.left(1200).simplified();
                contextParts.append(QStringLiteral("[Snippet of Page %1]:\n%2").arg(p + 1).arg(snippet));
            }
        }
    }
    return contextParts.join(QStringLiteral("\n\n"));
}

void AIPanelWidget::setPanelEnabled(bool enabled)
{
    if (m_panelEnabled == enabled) return;
    m_panelEnabled = enabled;
    if (!enabled) {
    m_debounceTimer->stop();
    m_visionTimer->stop();
    m_visionRequestPage = -1;
    abortOcr();
    m_searchHelper->cancel();
    m_llmClient->abort();
    m_openCodeClient->abort();
    m_searchPage = -1;
    m_llmRequestPage = -1;
} else if (m_document && m_currentPage >= 0 && !m_isAskingQuestion) {
        // Resume showing the current page once the panel is shown again - prefer
        // a cached/saved explanation, and only regenerate when auto-summarize is on.
        if (m_pageCache.contains(m_currentPage) && m_pageCache[m_currentPage].isComplete) {
            displayCurrentPage();
        } else if (tryRestoreSavedExplanation(m_currentPage)) {
            displayCurrentPage();
        } else if (m_settings.autoSummarize) {
            m_statusLabel->setText(QStringLiteral("Page changing..."));
            m_debounceTimer->start(m_settings.debounceDelayMs);
        }
    }
}

void AIPanelWidget::notifyViewportChanged(bool /*smoothMove*/)
{
    if (!m_document || m_document->pages() == 0 || !m_panelEnabled) return;

    const int newPage = static_cast<int>(m_document->currentPage());
    if (newPage != m_currentPage) {
        stopElapsedTimer();
        m_debounceTimer->stop();
        m_visionTimer->stop();
        m_visionRequestPage = -1;
        abortOcr();
        m_searchHelper->cancel();
        m_llmClient->abort();
        m_openCodeClient->abort();
        m_searchPage = -1;
        m_llmRequestPage = -1;
        if (m_isAskingQuestion && m_pageCache.contains(m_currentPage) && !m_pageCache[m_currentPage].chatHistory.isEmpty()) {
            m_pageCache[m_currentPage].chatHistory.removeLast();
        }
        m_isAskingQuestion = false;
        m_responseHasDisplayedChunks = false;
        m_sendBtn->setEnabled(true);
        m_currentPage = newPage;
        updateHeader();

        m_summaryBrowser->clear();
        m_chatBrowser->clear();
        if (m_pageCache.contains(m_currentPage) && m_pageCache[m_currentPage].isComplete) {
            // Already cached, show instantly without network calls
            displayCurrentPage();
        } else if (tryRestoreSavedExplanation(m_currentPage)) {
            // A saved explanation exists in the Obsidian notes - show it without
            // regenerating (works even when auto-summarize is off).
            displayCurrentPage();
        } else if (m_settings.autoSummarize) {
            // Debounce trigger
            m_statusLabel->setText(QStringLiteral("Page changing..."));
            m_debounceTimer->start(m_settings.debounceDelayMs);
        } else {
            m_statusLabel->setText(QStringLiteral("Press ↻ to explain this page."));
        }
    }
}

void AIPanelWidget::notifyPageChanged(int page, int flags)
{
    if (!(flags & Okular::DocumentObserver::Pixmap) || page != m_visionRequestPage) {
        return;
    }

    // Only act once the full-page pixmap we asked for is actually present;
    // partial updates (if any) are ignored and the final one triggers the flow.
    const Okular::Page *p = m_document ? m_document->page(page) : nullptr;
    if (!p) {
        return;
    }
    int width = 0, height = 0;
    visionImageSize(p, width, height);
    if (p->hasPixmap(this, width, height)) {
        onVisionPagePixmapReady();
    }
}

void AIPanelWidget::visionImageSize(const Okular::Page *page, int &width, int &height) const
{
    // ~200 DPI, capped so the image stays within typical VLM input limits
    const double scale = qMin(200.0 / 72.0, 1536.0 / qMax(page->width(), page->height()));
    width = qMax(1, qRound(page->width() * scale));
    height = qMax(1, qRound(page->height() * scale));

    // For odd rotations the generator stores the pixmap with swapped dimensions
    // (same as PageView's uncroppedWidth/Height); match that here.
    if (m_document && (m_document->rotation() == Okular::Rotation90 || m_document->rotation() == Okular::Rotation270)) {
        std::swap(width, height);
    }
}

void AIPanelWidget::notifySetup(const QList<Okular::Page *> &/*pages*/, int setupFlags)
{
    if (!(setupFlags & (Okular::DocumentObserver::DocumentChanged | Okular::DocumentObserver::UrlChanged))) {
        return;
    }

    stopElapsedTimer();
    m_debounceTimer->stop();
    m_visionTimer->stop();
    m_visionRequestPage = -1;
    abortOcr();
    m_searchHelper->cancel();
    m_llmClient->abort();
    m_openCodeClient->abort();
    m_searchPage = -1;
    m_llmRequestPage = -1;
    if (m_isAskingQuestion && m_pageCache.contains(m_currentPage) && !m_pageCache[m_currentPage].chatHistory.isEmpty()) {
        m_pageCache[m_currentPage].chatHistory.removeLast();
    }
    m_isAskingQuestion = false;
    m_responseHasDisplayedChunks = false;
    m_sendBtn->setEnabled(true);
    m_pageCache.clear();
    m_explainedPagesInSession.clear();
    m_currentPage = -1;
    updateHeader();
    m_summaryBrowser->clear();
    m_chatBrowser->clear();

    // opencode keeps one session per directory; point it at the document's folder
    QString docDir;
    if (m_document && !m_document->currentDocument().toLocalFile().isEmpty()) {
        docDir = QFileInfo(m_document->currentDocument().toLocalFile()).absolutePath();
    }
    if (docDir.isEmpty()) {
        docDir = QDir::homePath();
    }
    m_openCodeClient->setWorkingDirectory(docDir);

    m_statusLabel->setText(QStringLiteral("Document loaded."));
}

void AIPanelWidget::updateHeader()
{
    const int totalPages = m_document ? static_cast<int>(m_document->pages()) : 0;
    if (m_currentPage < 0 || totalPages == 0) {
        m_pageLabel->setText(QStringLiteral("<b>Page -</b>"));
    } else {
        m_pageLabel->setText(QStringLiteral("<b>Page %1 of %2</b>").arg(m_currentPage + 1).arg(totalPages));
    }
}

void AIPanelWidget::onDebounceTimeout()
{
    if (!m_panelEnabled) return;
    triggerCurrentPageExplanation(false);
}

void AIPanelWidget::onRefreshPage()
{
    triggerCurrentPageExplanation(true);
}

void AIPanelWidget::triggerCurrentPageExplanation(bool forceRefresh)
{
    if (!m_panelEnabled || !m_document || m_currentPage < 0 || m_currentPage >= static_cast<int>(m_document->pages())) return;

    if (!forceRefresh && m_pageCache.contains(m_currentPage) && m_pageCache[m_currentPage].isComplete) {
        displayCurrentPage();
        return;
    }

    // The in-memory cache is cleared on every document load, but explanations are
    // persisted to the Obsidian note file - restore the saved one instead of
    // regenerating it (unless the user explicitly forced a refresh).
    if (!forceRefresh && tryRestoreSavedExplanation(m_currentPage)) {
        displayCurrentPage();
        return;
    }

    if (m_isAskingQuestion && m_pageCache.contains(m_currentPage) && !m_pageCache[m_currentPage].chatHistory.isEmpty()) {
        m_pageCache[m_currentPage].chatHistory.removeLast();
    }
    m_isAskingQuestion = false;
    m_responseHasDisplayedChunks = false;
    m_visionTimer->stop();
    m_visionRequestPage = -1;
    abortOcr();
    m_llmRequestPage = m_currentPage;
    m_searchPage = -1;
    m_llmClient->abort();
    m_openCodeClient->abort();
    m_searchHelper->cancel();

    // Extract text from the current page via Okular::Page::text()
    QString pageText = extractPageText(m_currentPage);
    if (pageText.trimmed().length() < 30) {
        m_pageCache.remove(m_currentPage);
        m_llmRequestPage = -1;
        if (m_settings.enableVisionFallback) {
            // Render the page and OCR it (Tesseract first, vision model as fallback),
            // then feed the transcription through the normal explainer.
            qDebug() << "[AIPanel] No selectable text on page" << (m_currentPage + 1) << "- starting OCR chain";
            m_statusLabel->setText(QStringLiteral("No selectable text - OCR via Tesseract..."));
            startElapsedTimer();
            requestPageImageForVision(m_currentPage);
            return;
        }
        stopElapsedTimer();
        m_summaryBrowser->setMarkdown(QStringLiteral("*This page appears to have no selectable text. Attach a vision-capable model to explain it.*"));
        m_statusLabel->setText(QStringLiteral("No selectable text on page."));
        return;
    }

    PageAIData data = m_pageCache.value(m_currentPage);
    data.pageNumber = m_currentPage;
    data.pageText = pageText;
    data.explanationMarkdown.clear();
    data.webSearchContext.clear();
    data.isComplete = false;
    m_pageCache[m_currentPage] = data;

    m_summaryBrowser->clear();
    startElapsedTimer();

    // If this is a mixed document (current page has text but some previous pages
    // are scanned), OCR the scanned previous pages so the model has real context.
    if (m_settings.enableVisionFallback && needsContextBackfill()) {
        startContextBackfill();
        return;
    }

    sendExplanation();
}

void AIPanelWidget::onSearchCompleted(const QString &context)
{
    if (m_searchPage != m_currentPage || !m_pageCache.contains(m_currentPage)) return;

    m_searchPage = -1;
    m_llmRequestPage = m_currentPage;
    const QString prevContext = getPreviousPagesContext(5);
    m_pageCache[m_currentPage].webSearchContext = context;
    m_statusLabel->setText(QStringLiteral("Explaining page with web context..."));
    m_llmClient->explainPage(m_pageCache[m_currentPage].pageText, m_currentPage + 1, context, prevContext, m_settings);
}

void AIPanelWidget::requestPageImageForVision(int pageNum)
{
    m_visionRequestPage = pageNum;
    m_visionTimer->stop();
    m_visionTimer->start(15000); // fallback if the pixmap never arrives

    const Okular::Page *p = m_document ? m_document->page(pageNum) : nullptr;
    if (!p) {
        onVisionTimedOut();
        return;
    }

    int width = 0, height = 0;
    visionImageSize(p, width, height);

    if (!p->hasPixmap(this, width, height)) {
        // Priority 0 means other observers' requests never cancel it.
        Okular::PixmapRequest *req = new Okular::PixmapRequest(this, pageNum, width, height, 1.0, 0, Okular::PixmapRequest::Asynchronous);
        m_document->requestPixmaps({req});
    }
    // If the pixmap was already cached (or a non-threaded generator rendered it
    // synchronously inside requestPixmaps), handle it right away; otherwise
    // notifyPageChanged() will fire when the pixmap is ready.
    if (p->hasPixmap(this, width, height)) {
        onVisionPagePixmapReady();
    }
}

void AIPanelWidget::onVisionPagePixmapReady()
{
    const int pageNum = m_visionRequestPage;
    if (pageNum < 0) {
        return; // already handled
    }
    m_visionRequestPage = -1;
    m_visionTimer->stop();

    const Okular::Page *p = m_document ? m_document->page(pageNum) : nullptr;
    if (!p) {
        visionFailed(QStringLiteral("*Could not render this page for transcription. Check the vision model settings and try again.*"), QStringLiteral("Page image rendering failed."));
        return;
    }
    int width = 0, height = 0;
    visionImageSize(p, width, height);
    if (!p->hasPixmap(this, width, height)) {
        visionFailed(QStringLiteral("*Could not render this page for transcription. Check the vision model settings and try again.*"), QStringLiteral("Page image rendering failed."));
        return;
    }

    QImage img(width, height, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter painter(&img);
    PagePainter::paintPageOnPainter(&painter, p, this, 0, width, height, QRect(0, 0, width, height));
    painter.end();

    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    buffer.close();

    if (pngData.isEmpty()) {
        visionFailed(QStringLiteral("*Could not render this page for transcription. Check the vision model settings and try again.*"), QStringLiteral("Page image rendering failed."));
        return;
    }

    // The rendered page belongs to this OCR chain now (guards against page changes)
    m_ocrPage = pageNum;

    if (m_contextBackfill) {
        // Backfill is tesseract-only: without tesseract there's no cheap way to
        // OCR previous pages, so skip them instead of hitting the vision model.
        if (QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty()) {
            qDebug() << "[AIPanel] no tesseract for backfill - skipping page" << (pageNum + 1);
            continueAfterOcr(QString());
            return;
        }
        m_statusLabel->setText(QStringLiteral("OCR'ing previous pages for context..."));
        runTesseractOnImage(pngData);
        return;
    }

    // 1. Tesseract is the primary, local OCR fallback
    if (!QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty()) {
        m_statusLabel->setText(QStringLiteral("OCR via Tesseract..."));
        runTesseractOnImage(pngData);
        return;
    }

    // 2. No tesseract installed -> vision model (auto-detected llama.cpp server)
    qDebug() << "[AIPanel] tesseract binary not found - skipping straight to vision model";
    m_statusLabel->setText(QStringLiteral("No tesseract - trying vision model..."));
    fallbackToVisionModel(pngData);
}

void AIPanelWidget::runTesseractOnImage(const QByteArray &pngData)
{
    m_pendingVisionPng = pngData;
    m_tesseractProcess = new QProcess(this);
    m_tesseractProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_tesseractProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &AIPanelWidget::onTesseractFinished);
    m_tesseractProcess->start(QStringLiteral("tesseract"), { QStringLiteral("stdin"), QStringLiteral("stdout") });
    if (!m_tesseractProcess->waitForStarted(5000)) {
        QProcess *failed = m_tesseractProcess;
        m_tesseractProcess = nullptr;
        failed->deleteLater();
        if (m_contextBackfill) {
            continueAfterOcr(QString());
        } else if (m_currentPage == m_ocrPage) {
            fallbackToVisionModel(pngData);
        }
        return;
    }
    m_tesseractProcess->write(pngData);
    m_tesseractProcess->closeWriteChannel();
}

void AIPanelWidget::onTesseractFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = m_tesseractProcess;
    m_tesseractProcess = nullptr;
    const QByteArray out = proc ? proc->readAllStandardOutput() : QByteArray();
    if (proc) {
        proc->deleteLater();
    }

    // During a context backfill the OCR'd page is a *previous* page, so only bail
    // when the normal current-page chain was interrupted.
    if (!m_contextBackfill && m_currentPage != m_ocrPage) {
        return; // aborted or navigated away
    }
    if (m_ocrPage < 0) {
        return;
    }

    const QString text = QString::fromUtf8(out).trimmed();
    if (exitCode == 0 && exitStatus == QProcess::NormalExit && text.length() >= 30) {
        qDebug() << "[AIPanel] Tesseract OK:" << text.length() << "chars extracted";
        continueAfterOcr(text);
        return;
    }

    if (m_contextBackfill) {
        // Backfill is best-effort: a page tesseract can't read just yields no context.
        qDebug() << "[AIPanel] Backfill tesseract produced nothing for page" << (m_ocrPage + 1) << "- skipping";
        continueAfterOcr(QString());
        return;
    }

    // Tesseract produced nothing usable -> fall back to the vision model
    qDebug() << "[AIPanel] Tesseract failed: exitCode=" << exitCode << "status=" << static_cast<int>(exitStatus) << "chars=" << text.length();
    m_statusLabel->setText(QStringLiteral("Tesseract found no text - trying vision model..."));
    fallbackToVisionModel(m_pendingVisionPng);
}

void AIPanelWidget::fallbackToVisionModel(const QByteArray &pngData)
{
    qDebug() << "[AIPanel] Falling back to vision model (" << pngData.size() << " bytes PNG)";
    m_llmClient->transcribeImage(pngData, m_settings);
}

void AIPanelWidget::visionFailed(const QString &markdown, const QString &status)
{
    if (m_contextBackfill) {
        // A backfill page failed to render; skip it and continue the queue.
        m_visionRequestPage = -1;
        m_visionTimer->stop();
        qDebug() << "[AIPanel] Backfill page render failed - skipping";
        continueAfterOcr(QString());
        return;
    }
    m_visionRequestPage = -1;
    m_visionTimer->stop();
    stopElapsedTimer();
    m_summaryBrowser->setMarkdown(markdown);
    m_statusLabel->setText(status);
}

void AIPanelWidget::onVisionTimedOut()
{
    // Ignore if we already moved on (e.g. the pixmap arrived and transcription started)
    if (m_visionRequestPage < 0) {
        return;
    }
    if (m_contextBackfill) {
        // Backfill is best-effort: a page that won't render just yields no context.
        m_visionRequestPage = -1;
        m_visionTimer->stop();
        qDebug() << "[AIPanel] Backfill page render timed out - skipping";
        continueAfterOcr(QString());
        return;
    }
    visionFailed(QStringLiteral("*Could not render this page for transcription. Check the vision model settings and try again.*"), QStringLiteral("Page image rendering failed."));
}

void AIPanelWidget::onTranscriptionFinished(const QString &text)
{
    if (m_currentPage < 0 || m_currentPage != m_ocrPage) {
        return;
    }

    const QString transcribed = text.trimmed();
    if (transcribed.length() < 30) {
        m_ocrPage = -1;
        m_pendingVisionPng.clear();
        stopElapsedTimer();
        m_pageCache.remove(m_currentPage);
        m_llmRequestPage = -1;
        m_summaryBrowser->setMarkdown(QStringLiteral("*Could not read this page. Neither Tesseract nor the vision model returned usable text. Make sure tesseract is installed (pacman -S tesseract tesseract-data-eng) or a vision server is running.*"));
        m_statusLabel->setText(QStringLiteral("OCR transcription failed."));
        return;
    }

    continueAfterOcr(transcribed);
}

void AIPanelWidget::continueAfterOcr(const QString &text)
{
    // Backfill mode: this transcription belongs to a previous page being OCR'd
    // purely to build surrounding context, not to the current page.
    if (m_contextBackfill) {
        const int page = m_ocrPage;
        m_ocrPage = -1;
        if (page >= 0 && !text.trimmed().isEmpty()) {
            PageAIData data = m_pageCache.value(page);
            data.pageNumber = page;
            data.pageText = text.trimmed();
            m_pageCache[page] = data;
            qDebug() << "[AIPanel] Backfill OCR page" << (page + 1) << ":" << text.trimmed().length() << "chars";
        }
        if (!m_contextOcrQueue.isEmpty()) {
            m_statusLabel->setText(QStringLiteral("OCR'ing previous pages for context (%1 left)...").arg(m_contextOcrQueue.size() + 1));
            requestPageImageForVision(m_contextOcrQueue.takeFirst());
        } else {
            m_contextBackfill = false;
            m_statusLabel->setText(QStringLiteral("Generating explanation..."));
            sendExplanation();
        }
        return;
    }

    if (m_currentPage < 0 || m_currentPage != m_ocrPage) {
        return;
    }
    m_ocrPage = -1;
    m_pendingVisionPng.clear();

    const QString transcribed = text.trimmed();

    // Continue the normal explanation flow with the transcription as the page text.
    PageAIData data = m_pageCache.value(m_currentPage);
    data.pageNumber = m_currentPage;
    data.pageText = transcribed;
    data.explanationMarkdown.clear();
    data.webSearchContext.clear();
    data.isComplete = false;
    m_pageCache[m_currentPage] = data;

    m_summaryBrowser->clear();

    // The current page is scanned; the previous pages almost certainly are too.
    // OCR the lookback window so the model actually has surrounding context.
    if (m_settings.enableVisionFallback && needsContextBackfill()) {
        startContextBackfill();
        return;
    }

    sendExplanation();
}

void AIPanelWidget::sendExplanation()
{
    if (m_currentPage < 0 || !m_pageCache.contains(m_currentPage)) {
        return;
    }
    const QString transcribed = m_pageCache[m_currentPage].pageText;

    if (usingOpenCodeCli()) {
        m_llmRequestPage = m_currentPage;
        m_explainedPagesInSession.insert(m_currentPage);
        m_statusLabel->setText(QStringLiteral("Sending to opencode session..."));
        const QString prevContext = getPreviousPagesContext(5);
        m_openCodeClient->sendMessage(composeExplainMessage(m_currentPage + 1, transcribed, prevContext), m_settings);
        return;
    }

    const QString prevContext = getPreviousPagesContext(5);
    if (m_settings.enableWebSearch) {
        m_searchPage = m_currentPage;
        m_llmRequestPage = -1;
        m_statusLabel->setText(QStringLiteral("Searching web context for terms..."));
        m_searchHelper->searchContext(transcribed, m_settings.exaApiKey);
    } else {
        m_statusLabel->setText(QStringLiteral("Generating explanation..."));
        m_llmClient->explainPage(transcribed, m_currentPage + 1, QString(), prevContext, m_settings);
    }
}

bool AIPanelWidget::needsContextBackfill() const
{
    if (!m_settings.enableVisionFallback || !m_document || m_currentPage <= 0) {
        return false;
    }
    // If Obsidian notes already cover the window, no need to OCR pages for context.
    if (m_settings.enableObsidianSync && !loadPreviousContextFromObsidian(5).isEmpty()) {
        return false;
    }
    const int start = qMax(0, m_currentPage - 5);
    for (int p = start; p < m_currentPage; ++p) {
        const bool haveText = (m_pageCache.contains(p) && !m_pageCache[p].pageText.trimmed().isEmpty())
            || (m_pageCache.contains(p) && !m_pageCache[p].explanationMarkdown.isEmpty())
            || !extractPageText(p).trimmed().isEmpty();
        if (!haveText) {
            return true;
        }
    }
    return false;
}

void AIPanelWidget::startContextBackfill()
{
    m_contextOcrQueue.clear();
    const int start = qMax(0, m_currentPage - 5);
    for (int p = start; p < m_currentPage; ++p) {
        const bool haveText = (m_pageCache.contains(p) && !m_pageCache[p].pageText.trimmed().isEmpty())
            || (m_pageCache.contains(p) && !m_pageCache[p].explanationMarkdown.isEmpty())
            || !extractPageText(p).trimmed().isEmpty();
        if (!haveText) {
            m_contextOcrQueue.append(p);
        }
    }
    if (m_contextOcrQueue.isEmpty()) {
        sendExplanation();
        return;
    }
    m_contextBackfill = true;
    m_statusLabel->setText(QStringLiteral("OCR'ing previous pages for context..."));
    requestPageImageForVision(m_contextOcrQueue.takeFirst());
}

void AIPanelWidget::onChunkReceived(const QString &chunk)
{
    if (m_llmRequestPage != m_currentPage) return;

    m_responseHasDisplayedChunks = true;
    if (m_isAskingQuestion) {
        // Accumulate the streaming answer and re-render the chat so markdown
        // (bold, lists, code, headings) renders properly instead of raw text.
        QScrollBar *chatScroll = m_chatBrowser->verticalScrollBar();
        const double ratio = chatScroll->maximum() > 0 ? static_cast<double>(chatScroll->value()) / chatScroll->maximum() : 1.0;
        if (m_pageCache.contains(m_currentPage) && !m_pageCache[m_currentPage].chatHistory.isEmpty()) {
            m_pageCache[m_currentPage].chatHistory.last().second.append(chunk);
        }
        renderChat();
        if (ratio >= 0.98) {
            m_chatBrowser->verticalScrollBar()->setValue(m_chatBrowser->verticalScrollBar()->maximum());
        } else {
            m_chatBrowser->verticalScrollBar()->setValue(qRound(ratio * m_chatBrowser->verticalScrollBar()->maximum()));
        }
    } else if (m_pageCache.contains(m_currentPage)) {
        m_pageCache[m_currentPage].explanationMarkdown.append(chunk);

        // setMarkdown() rebuilds the document and would jump the scrollbar to the top;
        // keep it pinned to the bottom while streaming (or preserve the user's position).
        QScrollBar *scroll = m_summaryBrowser->verticalScrollBar();
        const double ratio = scroll->maximum() > 0 ? static_cast<double>(scroll->value()) / scroll->maximum() : 1.0;
        m_summaryBrowser->setMarkdown(m_pageCache[m_currentPage].explanationMarkdown);
        if (ratio >= 0.98) {
            m_summaryBrowser->verticalScrollBar()->setValue(m_summaryBrowser->verticalScrollBar()->maximum());
        } else {
            m_summaryBrowser->verticalScrollBar()->setValue(qRound(ratio * m_summaryBrowser->verticalScrollBar()->maximum()));
        }
    }
}

void AIPanelWidget::onStreamFinished(const QString &fullResponse)
{
    if (m_llmRequestPage != m_currentPage || m_currentPage < 0) return;

    m_llmRequestPage = -1;
    stopElapsedTimer();
    m_statusLabel->setText(QStringLiteral("Done."));
    if (m_isAskingQuestion) {
        if (m_pageCache.contains(m_currentPage) && !m_pageCache[m_currentPage].chatHistory.isEmpty()) {
            m_pageCache[m_currentPage].chatHistory.last().second = fullResponse;
            saveChatHistoryToObsidian(m_currentPage + 1, m_pageCache[m_currentPage].chatHistory);
        }
        renderChat();
        m_chatBrowser->verticalScrollBar()->setValue(m_chatBrowser->verticalScrollBar()->maximum());
        m_isAskingQuestion = false;
        m_sendBtn->setEnabled(true);
    } else {
        if (!m_pageCache.contains(m_currentPage)) {
            PageAIData data;
            data.pageNumber = m_currentPage;
            data.pageText = extractPageText(m_currentPage);
            m_pageCache[m_currentPage] = data;
        }
        m_pageCache[m_currentPage].explanationMarkdown = fullResponse;
        m_pageCache[m_currentPage].isComplete = !fullResponse.isEmpty();
        m_summaryBrowser->setMarkdown(fullResponse.isEmpty() ? QStringLiteral("*No explanation was generated.*") : fullResponse);
        if (!fullResponse.isEmpty()) {
            saveExplanationToObsidian(m_currentPage + 1, fullResponse);
        }
    }
}

void AIPanelWidget::onErrorOccurred(const QString &error)
{
    if (m_llmRequestPage != m_currentPage || m_currentPage < 0) return;

    m_llmRequestPage = -1;
    stopElapsedTimer();
    const QString escapedError = error.toHtmlEscaped();
    m_statusLabel->setText(QStringLiteral("<font color='red'>Error: %1</font>").arg(escapedError));
    if (m_isAskingQuestion) {
        m_isAskingQuestion = false;
        m_sendBtn->setEnabled(true);
        if (m_pageCache.contains(m_currentPage) && !m_pageCache[m_currentPage].chatHistory.isEmpty()) {
            m_pageCache[m_currentPage].chatHistory.removeLast();
            displayCurrentPage();
        }
    } else if (m_pageCache.contains(m_currentPage)) {
        m_pageCache[m_currentPage].isComplete = false;
        m_summaryBrowser->setMarkdown(QStringLiteral("### Error Generating Explanation\n%1\n\n*Check your API key and provider settings in the settings dialog.*").arg(escapedError));
    }
}

void AIPanelWidget::onSendQuestion()
{
    QString question = m_questionInput->text().trimmed();
    if (question.isEmpty() || m_currentPage < 0 || (m_isAskingQuestion && m_llmRequestPage == m_currentPage)) return;

    m_questionInput->clear();
    m_searchHelper->cancel();
    m_searchPage = -1;
    m_isAskingQuestion = true;
    m_responseHasDisplayedChunks = false;
    startElapsedTimer();

    if (!m_pageCache.contains(m_currentPage)) {
        PageAIData data;
        data.pageNumber = m_currentPage;
        data.pageText = extractPageText(m_currentPage);
        m_pageCache[m_currentPage] = data;
    }

    m_pageCache[m_currentPage].chatHistory.append(qMakePair(question, QString()));

    m_llmRequestPage = m_currentPage;
    m_llmClient->abort();
    m_openCodeClient->abort();
    appendChatMessage(QStringLiteral("You"), question, true);
    appendChatMessage(QStringLiteral("AI"), QString(), false);

    m_sendBtn->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("Answering question..."));
    if (usingOpenCodeCli()) {
        // The opencode session already holds the conversation (including the page
        // explanation), so the question is sent as a follow-up message.
        m_openCodeClient->sendMessage(composeQuestionMessage(m_currentPage + 1, m_pageCache[m_currentPage].pageText, question), m_settings);
        return;
    }
    QList<QPair<QString, QString>> previousHistory = m_pageCache[m_currentPage].chatHistory;
    previousHistory.removeLast();
    m_llmClient->askQuestion(m_pageCache[m_currentPage].pageText, m_currentPage + 1, previousHistory, question, m_settings);
}

void AIPanelWidget::appendChatMessage(const QString &sender, const QString &text, bool isUser)
{
    const QString escapedSender = sender.toHtmlEscaped();
    const QString escapedText = text.toHtmlEscaped();
    if (isUser) {
        m_chatBrowser->append(QStringLiteral("<b>%1:</b> %2\n").arg(escapedSender, escapedText));
    } else {
        m_chatBrowser->append(QStringLiteral("<b>%1:</b> ").arg(escapedSender));
    }
}

void AIPanelWidget::renderChat()
{
    if (!m_pageCache.contains(m_currentPage)) {
        m_chatBrowser->clear();
        return;
    }
    const auto &data = m_pageCache[m_currentPage];
    QString html;
    for (const auto &pair : data.chatHistory) {
        html += QStringLiteral("<p><b>You:</b> %1</p>").arg(pair.first.toHtmlEscaped());
        html += QStringLiteral("<p><b>AI:</b> %1</p>").arg(markdownToBodyHtml(pair.second));
    }
    m_chatBrowser->setHtml(html);
}

void AIPanelWidget::displayCurrentPage()
{
    if (!m_pageCache.contains(m_currentPage)) return;

    const auto &data = m_pageCache[m_currentPage];
    m_summaryBrowser->setMarkdown(data.explanationMarkdown);
    renderChat();
    m_statusLabel->setText(QStringLiteral("Cached summary loaded."));
}

void AIPanelWidget::startElapsedTimer()
{
    m_elapsedSeconds = 0;
    m_timerLabel->setText(QStringLiteral("0s"));
    m_elapsedTimer->start();
}

void AIPanelWidget::stopElapsedTimer()
{
    m_elapsedTimer->stop();
    m_elapsedSeconds = 0;
    m_timerLabel->clear();
}

void AIPanelWidget::onElapsedTick()
{
    ++m_elapsedSeconds;
    m_timerLabel->setText(QStringLiteral("%1s").arg(m_elapsedSeconds));
}

void AIPanelWidget::onClosePanel()
{
    Q_EMIT closeRequested();
}

void AIPanelWidget::onToggleSearch()
{
    m_settings.enableWebSearch = !m_settings.enableWebSearch;
    m_settings.save();
    m_searchToggleBtn->setText(m_settings.enableWebSearch ? QStringLiteral("Search: ON") : QStringLiteral("Search: OFF"));

    // When turning search on, re-explain the current page so the toggle takes effect immediately
    // (only when auto-summarize is on, to respect an "auto off" preference)
    if (m_settings.enableWebSearch && m_settings.autoSummarize && m_panelEnabled && m_currentPage >= 0 && !m_isAskingQuestion) {
        triggerCurrentPageExplanation(true);
    }
}

void AIPanelWidget::onToggleAutoSummarize()
{
    m_settings.autoSummarize = !m_settings.autoSummarize;
    m_settings.save();
    m_autoSummarizeBtn->setText(m_settings.autoSummarize ? QStringLiteral("Auto: ON") : QStringLiteral("Auto: OFF"));
}

void AIPanelWidget::onOpenSettings()
{
    AISettingsDialog dlg(m_nam, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_settings = dlg.settings();
        m_searchToggleBtn->setText(m_settings.enableWebSearch ? QStringLiteral("Search: ON") : QStringLiteral("Search: OFF"));
        m_autoSummarizeBtn->setText(m_settings.autoSummarize ? QStringLiteral("Auto: ON") : QStringLiteral("Auto: OFF"));
    }
}
