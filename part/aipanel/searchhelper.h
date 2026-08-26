#ifndef SEARCHHELPER_H
#define SEARCHHELPER_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class SearchHelper : public QObject
{
    Q_OBJECT
public:
    explicit SearchHelper(QNetworkAccessManager *nam, QObject *parent = nullptr);

    void searchContext(const QString &pageText, const QString &exaApiKey = QString());
    void cancel();

Q_SIGNALS:
    void searchCompleted(const QString &searchContext);
    void searchFailed(const QString &errorMessage);

private Q_SLOTS:
    void onExaReplyFinished();
    void onDuckDuckGoReplyFinished();

private:
    QString extractSearchQuery(const QString &pageText);
    void searchExa(const QString &query, const QString &apiKey);
    void searchDuckDuckGo(const QString &query);

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_currentReply = nullptr;
};

#endif // SEARCHHELPER_H
