#include "okjsongbookapi.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QUrlQuery>
#include <QDateTime>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSqlQuery>
#include <QSqlError>
#include <QMessageBox>
#include <QPushButton>
#include <algorithm>
#include "idledetect.h"

extern IdleDetect *filter;

std::ostream &operator<<(std::ostream &os, const OkjsVenue &v) {
    return os << "venue_id: " << v.venueId
              << "name: " << v.name
              << "urlName: " << v.urlName
              << "accepting: " << v.accepting;
}

OKJSongbookAPI::OKJSongbookAPI(QObject *parent) : QObject(parent)
{
    m_logger = spdlog::get("logger");
    programIsIdle = false;
    delayErrorEmitted = false;
    connectionReset = false;
    cancelUpdate = false;
    updateInProgress = false;
    serial = 0;
    entitledSystems = 1;
    timer = new QTimer(this);
    timer->setInterval(m_settings.requestServerInterval() * 1000);
    alertTimer = new QTimer(this);
    alertTimer->start(600000);
    manager = new QNetworkAccessManager(this);
    connect(manager, &QNetworkAccessManager::sslErrors, this, &OKJSongbookAPI::onSslErrors);
    connect(manager, &QNetworkAccessManager::finished, this, &OKJSongbookAPI::onNetworkReply);
    connect(timer, &QTimer::timeout, this, &OKJSongbookAPI::timerTimeout);
    connect(alertTimer, &QTimer::timeout, this, &OKJSongbookAPI::alertTimerTimeout);
    connect(filter, &IdleDetect::idleStateChanged, this, &OKJSongbookAPI::idleStateChanged);

    startLocalHttpServer();

    if (m_settings.requestServerEnabled() && !m_settings.requestServerUrl().isEmpty() && !m_settings.requestServerApiKey().isEmpty())
    {
        getEntitledSystemCount();
        refreshVenues();
        alertCheck();
    }
    timer->start();
}

void OKJSongbookAPI::startLocalHttpServer()
{
    m_httpServer = new QTcpServer(this);
    connect(m_httpServer, &QTcpServer::newConnection, this, &OKJSongbookAPI::handleHttpConnection);

    for (quint16 port = 8080; port <= 8095; ++port)
    {
        if (m_httpServer->listen(QHostAddress::AnyIPv4, port))
        {
            m_httpPort = port;
            m_logger->info("{} Local karaoke request web server started at {}", m_loggingPrefix, localServerUrl().toStdString());
            break;
        }
    }
    if (!m_httpServer->isListening())
    {
        m_logger->error("{} Failed to bind local web server on ports 8080..8095", m_loggingPrefix);
    }

    OkjsVenue localVenue;
    localVenue.venueId = 1;
    localVenue.name = tr("Локальный Wi-Fi сервер (QR)");
    localVenue.urlName = "local";
    localVenue.accepting = m_localAccepting;
    venues.clear();
    venues.append(localVenue);
}

QString OKJSongbookAPI::localServerUrl() const
{
    QString bestIp = "127.0.0.1";
    int bestPriority = 0;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces)
    {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) || (flags & QNetworkInterface::IsLoopBack))
            continue;
        QString humanName = iface.humanReadableName().toLower();
        if (humanName.contains("virtual") || humanName.contains("vmware") || humanName.contains("vbox") ||
            humanName.contains("wsl") || humanName.contains("hyper-v") || humanName.contains("docker"))
            continue;

        for (const QNetworkAddressEntry &entry : iface.addressEntries())
        {
            QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback())
                continue;
            QString ipStr = addr.toString();
            if (ipStr.startsWith("169.254."))
                continue;

            int priority = 1;
            if (ipStr.startsWith("192.168."))
                priority = 4;
            else if (ipStr.startsWith("10."))
                priority = 3;
            else if (ipStr.startsWith("172."))
                priority = 2;

            if (humanName.contains("wi-fi") || humanName.contains("wlan") || humanName.contains("wireless") || humanName.contains("беспровод"))
                priority += 2;

            if (priority > bestPriority)
            {
                bestPriority = priority;
                bestIp = ipStr;
            }
        }
    }
    return QString("http://%1:%2").arg(bestIp).arg(m_httpPort);
}

void OKJSongbookAPI::setStageStatus(const QString &currentSinger, const QString &currentSong, const QStringList &upcomingSingers)
{
    m_currentStageSinger = currentSinger;
    m_currentStageSong = currentSong;
    m_upcomingSingers = upcomingSingers;
}

