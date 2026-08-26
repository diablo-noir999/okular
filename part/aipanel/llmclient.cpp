#include "llmclient.h"
#include <QUrl>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

static bool usesResponsesApi(const QString &model)
{
    // OpenCode Zen serves some models only through the OpenAI Responses API (/v1/responses),
    // e.g. the GPT-5, Grok and Muse Spark families (including muse-spark-1.2-contributor-free).
    // Chat-completions-only models (big-pickle, mimo-v2.5-free, hy3-free, deepseek-*, glm-*, kimi-*) must NOT match.
    return model.startsWith(QLatin1String("gpt-5")) || model.startsWith(QLatin1String("grok-")) || model.startsWith(QLatin1String("muse-spark"));
}

// Conservative estimate: ~3 characters per token (English text).
static int estimateTokens(const QString &text)
{
    return (text.size() + 2) / 3;
}

static QString truncateToTokens(const QString &text, int budgetTokens)
{
    if (estimateTokens(text) <= budgetTokens) {
        return text;
    }
    const int maxChars = qMax(300, budgetTokens * 3);
    return text.left(maxChars) + QStringLiteral("\n[... content truncated to fit the model context window ...]");
}

static QString extractResponsesApiText(const QJsonObject &obj)
{
    const QJsonArray output = obj.value(QLatin1String("output")).toArray();
    for (const auto &item : output) {
        const QJsonObject o = item.toObject();
        if (o.value(QLatin1String("type")).toString() == QLatin1String("message")) {
            const QJsonArray content = o.value(QLatin1String("content")).toArray();
            if (!content.isEmpty()) {
                return content.first().toObject().value(QLatin1String("text")).toString();
            }
        }
    }
    return QString();
}

// Retry budget for rate-limited (HTTP 429) requests, with matching backoff delays in seconds
static const int kMaxRetries = 2;
static const int kRetryDelays[kMaxRetries] = { 3, 6 };

// Hard cap on how long a single generation may run before we give up with a clear error.
// Reasoning models (e.g. hy3-free) can legitimately think for minutes, and their output
// budget is now generous (16384 tokens), so this is purely a runaway guard.
static const int kGenerationTimeoutSec = 600;

static bool isVisionModelId(const QString &id)
{
    const QString lower = id.toLower();
    static const QStringList hints = {
        QStringLiteral("vision"), QStringLiteral("qwen2.5vl"), QStringLiteral("qwen2-vl"), QStringLiteral("llava"), QStringLiteral("moondream"),
        QStringLiteral("minicpm"), QStringLiteral("granite3.2-vision"), QStringLiteral("ocr"), QStringLiteral("smolvlm"), QStringLiteral("internvl"),
        QStringLiteral("phi-3.5-vision"), QStringLiteral("pixtral"), QStringLiteral("idefics"), QStringLiteral("bakllava"),
    };
    for (const QString &hint : hints) {
        if (lower.contains(hint)) {
            return true;
        }
    }
    return false;
}

// Pick a vision-capable model from a server's model list; fall back to the first one.
static QString pickVisionModel(const QStringList &ids)
{
    for (const QString &id : ids) {
        if (isVisionModelId(id)) {
            return id;
        }
    }
    return ids.isEmpty() ? QString() : ids.first();
}

LLMClient::LLMClient(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_currentProvider(AISettings::OpenCode)
{
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &LLMClient::sendStoredRequest);

    m_generationTimer = new QTimer(this);
    m_generationTimer->setSingleShot(true);
    connect(m_generationTimer, &QTimer::timeout, this, &LLMClient::onGenerationTimeout);
}

LLMClient::~LLMClient()
{
    abort();
}

void LLMClient::abort()
{
    m_retryTimer->stop();
    m_generationTimer->stop();
    m_retriesLeft = 0;
    m_streamDone = false;
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    if (m_transcribeReply) {
        m_transcribeReply->disconnect(this);
        m_transcribeReply->abort();
        m_transcribeReply->deleteLater();
        m_transcribeReply = nullptr;
    }
    if (m_probeReply) {
        m_probeReply->disconnect(this);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply = nullptr;
    }
    m_isTranscribing = false;
    m_buffer.clear();
    m_accumulatedResponse.clear();
    m_accumulatedReasoning.clear();
    m_lastReasoningEmitMs = 0;
}

