#ifndef OKJSONGBOOKAPI_H
#define OKJSONGBOOKAPI_H

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QStringList>
#include "settings.h"
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>
#include <spdlog/fmt/ostr.h>

std::ostream& operator<<(std::ostream& os, const QString& s);


class OkjsRequest
{
public:
    int requestId{0};
    QString singer;
    QString artist;
    QString title;
    int key{0};
    int time{0};
    int songId{-1};
    int micCount{1};
    bool operator == (const OkjsRequest& r) const;
};


typedef QList<OkjsRequest> OkjsRequests;

class OkjsVenue
{
public:
    int venueId{0};
    QString name;
    QString urlName;
    bool accepting{true};
    bool operator == (const OkjsVenue& v) const;
};

std::ostream& operator<<(std::ostream& os, const OkjsVenue& v);


typedef QList<OkjsVenue> OkjsVenues;

class OKJSongbookAPI : public QObject
{
    Q_OBJECT
private:
    std::string m_loggingPrefix{"[SongbookAPI]"};
    std::shared_ptr<spdlog::logger> m_logger;
    int serial;
    OkjsVenues venues;
    OkjsRequests requests;
    QNetworkAccessManager *manager;
    QTimer *timer;
    QTimer *alertTimer;
    QTime lastSync;
    bool delayErrorEmitted;
    bool connectionReset;
    int entitledSystems;
    bool programIsIdle;
    bool cancelUpdate;
    bool updateInProgress;
    Settings m_settings;

    QTcpServer *m_httpServer{nullptr};
    quint16 m_httpPort{8080};
    bool m_localAccepting{true};
    int m_nextLocalRequestId{1};
    QString m_currentStageSinger;
    QString m_currentStageSong;
    QStringList m_upcomingSingers;

    void startLocalHttpServer();
    void handleHttpConnection();
    void processHttpRequest(QTcpSocket *socket, const QByteArray &rawRequest);
    void sendHttpResponse(QTcpSocket *socket, int statusCode, const QByteArray &statusText,
                          const QByteArray &contentType, const QByteArray &body);
    QByteArray buildMobileWebAppHtml() const;

public:
    explicit OKJSongbookAPI(QObject *parent = nullptr);
    void getSerial();
    void refreshRequests();
    void removeRequest(int requestId);
    bool getAccepting();
    void setAccepting(bool enabled);
    void refreshVenues(bool blocking = false);
    void clearRequests();
    void updateSongDb();
    bool test();
    void alertCheck();
    void getEntitledSystemCount();
    [[nodiscard]] int entitledSystemCount() const { return entitledSystems; }
    [[nodiscard]] bool updateWasCancelled() const {return cancelUpdate; }
    void triggerTestAdd();

    [[nodiscard]] QString localServerUrl() const;
    [[nodiscard]] quint16 localServerPort() const { return m_httpPort; }
    [[nodiscard]] bool isLocalServerRunning() const { return m_httpServer && m_httpServer->isListening(); }
    void setStageStatus(const QString &currentSinger, const QString &currentSong, const QStringList &upcomingSingers);
    [[nodiscard]] const OkjsRequests &currentRequests() const { return requests; }

signals:
    void venuesChanged(OkjsVenues);
    void sslError();
    void remoteSongDbUpdateProgress(int);
    void remoteSongDbUpdateNumDocs(int);
    void remoteSongDbUpdateDone();
    void remoteSongDbUpdateStart();
    void requestsChanged(OkjsRequests);
    void localRequestSubmitted(OkjsRequest);
    void synchronized(QTime);
    void delayError(int);
    void testPassed();
    void testFailed(QString error);
    void testSslError(QString error);
    void alertRecieved(QString title, QString message);
    void entitledSystemCountChanged(int count);


public slots:
    void dbUpdateCanceled();
    void setInterval(int interval);


private slots:
        void onSslErrors(QNetworkReply * reply, const QList<QSslError>& errors);
        void onTestSslErrors(QNetworkReply * reply, QList<QSslError> errors);
        void onNetworkReply(QNetworkReply* reply);
        void timerTimeout();
        void alertTimerTimeout();
        void idleStateChanged(bool isIdle);
};

#endif // OKJSONGBOOKAPI_H
