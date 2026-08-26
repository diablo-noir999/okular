#ifndef AIPANELWIDGET_H
#define AIPANELWIDGET_H

#include <QWidget>
#include <QTextBrowser>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QNetworkAccessManager>
#include <QProcess>
#include "core/observer.h"
#include "aisettings.h"
#include "llmclient.h"
#include "opencodeclient.h"
#include "searchhelper.h"

namespace Okular {
class Document;
class Annotation;
}

struct PageAIData {
    int pageNumber = -1;
    QString pageText;
    QString explanationMarkdown;
    QString webSearchContext;
    QList<QPair<QString, QString>> chatHistory;
    bool isComplete = false;
};

class AIPanelWidget : public QWidget, public Okular::DocumentObserver
{
    Q_OBJECT
public:
    explicit AIPanelWidget(Okular::Document *document, QWidget *parent = nullptr);
    ~AIPanelWidget() override;

    // DocumentObserver overrides
    void notifyViewportChanged(bool smoothMove) override;
    void notifySetup(const QList<Okular::Page *> &pages, int setupFlags) override;
    void notifyPageChanged(int page, int flags) override;

    /**
     * Enables/disables the panel. When disabled the panel stays silent: no
     * automatic explanations, debounced triggers or network requests.
     */
    void setPanelEnabled(bool enabled);

    void triggerCurrentPageExplanation(bool forceRefresh = false);

Q_SIGNALS:
    void closeRequested();

private Q_SLOTS:
    void onDebounceTimeout();
    void onSearchCompleted(const QString &context);
    void onChunkReceived(const QString &chunk);
    void onStreamFinished(const QString &fullResponse);
    void onErrorOccurred(const QString &error);
    void onSendQuestion();
    void onElapsedTick();
    void onOpenSettings();
    void onToggleSearch();
    void onToggleAutoSummarize();
    void onRefreshPage();
    void onClosePanel();
    void onTranscriptionFinished(const QString &text);
    void onVisionTimedOut();
    void onTesseractFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void setupUI();
    void updateHeader();
    void displayCurrentPage();
    void appendChatMessage(const QString &sender, const QString &text, bool isUser);
    /** Re-renders the whole Q&A chat as HTML (user text escaped, AI answers as markdown). */
    void renderChat();
    QString extractPageText(int pageNum) const;
    QString getPreviousPagesContext(int maxLookback = 3) const;
    void requestPageImageForVision(int pageNum);
    void onVisionPagePixmapReady();
    /** Fills @p width/@p height with the on-screen (rotation-aware) page size at ~200 DPI. */
    void visionImageSize(const Okular::Page *page, int &width, int &height) const;
    void visionFailed(const QString &markdown, const QString &status);
    void runTesseractOnImage(const QByteArray &pngData);
    void fallbackToVisionModel(const QByteArray &pngData);
    void continueAfterOcr(const QString &text);
    void sendExplanation();
    bool needsContextBackfill() const;
    void startContextBackfill();
    void abortOcr();
    bool usingOpenCodeCli() const;
    QString composeExplainMessage(int pageNum, const QString &pageText, const QString &prevContext = QString()) const;
    QString composeQuestionMessage(int pageNum, const QString &pageText, const QString &question);
    void startElapsedTimer();
    void stopElapsedTimer();
    QString getObsidianNoteFilePath() const;
    QString getObsidianQnAFilePath() const;
    void saveExplanationToObsidian(int pageNum, const QString &explanation);
    QString loadPreviousContextFromObsidian(int maxLookback = 3) const;
    /** Returns the saved explanation for the given 1-based page from the Obsidian note file. */
    QString loadExplanationFromObsidian(int pageNum) const;
    /** Persists the Q&A history for a 1-based page to the Obsidian Q&A file. */
    void saveChatHistoryToObsidian(int pageNum, const QList<QPair<QString, QString>> &history);
    /** Loads the Q&A history for a 1-based page from the Obsidian Q&A file. */
    QList<QPair<QString, QString>> loadChatHistoryFromObsidian(int pageNum) const;
    /**
     * If a saved explanation exists for @p pageNum (0-based) in the Obsidian note
     * file, loads it into the page cache and returns true.
     */
    bool tryRestoreSavedExplanation(int pageNum);

    Okular::Document *m_document;
    QNetworkAccessManager *m_nam;
    LLMClient *m_llmClient;
    OpenCodeClient *m_openCodeClient;
    SearchHelper *m_searchHelper;
    AISettings m_settings;

    QTimer *m_debounceTimer;
    int m_currentPage = -1;
    bool m_panelEnabled = true;
    bool m_isAskingQuestion = false;
    bool m_responseHasDisplayedChunks = false;

    QHash<int, PageAIData> m_pageCache;
    QSet<int> m_explainedPagesInSession; // pages already explained in the opencode session
    int m_searchPage = -1;
    int m_llmRequestPage = -1;
    int m_visionRequestPage = -1;
    int m_ocrPage = -1;
    bool m_contextBackfill = false;    // true while OCR'ing previous pages for context
    QList<int> m_contextOcrQueue;      // pages still to OCR for previous-context
    QTimer *m_visionTimer = nullptr;
    QProcess *m_tesseractProcess = nullptr;
    QByteArray m_pendingVisionPng;

    // UI Widgets
    QLabel *m_pageLabel;
    QLabel *m_timerLabel = nullptr;
    QTimer *m_elapsedTimer = nullptr;
    int m_elapsedSeconds = 0;
    QPushButton *m_searchToggleBtn;
    QPushButton *m_autoSummarizeBtn;
    QPushButton *m_refreshBtn;
    QPushButton *m_settingsBtn;
    QPushButton *m_closeBtn;
    QTextBrowser *m_summaryBrowser;
    QTextBrowser *m_chatBrowser;
    QLineEdit *m_questionInput;
    QPushButton *m_sendBtn;
    QLabel *m_statusLabel;
};

#endif // AIPANELWIDGET_H