void LLMClient::explainPage(const QString &pageText, int pageNumber, const QString &webSearchContext, const QString &previousContext, const AISettings &settings)
{
    abort();
    m_isTestMode = false;
    m_currentProvider = settings.provider;

    QString systemPrompt = QStringLiteral(
        "You are an expert tutor embedded in the Okular PDF viewer, helping a learner understand a document page by page.\n"
        "For page %1:\n"
        "1. Write a concise, plain-English summary of the page text (3-6 bullet points).\n"
        "2. Explain the key terms, formulas, or concepts clearly, including the building blocks a beginner might be missing.\n"
        "3. Connect the page to previous pages and to any web search background when provided.\n"
        "4. Be honest when the page is mostly figures, tables of contents, or references.\n"
        "5. End with one short question or prompt that moves the learner forward (e.g. what to look out for next).\n"
        "Keep it structured with markdown headings and bullets.\n"
        "Style: warm, direct, specific; no emoji, no cheerleading. Point out what commonly trips people up.\n"
        "If the page appears to be graded coursework or an exam, explain concepts and method rather than providing answers that could be submitted."
    ).arg(pageNumber);

    // Keep the whole request inside the model's context window. The available input
    // budget is (window - response budget - reserve), split by priority:
    // current page text first, then previous context, then web search context.
    const int windowTokens = qMax(4096, settings.contextWindowTokens);
    const int responseTokens = qBound(100, settings.maxTokens, 262144);
    const int reserveTokens = 1024;
    const int inputBudgetTokens = qMax(2048, windowTokens - responseTokens - reserveTokens - estimateTokens(systemPrompt));

    int remainingTokens = inputBudgetTokens;
    const QString cappedPageText = truncateToTokens(pageText, (remainingTokens * 3) / 5);
    remainingTokens -= estimateTokens(cappedPageText);

    const QString cappedPrevContext = truncateToTokens(previousContext, (remainingTokens * 3) / 4);
    remainingTokens -= estimateTokens(cappedPrevContext);

    const QString cappedWebContext = truncateToTokens(webSearchContext, remainingTokens);

    QString userContent;
    if (!cappedPrevContext.isEmpty()) {
        userContent += QStringLiteral("Previous Document Context / Summary:\n---\n%1\n---\n\n").arg(cappedPrevContext);
    }
    userContent += QStringLiteral("Here is the text of page %1:\n---\n%2\n---\n").arg(QString::number(pageNumber), cappedPageText);
    if (!cappedWebContext.isEmpty()) {
        userContent += QStringLiteral("\nAdditional Web Search Background:\n---\n%1\n---\n").arg(cappedWebContext);
    }
    userContent += QStringLiteral("\nPlease explain and summarize page %1.").arg(pageNumber);

    QJsonArray messages;
    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = userContent;
    messages.append(userMsg);

    sendChatCompletion(systemPrompt, messages, settings, false);
}

