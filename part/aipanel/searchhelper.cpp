#include "searchhelper.h"
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

SearchHelper::SearchHelper(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
{
}

void SearchHelper::cancel()
{
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
}

QString SearchHelper::extractSearchQuery(const QString &pageText)
{
    // Extract first clean line or prominent title / keywords
    QStringList lines = pageText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.length() > 10 && trimmed.length() < 120) {
            return trimmed;
        }
    }
    return pageText.left(100).simplified();
}

void SearchHelper::searchContext(const QString &pageText, const QString &exaApiKey)
{
    cancel();

    QString query = extractSearchQuery(pageText);
    if (query.isEmpty()) {
        Q_EMIT searchCompleted(QString());
        return;
    }

    if (!exaApiKey.trimmed().isEmpty()) {
        searchExa(query, exaApiKey.trimmed());
    } else {
        searchDuckDuckGo(query);
    }
}

void SearchHelper::searchExa(const QString &query, const QString &apiKey)
{
    QUrl url(QStringLiteral("https://api.exa.ai/search"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(4000); // 4s fail-fast timeout

    QJsonObject payload;
    payload[QStringLiteral("query")] = query;
    payload[QStringLiteral("type")] = QStringLiteral("auto");
    payload[QStringLiteral("numResults")] = 5;

    QJsonObject contentsObj;
    contentsObj[QStringLiteral("highlights")] = true;
    payload[QStringLiteral("contents")] = contentsObj;

    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    m_currentReply = m_nam->post(request, body);
    connect(m_currentReply, &QNetworkReply::finished, this, &SearchHelper::onExaReplyFinished);
}

void SearchHelper::onExaReplyFinished()
{
    if (!m_currentReply) return;

    QNetworkReply *reply = m_currentReply;
    m_currentReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // Fallback silently so explanation generation is never blocked
        Q_EMIT searchCompleted(QString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QString resultText;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QJsonArray results = obj.value(QStringLiteral("results")).toArray();
        QStringList snippets;

        for (const auto &item : results) {
            if (!item.isObject()) continue;
            QJsonObject res = item.toObject();
            QString title = res.value(QStringLiteral("title")).toString();
            QString url = res.value(QStringLiteral("url")).toString();

            QString contentSnippet;
            QJsonArray highlights = res.value(QStringLiteral("highlights")).toArray();
            if (!highlights.isEmpty()) {
                QStringList hlList;
                for (const auto &hl : highlights) {
                    hlList.append(hl.toString().trimmed());
                }
                contentSnippet = hlList.join(QStringLiteral(" ... "));
            } else {
                contentSnippet = res.value(QStringLiteral("text")).toString().left(300).simplified();
            }

            if (!contentSnippet.isEmpty()) {
                snippets.append(QStringLiteral("- **[%1](%2)**: %3").arg(title.isEmpty() ? url : title, url, contentSnippet));
            }
        }

        if (!snippets.isEmpty()) {
            resultText = QStringLiteral("### Exa Search Web Context:\n%1\n").arg(snippets.join(QStringLiteral("\n")));
        }
    }

    Q_EMIT searchCompleted(resultText);
}

void SearchHelper::searchDuckDuckGo(const QString &query)
{
    QUrl url(QStringLiteral("https://api.duckduckgo.com/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("no_html"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("skip_disambig"), QStringLiteral("1"));
    url.setQuery(q);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (compatible; OkularAIExplainer/1.0)"));
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(3500);

    m_currentReply = m_nam->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, &SearchHelper::onDuckDuckGoReplyFinished);
}

void SearchHelper::onDuckDuckGoReplyFinished()
{
    if (!m_currentReply) return;

    QNetworkReply *reply = m_currentReply;
    m_currentReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        Q_EMIT searchCompleted(QString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QString resultText;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString abstract = obj.value(QStringLiteral("AbstractText")).toString();
        QString heading = obj.value(QStringLiteral("Heading")).toString();
        
        if (!abstract.isEmpty()) {
            resultText = QStringLiteral("### Web Context: %1\n%2\n").arg(heading, abstract);
        } else {
            QJsonArray related = obj.value(QStringLiteral("RelatedTopics")).toArray();
            if (!related.isEmpty() && related.first().isObject()) {
                QString relText = related.first().toObject().value(QStringLiteral("Text")).toString();
                if (!relText.isEmpty()) {
                    resultText = QStringLiteral("### Web Context:\n%1\n").arg(relText);
                }
            }
        }
    }

    Q_EMIT searchCompleted(resultText);
}
