#ifndef REMOTELIBRARYCLIENT_H
#define REMOTELIBRARYCLIENT_H

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

struct RemoteTrack
{
    QString id;
    QString type; // "karaoke" or "break"
    QString artist;
    QString title;
    QString songId;
    QString filename;
    QString relPath;
    QString ext;
    qint64 size{0};
    qint64 mtime{0};
};
Q_DECLARE_METATYPE(RemoteTrack)
Q_DECLARE_METATYPE(QList<RemoteTrack>)

class RemoteLibraryClient : public QObject
{
    Q_OBJECT

public:
    explicit RemoteLibraryClient(QObject *parent = nullptr);
    ~RemoteLibraryClient() override;

    void testConnection(const QString &baseUrl, const QString &token);
    void fetchCatalog(const QString &baseUrl, const QString &token);
    void downloadTrack(const RemoteTrack &track, const QString &baseUrl, const QString &token, const QString &destPath);
    void cancelDownload();
    bool isDownloading() const;

    static QList<RemoteTrack> parseCatalogJson(const QByteArray &jsonData, QString *errorOut = nullptr);
    static QString cacheFilePath();
    static bool saveCatalogCache(const QByteArray &jsonData);
    static QList<RemoteTrack> loadCatalogCache();

signals:
    void connectionTested(bool ok, const QString &message, int trackCount);
    void catalogFetched(bool ok, const QList<RemoteTrack> &tracks, const QString &error);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(bool ok, const RemoteTrack &track, const QString &destPath, const QString &error);

private:
    QNetworkAccessManager m_nam;
    QNetworkReply *m_activeDownloadReply{nullptr};
    QFile *m_activeDownloadFile{nullptr};
    RemoteTrack m_activeTrack;
    QString m_activeDestPath;
    bool m_downloadCancelled{false};

    static QUrl buildEndpointUrl(const QString &baseUrl, const QString &path);
    static QNetworkRequest makeAuthorizedRequest(const QUrl &url, const QString &token);
    void cleanupActiveDownload(bool removePartFile);
};

#endif // REMOTELIBRARYCLIENT_H