void LLMClient::askQuestion(const QString &pageText, int pageNumber, const QList<QPair<QString, QString>> &history, const QString &question, const AISettings &settings)
{
    abort();
    m_isTestMode = false;
    m_currentProvider = settings.provider;

    const int windowTokens = qMax(4096, settings.contextWindowTokens);
    const int responseTokens = qBound(100, settings.maxTokens, 262144);
    const int reserveTokens = 1024;

    // Keep only the most recent history pairs that fit within a quarter of the window,
    // so the page context and the current question always have room.
    QList<QPair<QString, QString>> cappedHistory;
    int historyTokens = 0;
    const int historyBudgetTokens = windowTokens / 4;
    for (auto it = history.crbegin(); it != history.crend(); ++it) {
        const int pairTokens = estimateTokens(it->first) + estimateTokens(it->second);
        if (!cappedHistory.isEmpty() && historyTokens + pairTokens > historyBudgetTokens) {
            break;
        }
        cappedHistory.prepend(*it);
        historyTokens += pairTokens;
    }

    // The page text embedded in the system prompt gets whatever budget is left
    const QString preambleWithText = QStringLiteral(
        "You are an expert tutor embedded in the Okular PDF viewer, helping with page %1.\n"
        "Here is the page text:\n---\n%2\n---\n"
        "Teaching style:\n"
        "- The goal is to help the learner answer their own question, this time and next time.\n"
        "- Diagnose first: is the confusion about the concept, the procedure, the notation, or what the question is asking? If they already show fluency, teach at their level; if they are stuck, give a foothold (do the first step, name the rule) rather than just the answer.\n"
        "- One step per turn: a focused question, a hint, a worked parallel example, or a restatement of what they got right. Never a wall of questions, never an empty turn.\n"
        "- Answer directly when it is a time-boxed request, a quick confirmation, or a topic where they want substance rather than scaffolding, then offer to go deeper.\n"
        "- Do not produce final answers to graded problem sets or exams; teach with examples distinct from their assigned work.\n"
        "- Tone: warm, direct, no emoji, no cheerleading. If unsure of your own reasoning, say so."
    ).arg(QString::number(pageNumber), QStringLiteral("X").repeated(100));
    const int pageBudgetTokens = qMax(512, windowTokens - responseTokens - reserveTokens - estimateTokens(preambleWithText) - historyTokens - estimateTokens(question));

    QString systemPrompt = QStringLiteral(
        "You are an expert tutor embedded in the Okular PDF viewer, helping with page %1.\n"
        "Here is the page text:\n---\n%2\n---\n"
        "Teaching style:\n"
        "- The goal is to help the learner answer their own question, this time and next time.\n"
        "- Diagnose first: is the confusion about the concept, the procedure, the notation, or what the question is asking? If they already show fluency, teach at their level; if they are stuck, give a foothold (do the first step, name the rule) rather than just the answer.\n"
        "- One step per turn: a focused question, a hint, a worked parallel example, or a restatement of what they got right. Never a wall of questions, never an empty turn.\n"
        "- Answer directly when it is a time-boxed request, a quick confirmation, or a topic where they want substance rather than scaffolding, then offer to go deeper.\n"
        "- Do not produce final answers to graded problem sets or exams; teach with examples distinct from their assigned work.\n"
        "- Tone: warm, direct, no emoji, no cheerleading. If unsure of your own reasoning, say so."
    ).arg(QString::number(pageNumber), truncateToTokens(pageText, pageBudgetTokens));

    QJsonArray messages;
    for (const auto &pair : std::as_const(cappedHistory)) {
        QJsonObject qMsg;
        qMsg[QStringLiteral("role")] = QStringLiteral("user");
        qMsg[QStringLiteral("content")] = pair.first;
        messages.append(qMsg);

        QJsonObject aMsg;
        aMsg[QStringLiteral("role")] = QStringLiteral("assistant");
        aMsg[QStringLiteral("content")] = pair.second;
        messages.append(aMsg);
    }

    QJsonObject currentQ;
    currentQ[QStringLiteral("role")] = QStringLiteral("user");
    currentQ[QStringLiteral("content")] = question;
    messages.append(currentQ);

    sendChatCompletion(systemPrompt, messages, settings, false);
}

void LLMClient::testConnection(const AISettings &settings)
{
    abort();
    m_isTestMode = true;
    m_currentProvider = settings.provider;

    QString systemPrompt = QStringLiteral("You are a helpful assistant.");
    QJsonArray messages;
    QJsonObject msg;
    msg[QStringLiteral("role")] = QStringLiteral("user");
    msg[QStringLiteral("content")] = QStringLiteral("Say hello in one word.");
    messages.append(msg);

    sendChatCompletion(systemPrompt, messages, settings, true);
}

void LLMClient::transcribeImage(const QByteArray &imageData, const AISettings &settings)
{
    if (imageData.isEmpty()) {
        Q_EMIT transcriptionFinished(QString());
        return;
    }
    abort();
    m_isTranscribing = true;

    const QString configuredUrl = settings.visionApiUrl.trimmed();
    const QString configuredModel = settings.visionModel.trimmed();

    if (!configuredUrl.isEmpty() && !configuredModel.isEmpty()) {
        // Explicit configuration wins (advanced users)
        sendTranscription(imageData, settings, configuredUrl, configuredModel);
        return;
    }

    // Otherwise auto-detect: probe the configured URL (if any) and the common
    // llama.cpp default port for a vision-capable model.
    QStringList bases;
    if (!configuredUrl.isEmpty()) {
        bases << configuredUrl;
    }
    bases << QStringLiteral("http://localhost:8080") << QStringLiteral("http://127.0.0.1:8080");
    QStringList uniqueBases;
    for (const QString &base : std::as_const(bases)) {
        if (!uniqueBases.contains(base)) {
            uniqueBases.append(base);
        }
    }

    probeVisionModels(uniqueBases, 0, imageData, settings);
}