void OKJSongbookAPI::handleHttpConnection()
{
    while (m_httpServer->hasPendingConnections())
    {
        QTcpSocket *socket = m_httpServer->nextPendingConnection();
        if (!socket)
            continue;

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            QByteArray buf = socket->property("reqBuf").toByteArray();
            buf.append(socket->readAll());
            if (buf.size() > 65536)
            {
                socket->disconnectFromHost();
                return;
            }
            int headerEnd = buf.indexOf("\r\n\r\n");
            if (headerEnd < 0)
            {
                socket->setProperty("reqBuf", buf);
                return;
            }
            int contentLength = 0;
            QList<QByteArray> headerLines = buf.left(headerEnd).split('\n');
            for (const QByteArray &line : headerLines)
            {
                QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:"))
                {
                    contentLength = trimmed.mid(15).trimmed().toInt();
                }
            }
            int totalNeeded = headerEnd + 4 + contentLength;
            if (buf.size() < totalNeeded)
            {
                socket->setProperty("reqBuf", buf);
                return;
            }
            socket->setProperty("reqBuf", QByteArray());
            processHttpRequest(socket, buf.left(totalNeeded));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void OKJSongbookAPI::sendHttpResponse(QTcpSocket *socket, int statusCode, const QByteArray &statusText,
                                      const QByteArray &contentType, const QByteArray &body)
{
    if (!socket || !socket->isOpen())
        return;
    QByteArray header;
    header.append("HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText + "\r\n");
    header.append("Content-Type: " + contentType + "\r\n");
    header.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    header.append("Access-Control-Allow-Origin: *\r\n");
    header.append("Cache-Control: no-store, no-cache, must-revalidate\r\n");
    header.append("Connection: close\r\n\r\n");
    socket->write(header);
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

void OKJSongbookAPI::processHttpRequest(QTcpSocket *socket, const QByteArray &rawRequest)
{
    int headerEnd = rawRequest.indexOf("\r\n\r\n");
    if (headerEnd < 0)
    {
        sendHttpResponse(socket, 400, "Bad Request", "text/plain; charset=utf-8", "Bad Request");
        return;
    }

    QByteArray headerPart = rawRequest.left(headerEnd);
    QByteArray bodyPart = rawRequest.mid(headerEnd + 4);
    int firstLineEnd = headerPart.indexOf("\r\n");
    QByteArray requestLine = (firstLineEnd >= 0) ? headerPart.left(firstLineEnd) : headerPart;
    QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2)
    {
        sendHttpResponse(socket, 400, "Bad Request", "text/plain; charset=utf-8", "Bad Request");
        return;
    }

    QString method = QString::fromLatin1(parts[0]).toUpper();
    QUrl reqUrl(QString::fromUtf8(parts[1]));
    QString path = reqUrl.path();
    QUrlQuery query(reqUrl);

    if (method == "OPTIONS")
    {
        sendHttpResponse(socket, 200, "OK", "text/plain", "");
        return;
    }

    if (method == "GET" && (path == "/" || path == "/index.html"))
    {
        sendHttpResponse(socket, 200, "OK", "text/html; charset=utf-8", buildMobileWebAppHtml());
        return;
    }

    if (method == "GET" && path == "/api/status")
    {
        QJsonObject obj;
        obj["accepting"] = m_localAccepting;
        obj["maxMics"] = 2;
        obj["currentSinger"] = m_currentStageSinger;
        obj["currentSong"] = m_currentStageSong;
        QJsonArray upArr;
        for (const QString &s : m_upcomingSingers)
            upArr.append(s);
        obj["upcomingSingers"] = upArr;

        QJsonArray reqArr;
        for (const auto &r : requests)
        {
            QJsonObject ro;
            ro["id"] = r.requestId;
            ro["singer"] = r.singer;
            ro["artist"] = r.artist;
            ro["title"] = r.title;
            ro["key"] = r.key;
            ro["micCount"] = r.micCount;
            ro["time"] = r.time;
            reqArr.append(ro);
        }
        obj["requests"] = reqArr;
        sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "GET" && path == "/api/songs")
    {
        QString q = query.queryItemValue("q", QUrl::FullyDecoded).trimmed();
        int limit = std::clamp(query.queryItemValue("limit").toInt(), 10, 100);
        if (limit <= 0)
            limit = 60;

        QSqlQuery sqlQuery;
        if (q.isEmpty())
        {
            sqlQuery.prepare("SELECT songid, artist, title, discid, duration FROM dbsongs "
                             "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' "
                             "ORDER BY artist COLLATE NOCASE ASC, title COLLATE NOCASE ASC LIMIT :lim");
            sqlQuery.bindValue(":lim", limit);
        }
        else
        {
            if (m_settings.ignoreAposInSearch())
                q.remove('\'');
            QStringList terms = q.split(' ', Qt::SkipEmptyParts);
            QString sql = "SELECT songid, artist, title, discid, duration FROM dbsongs "
                          "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!'";
            for (int i = 0; i < terms.size(); ++i)
            {
                sql += QString(" AND searchstring LIKE :term%1").arg(i);
            }
            sql += " ORDER BY artist COLLATE NOCASE ASC, title COLLATE NOCASE ASC LIMIT :lim";
            sqlQuery.prepare(sql);
            for (int i = 0; i < terms.size(); ++i)
            {
                sqlQuery.bindValue(QString(":term%1").arg(i), "%" + terms[i] + "%");
            }
            sqlQuery.bindValue(":lim", limit);
        }

        QJsonArray songsArr;
        if (sqlQuery.exec())
        {
            while (sqlQuery.next())
            {
                QJsonObject so;
                so["id"] = sqlQuery.value(0).toInt();
                so["artist"] = sqlQuery.value(1).toString();
                so["title"] = sqlQuery.value(2).toString();
                so["discid"] = sqlQuery.value(3).toString();
                int durMs = sqlQuery.value(4).toInt();
                if (durMs > 0)
                {
                    int totalSec = durMs / 1000;
                    so["duration"] = QString("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
                }
                else
                {
                    so["duration"] = "";
                }
                songsArr.append(so);
            }
        }
        QJsonObject resp;
        resp["songs"] = songsArr;
        sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "POST" && path == "/api/request")
    {
        if (!m_localAccepting)
        {
            QJsonObject err{{"error", true}, {"message", "Приём заказов сейчас временно закрыт ведущим."}};
            sendHttpResponse(socket, 403, "Forbidden", "application/json; charset=utf-8", QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(bodyPart);
        if (!doc.isObject())
        {
            QJsonObject err{{"error", true}, {"message", "Некорректный формат запроса."}};
            sendHttpResponse(socket, 400, "Bad Request", "application/json; charset=utf-8", QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }

        QJsonObject obj = doc.object();
        QString singer = obj.value("singer").toString().trimmed();
        QString partner = obj.value("partner").toString().trimmed();
        int songId = obj.value("songId").toInt(-1);
        QString artist = obj.value("artist").toString().trimmed();
        QString title = obj.value("title").toString().trimmed();
        int keyChg = std::clamp(obj.value("key").toInt(0), -6, 6);
        int micCount = std::clamp(obj.value("micCount").toInt(1), 1, 2);

        if (singer.isEmpty())
        {
            QJsonObject err{{"error", true}, {"message", "Пожалуйста, укажите ваше имя или столик."}};
            sendHttpResponse(socket, 400, "Bad Request", "application/json; charset=utf-8", QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }

        if (songId > 0)
        {
            QSqlQuery q;
            q.prepare("SELECT artist, title FROM dbsongs WHERE songid = :id");
            q.bindValue(":id", songId);
            if (q.exec() && q.next())
            {
                artist = q.value(0).toString();
                title = q.value(1).toString();
            }
        }

        if (artist.isEmpty() && title.isEmpty())
        {
            QJsonObject err{{"error", true}, {"message", "Песня не выбрана."}};
            sendHttpResponse(socket, 400, "Bad Request", "application/json; charset=utf-8", QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }

        QString finalSingerName = singer;
        if (micCount == 2 && !partner.isEmpty())
        {
            finalSingerName = singer + " + " + partner;
        }

        OkjsRequest newReq;
        newReq.requestId = m_nextLocalRequestId++;
        newReq.singer = finalSingerName;
        newReq.artist = artist;
        newReq.title = title;
        newReq.key = keyChg;
        newReq.time = static_cast<int>(QDateTime::currentSecsSinceEpoch());
        newReq.songId = songId;
        newReq.micCount = micCount;

        requests.append(newReq);
        lastSync = QTime::currentTime();
        m_logger->info("{} New local QR request #{} from '{}' ({} mic): {} - {} (key {})",
                       m_loggingPrefix, newReq.requestId, finalSingerName.toStdString(),
                       micCount, artist.toStdString(), title.toStdString(), keyChg);

        QJsonObject ok{
            {"error", false},
            {"requestId", newReq.requestId},
            {"message", QString("Заказ принят! (%1 — %2, микрофонов: %3)").arg(artist, title).arg(micCount)}
        };
        sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", QJsonDocument(ok).toJson(QJsonDocument::Compact));

        emit requestsChanged(requests);
        emit synchronized(lastSync);
        emit localRequestSubmitted(newReq);
        return;
    }

    sendHttpResponse(socket, 404, "Not Found", "text/plain; charset=utf-8", "Not Found");
}

QByteArray OKJSongbookAPI::buildMobileWebAppHtml() const
{
    static const char *kHtml = R"HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>Караоке — Заказ песен</title>
<style>
  :root {
    --bg: #080b18;
    --card: rgba(17, 24, 48, 0.88);
    --border: rgba(56, 189, 248, 0.25);
    --cyan: #06b6d4;
    --violet: #8b5cf6;
    --pink: #ec4899;
    --text: #f8fafc;
    --muted: #94a3b8;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: radial-gradient(circle at 50% 0%, #1e1b4b 0%, var(--bg) 70%);
    color: var(--text);
    min-height: 100vh;
    padding-bottom: 90px;
  }
  .header {
    padding: 16px 16px 12px;
    background: rgba(8, 11, 24, 0.9);
    backdrop-filter: blur(12px);
    position: sticky;
    top: 0;
    z-index: 20;
    border-bottom: 1px solid var(--border);
  }
  .title-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 8px;
  }
  .logo {
    font-size: 19px;
    font-weight: 800;
    background: linear-gradient(90deg, #38bdf8, #c084fc, #f472b6);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
  }
  .badge {
    font-size: 12px;
    padding: 4px 10px;
    border-radius: 999px;
    font-weight: 600;
    background: rgba(16, 185, 129, 0.18);
    color: #34d399;
    border: 1px solid rgba(52, 211, 153, 0.35);
  }
  .badge.closed {
    background: rgba(239, 68, 68, 0.2);
    color: #f87171;
    border-color: rgba(248, 113, 113, 0.35);
  }
  .mic-banner {
    font-size: 12px;
    color: #bae6fd;
    background: rgba(6, 182, 212, 0.12);
    border: 1px solid rgba(6, 182, 212, 0.28);
    border-radius: 10px;
    padding: 7px 11px;
    margin-bottom: 10px;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .search-wrap {
    position: relative;
  }
  .search-input {
    width: 100%;
    padding: 12px 38px 12px 14px;
    border-radius: 12px;
    border: 1px solid rgba(139, 92, 246, 0.45);
    background: rgba(15, 23, 42, 0.92);
    color: var(--text);
    font-size: 15px;
    outline: none;
  }
  .search-input:focus {
    border-color: var(--cyan);
    box-shadow: 0 0 0 3px rgba(6, 182, 212, 0.2);
  }
  .clear-btn {
    position: absolute;
    right: 10px;
    top: 50%;
    transform: translateY(-50%);
    background: none;
    border: none;
    color: var(--muted);
    font-size: 18px;
    padding: 4px;
    cursor: pointer;
    display: none;
  }
  .container { padding: 12px 16px; max-width: 640px; margin: 0 auto; }
  .stage-card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 14px;
    padding: 12px 14px;
    margin-bottom: 14px;
  }
  .stage-title { font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em; color: #38bdf8; margin-bottom: 4px; font-weight: 700; }
  .stage-now { font-size: 14px; font-weight: 600; color: #fff; }
  .stage-rot { font-size: 12px; color: var(--muted); margin-top: 5px; }
  .section-lbl { font-size: 13px; color: var(--muted); margin: 8px 0 10px; font-weight: 600; display: flex; justify-content: space-between; }
  .song-list { display: flex; flex-direction: column; gap: 8px; }
  .song-item {
    background: var(--card);
    border: 1px solid rgba(148, 163, 184, 0.16);
    border-radius: 13px;
    padding: 12px 14px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    transition: transform 0.1s, border-color 0.15s;
  }
  .song-item:active { transform: scale(0.99); border-color: var(--cyan); }
  .song-info { min-width: 0; flex: 1; }
  .song-title { font-size: 15px; font-weight: 700; color: #f1f5f9; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .song-artist { font-size: 13px; color: #7dd3fc; margin-top: 2px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .song-meta { font-size: 11px; color: var(--muted); margin-top: 3px; }
  .order-btn {
    background: linear-gradient(135deg, var(--cyan), var(--violet));
    color: #fff;
    border: none;
    border-radius: 10px;
    padding: 9px 14px;
    font-size: 13px;
    font-weight: 700;
    cursor: pointer;
    flex-shrink: 0;
    box-shadow: 0 3px 12px rgba(139, 92, 246, 0.3);
  }
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(3, 6, 17, 0.78);
    backdrop-filter: blur(5px);
    z-index: 50;
    display: none;
    align-items: flex-end;
    justify-content: center;
  }
  .modal-backdrop.open { display: flex; }
  .modal {
    background: #111827;
    border-top: 1px solid rgba(56, 189, 248, 0.4);
    border-radius: 22px 22px 0 0;
    width: 100%;
    max-width: 540px;
    padding: 20px 18px 26px;
    animation: slideUp 0.2s ease-out;
  }
  @keyframes slideUp { from { transform: translateY(100%); } to { transform: translateY(0); } }
  .modal-song { font-size: 16px; font-weight: 800; color: #fff; margin-bottom: 2px; }
  .modal-artist { font-size: 13px; color: #38bdf8; margin-bottom: 14px; }
  .field-lbl { font-size: 12px; font-weight: 600; color: #cbd5e1; margin: 11px 0 5px; display: block; }
  .text-input {
    width: 100%;
    padding: 11px 13px;
    border-radius: 10px;
    border: 1px solid rgba(148, 163, 184, 0.3);
    background: #0b1120;
    color: #fff;
    font-size: 15px;
  }
  .mic-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
  .mic-option {
    padding: 11px 10px;
    border-radius: 11px;
    border: 1px solid rgba(148, 163, 184, 0.25);
    background: #0b1120;
    color: #cbd5e1;
    font-size: 13px;
    font-weight: 600;
    text-align: center;
    cursor: pointer;
  }
  .mic-option.active {
    border-color: #38bdf8;
    background: rgba(6, 182, 212, 0.18);
    color: #fff;
    box-shadow: 0 0 0 1px #38bdf8;
  }
  .key-row { display: flex; align-items: center; justify-content: space-between; background: #0b1120; border-radius: 10px; padding: 6px 10px; border: 1px solid rgba(148, 163, 184, 0.25); }
  .key-btn { width: 38px; height: 34px; border-radius: 8px; border: none; background: #1e293b; color: #fff; font-size: 18px; font-weight: 700; cursor: pointer; }
  .key-val { font-size: 14px; font-weight: 700; color: #38bdf8; }
  .modal-actions { display: flex; gap: 10px; margin-top: 18px; }
  .btn-cancel { flex: 1; padding: 12px; border-radius: 11px; border: 1px solid #334155; background: transparent; color: #cbd5e1; font-weight: 600; font-size: 14px; cursor: pointer; }
  .btn-submit { flex: 2; padding: 12px; border-radius: 11px; border: none; background: linear-gradient(135deg, var(--cyan), var(--violet)); color: #fff; font-weight: 700; font-size: 15px; cursor: pointer; }
  .toast {
    position: fixed;
    bottom: 20px;
    left: 50%;
    transform: translateX(-50%);
    background: #059669;
    color: #fff;
    padding: 12px 18px;
    border-radius: 12px;
    font-size: 14px;
    font-weight: 600;
    box-shadow: 0 8px 25px rgba(0,0,0,0.45);
    z-index: 80;
    display: none;
    max-width: 92%;
    text-align: center;
  }
</style>
</head>
<body>
  <div class="header">
    <div class="title-row">
      <div class="logo">♫ KARAOKE LIVE</div>
      <div id="statusBadge" class="badge">🟢 Приём открыт</div>
    </div>
    <div class="mic-banner">
      <span>🎤🎤</span>
      <span>В наличии <b>2 микрофона</b> — при заказе выберите <b>Соло (1 микр.)</b> или <b>Дуэт (2 микр.)</b></span>
    </div>
    <div class="search-wrap">
      <input id="searchInput" class="search-input" type="search" placeholder="🔍 Поиск по исполнителю или названию песни..." autocomplete="off">
      <button id="clearBtn" class="clear-btn" onclick="clearSearch()">✕</button>
    </div>
  </div>

  <div class="container">
    <div class="stage-card">
      <div class="stage-title">Сейчас на сцене</div>
      <div id="stageNow" class="stage-now">Фоновая музыка — сцена свободна</div>
      <div id="stageRot" class="stage-rot">Очередь вокалистов пуста — будьте первым!</div>
    </div>

    <div id="myReqsSection" style="display:none; margin-bottom:14px;">
      <div class="section-lbl"><span>Отправленные заказы у оператора</span></div>
      <div id="myReqsList" class="song-list"></div>
    </div>

    <div class="section-lbl">
      <span id="catalogHeader">Каталог караоке</span>
      <span id="songCountLbl"></span>
    </div>
    <div id="songList" class="song-list"></div>
  </div>

  <div id="orderModal" class="modal-backdrop" onclick="if(event.target===this)closeModal()">
    <div class="modal">
      <div id="modalTitle" class="modal-song">Название песни</div>
      <div id="modalArtist" class="modal-artist">Исполнитель</div>

      <label class="field-lbl">Ваше имя (или номер столика) *</label>
      <input id="singerInput" class="text-input" type="text" placeholder="Например: Алексей (Стол 3)" maxlength="45">

      <label class="field-lbl">Сколько микрофонов нужно? (всего в зале 2 шт.)</label>
      <div class="mic-grid">
        <div id="mic1Btn" class="mic-option active" onclick="selectMics(1)">🎤 1 микрофон<br><small style="color:#94a3b8;font-weight:400">Соло</small></div>
        <div id="mic2Btn" class="mic-option" onclick="selectMics(2)">🎤🎤 2 микрофона<br><small style="color:#94a3b8;font-weight:400">Дуэт / Пара</small></div>
      </div>

      <div id="partnerWrap" style="display:none;">
        <label class="field-lbl">Второй участник для дуэта (необязательно)</label>
        <input id="partnerInput" class="text-input" type="text" placeholder="Например: Мария" maxlength="35">
      </div>

      <label class="field-lbl">Тональность (Key)</label>
      <div class="key-row">
        <button class="key-btn" type="button" onclick="changeKey(-1)">−</button>
        <span id="keyLabel" class="key-val">0 (Оригинал)</span>
        <button class="key-btn" type="button" onclick="changeKey(1)">+</button>
      </div>

      <div class="modal-actions">
        <button class="btn-cancel" type="button" onclick="closeModal()">Отмена</button>
        <button id="submitBtn" class="btn-submit" type="button" onclick="submitOrder()">Отправить заказ</button>
      </div>
    </div>
  </div>

  <div id="toast" class="toast"></div>

<script>
let selectedSong = null;
let selectedMics = 1;
let selectedKey = 0;
let searchTimer = null;

const savedSinger = localStorage.getItem('okj_singer_name') || '';
if (savedSinger) document.getElementById('singerInput').value = savedSinger;

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s || '';
  return d.innerHTML;
}

function showToast(msg, isErr) {
  const t = document.getElementById('toast');
  t.style.background = isErr ? '#dc2626' : '#059669';
  t.textContent = msg;
  t.style.display = 'block';
  setTimeout(() => { t.style.display = 'none'; }, 3500);
}

async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    const badge = document.getElementById('statusBadge');
    if (d.accepting) {
      badge.textContent = '🟢 Приём открыт';
      badge.className = 'badge';
    } else {
      badge.textContent = '🔴 Приём закрыт';
      badge.className = 'badge closed';
    }
    const nowEl = document.getElementById('stageNow');
    if (d.currentSinger || d.currentSong) {
      nowEl.textContent = (d.currentSinger ? ('🎤 ' + d.currentSinger + ' — ') : '♫ ') + (d.currentSong || '');
    } else {
      nowEl.textContent = 'Фоновая музыка — сцена свободна';
    }
    const rotEl = document.getElementById('stageRot');
    if (d.upcomingSingers && d.upcomingSingers.length > 0) {
      rotEl.textContent = 'В очереди (' + d.upcomingSingers.length + '): ' + d.upcomingSingers.join(' • ');
    } else {
      rotEl.textContent = 'Очередь вокалистов свободна';
    }
    const reqSec = document.getElementById('myReqsSection');
    const reqList = document.getElementById('myReqsList');
    if (d.requests && d.requests.length > 0) {
      reqSec.style.display = 'block';
      reqList.innerHTML = d.requests.map(req => `
        <div class="song-item" style="border-color:rgba(6,182,212,0.35);padding:9px 12px;">
          <div class="song-info">
            <div class="song-title" style="font-size:13px;">${esc(req.title)} — <span style="color:#7dd3fc">${esc(req.artist)}</span></div>
            <div class="song-meta">Вокалист: <b>${esc(req.singer)}</b> • ${req.micCount===2 ? '🎤🎤 2 микр.' : '🎤 1 микр.'}${req.key ? (' • Key ' + (req.key>0?'+'+req.key:req.key)) : ''}</div>
          </div>
          <span style="font-size:11px;color:#34d399;font-weight:700;">У ведущего</span>
        </div>
      `).join('');
    } else {
      reqSec.style.display = 'none';
    }
  } catch (e) {}
}

async function searchSongs(query) {
  try {
    const r = await fetch('/api/songs?q=' + encodeURIComponent(query || '') + '&limit=60');
    const d = await r.json();
    const list = document.getElementById('songList');
    const songs = d.songs || [];
    document.getElementById('catalogHeader').textContent = query ? 'Результаты поиска' : 'Каталог песен';
    document.getElementById('songCountLbl').textContent = songs.length ? ('Показано: ' + songs.length) : '';
    if (songs.length === 0) {
      list.innerHTML = '<div style="text-align:center;color:#94a3b8;padding:28px 10px;">Песни не найдены. Попробуйте другое название или исполнителя.</div>';
      return;
    }
    list.innerHTML = songs.map(s => `
      <div class="song-item" onclick='openModal(${JSON.stringify(s).replace(/'/g, "&#39;")})'>
        <div class="song-info">
          <div class="song-title">${esc(s.title || s.discid || 'Без названия')}</div>
          <div class="song-artist">${esc(s.artist || 'Неизвестный исполнитель')}</div>
          ${s.duration ? `<div class="song-meta">⏱ ${esc(s.duration)}</div>` : ''}
        </div>
        <button class="order-btn" type="button">Заказать</button>
      </div>
    `).join('');
  } catch (e) {}
}

function clearSearch() {
  const inp = document.getElementById('searchInput');
  inp.value = '';
  document.getElementById('clearBtn').style.display = 'none';
  searchSongs('');
}

document.getElementById('searchInput').addEventListener('input', (e) => {
  const val = e.target.value;
  document.getElementById('clearBtn').style.display = val ? 'block' : 'none';
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => searchSongs(val), 180);
});

function selectMics(count) {
  selectedMics = count;
  document.getElementById('mic1Btn').className = 'mic-option' + (count === 1 ? ' active' : '');
  document.getElementById('mic2Btn').className = 'mic-option' + (count === 2 ? ' active' : '');
  document.getElementById('partnerWrap').style.display = (count === 2) ? 'block' : 'none';
}

function changeKey(delta) {
  selectedKey = Math.max(-6, Math.min(6, selectedKey + delta));
  const lbl = document.getElementById('keyLabel');
  if (selectedKey === 0) lbl.textContent = '0 (Оригинал)';
  else lbl.textContent = (selectedKey > 0 ? '+' + selectedKey : selectedKey) + ' полутон.';
}

function openModal(song) {
  selectedSong = song;
  selectedKey = 0;
  changeKey(0);
  document.getElementById('modalTitle').textContent = song.title || 'Без названия';
  document.getElementById('modalArtist').textContent = song.artist || '';
  document.getElementById('orderModal').classList.add('open');
  if (!document.getElementById('singerInput').value) {
    setTimeout(() => document.getElementById('singerInput').focus(), 100);
  }
}

function closeModal() {
  document.getElementById('orderModal').classList.remove('open');
}

async function submitOrder() {
  if (!selectedSong) return;
  const singer = document.getElementById('singerInput').value.trim();
  const partner = document.getElementById('partnerInput').value.trim();
  if (!singer) {
    showToast('Пожалуйста, введите ваше имя или номер столика', true);
    document.getElementById('singerInput').focus();
    return;
  }
  localStorage.setItem('okj_singer_name', singer);
  const btn = document.getElementById('submitBtn');
  btn.disabled = true;
  btn.textContent = 'Отправка...';
  try {
    const r = await fetch('/api/request', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        songId: selectedSong.id,
        artist: selectedSong.artist,
        title: selectedSong.title,
        singer: singer,
        partner: selectedMics === 2 ? partner : '',
        micCount: selectedMics,
        key: selectedKey
      })
    });
    const d = await r.json();
    if (d.error) {
      showToast(d.message || 'Ошибка при отправке заказа', true);
    } else {
      closeModal();
      showToast(d.message || 'Заказ отправлен ведущему!', false);
      fetchStatus();
    }
  } catch (e) {
    showToast('Нет связи с сервером караоке', true);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Отправить заказ';
  }
}

fetchStatus();
searchSongs('');
setInterval(fetchStatus, 4000);
</script>
</body>
</html>)HTML";
    return QByteArray(kHtml);
}

void OKJSongbookAPI::getSerial()
{
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","getSerial");
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

void OKJSongbookAPI::refreshRequests()
{
    emit requestsChanged(requests);
    emit synchronized(QTime::currentTime());
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject jsonObject;
    jsonObject.insert("api_key", m_settings.requestServerApiKey());
    jsonObject.insert("command","getRequests");
    jsonObject.insert("venue_id", m_settings.requestServerVenue());
    QJsonDocument jsonDocument;
    jsonDocument.setObject(jsonObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

void OKJSongbookAPI::triggerTestAdd()
{
    QSqlQuery q("SELECT songid, artist, title FROM dbsongs WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' ORDER BY RANDOM() LIMIT 1");
    if (q.exec() && q.next())
    {
        OkjsRequest req;
        req.requestId = m_nextLocalRequestId++;
        req.songId = q.value(0).toInt();
        req.artist = q.value(1).toString();
        req.title = q.value(2).toString();
        req.singer = QString("Гость %1").arg(req.requestId);
        req.micCount = (req.requestId % 2 == 0) ? 2 : 1;
        req.key = 0;
        req.time = static_cast<int>(QDateTime::currentSecsSinceEpoch());
        requests.append(req);
        emit requestsChanged(requests);
        emit localRequestSubmitted(req);
        return;
    }
}

void OKJSongbookAPI::removeRequest(int requestId)
{
    auto it = std::remove_if(requests.begin(), requests.end(), [requestId](const OkjsRequest &r) {
        return r.requestId == requestId;
    });
    if (it != requests.end())
    {
        requests.erase(it, requests.end());
        emit requestsChanged(requests);
    }
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","deleteRequest");
    mainObject.insert("venue_id", m_settings.requestServerVenue());
    mainObject.insert("request_id", requestId);
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

bool OKJSongbookAPI::getAccepting()
{
    return m_localAccepting;
}

void OKJSongbookAPI::setAccepting(bool enabled)
{
    m_localAccepting = enabled;
    for (auto &v : venues)
        v.accepting = enabled;
    emit venuesChanged(venues);

    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    alertCheck();
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","setAccepting");
    mainObject.insert("venue_id", m_settings.requestServerVenue());
    mainObject.insert("accepting", enabled);
    mainObject.insert("system_id", m_settings.systemId());
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

void OKJSongbookAPI::refreshVenues(bool blocking)
{
    emit venuesChanged(venues);
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","getVenues");
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = manager->post(request, jsonDocument.toJson());
    if (blocking)
    {
        while (!reply->isFinished())
            QApplication::processEvents();
    }
}

void OKJSongbookAPI::clearRequests()
{
    requests.clear();
    emit requestsChanged(requests);
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","clearRequests");
    mainObject.insert("venue_id", m_settings.requestServerVenue());
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

void OKJSongbookAPI::updateSongDb()
{
    cancelUpdate = false;
    updateInProgress = true;
    emit remoteSongDbUpdateStart();
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
    {
        emit remoteSongDbUpdateNumDocs(1);
        emit remoteSongDbUpdateProgress(1);
        updateInProgress = false;
        emit remoteSongDbUpdateDone();
        return;
    }
    int songsPerDoc = 1000;
    QList<QJsonDocument> jsonDocs;
    QSqlQuery query;
    int numEntries = 0;
    if (cancelUpdate)
        return;
    if (query.exec("SELECT COUNT(DISTINCT artist||title) FROM dbsongs WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!'"))
    {
        if (query.next())
            numEntries = query.value(0).toInt();
    }
    if (cancelUpdate)
        return;
    if (query.exec("SELECT DISTINCT artist,title FROM dbsongs WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' ORDER BY artist ASC, title ASC"))
    {
        if (cancelUpdate)
            return;
        bool done = false;
        int numDocs = numEntries / songsPerDoc;
        if (numEntries % songsPerDoc > 0)
            numDocs++;
        emit remoteSongDbUpdateNumDocs(numDocs);
        int docs = 0;
        while (!done)
        {
            if (cancelUpdate)
                return;
            QApplication::processEvents();
            QJsonArray songsArray;
            int count = 0;
            while ((query.next()) && (count < songsPerDoc))
            {
                if (cancelUpdate)
                    return;
                QJsonObject songObject;
                songObject.insert("artist", query.value(0).toString());
                songObject.insert("title", query.value(1).toString());
                songsArray.insert(0, songObject);
                QApplication::processEvents();
                count++;
            }
            docs++;
            if (count < songsPerDoc)
                done = true;
            QJsonObject mainObject;
            mainObject.insert("api_key", m_settings.requestServerApiKey());
            mainObject.insert("command","addSongs");
            mainObject.insert("songs", songsArray);
            mainObject.insert("system_id", m_settings.systemId());
            QJsonDocument jsonDocument;
            jsonDocument.setObject(mainObject);
            jsonDocs.append(jsonDocument);
        }
        QUrl url(m_settings.requestServerUrl());
        QJsonObject mainObject;
        mainObject.insert("api_key", m_settings.requestServerApiKey());
        mainObject.insert("command","clearDatabase");
        mainObject.insert("system_id", m_settings.systemId());
        QJsonDocument jsonDocument;
        jsonDocument.setObject(mainObject);
        QNetworkRequest request(url);
        if (cancelUpdate)
            return;
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkAccessManager *mgr = new QNetworkAccessManager(this);
        QNetworkReply *reply = mgr->post(request, jsonDocument.toJson());
        while (!reply->isFinished())
            QApplication::processEvents();
        for (int i=0; i < jsonDocs.size(); i++)
        {
            if (cancelUpdate)
                return;
            QApplication::processEvents();
            QNetworkRequest req(url);
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QNetworkAccessManager *stepMgr = new QNetworkAccessManager(this);
            QNetworkReply *stepReply = stepMgr->post(req, jsonDocs.at(i).toJson());
            while (!stepReply->isFinished()){
                if (cancelUpdate)
                    return;
                QApplication::processEvents();
            }
            if (cancelUpdate)
                return;
            emit remoteSongDbUpdateProgress(i + 1);
        }
    }
    if (cancelUpdate)
        return;
    updateInProgress = false;
    emit remoteSongDbUpdateDone();
}

bool OKJSongbookAPI::test()
{
    if (isLocalServerRunning() && (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty()))
    {
        emit testPassed();
        return true;
    }
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","getSerial");
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkAccessManager m_NetworkMngr;

    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_NetworkMngr.post(request, jsonDocument.toJson());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError)
    {
        m_logger->error("{} Network error: {}", m_loggingPrefix, reply->errorString());
        emit testFailed(reply->errorString());
        return false;
    }
    QByteArray data = reply->readAll();
    delete reply;
    QJsonDocument json = QJsonDocument::fromJson(data);
    QString command = json.object().value("command").toString();
    if (json.object().value("errorString").toString() != "")
    {
        emit testFailed(json.object().value("errorString").toString());
        return false;
    }
    if (command == "getSerial")
    {
        int newSerial = json.object().value("serial").toInt();
        if (newSerial != 0)
        {
            emit testPassed();
            return true;
        }
    }
    emit testFailed("Unknown error");
    return false;
}

void OKJSongbookAPI::alertCheck()
{
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","getAlert");
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(QUrl(m_settings.requestServerUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

void OKJSongbookAPI::onSslErrors(QNetworkReply *reply, const QList<QSslError>& errors)
{
    Q_UNUSED(errors)
    static QString lastUrl;
    static bool errorEmitted = false;
    if (lastUrl != m_settings.requestServerUrl())
        errorEmitted = false;
    if (m_settings.requestServerIgnoreCertErrors())
        reply->ignoreSslErrors();
    else if (!errorEmitted)
    {
        emit sslError();
        errorEmitted = true;
    }
    lastUrl = m_settings.requestServerUrl();
}

void OKJSongbookAPI::onTestSslErrors(QNetworkReply *reply, QList<QSslError> errors)
{
    if (m_settings.requestServerIgnoreCertErrors())
    {
        reply->ignoreSslErrors();
        return;
    }
    QString errorText;
    foreach (QSslError error, errors) {
        errorText += error.errorString() + "\n";
    }
    emit testSslError(errorText);
}

void OKJSongbookAPI::onNetworkReply(QNetworkReply *reply)
{
    if (m_settings.requestServerIgnoreCertErrors())
        reply->ignoreSslErrors();
    if (reply->error() != QNetworkReply::NoError)
    {
        m_logger->warn("{} Network error: {}", m_loggingPrefix, reply->errorString());
        return;
    }
    QByteArray data = reply->readAll();
    QJsonDocument json = QJsonDocument::fromJson(data);
    QString command = json.object().value("command").toString();
    bool error = json.object().value("error").toBool();
    if (error)
        return;
    if (command == "testingAddRandomRequest")
        refreshRequests();
    if (command == "getEntitledSystemCount")
    {
        entitledSystems = json.object().value("count").toInt();
        emit entitledSystemCountChanged(entitledSystems);
    }
    if (command == "getAlert")
    {
        if(json.object().value("alert").toBool())
        {
            emit alertRecieved(json.object().value("title").toString(), json.object().value("message").toString());
            venues.clear();
            refreshVenues();
        }
    }
    if (command == "getSerial")
    {
        int newSerial = json.object().value("serial").toInt();
        if (newSerial == 0)
            return;
        if (serial == newSerial)
        {
            lastSync = QTime::currentTime();
            emit synchronized(lastSync);
        }
        else
        {
            serial = newSerial;
            refreshRequests();
            refreshVenues();
            lastSync = QTime::currentTime();
            emit synchronized(lastSync);
        }
    }
    if (command == "getVenues")
    {
        QJsonArray venuesArray = json.object().value("venues").toArray();
        OkjsVenues l_venues;
        for (const auto &venuesEntry : venuesArray)
        {
            OkjsVenue venue;
            QJsonObject jsonObject = venuesEntry.toObject();
            venue.venueId = jsonObject.value("venue_id").toInt();
            venue.name = jsonObject.value("name").toString();
            venue.urlName = jsonObject.value("url_name").toString();
            venue.accepting = jsonObject.value("accepting").toBool();
            l_venues.append(venue);
        }
        if (venues != l_venues && !l_venues.isEmpty())
        {
            venues = l_venues;
            emit venuesChanged(venues);
            getEntitledSystemCount();
        }
        lastSync = QTime::currentTime();
        emit synchronized(lastSync);
    }
    if (command == "clearRequests" || command == "setAccepting" || command == "deleteRequest")
    {
        refreshRequests();
        refreshVenues();
    }
    if (command == "getRequests")
    {
        QJsonArray requestsArray = json.object().value("requests").toArray();
        OkjsRequests l_requests;
        for (const auto &requestEntry : requestsArray)
        {
            OkjsRequest request;
            QJsonObject jsonObject = requestEntry.toObject();
            request.requestId = jsonObject.value("request_id").toInt();
            request.artist = jsonObject.value("artist").toString();
            request.title = jsonObject.value("title").toString();
            request.singer = jsonObject.value("singer").toString();
            request.time = jsonObject.value("request_time").toInt();
            request.key = jsonObject.value("key_change").toInt();
            request.micCount = 1;
            l_requests.append(request);
        }
        if (requests != l_requests)
        {
            requests = l_requests;
            emit requestsChanged(requests);
        }
        lastSync = QTime::currentTime();
        emit synchronized(lastSync);
    }
}

void OKJSongbookAPI::timerTimeout()
{
    if (m_settings.requestServerEnabled() && !m_settings.requestServerUrl().isEmpty() &&
        !m_settings.requestServerApiKey().isEmpty() && !programIsIdle)
    {
        getSerial();
    }
}

void OKJSongbookAPI::alertTimerTimeout()
{
    if (m_settings.requestServerEnabled() && !m_settings.requestServerUrl().isEmpty() && !m_settings.requestServerApiKey().isEmpty())
        alertCheck();
}

void OKJSongbookAPI::setInterval(int interval)
{
    timer->setInterval(interval * 1000);
}

void OKJSongbookAPI::idleStateChanged(bool isIdle)
{
    if (!isIdle)
    {
        lastSync = QTime::currentTime();
        timerTimeout();
    }
    programIsIdle = isIdle;
}

void OKJSongbookAPI::getEntitledSystemCount()
{
    if (m_settings.requestServerUrl().isEmpty() || m_settings.requestServerApiKey().isEmpty())
        return;
    QJsonObject mainObject;
    mainObject.insert("api_key", m_settings.requestServerApiKey());
    mainObject.insert("command","getEntitledSystemCount");
    QJsonDocument jsonDocument;
    jsonDocument.setObject(mainObject);
    QNetworkRequest request(m_settings.requestServerUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    manager->post(request, jsonDocument.toJson());
}

void OKJSongbookAPI::dbUpdateCanceled()
{
    if (!cancelUpdate && updateInProgress)
    {
        QMessageBox msgBox(nullptr);
        msgBox.setWindowTitle(tr("Cancelling Update"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setText("Are you sure you want to cancel the Songbook DB update?");
        QPushButton *yesButton = msgBox.addButton(tr("Cancel Update"), QMessageBox::AcceptRole);
        msgBox.addButton(tr("Continue Update"), QMessageBox::RejectRole);
        msgBox.exec();
        if (msgBox.clickedButton() == yesButton)
        {
            cancelUpdate = true;
            updateInProgress = false;
        }
    }
}

bool OkjsVenue::operator ==(const OkjsVenue &v) const
{
    return venueId == v.venueId && name == v.name && urlName == v.urlName && accepting == v.accepting;
}

bool OkjsRequest::operator ==(const OkjsRequest& r) const
{
    return r.requestId == requestId && r.artist == artist && r.title == title &&
           r.time == time && r.singer == singer && r.songId == songId && r.micCount == micCount;
}
