#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDataStream>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QStandardPaths>
#include <gst/gst.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include "cdg/cdgimageframe.h"
#include "dlgcdg.h"
#include "dlgrequests.h"
#include "dlgsettings.h"
#include "idledetect.h"
#include "okjsongbookapi.h"
#include "okjtranslator.h"
#include "mediabackend.h"
#include "settings.h"
#include "updatechecker.h"
#include "models/tablemodelkaraokesongs.h"
#include "models/tablemodelrotation.h"
#include "models/tablemodelqueuesongs.h"

IdleDetect *filter = nullptr;

class OpenKJTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;

    static void createTestSchema()
    {
        QSqlQuery q;
        QVERIFY(q.exec("CREATE TABLE IF NOT EXISTS dbSongs (songid INTEGER PRIMARY KEY AUTOINCREMENT, Artist COLLATE NOCASE, Title COLLATE NOCASE, DiscId COLLATE NOCASE, 'Duration' INTEGER, path VARCHAR(700) NOT NULL UNIQUE, filename COLLATE NOCASE, searchstring TEXT, plays INT DEFAULT(0), lastplay TIMESTAMP)"));
        QVERIFY(q.exec("CREATE TABLE IF NOT EXISTS rotationSingers (singerid INTEGER PRIMARY KEY AUTOINCREMENT, name COLLATE NOCASE UNIQUE, 'position' INTEGER NOT NULL, 'regular' LOGICAL DEFAULT(0), 'regularid' INTEGER, addts TIMESTAMP)"));
        QVERIFY(q.exec("CREATE TABLE IF NOT EXISTS queueSongs (qsongid INTEGER PRIMARY KEY AUTOINCREMENT, singer INT, song INTEGER NOT NULL, artist INT, title INT, discid INT, path INT, keychg INT, played LOGICAL DEFAULT(0), 'position' INT)"));
        QVERIFY(q.exec("CREATE TABLE IF NOT EXISTS regularSingers (regsingerid INTEGER PRIMARY KEY AUTOINCREMENT, Name COLLATE NOCASE UNIQUE, ph1 INT, ph2 INT, ph3 INT)"));
        QVERIFY(q.exec("CREATE TABLE IF NOT EXISTS regularSongs (regsongid INTEGER PRIMARY KEY AUTOINCREMENT, regsingerid INTEGER NOT NULL, songid INTEGER NOT NULL, 'keychg' INTEGER, 'position' INTEGER)"));
    }

    static bool writeTestWav(const QString &path, int durationMs = 500)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        const int sampleRate = 44100;
        const int channels = 2;
        const int bitsPerSample = 16;
        const int numSamples = (sampleRate * durationMs) / 1000;
        const quint32 dataSize = numSamples * channels * (bitsPerSample / 8);
        const quint32 chunkSize = 36 + dataSize;

        QDataStream out(&f);
        out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("RIFF", 4);
        out << chunkSize;
        out.writeRawData("WAVE", 4);
        out.writeRawData("fmt ", 4);
        out << quint32(16) << quint16(1) << quint16(channels) << quint32(sampleRate);
        out << quint32(sampleRate * channels * (bitsPerSample / 8));
        out << quint16(channels * (bitsPerSample / 8)) << quint16(bitsPerSample);
        out.writeRawData("data", 4);
        out << dataSize;
        QByteArray silence(dataSize, 0);
        out.writeRawData(silence.constData(), silence.size());
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tempDir.isValid());
        filter = new IdleDetect(this);
        auto nullSink = std::make_shared<spdlog::sinks::null_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("logger", nullSink);
        spdlog::register_logger(logger);

        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
        db.setDatabaseName(m_tempDir.filePath("test_openkj.sqlite"));
        QVERIFY2(db.open(), qPrintable(db.lastError().text()));
        createTestSchema();

        QSqlQuery q;
        QVERIFY(q.exec("INSERT INTO dbSongs (Artist, Title, DiscId, Duration, path, filename, searchstring) "
                       "VALUES ('Баста', 'Сансара', 'TEST01', 240, 'C:/music/basta_sansara.zip', 'Баста - Сансара.zip', 'Баста - Сансара.zip Баста Сансара TEST01')"));
        QVERIFY(q.exec("INSERT INTO dbSongs (Artist, Title, DiscId, Duration, path, filename, searchstring) "
                       "VALUES ('Queen', 'Bohemian Rhapsody', 'TEST02', 354, 'C:/music/queen_bohemian.mp4', 'Queen - Bohemian Rhapsody.mp4', 'Queen - Bohemian Rhapsody.mp4 Queen Bohemian Rhapsody TEST02')"));
    }

    void testDatabaseAndRotation()
    {
        TableModelKaraokeSongs songsModel;
        songsModel.loadData();
        QTest::qWait(150);
        QCOMPARE(songsModel.rowCount(QModelIndex()), 2);

        songsModel.search("Баста");
        QTest::qWait(150);
        QCOMPARE(songsModel.rowCount(QModelIndex()), 1);
        okj::KaraokeSong song = songsModel.getSong(1);
        QCOMPARE(song.artist, QString("Баста"));
        QCOMPARE(song.title, QString("Сансара"));
        songsModel.search("");
        QTest::qWait(150);
        QCOMPARE(songsModel.rowCount(QModelIndex()), 2);

        TableModelRotation rotModel;
        int s1 = rotModel.singerAdd("Стол 1");
        int s2 = rotModel.singerAdd("Стол 2");
        QVERIFY(s1 > 0 && s2 > 0);
        QCOMPARE(rotModel.rowCount(QModelIndex()), 2);

        TableModelQueueSongs queueModel(songsModel);
        queueModel.loadSinger(s1);
        QCOMPARE(queueModel.rowCount(), 0);
        queueModel.songAddSlot(1, s1, 0);
        QCOMPARE(queueModel.rowCount(), 1);
        int qSongId = queueModel.index(0, TableModelQueueSongs::COL_ID).data(Qt::DisplayRole).toInt();
        queueModel.setKey(qSongId, 2);
        QCOMPARE(queueModel.getKey(qSongId), 2);
    }

    void testCdgDecoderAndCorruptedPackets()
    {
        CdgImageFrame frame;
        cdg::CDG_SubCode sub{};
        sub.command = static_cast<cdg::CdgCommand>(0x09);

        // Valid MemoryPreset
        sub.instruction = 0x01;
        sub.data[0] = 2;
        QVERIFY(frame.applySubCode(sub));

        // Valid BorderPreset
        sub.instruction = 0x02;
        sub.data[0] = 3;
        frame.applySubCode(sub);

        // Valid TileBlock
        sub.instruction = 0x06;
        sub.data[0] = 1; // color0
        sub.data[1] = 4; // color1
        sub.data[2] = 5; // row
        sub.data[3] = 10; // column
        QVERIFY(frame.applySubCode(sub));

        // Scroll in all 4 directions (verifies memmove & buffer types)
        for (int hCmd : {0, 1, 2}) {
            for (int vCmd : {0, 1, 2}) {
                if (hCmd == 0 && vCmd == 0) continue;
                sub.instruction = 0x14; // ScrollPreset
                sub.data[0] = 1;
                sub.data[1] = static_cast<char>(hCmd << 4);
                sub.data[2] = static_cast<char>(vCmd << 4);
                frame.applySubCode(sub);

                sub.instruction = 0x18; // ScrollCopy
                frame.applySubCode(sub);
            }
        }

        // Corrupted packets (color >= 16, row/col out of bounds) must not crash
        sub.instruction = 0x01;
        sub.data[0] = 31; // masked to 4 bits (15) in CdgMemoryPresetData
        QVERIFY(frame.applySubCode(sub));

        sub.instruction = 0x06;
        sub.data[0] = 25;
        sub.data[1] = 25;
        sub.data[2] = 99;
        sub.data[3] = 99;
        frame.applySubCode(sub);

        sub.instruction = 0x14;
        sub.data[0] = 31; // invalid palette color
        sub.data[1] = static_cast<char>(2 << 4);
        sub.data[2] = static_cast<char>(2 << 4);
        frame.applySubCode(sub);

        QCOMPARE(frame.getImage().size(), cdg::FRAME_DIM_FULL);
    }

    void testLocalQrHttpServerAndValidation()
    {
        OKJSongbookAPI api;
        QVERIFY2(api.isLocalServerRunning(), "Local QR HTTP server failed to start");
        const quint16 port = api.localServerPort();
        QVERIFY(port > 0);

        api.setStageStatus("Стол 3", "Баста — Сансара", {"Анна", "Дмитрий"});
        api.setAccepting(true);

        QNetworkAccessManager nam;
        const QString baseUrl = QString("http://127.0.0.1:%1").arg(port);

        // 1. GET /
        {
            QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(baseUrl + "/")));
            QSignalSpy spy(reply, &QNetworkReply::finished);
            QVERIFY(spy.wait(3000));
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            QByteArray html = reply->readAll();
            QVERIFY(html.contains("<html") || html.contains("<!DOCTYPE"));
            reply->deleteLater();
        }

        // 2. GET /api/status
        {
            QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(baseUrl + "/api/status")));
            QSignalSpy spy(reply, &QNetworkReply::finished);
            QVERIFY(spy.wait(3000));
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            QCOMPARE(obj["accepting"].toBool(), true);
            QCOMPARE(obj["maxMics"].toInt(), 2);
            QCOMPARE(obj["currentSinger"].toString(), QString("Стол 3"));
            reply->deleteLater();
        }

        // 3. GET /api/songs?q=Queen
        {
            QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(baseUrl + "/api/songs?q=Queen")));
            QSignalSpy spy(reply, &QNetworkReply::finished);
            QVERIFY(spy.wait(3000));
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            QJsonArray songs = obj["songs"].toArray();
            QVERIFY(songs.size() >= 1);
            reply->deleteLater();
        }

        // 4. POST /api/request (1 mic & 2 mics)
        auto submitReq = [&](const QString &singer, int micCount) {
            QNetworkRequest req(QUrl(baseUrl + "/api/request"));
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QJsonObject body;
            body["singer"] = singer;
            body["songId"] = 1;
            body["micCount"] = micCount;
            QNetworkReply *reply = nam.post(req, QJsonDocument(body).toJson());
            QSignalSpy spy(reply, &QNetworkReply::finished);
            QVERIFY(spy.wait(3000));
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            reply->deleteLater();
        };
        submitReq("Стол 1", 1);
        submitReq("Стол 2 (Дуэт)", 2);

        const auto &reqs = api.currentRequests();
        QCOMPARE(reqs.size(), 2);
        QCOMPARE(reqs[0].singer, QString("Стол 1"));
        QCOMPARE(reqs[0].micCount, 1);
        QCOMPARE(reqs[1].singer, QString("Стол 2 (Дуэт)"));
        QCOMPARE(reqs[1].micCount, 2);

        // 5. Negative test: invalid Content-Length: -5 -> 400 Bad Request
        {
            QTcpSocket sock;
            sock.connectToHost("127.0.0.1", port);
            for (int i = 0; i < 200 && sock.state() != QAbstractSocket::ConnectedState; ++i) {
                QCoreApplication::processEvents();
                QTest::qWait(10);
            }
            QCOMPARE(sock.state(), QAbstractSocket::ConnectedState);
            sock.write("POST /api/request HTTP/1.1\r\nHost: localhost\r\nContent-Length: -5\r\n\r\n");
            sock.flush();
            QByteArray resp;
            for (int i = 0; i < 200 && resp.isEmpty(); ++i) {
                QCoreApplication::processEvents();
                QTest::qWait(10);
                resp.append(sock.readAll());
            }
            QVERIFY2(!resp.isEmpty(), "Expected HTTP 400 response, got empty");
            QVERIFY2(resp.startsWith("HTTP/1.1 400"), resp.constData());
        }
    }

    void testGStreamerElementsAndPlayback()
    {
        if (!gst_is_initialized())
            gst_init(nullptr, nullptr);

        for (const char *factory : {"playbin", "audioconvert", "audioresample", "volume", "equalizer-10bands", "spectrum", "videoconvert", "videoscale", "appsink"}) {
            GstElementFactory *f = gst_element_factory_find(factory);
            QVERIFY2(f != nullptr, qPrintable(QString("Missing GStreamer element: %1").arg(factory)));
            gst_object_unref(f);
        }

        QString wavPath = m_tempDir.filePath("test_silence.wav");
        QVERIFY(writeTestWav(wavPath, 800));

        QWidget videoSurface1;
        QWidget videoSurface2;
        videoSurface1.resize(320, 240);
        videoSurface2.resize(320, 240);
        MediaBackend videoBackend(nullptr, "TEST_VIDEO", MediaBackend::Karaoke);
        videoBackend.setVideoOutputWidgets({&videoSurface1, &videoSurface2});
        videoBackend.setMedia(wavPath);
        videoBackend.setVolume(0);
        videoBackend.play();
        QElapsedTimer vwt;
        vwt.start();
        while (vwt.elapsed() < 2000 && videoBackend.state() != MediaBackend::PlayingState) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(20);
        }
        QCOMPARE(videoBackend.state(), MediaBackend::PlayingState);
        videoBackend.stop(true);

        MediaBackend backend(nullptr, "TEST", MediaBackend::BackgroundMusic);
        backend.setMedia(wavPath);
        backend.setVolume(0);
        backend.play();

        auto waitForState = [&](MediaBackend::State target) {
            QElapsedTimer wt;
            wt.start();
            while (wt.elapsed() < 2000 && backend.state() != target) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QTest::qWait(20);
            }
            return backend.state() == target;
        };

        QVERIFY(waitForState(MediaBackend::PlayingState));
        backend.pause();
        QVERIFY(waitForState(MediaBackend::PausedState));
        backend.stop();
        QVERIFY(waitForState(MediaBackend::StoppedState));
    }

    void testRussianTranslation()
    {
        OkjTranslator translator;
        QVERIFY(translator.loadTs(":/translations/openkj_ru.ts"));
        QCOMPARE(translator.translate("MainWindow", "Add singer", nullptr, -1), QString("Добавить исполнителя"));
        QCOMPARE(translator.translate("DlgSettings", "Language", nullptr, -1), QString("Язык интерфейса"));
    }

    void testUiAuditAndDialogs()
    {
        QStandardPaths::setTestModeEnabled(true);
        Settings s;
        s.setRequestServerEnabled(false);

        TableModelRotation rotModel;
        OKJSongbookAPI api;
        DlgRequests dlgReq(rotModel, api);

        // External db.openkj.org search button must be removed
        QVERIFY(dlgReq.findChild<QPushButton *>("pushButtonWebSearch") == nullptr);

        // Cloud-only controls must be hidden when requestServerEnabled is false
        auto *cbVenue = dlgReq.findChild<QComboBox *>("comboBoxVenue");
        auto *btnUpdateDb = dlgReq.findChild<QPushButton *>("pushButtonUpdateDb");
        auto *lblLastUpd = dlgReq.findChild<QLabel *>("labelLastUpdate");
        auto *btnRefresh = dlgReq.findChild<QPushButton *>("buttonRefresh");
        QVERIFY(cbVenue && cbVenue->isHidden());
        QVERIFY(btnUpdateDb && btnUpdateDb->isHidden());
        QVERIFY(lblLastUpd && lblLastUpd->isHidden());
        QVERIFY(btnRefresh && btnRefresh->isHidden());

        // DlgSettings Network tab must contain local QR server info label
        MediaBackend karBackend(nullptr, "KTEST", MediaBackend::Karaoke);
        MediaBackend bmBackend(nullptr, "BTEST", MediaBackend::BackgroundMusic);
        DlgSettings dlgSettings(karBackend, bmBackend, api);
        auto *netTab = dlgSettings.findChild<QWidget *>("tabWidgetPage3");
        QVERIFY(netTab != nullptr);
        bool hasQrNote = false;
        for (QLabel *lbl : netTab->findChildren<QLabel *>()) {
            if (lbl->text().contains("QR", Qt::CaseInsensitive)) {
                hasQrNote = true;
                break;
            }
        }
        QVERIFY(hasQrNote);

        // Default karaoke alert must be enabled with a large readable font (>= 24pt), and small saved fonts must be boosted
        QVERIFY(s.karaokeAAAlertEnabled());
        QVERIFY(s.karaokeAAAlertFont().pointSize() >= 24);
        DlgCdg dlgCdg(karBackend, bmBackend);
        QFont tinyFont = QApplication::font();
        tinyFont.setPointSize(9);
        dlgCdg.alertFontChanged(tinyFont);
        auto *lblNextSinger = dlgCdg.findChild<QLabel *>("lblNextSinger");
        auto *lblNextSong = dlgCdg.findChild<QLabel *>("lblNextSong");
        QVERIFY(lblNextSinger && lblNextSinger->font().pointSize() >= 24);
        QVERIFY(lblNextSong && lblNextSong->font().pointSize() >= 24);
    }

    void testUpdateCheckerManifestAndHash()
    {
        QVERIFY(UpdateChecker::isNewerVersion("2.1.40", "2.1.39"));
        QVERIFY(UpdateChecker::isNewerVersion("v2.2.0", "2.1.39"));
        QVERIFY(!UpdateChecker::isNewerVersion("2.1.39", "2.1.39"));
        QVERIFY(!UpdateChecker::isNewerVersion("2.1.38", "2.1.39"));

        // 1. GitHub Releases API JSON parsing
        const QByteArray ghJson =
            "{"
            "\"tag_name\": \"v2.1.40\","
            "\"body\": \"Bug fixes and improvements\","
            "\"html_url\": \"https://github.com/Riv33R/OpenKJ/releases/tag/v2.1.40\","
            "\"assets\": ["
            "{\"name\": \"OpenKJ-2.1.40-x64-setup.exe\", \"browser_download_url\": \"https://example.com/OpenKJ-2.1.40-x64-setup.exe\"},"
            "{\"name\": \"OpenKJ-2.1.40-x64-portable.zip\", \"browser_download_url\": \"https://example.com/OpenKJ-2.1.40-x64-portable.zip\"},"
            "{\"name\": \"OpenKJ-2.1.40-x86-setup.exe\", \"browser_download_url\": \"https://example.com/OpenKJ-2.1.40-x86-setup.exe\"},"
            "{\"name\": \"OpenKJ-2.1.40-x86-portable.zip\", \"browser_download_url\": \"https://example.com/OpenKJ-2.1.40-x86-portable.zip\"}"
            "]"
            "}";
        UpdateInfo gh64 = UpdateChecker::parseUpdatePayload(ghJson, true);
        QCOMPARE(gh64.version, QString("2.1.40"));
        QCOMPARE(gh64.installerUrl, QString("https://example.com/OpenKJ-2.1.40-x64-setup.exe"));
        QCOMPARE(gh64.portableUrl, QString("https://example.com/OpenKJ-2.1.40-x64-portable.zip"));

        UpdateInfo gh86 = UpdateChecker::parseUpdatePayload(ghJson, false);
        QCOMPARE(gh86.installerUrl, QString("https://example.com/OpenKJ-2.1.40-x86-setup.exe"));
        QCOMPARE(gh86.portableUrl, QString("https://example.com/OpenKJ-2.1.40-x86-portable.zip"));

        // 2. SHA-256 verification
        const QString testBin = m_tempDir.filePath("update_payload.bin");
        {
            QFile f(testBin);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("OpenKJ-Update-Test");
        }
        const QByteArray expectedSha = QCryptographicHash::hash("OpenKJ-Update-Test", QCryptographicHash::Sha256).toHex();
        QVERIFY(UpdateChecker::verifyFileSha256(testBin, QString::fromLatin1(expectedSha)));
        QVERIFY(!UpdateChecker::verifyFileSha256(testBin, "0000000000000000000000000000000000000000000000000000000000000000"));
    }
};

QTEST_MAIN(OpenKJTests)
#include "openkj_tests.moc"