void LLMClient::probeVisionModels(const QStringList &bases, int index, const QByteArray &imageData, const AISettings &settings)
{
    if (index >= bases.size() || !m_isTranscribing) {
        m_isTranscribing = false;
        Q_EMIT transcriptionFinished(QString());
        return;
    }

    QString base = bases[index];
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }

    QNetworkRequest request(QUrl(base + QStringLiteral("/v1/models")));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(3000);
    if (!settings.visionApiKey.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(settings.visionApiKey).toUtf8());
    }

    qDebug() << "[AIPanel] Vision auto-detect: probing" << base << "/v1/models";
    m_probeReply = m_nam->get(request);
    QNetworkReply *probeReply = m_probeReply;
    connect(probeReply, &QNetworkReply::finished, this, [this, probeReply, bases, index, imageData, settings] {
        if (m_probeReply == probeReply) {
            m_probeReply = nullptr;
        }
        probeReply->deleteLater();

        if (!m_isTranscribing) {
            return; // aborted
        }
        if (probeReply->error() == QNetworkReply::NoError) {
            const QJsonDocument doc = QJsonDocument::fromJson(probeReply->readAll());
            const QJsonArray models = doc.object().value(QLatin1String("data")).toArray();
            QStringList ids;
            for (const auto &m : models) {
                ids.append(m.toObject().value(QLatin1String("id")).toString());
            }
            const QString model = pickVisionModel(ids);
            qDebug() << "[AIPanel] Vision server found at" << bases[index] << "- models:" << ids.join(QLatin1String(", ")) << "-> picked:" << model;
            if (!model.isEmpty()) {
                sendTranscription(imageData, settings, bases[index], model);
                return;
            }
        } else {
            qDebug() << "[AIPanel] No vision server at" << bases[index] << ":" << probeReply->errorString();
        }
        probeVisionModels(bases, index + 1, imageData, settings);
    });
}

void LLMClient::sendTranscription(const QByteArray &imageData, const AISettings &settings, const QString &baseUrl, const QString &model)
{
    if (!m_isTranscribing) {
        Q_EMIT transcriptionFinished(QString());
        return;
    }

    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    QUrl url(base + QStringLiteral("/v1/chat/completions"));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // VLM inference on CPU (e.g. llama.cpp) can take a while
    request.setTransferTimeout(120000);
    if (!settings.visionApiKey.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(settings.visionApiKey).toUtf8());
    }

    QJsonObject payload;
    payload[QStringLiteral("model")] = model;
    payload[QStringLiteral("stream")] = false;
    payload[QStringLiteral("max_tokens")] = 2048;

    QJsonArray messages;
    QJsonObject sysMsg;
    sysMsg[QStringLiteral("role")] = QStringLiteral("system");
    sysMsg[QStringLiteral("content")] = QStringLiteral(
        "You are an OCR engine embedded in a PDF viewer. Transcribe all visible text from the provided page image "
        "verbatim, preserving line breaks, paragraph structure, headings and lists as best you can. "
        "Ignore page numbers, running headers and footers unless they carry content. Output only the transcription.");
    messages.append(sysMsg);

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    QJsonArray content;
    QJsonObject textPart;
    textPart[QStringLiteral("type")] = QStringLiteral("text");
    textPart[QStringLiteral("text")] = QStringLiteral("Transcribe the text in this page image.");
    content.append(textPart);
    QJsonObject imagePart;
    imagePart[QStringLiteral("type")] = QStringLiteral("image_url");
    QJsonObject imageUrl;
    imageUrl[QStringLiteral("url")] = QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(imageData.toBase64()));
    imagePart[QStringLiteral("image_url")] = imageUrl;
    content.append(imagePart);
    userMsg[QStringLiteral("content")] = content;
    messages.append(userMsg);

    payload[QStringLiteral("messages")] = messages;

    m_transcribeReply = m_nam->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(m_transcribeReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_transcribeReply;
        m_transcribeReply = nullptr;
        m_isTranscribing = false;
        reply->deleteLater();

        QString text;
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray data = reply->readAll();
            const QJsonDocument doc = QJsonDocument::fromJson(data);
            const QJsonObject obj = doc.object();
            const QJsonArray choices = obj.value(QLatin1String("choices")).toArray();
            if (!choices.isEmpty()) {
                text = choices.first().toObject().value(QLatin1String("message")).toObject().value(QLatin1String("content")).toString();
            }
        } else {
            qDebug() << "[AIPanel] Vision transcription error:" << reply->errorString();
        }
        qDebug() << "[AIPanel] Vision transcription:" << text.trimmed().length() << "chars";
        Q_EMIT transcriptionFinished(text.trimmed());
    });
}

