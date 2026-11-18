#include "remotelibraryclient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

RemoteLibraryClient::RemoteLibraryClient(QObject *parent)
    : QObject(parent)
{
}

RemoteLibraryClient::~RemoteLibraryClient()
{
    cancelDownload();
}

QUrl RemoteLibraryClient::buildEndpointUrl(const QString &baseUrl, const QString &path)
{
    QString cleanBase = baseUrl.trimmed();
    while (cleanBase.endsWith('/'))
        cleanBase.chop(1);
    return QUrl(cleanBase + path);
}

QNetworkRequest RemoteLibraryClient::makeAuthorizedRequest(const QUrl &url, const QString &token)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "OpenKJ-RemoteLibrary/1.0");
    if (!token.trimmed().isEmpty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + token.trimmed().toUtf8());
    return req;
}

void RemoteLibraryClient::testConnection(const QString &baseUrl, const QString &token)
{
    QUrl url = buildEndpointUrl(baseUrl, "/api/v1/health");
    if (!url.isValid() || url.scheme().isEmpty()) {
        emit connectionTested(false, tr("Укажите корректный URL (например, https://example.com)"), 0);
        return;
    }
    QNetworkReply *reply = m_nam.get(makeAuthorizedRequest(url, token));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus == 401 || httpStatus == 403) {
            emit connectionTested(false, tr("Ошибка авторизации (%1): неверный Bearer-токен").arg(httpStatus), 0);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit connectionTested(false, reply->errorString(), 0);
            return;
        }
        QJsonParseError perr{};
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            emit connectionTested(false, tr("Некорректный ответ сервера"), 0);
            return;
        }
        int tracks = doc.object().value("tracks").toInt(0);
        emit connectionTested(true, tr("Подключение успешно! Треков в библиотеке: %1").arg(tracks), tracks);
    });
}

void RemoteLibraryClient::fetchCatalog(const QString &baseUrl, const QString &token)
{
    QUrl url = buildEndpointUrl(baseUrl, "/api/v1/catalog");
    if (!url.isValid() || url.scheme().isEmpty()) {
        emit catalogFetched(false, {}, tr("Некорректный URL удалённой библиотеки"));
        return;
    }
    QNetworkReply *reply = m_nam.get(makeAuthorizedRequest(url, token));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus == 401 || httpStatus == 403) {
            emit catalogFetched(false, {}, tr("Ошибка авторизации (%1): проверьте Bearer-токен").arg(httpStatus));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit catalogFetched(false, {}, reply->errorString());
            return;
        }
        QByteArray raw = reply->readAll();
        QString parseErr;
        QList<RemoteTrack> tracks = parseCatalogJson(raw, &parseErr);
        if (!parseErr.isEmpty()) {
            emit catalogFetched(false, {}, parseErr);
            return;
        }
        saveCatalogCache(raw);
        emit catalogFetched(true, tracks, QString());
    });
}

