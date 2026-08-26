#include "opencodeclient.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

OpenCodeClient::OpenCodeClient(QObject *parent)
    : QObject(parent)
{
}

OpenCodeClient::~OpenCodeClient()
{
    abort();
}

void OpenCodeClient::setWorkingDirectory(const QString &dir)
{
    m_workingDir = dir;
}

QString OpenCodeClient::opencodeBinaryPath()
{
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("opencode"));
    if (!fromPath.isEmpty()) {
        return fromPath;
    }
    // Common install location when ~/.opencode/bin is not on PATH
    const QString homeBin = QDir::home().filePath(QStringLiteral(".opencode/bin/opencode"));
    if (QFile::exists(homeBin)) {
        return homeBin;
    }
    return QStringLiteral("opencode");
}

void OpenCodeClient::sendMessage(const QString &message, const AISettings &settings)
{
    if (message.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Empty message."));
        return;
    }
    abort();
    m_isTestMode = false;
    m_accumulatedResponse.clear();
    startProcess(message, settings);
}

void OpenCodeClient::testConnection(const AISettings &settings)
{
    abort();
    m_isTestMode = true;
    m_accumulatedResponse.clear();
    startProcess(QStringLiteral("Say hello in one word."), settings);
}

void OpenCodeClient::startProcess(const QString &message, const AISettings &settings)
{
    m_buffer.clear();

    QStringList args;
    args << QStringLiteral("run") << QStringLiteral("--format") << QStringLiteral("json") << QStringLiteral("--continue");
    if (!settings.model.trimmed().isEmpty()) {
        args << QStringLiteral("-m") << settings.model.trimmed();
    }
    args << message;

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    // opencode's `run` command waits on stdin if it's an open pipe; give it EOF immediately
    m_process->setStandardInputFile(QProcess::nullDevice());
    if (!m_workingDir.isEmpty() && QDir(m_workingDir).exists()) {
        m_process->setWorkingDirectory(m_workingDir);
    }
    connect(m_process, &QProcess::readyReadStandardOutput, this, &OpenCodeClient::onReadyReadStdout);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &OpenCodeClient::onProcessFinished);

    qDebug() << "[AIPanel] opencode:" << opencodeBinaryPath() << args.join(QLatin1Char(' ')).left(200);
    m_process->start(opencodeBinaryPath(), args);
    if (!m_process->waitForStarted(10000)) {
        const QString err = QStringLiteral("Could not start opencode (%1). Make sure it is installed (https://opencode.ai).").arg(opencodeBinaryPath());
        qDebug() << "[AIPanel] opencode start failed:" << err;
        m_process->deleteLater();
        m_process = nullptr;
        if (m_isTestMode) {
            Q_EMIT testConnectionResult(false, err);
        } else {
            Q_EMIT errorOccurred(err);
        }
        return;
    }
}

void OpenCodeClient::onReadyReadStdout()
{
    if (!m_process) {
        return;
    }
    m_buffer.append(m_process->readAllStandardOutput());

    int newlineIdx = -1;
    while ((newlineIdx = m_buffer.indexOf('\n')) != -1) {
        const QByteArray line = m_buffer.left(newlineIdx).trimmed();
        m_buffer.remove(0, newlineIdx + 1);
        if (line.isEmpty()) {
            continue;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        const QString type = obj.value(QLatin1String("type")).toString();
        const QJsonObject part = obj.value(QLatin1String("part")).toObject();

        if (type == QLatin1String("text")) {
            const QString text = part.value(QLatin1String("text")).toString();
            if (!text.isEmpty()) {
                m_accumulatedResponse.append(text);
                Q_EMIT chunkReceived(text);
            }
        } else if (type == QLatin1String("error")) {
            const QString err = obj.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
            if (!err.isEmpty()) {
                qDebug() << "[AIPanel] opencode error event:" << err.left(200);
                if (m_isTestMode) {
                    Q_EMIT testConnectionResult(false, err);
                } else {
                    Q_EMIT errorOccurred(err);
                }
                abort();
                return;
            }
        }
    }
}

void OpenCodeClient::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = m_process;
    m_process = nullptr;
    if (!proc) {
        return;
    }
    // Flush any remaining buffered line(s)
    if (!m_buffer.isEmpty()) {
        const QByteArray line = m_buffer.trimmed();
        m_buffer.clear();
        if (!line.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                if (obj.value(QLatin1String("type")).toString() == QLatin1String("text")) {
                    const QString text = obj.value(QLatin1String("part")).toObject().value(QLatin1String("text")).toString();
                    if (!text.isEmpty()) {
                        m_accumulatedResponse.append(text);
                        Q_EMIT chunkReceived(text);
                    }
                }
            }
        }
    }

    const QByteArray stderrOut = proc->readAllStandardError();
    const bool normalExit = exitStatus == QProcess::NormalExit && exitCode == 0;
    proc->deleteLater();

    qDebug() << "[AIPanel] opencode exited rc=" << exitCode << "status=" << static_cast<int>(exitStatus) << "chars=" << m_accumulatedResponse.size();

    if (m_isTestMode) {
        if (normalExit && !m_accumulatedResponse.isEmpty()) {
            Q_EMIT testConnectionResult(true, m_accumulatedResponse.trimmed());
        } else {
            const QString err = QString::fromUtf8(stderrOut).trimmed();
            Q_EMIT testConnectionResult(false, err.isEmpty() ? QStringLiteral("opencode produced no response.") : err);
        }
        return;
    }

    if (!m_accumulatedResponse.isEmpty()) {
        Q_EMIT finished(m_accumulatedResponse);
        return;
    }

    const QString stderrText = QString::fromUtf8(stderrOut).trimmed();
    Q_EMIT errorOccurred(stderrText.isEmpty() ? QStringLiteral("opencode produced no response (exit code %1).").arg(exitCode) : stderrText);
}

void OpenCodeClient::abort()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_buffer.clear();
}