void LLMClient::sendChatCompletion(const QString &systemPrompt, const QJsonArray &messages, const AISettings &settings, bool isTest)
{
    QUrl url;
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream");
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(30000);

    QJsonObject payload;
    payload[QStringLiteral("model")] = settings.model.isEmpty() ? QStringLiteral("mimo-v2.5-free") : settings.model;
    payload[QStringLiteral("stream")] = !isTest;
    // Test mode needs enough tokens for reasoning models (they spend tokens thinking
    // before producing any content; 20 tokens is consumed entirely by reasoning).
    payload[QStringLiteral("max_tokens")] = isTest ? 300 : settings.maxTokens;

    if (settings.provider == AISettings::Anthropic) {
        QString base = settings.apiUrl.isEmpty() ? QStringLiteral("https://api.anthropic.com") : settings.apiUrl;
        if (base.endsWith(QLatin1Char('/'))) base.chop(1);
        url = QUrl(base + QStringLiteral("/v1/messages"));

        request.setRawHeader("x-api-key", settings.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");

        payload[QStringLiteral("system")] = systemPrompt;
        payload[QStringLiteral("messages")] = messages;
    } else if (settings.provider == AISettings::Ollama) {
        QString base = settings.apiUrl.isEmpty() ? QStringLiteral("http://localhost:11434") : settings.apiUrl;
        if (base.endsWith(QLatin1Char('/'))) base.chop(1);
        url = QUrl(base + QStringLiteral("/api/chat"));

        QJsonArray allMessages;
        QJsonObject sysObj;
        sysObj[QStringLiteral("role")] = QStringLiteral("system");
        sysObj[QStringLiteral("content")] = systemPrompt;
        allMessages.append(sysObj);
        for (const auto &m : messages) {
            allMessages.append(m);
        }
        payload[QStringLiteral("messages")] = allMessages;
        // Ollama ignores max_tokens; use options.num_predict instead
        QJsonObject options;
        options[QStringLiteral("num_predict")] = isTest ? 20 : settings.maxTokens;
        payload[QStringLiteral("options")] = options;
    } else {
        // OpenCode, OpenAI, Custom (OpenAI-compatible)
        QString base = settings.apiUrl.isEmpty() ? QStringLiteral("https://opencode.ai/zen") : settings.apiUrl;
        if (base.endsWith(QLatin1Char('/'))) base.chop(1);

        if (!settings.apiKey.isEmpty()) {
            request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(settings.apiKey).toUtf8());
        }

        if (settings.provider == AISettings::OpenCode && usesResponsesApi(settings.model)) {
            // OpenAI Responses API format (only some OpenCode Zen models are served via /v1/responses)
            url = QUrl(base + QStringLiteral("/v1/responses"));
            payload.remove(QStringLiteral("max_tokens"));
            payload[QStringLiteral("instructions")] = systemPrompt;
            // Responses-API reasoning models (e.g. muse-spark) burn hundreds of tokens
            // thinking before answering; the test needs a large enough budget to get any output.
            payload[QStringLiteral("max_output_tokens")] = isTest ? 2048 : settings.maxTokens;
            payload[QStringLiteral("input")] = messages;
        } else {
            url = QUrl(base + QStringLiteral("/v1/chat/completions"));

            QJsonArray allMessages;
            QJsonObject sysObj;
            sysObj[QStringLiteral("role")] = QStringLiteral("system");
            sysObj[QStringLiteral("content")] = systemPrompt;
            allMessages.append(sysObj);
            for (const auto &m : messages) {
                allMessages.append(m);
            }
            payload[QStringLiteral("messages")] = allMessages;
        }
    }

    request.setUrl(url);

    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    // Remember the request so a rate-limited response can be retried
    m_retryRequest = request;
    m_retryBody = body;
    m_retriesLeft = kMaxRetries;
    m_retryTimer->stop();
    m_lastReasoningEmitMs = 0;
    m_streamDone = false;

    qDebug() << "[AIPanel] LLM request:" << url.toString() << "model=" << payload.value(QLatin1String("model")).toString() << "stream=" << !isTest;
    m_requestTimer.start();
    m_generationTimer->start(kGenerationTimeoutSec * 1000);
    m_currentReply = m_nam->post(request, body);
    connect(m_currentReply, &QNetworkReply::readyRead, this, &LLMClient::onReadyRead);
    connect(m_currentReply, &QNetworkReply::finished, this, &LLMClient::onFinished);
}

