#ifndef OPENCODECLIENT_H
#define OPENCODECLIENT_H

#include <QObject>
#include <QString>
#include <QProcess>
#include "aisettings.h"

/**
 * Drives the local `opencode` CLI (https://opencode.ai) to answer prompts through
 * the user's opencode sessions. Works anonymously with OpenCode Zen free models
 * (no API key needed), e.g. `opencode/mimo-v2.5-free`.
 *
 * Every message runs `opencode run --format json --continue -m <model> <message>`
 * in the current document's folder, so all messages for one document land in the
 * same opencode session (which also provides the conversation context for free).
 *
 * Streaming output arrives as newline-delimited JSON events; `text` events are
 * emitted as chunks.
 */
class OpenCodeClient : public QObject
{
    Q_OBJECT
public:
    explicit OpenCodeClient(QObject *parent = nullptr);
    ~OpenCodeClient() override;

    /** Directory the opencode process runs in; one session is kept per directory. */
    void setWorkingDirectory(const QString &dir);

    void sendMessage(const QString &message, const AISettings &settings);
    void testConnection(const AISettings &settings);
    void abort();

Q_SIGNALS:
    void chunkReceived(const QString &chunk);
    void finished(const QString &fullResponse);
    void errorOccurred(const QString &errorMessage);
    void testConnectionResult(bool success, const QString &message);

private:
    void startProcess(const QString &message, const AISettings &settings);
    void onReadyReadStdout();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    static QString opencodeBinaryPath();

    QProcess *m_process = nullptr;
    QByteArray m_buffer;
    QString m_accumulatedResponse;
    QString m_workingDir;
    bool m_isTestMode = false;
};

#endif // OPENCODECLIENT_H
