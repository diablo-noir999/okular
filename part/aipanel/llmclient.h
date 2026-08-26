#ifndef LLMCLIENT_H
#define LLMCLIENT_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QPair>
#include <QList>
#include <QElapsedTimer>
#include "aisettings.h"

class LLMClient : public QObject
{
    Q_OBJECT
public:
    explicit LLMClient(QNetworkAccessManager *nam, QObject *parent = nullptr);
    virtual ~LLMClient();

    void explainPage(const QString &pageText, int pageNumber, const QString &webSearchContext, const QString &previousContext, const AISettings &settings);
    void askQuestion(const QString &pageText, int pageNumber, const QList<QPair<QString, QString>> &history, const QString &question, const AISettings &settings);
    void testConnection(const AISettings &settings);
    /**
     * Sends @p imageData (PNG bytes) to the configured vision endpoint (e.g. llama.cpp
     * with a small VLM) for OCR transcription. The transcribed text is emitted via
     * transcriptionFinished(); an empty string means failure.
     */
    void transcribeImage(const QByteArray &imageData, const AISettings &settings);
    void abort();

Q_SIGNALS:
    void chunkReceived(const QString &chunk);
    void finished(const QString &fullResponse);
    void errorOccurred(const QString &errorMessage);
    void testConnectionResult(bool success, const QString &message);
    /** Emitted when the provider rate-limited the request and a retry was scheduled. */
    void retrying(int delaySeconds);
    /** Emitted with the transcribed page text (empty if transcription failed). */
    void transcriptionFinished(const QString &text);
    /**
     * Emitted (throttled to ~1/sec) while a reasoning model is silently streaming
     * chain-of-thought, so the UI can show "model is thinking…" instead of looking stuck.
     * @p chars is the number of reasoning characters accumulated so far.
     */
    void reasoningProgress(int chars);

private Q_SLOTS:
    void onReadyRead();
    void onFinished();
    void onGenerationTimeout();

private:
    void sendChatCompletion(const QString &systemPrompt, const QJsonArray &messages, const AISettings &settings, bool isTest = false);
    void parseSSE(const QByteArray &data, AISettings::Provider provider);
    void sendStoredRequest();
    bool isRateLimitError(const QNetworkReply *reply, const QByteArray &body) const;
    void probeVisionModels(const QStringList &bases, int index, const QByteArray &imageData, const AISettings &settings);
    void sendTranscription(const QByteArray &imageData, const AISettings &settings, const QString &baseUrl, const QString &model);
    /** Called when the stream's [DONE] marker arrives (some servers keep the connection open). */
    void completeStreamEarly();

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_currentReply = nullptr;
    QNetworkReply *m_transcribeReply = nullptr;
    QNetworkReply *m_probeReply = nullptr;
    bool m_isTranscribing = false;
    QElapsedTimer m_requestTimer;
    qint64 m_lastReasoningEmitMs = 0;
    QByteArray m_buffer;
    QString m_accumulatedResponse;
    // Chain-of-thought deltas from reasoning models (DeepSeek-style reasoning_content);
    // emitted for display and used as a last-resort fallback when no final answer is produced.
    QString m_accumulatedReasoning;
    bool m_isTestMode = false;
    bool m_streamDone = false;
    AISettings::Provider m_currentProvider;
    QTimer *m_generationTimer = nullptr;

    // State for retrying rate-limited requests
    QNetworkRequest m_retryRequest;
    QByteArray m_retryBody;
    int m_retriesLeft = 0;
    QTimer *m_retryTimer = nullptr;
};

#endif // LLMCLIENT_H