void LLMClient::sendStoredRequest()
{
    if (m_retryRequest.url().isEmpty()) {
        return;
    }
    m_buffer.clear();
    m_accumulatedResponse.clear();
    m_accumulatedReasoning.clear();
    m_lastReasoningEmitMs = 0;
    m_streamDone = false;
    m_requestTimer.restart();
    m_generationTimer->start(kGenerationTimeoutSec * 1000);

    m_currentReply = m_nam->post(m_retryRequest, m_retryBody);
    connect(m_currentReply, &QNetworkReply::readyRead, this, &LLMClient::onReadyRead);
    connect(m_currentReply, &QNetworkReply::finished, this, &LLMClient::onFinished);
}

void LLMClient::onReadyRead()
{
    if (!m_currentReply) return;

    QByteArray data = m_currentReply->readAll();
    m_buffer.append(data);

    if (m_isTestMode) {
        return; // handle in onFinished for non-streamed test
    }

    parseSSE(m_buffer, m_currentProvider);
}

void LLMClient::parseSSE(const QByteArray &/*data*/, AISettings::Provider provider)
{
    int newlineIdx = -1;
    while ((newlineIdx = m_buffer.indexOf('\n')) != -1) {
        QByteArray line = m_buffer.left(newlineIdx).trimmed();
        m_buffer.remove(0, newlineIdx + 1);

        if (line.isEmpty()) {
            continue;
        }

        QByteArray jsonStr;

        if (provider == AISettings::Ollama) {
            // Ollama streams newline-delimited JSON (no SSE "data: " prefix)
            jsonStr = line;
        } else {
            // SSE format: lines prefixed with "data: "
            if (!line.startsWith("data: ")) {
                continue;
            }
            jsonStr = line.mid(6).trimmed();
            if (jsonStr == "[DONE]") {
                // Some servers send the [DONE] marker and then hold the connection open;
                // stop parsing and finish the request now instead of waiting forever.
                m_streamDone = true;
                break;
            }
        }

        QJsonDocument doc = QJsonDocument::fromJson(jsonStr);
        if (!doc.isObject()) {
            continue;
        }

        QJsonObject obj = doc.object();
        QString deltaText;

        if (provider == AISettings::Anthropic) {
            if (obj.value(QStringLiteral("type")).toString() == QLatin1String("content_block_delta")) {
                deltaText = obj.value(QStringLiteral("delta")).toObject().value(QStringLiteral("text")).toString();
            } else if (obj.contains(QStringLiteral("content"))) {
                QJsonArray contentArr = obj.value(QStringLiteral("content")).toArray();
                if (!contentArr.isEmpty()) {
                    deltaText = contentArr.first().toObject().value(QStringLiteral("text")).toString();
                }
            }
        } else if (provider == AISettings::Ollama) {
            deltaText = obj.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
            if (deltaText.isEmpty()) {
                deltaText = obj.value(QStringLiteral("response")).toString();
            }
        } else if (provider == AISettings::OpenCode && obj.value(QStringLiteral("type")).toString() == QLatin1String("response.output_text.delta")) {
            // OpenAI Responses API streaming event
            deltaText = obj.value(QStringLiteral("delta")).toString();
        } else {
            // OpenCode / OpenAI / Custom
            QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
            if (!choices.isEmpty()) {
                QJsonObject firstChoice = choices.first().toObject();
                if (firstChoice.contains(QStringLiteral("delta"))) {
                    const QJsonObject delta = firstChoice.value(QStringLiteral("delta")).toObject();
                    deltaText = delta.value(QStringLiteral("content")).toString();
                    if (deltaText.isEmpty()) {
                        // DeepSeek-style reasoning models stream chain-of-thought here first;
                        // keep it only as a fallback if no final content is produced.
                        const QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString();
                        if (!reasoning.isEmpty()) {
                            if (m_accumulatedReasoning.isEmpty()) {
                                qDebug() << "[AIPanel] Model started reasoning...";
                            }
                            m_accumulatedReasoning.append(reasoning);
                            // Throttle to ~1/sec so the UI can show "model is thinking…"
                            if (m_requestTimer.isValid() && m_requestTimer.elapsed() - m_lastReasoningEmitMs > 800) {
                                m_lastReasoningEmitMs = m_requestTimer.elapsed();
                                Q_EMIT reasoningProgress(m_accumulatedReasoning.size());
                            }
                        }
                    }
                } else if (firstChoice.contains(QStringLiteral("message"))) {
                    const QJsonObject message = firstChoice.value(QStringLiteral("message")).toObject();
                    deltaText = message.value(QStringLiteral("content")).toString();
                    if (deltaText.isEmpty()) {
                        const QString reasoning = message.value(QStringLiteral("reasoning_content")).toString();
                        if (!reasoning.isEmpty()) {
                            if (m_accumulatedReasoning.isEmpty()) {
                                qDebug() << "[AIPanel] Model started reasoning...";
                            }
                            m_accumulatedReasoning.append(reasoning);
                            if (m_requestTimer.isValid() && m_requestTimer.elapsed() - m_lastReasoningEmitMs > 800) {
                                m_lastReasoningEmitMs = m_requestTimer.elapsed();
                                Q_EMIT reasoningProgress(m_accumulatedReasoning.size());
                            }
                        }
                    }
                } else if (firstChoice.contains(QStringLiteral("text"))) {
                    deltaText = firstChoice.value(QStringLiteral("text")).toString();
                }
            }
        }

        if (!deltaText.isEmpty()) {
            if (m_accumulatedResponse.isEmpty()) {
                qDebug() << "[AIPanel] Content streaming started after" << m_requestTimer.elapsed() / 1000 << "s of reasoning";
            }
            m_accumulatedResponse.append(deltaText);
            Q_EMIT chunkReceived(deltaText);
        }
    }

    if (m_streamDone) {
        completeStreamEarly();
    }
}