void RemoteLibraryClient::downloadTrack(const RemoteTrack &track, const QString &baseUrl, const QString &token, const QString &destPath)
{
    if (isDownloading()) {
        emit downloadFinished(false, track, destPath, tr("Уже выполняется другая загрузка"));
        return;
    }

    QFileInfo destInfo(destPath);
    QDir().mkpath(destInfo.absolutePath());
    const QString partPath = destPath + ".part";
    QFile::remove(partPath);

    auto *file = new QFile(partPath, this);
    if (!file->open(QIODevice::WriteOnly)) {
        QString err = tr("Не удалось создать файл %1: %2").arg(partPath, file->errorString());
        file->deleteLater();
        emit downloadFinished(false, track, destPath, err);
        return;
    }

    QUrl url = buildEndpointUrl(baseUrl, "/api/v1/download/" + track.id);
    m_activeTrack = track;
    m_activeDestPath = destPath;
    m_activeDownloadFile = file;
    m_downloadCancelled = false;
    m_activeDownloadReply = m_nam.get(makeAuthorizedRequest(url, token));

    connect(m_activeDownloadReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_activeDownloadFile && m_activeDownloadReply) {
            m_activeDownloadFile->write(m_activeDownloadReply->readAll());
        }
    });

    connect(m_activeDownloadReply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        emit downloadProgress(received, total);
    });

    connect(m_activeDownloadReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_activeDownloadReply;
        QFile *file = m_activeDownloadFile;
        const RemoteTrack trackCopy = m_activeTrack;
        const QString destCopy = m_activeDestPath;
        const bool cancelled = m_downloadCancelled;

        if (file && reply && reply->bytesAvailable() > 0) {
            file->write(reply->readAll());
        }

        const int httpStatus = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
        const QNetworkReply::NetworkError netErr = reply ? reply->error() : QNetworkReply::UnknownNetworkError;
        const QString errStr = reply ? reply->errorString() : tr("Ошибка загрузки");

        if (cancelled || netErr != QNetworkReply::NoError || httpStatus >= 400) {
            cleanupActiveDownload(true);
            QString msg = cancelled ? tr("Загрузка отменена")
                                    : (httpStatus >= 400 ? tr("HTTP %1: %2").arg(httpStatus).arg(errStr) : errStr);
            emit downloadFinished(false, trackCopy, destCopy, msg);
            return;
        }

        if (file) {
            file->flush();
            file->close();
        }
        QFile::remove(destCopy);
        const QString partPath = destCopy + ".part";
        bool renamed = QFile::rename(partPath, destCopy);
        cleanupActiveDownload(!renamed);
        if (!renamed) {
            emit downloadFinished(false, trackCopy, destCopy, tr("Не удалось сохранить файл %1").arg(destCopy));
            return;
        }
        emit downloadFinished(true, trackCopy, destCopy, QString());
    });
}

void RemoteLibraryClient::cancelDownload()
{
    if (!m_activeDownloadReply)
        return;
    m_downloadCancelled = true;
    m_activeDownloadReply->abort();
}

bool RemoteLibraryClient::isDownloading() const
{
    return m_activeDownloadReply != nullptr;
}

void RemoteLibraryClient::cleanupActiveDownload(bool removePartFile)
{
    if (m_activeDownloadReply) {
        m_activeDownloadReply->deleteLater();
        m_activeDownloadReply = nullptr;
    }
    if (m_activeDownloadFile) {
        if (m_activeDownloadFile->isOpen())
            m_activeDownloadFile->close();
        m_activeDownloadFile->deleteLater();
        m_activeDownloadFile = nullptr;
    }
    if (removePartFile && !m_activeDestPath.isEmpty()) {
        QFile::remove(m_activeDestPath + ".part");
    }
}

QList<RemoteTrack> RemoteLibraryClient::parseCatalogJson(const QByteArray &jsonData, QString *errorOut)
{
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(jsonData, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = tr("Ошибка разбора каталога JSON: %1").arg(perr.errorString());
        return {};
    }
    QJsonArray arr = doc.object().value("tracks").toArray();
    QList<RemoteTrack> out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        QJsonObject obj = v.toObject();
        RemoteTrack t;
        t.id = obj.value("id").toString();
        t.type = obj.value("type").toString();
        t.artist = obj.value("artist").toString();
        t.title = obj.value("title").toString();
        t.songId = obj.value("songid").toString();
        t.filename = obj.value("filename").toString();
        t.relPath = obj.value("rel_path").toString();
        t.ext = obj.value("ext").toString();
        t.size = static_cast<qint64>(obj.value("size").toDouble(0));
        t.mtime = static_cast<qint64>(obj.value("mtime").toDouble(0));
        if (!t.id.isEmpty() && !t.filename.isEmpty())
            out.append(t);
    }
    return out;
}

QString RemoteLibraryClient::cacheFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath("remote_catalog_cache.json");
}

bool RemoteLibraryClient::saveCatalogCache(const QByteArray &jsonData)
{
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(jsonData);
    return true;
}

QList<RemoteTrack> RemoteLibraryClient::loadCatalogCache()
{
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parseCatalogJson(f.readAll());
}