void LLMClient::completeStreamEarly()
{
    m_streamDone = false;
    m_generationTimer->stop();
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    const QString cleanedResponse = m_accumulatedResponse.isEmpty() ? m_accumulatedReasoning : m_accumulatedResponse;
    qDebug() << "[AIPanel] LLM stream done (DONE marker) after" << m_requestTimer.elapsed() / 1000 << "s:" << m_accumulatedResponse.size() << "content chars, " << m_accumulatedReasoning.size() << "reasoning chars";
    Q_EMIT finished(cleanedResponse);
}

void LLMClient::onGenerationTimeout()
{
    if (!m_currentReply) {
        return;
    }
    qDebug() << "[AIPanel] Generation exceeded" << kGenerationTimeoutSec << "s - aborting";
    m_retryTimer->stop();
    m_retriesLeft = 0;
    m_streamDone = false;
    m_currentReply->disconnect(this);
    m_currentReply->abort();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    m_buffer.clear();

    if (m_isTestMode) {
        Q_EMIT testConnectionResult(false, QStringLiteral("Connection timed out."));
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Generation took longer than %1 minutes. The model may be overloaded or stuck - try a faster model (e.g. mimo-v2.5-free) or check the provider.").arg(kGenerationTimeoutSec / 60));
    }
}

void LLMClient::onFinished()
{
    if (!m_currentReply) return;

    m_generationTimer->stop();
    m_streamDone = false;

    QNetworkReply *reply = m_currentReply;
    m_currentReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        QByteArray respBody = reply->readAll();
        if (!respBody.isEmpty()) {
            err += QStringLiteral(" (") + QString::fromUtf8(respBody).left(200) + QStringLiteral(")");
        }

        // Free-tier providers commonly rate-limit; retry a couple of times with backoff
        if (m_retriesLeft > 0 && isRateLimitError(reply, respBody)) {
            m_retriesLeft--;
            const int delay = kRetryDelays[kMaxRetries - m_retriesLeft - 1];
            qDebug() << "[AIPanel] Rate limited after" << m_requestTimer.elapsed() / 1000 << "s - retrying in" << delay << "s";
            Q_EMIT retrying(delay);
            m_retryTimer->start(delay * 1000);
            return;
        }

        qDebug() << "[AIPanel] LLM error after" << m_requestTimer.elapsed() / 1000 << "s:" << err.left(200);
        if (m_isTestMode) {
            Q_EMIT testConnectionResult(false, err);
        } else {
            Q_EMIT errorOccurred(err);
        }
        return;
    }

    QByteArray remaining = m_buffer + reply->readAll();
    m_buffer.clear();

    if (m_isTestMode) {
        QJsonDocument doc = QJsonDocument::fromJson(remaining);
        QString answer;
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (m_currentProvider == AISettings::Anthropic) {
                QJsonArray content = obj.value(QStringLiteral("content")).toArray();
                if (!content.isEmpty()) answer = content.first().toObject().value(QStringLiteral("text")).toString();
            } else if (m_currentProvider == AISettings::Ollama) {
                answer = obj.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
                if (answer.isEmpty()) answer = obj.value(QStringLiteral("response")).toString();
            } else if (m_currentProvider == AISettings::OpenCode && obj.contains(QStringLiteral("output"))) {
                answer = extractResponsesApiText(obj);
            } else {
                QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
                if (!choices.isEmpty()) {
                    const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
                    answer = message.value(QStringLiteral("content")).toString();
                    if (answer.isEmpty()) {
                        answer = message.value(QStringLiteral("reasoning_content")).toString();
                    }
                }
            }
        }
        if (answer.isEmpty()) {
            Q_EMIT testConnectionResult(false, QStringLiteral("The provider returned no response."));
        } else {
            Q_EMIT testConnectionResult(true, answer.trimmed());
        }
        return;
    }

    // Flush a final record that did not end with a newline. Leave ordinary JSON
    // responses intact for the non-streaming fallback below.
    const QByteArray trimmedRemaining = remaining.trimmed();
    const bool isStreamRecord = m_currentProvider == AISettings::Ollama || trimmedRemaining.startsWith("data: ");
    if (!trimmedRemaining.isEmpty() && isStreamRecord) {
        m_buffer = trimmedRemaining;
        if (!m_buffer.endsWith('\n')) {
            m_buffer.append('\n');
        }
        parseSSE(m_buffer, m_currentProvider);
        remaining = m_buffer;
    }

    // If accumulated response is empty (e.g. server returned a single non-streamed JSON object)
    if (m_accumulatedResponse.isEmpty() && !remaining.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(remaining.trimmed());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (m_currentProvider == AISettings::Anthropic) {
                QJsonArray content = obj.value(QStringLiteral("content")).toArray();
                if (!content.isEmpty()) {
                    m_accumulatedResponse = content.first().toObject().value(QStringLiteral("text")).toString();
                }
            } else if (m_currentProvider == AISettings::Ollama) {
                m_accumulatedResponse = obj.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
                if (m_accumulatedResponse.isEmpty()) {
                    m_accumulatedResponse = obj.value(QStringLiteral("response")).toString();
                }
            } else if (m_currentProvider == AISettings::OpenCode && obj.contains(QStringLiteral("output"))) {
                m_accumulatedResponse = extractResponsesApiText(obj);
            } else {
                QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
                if (!choices.isEmpty()) {
                    QJsonObject firstChoice = choices.first().toObject();
                    if (firstChoice.contains(QStringLiteral("message"))) {
                        const QJsonObject message = firstChoice.value(QStringLiteral("message")).toObject();
                        m_accumulatedResponse = message.value(QStringLiteral("content")).toString();
                        if (m_accumulatedResponse.isEmpty()) {
                            m_accumulatedResponse = message.value(QStringLiteral("reasoning_content")).toString();
                        }
                    } else if (firstChoice.contains(QStringLiteral("text"))) {
                        m_accumulatedResponse = firstChoice.value(QStringLiteral("text")).toString();
                    }
                }
            }
        }
    }

    // Reasoning models sometimes spend the whole token budget thinking and produce
    // no final content; fall back to the chain-of-thought so the response is never empty.
    const QString cleanedResponse = m_accumulatedResponse.isEmpty() ? m_accumulatedReasoning : m_accumulatedResponse;

    qDebug() << "[AIPanel] LLM finished after" << m_requestTimer.elapsed() / 1000 << "s:" << m_accumulatedResponse.size() << "content chars, " << m_accumulatedReasoning.size() << "reasoning chars";
    Q_EMIT finished(cleanedResponse);
}

bool LLMClient::isRateLimitError(const QNetworkReply *reply, const QByteArray &body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429) {
        return true;
    }
    const QByteArray lower = body.toLower();
    return lower.contains("rate limit") || lower.contains("freeusagelimiterror") || lower.contains("too many requests");
}
