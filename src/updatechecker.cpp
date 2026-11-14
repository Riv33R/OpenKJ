#include "updatechecker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <memory>
#include <spdlog/spdlog.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#endif

namespace {

bool is64BitBuild()
{
#ifdef Q_OS_WIN64
    return true;
#else
    return false;
#endif
}

QString normalizeVersion(QString v)
{
    v = v.trimmed();
    if (v.startsWith('v', Qt::CaseInsensitive))
        v.remove(0, 1);
    return v;
}

#ifdef Q_OS_WIN
struct WinHttpHandle
{
    HINTERNET h{nullptr};
    explicit WinHttpHandle(HINTERNET handle = nullptr) : h(handle) {}
    ~WinHttpHandle()
    {
        if (h)
            WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle &) = delete;
    WinHttpHandle &operator=(const WinHttpHandle &) = delete;
    operator HINTERNET() const { return h; }
};

QString winHttpErrorString(DWORD err)
{
    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
            GetModuleHandleW(L"winhttp.dll"),
            err,
            0,
            reinterpret_cast<LPWSTR>(&buf),
            0,
            nullptr);
    QString msg = (len > 0 && buf) ? QString::fromWCharArray(buf, static_cast<int>(len)).trimmed()
                                   : QString("WinHTTP error %1").arg(err);
    if (buf)
        LocalFree(buf);
    return msg;
}

bool winHttpOpenGetRequest(const QString &urlStr,
                           WinHttpHandle &hSession,
                           WinHttpHandle &hConnect,
                           WinHttpHandle &hRequest,
                           DWORD &statusCode,
                           qint64 &contentLength,
                           QString &errorOut)
{
    const std::wstring wUrl = urlStr.toStdWString();
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[512]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = _countof(hostBuf);
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = _countof(pathBuf);

    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp)) {
        errorOut = QString("Некорректный URL: %1").arg(urlStr);
        return false;
    }

    const std::wstring ua = QString("OpenKJ-Updater/%1").arg(OKJ_VERSION_STRING).toStdWString();
    hSession.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession.h) {
        hSession.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!hSession.h) {
        errorOut = winHttpErrorString(GetLastError());
        return false;
    }

    WinHttpSetTimeouts(hSession.h, 10000, 10000, 15000, 30000);

    hConnect.h = WinHttpConnect(hSession.h, hostBuf, urlComp.nPort, 0);
    if (!hConnect.h) {
        errorOut = winHttpErrorString(GetLastError());
        return false;
    }

    const DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    const wchar_t *pathPtr = (pathBuf[0] != L'\0') ? pathBuf : L"/";
    hRequest.h = WinHttpOpenRequest(hConnect.h, L"GET", pathPtr, nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest.h) {
        errorOut = winHttpErrorString(GetLastError());
        return false;
    }

    WinHttpAddRequestHeaders(hRequest.h, L"Accept: application/vnd.github+json\r\n", -1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest.h, nullptr)) {
        errorOut = winHttpErrorString(GetLastError());
        return false;
    }

    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        errorOut = winHttpErrorString(GetLastError());
        return false;
    }

    if (statusCode < 200 || statusCode >= 300) {
        errorOut = QString("HTTP %1").arg(statusCode);
        return false;
    }

    wchar_t lenBuf[64]{};
    DWORD lenBufSize = sizeof(lenBuf);
    contentLength = -1;
    if (WinHttpQueryHeaders(hRequest.h, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenBufSize, WINHTTP_NO_HEADER_INDEX)) {
        bool ok = false;
        qint64 parsed = QString::fromWCharArray(lenBuf).toLongLong(&ok);
        if (ok && parsed > 0)
            contentLength = parsed;
    }
    return true;
}

bool winHttpGetBytes(const QString &urlStr, QByteArray &outBytes, QString &errorOut)
{
    WinHttpHandle hSession, hConnect, hRequest;
    DWORD statusCode = 0;
    qint64 contentLength = -1;
    if (!winHttpOpenGetRequest(urlStr, hSession, hConnect, hRequest, statusCode, contentLength, errorOut))
        return false;

    QByteArray buffer;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest.h, &avail) && avail > 0) {
        QByteArray chunk(static_cast<int>(avail), Qt::Uninitialized);
        DWORD readBytes = 0;
        if (!WinHttpReadData(hRequest.h, chunk.data(), avail, &readBytes)) {
            errorOut = winHttpErrorString(GetLastError());
            return false;
        }
        buffer.append(chunk.constData(), static_cast<int>(readBytes));
    }
    outBytes = buffer;
    return true;
}

template <typename ProgressFn>
bool winHttpDownloadFile(const QString &urlStr,
                         const QString &destPath,
                         const std::shared_ptr<std::atomic_bool> &cancelled,
                         ProgressFn onProgress,
                         QString &errorOut)
{
    WinHttpHandle hSession, hConnect, hRequest;
    DWORD statusCode = 0;
    qint64 contentLength = -1;
    if (!winHttpOpenGetRequest(urlStr, hSession, hConnect, hRequest, statusCode, contentLength, errorOut))
        return false;

    QFile file(destPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorOut = QString("Не удалось создать файл:\n%1").arg(destPath);
        return false;
    }

    qint64 received = 0;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest.h, &avail) && avail > 0) {
        if (cancelled && cancelled->load()) {
            file.close();
            QFile::remove(destPath);
            errorOut = "CANCELLED";
            return false;
        }
        QByteArray chunk(static_cast<int>(avail), Qt::Uninitialized);
        DWORD readBytes = 0;
        if (!WinHttpReadData(hRequest.h, chunk.data(), avail, &readBytes)) {
            file.close();
            QFile::remove(destPath);
            errorOut = winHttpErrorString(GetLastError());
            return false;
        }
        if (readBytes > 0) {
            file.write(chunk.constData(), static_cast<qint64>(readBytes));
            received += static_cast<qint64>(readBytes);
            onProgress(received, contentLength);
        }
    }
    file.flush();
    file.close();
    return true;
}
#endif

} // namespace

UpdateChecker::UpdateChecker(QWidget *parentWidget)
    : QObject(parentWidget),
      m_manager(new QNetworkAccessManager(this)),
      m_parentWidget(parentWidget)
{
}

bool UpdateChecker::isInstalledBuild()
{
    return QFile::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
}

bool UpdateChecker::isNewerVersion(const QString &available, const QString &current)
{
    const QVersionNumber availVer = QVersionNumber::fromString(normalizeVersion(available));
    const QVersionNumber curVer = QVersionNumber::fromString(normalizeVersion(current));
    if (availVer.isNull() || curVer.isNull())
        return false;
    return QVersionNumber::compare(availVer, curVer) > 0;
}

bool UpdateChecker::verifyFileSha256(const QString &filePath, const QString &expectedHex)
{
    if (expectedHex.trimmed().isEmpty())
        return true;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return false;
    const QString actual = QString::fromLatin1(hash.result().toHex()).toLower();
    return actual == expectedHex.trimmed().toLower();
}

UpdateInfo UpdateChecker::parseUpdatePayload(const QByteArray &data, bool is64Bit)
{
    UpdateInfo info;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return info;
    const QJsonObject root = doc.object();

    // GitHub Releases API format
    if (root.contains("tag_name")) {
        info.version = normalizeVersion(root.value("tag_name").toString());
        info.notes = root.value("body").toString().trimmed();
        info.htmlUrl = root.value("html_url").toString();

        const QJsonArray assets = root.value("assets").toArray();
        for (const QJsonValue &val : assets) {
            const QJsonObject asset = val.toObject();
            const QString name = asset.value("name").toString().toLower();
            const QString dlUrl = asset.value("browser_download_url").toString();
            if (dlUrl.isEmpty())
                continue;

            if (is64Bit) {
                if (name.endsWith("x64-setup.exe") || name.endsWith("64bit-setup.exe"))
                    info.installerUrl = dlUrl;
                else if (name.endsWith("x64-portable.zip"))
                    info.portableUrl = dlUrl;
            } else {
                if (name.endsWith("x86-setup.exe") || name.endsWith("32bit-setup.exe"))
                    info.installerUrl = dlUrl;
                else if (name.endsWith("x86-portable.zip"))
                    info.portableUrl = dlUrl;
            }
        }
        return info;
    }

    // Custom update.json manifest format
    if (root.contains("version")) {
        info.version = normalizeVersion(root.value("version").toString());
        info.notes = root.value("notes").toString().trimmed();
        info.htmlUrl = root.value("html_url").toString();
        const QString archKey = is64Bit ? "win64" : "win32";
        const QJsonObject archObj = root.value(archKey).toObject();
        info.installerUrl = archObj.value("url").toString();
        info.installerSha256 = archObj.value("sha256").toString();
        info.portableUrl = archObj.value("portableUrl").toString();
        info.portableSha256 = archObj.value("portableSha256").toString();
    }
    return info;
}

void UpdateChecker::checkForUpdates(bool interactive)
{
    if (!interactive && !m_settings.checkUpdates())
        return;
    if (m_checkInProgress)
        return;

    const QString manifestUrl = m_settings.updateManifestUrl().trimmed();
    if (manifestUrl.isEmpty()) {
        if (interactive) {
            QMessageBox::warning(m_parentWidget, tr("Обновления"),
                                 tr("Не задан URL источника обновлений."));
        }
        emit checkFinished(false, tr("Пустой URL источника обновлений"));
        return;
    }

    m_checkInProgress = true;

#ifdef Q_OS_WIN
    QPointer<UpdateChecker> self(this);
    QtConcurrent::run([self, manifestUrl, interactive]() {
        QByteArray payload;
        QString err;
        const bool ok = winHttpGetBytes(manifestUrl, payload, err);
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, ok, payload, err, interactive]() {
            if (!self)
                return;
            self->m_checkInProgress = false;
            if (!ok) {
                if (auto log = spdlog::get("logger"))
                    log->warn("[Updater] Update check failed: {}", err.toStdString());
                if (interactive) {
                    QMessageBox::warning(self->m_parentWidget, tr("Проверка обновлений"),
                                         tr("Не удалось проверить обновления:\n%1").arg(err));
                }
                emit self->checkFinished(false, err);
                return;
            }
            const UpdateInfo info = parseUpdatePayload(payload, is64BitBuild());
            if (!info.isValid()) {
                if (interactive) {
                    QMessageBox::warning(self->m_parentWidget, tr("Проверка обновлений"),
                                         tr("Сервер вернул некорректную информацию о версии."));
                }
                emit self->checkFinished(false, tr("Некорректный формат ответа сервера"));
                return;
            }
            if (!isNewerVersion(info.version, OKJ_VERSION_STRING)) {
                if (interactive) {
                    QMessageBox::information(self->m_parentWidget, tr("Проверка обновлений"),
                                             tr("У вас установлена актуальная версия OpenKJ (%1).").arg(OKJ_VERSION_STRING));
                }
                emit self->checkFinished(false, QString());
                return;
            }
            emit self->newVersionAvailable(info.version);
            emit self->checkFinished(true, QString());
            self->promptAndUpdate(info);
        }, Qt::QueuedConnection);
    });
#else
    QNetworkRequest req{QUrl(manifestUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString("OpenKJ-Updater/%1").arg(OKJ_VERSION_STRING));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_manager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, interactive]() {
        m_checkInProgress = false;
        handleManifestReply(reply, interactive);
        reply->deleteLater();
    });
#endif
}

void UpdateChecker::handleManifestReply(QNetworkReply *reply, bool interactive)
{
    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        if (auto log = spdlog::get("logger"))
            log->warn("[Updater] Update check failed: {}", err.toStdString());
        if (interactive) {
            QMessageBox::warning(m_parentWidget, tr("Проверка обновлений"),
                                 tr("Не удалось проверить обновления:\n%1").arg(err));
        }
        emit checkFinished(false, err);
        return;
    }

    const UpdateInfo info = parseUpdatePayload(reply->readAll(), is64BitBuild());
    if (!info.isValid()) {
        if (interactive) {
            QMessageBox::warning(m_parentWidget, tr("Проверка обновлений"),
                                 tr("Сервер вернул некорректную информацию о версии."));
        }
        emit checkFinished(false, tr("Некорректный формат ответа сервера"));
        return;
    }

    if (!isNewerVersion(info.version, OKJ_VERSION_STRING)) {
        if (interactive) {
            QMessageBox::information(m_parentWidget, tr("Проверка обновлений"),
                                     tr("У вас установлена актуальная версия OpenKJ (%1).").arg(OKJ_VERSION_STRING));
        }
        emit checkFinished(false, QString());
        return;
    }

    emit newVersionAvailable(info.version);
    emit checkFinished(true, QString());
    promptAndUpdate(info);
}

void UpdateChecker::promptAndUpdate(const UpdateInfo &info)
{
    const bool installed = isInstalledBuild();
    QString msg = tr("Доступна новая версия OpenKJ: <b>%1</b> (текущая: %2).")
            .arg(info.version.toHtmlEscaped(), QString(OKJ_VERSION_STRING).toHtmlEscaped());
    if (!info.notes.isEmpty()) {
        QString shortNotes = info.notes.left(600);
        if (info.notes.size() > 600)
            shortNotes += "...";
        msg += "<br><br>" + tr("Что нового:") + "<br><pre>" + shortNotes.toHtmlEscaped() + "</pre>";
    }

    if (installed && !info.installerUrl.isEmpty()) {
        msg += "<br>" + tr("Скачать и установить обновление в тихом режиме (программа перезапустится автоматически)?");
        if (QMessageBox::question(m_parentWidget, tr("Доступно обновление"), msg,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) {
            return;
        }
        const QString destPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                .filePath(QString("OpenKJ-%1-setup.exe").arg(info.version));
        downloadFile(info.installerUrl, destPath, info.installerSha256,
                     tr("Загрузка обновления %1...").arg(info.version), true);
        return;
    }

    // Portable build (or fallback if no setup.exe asset)
    if (!info.portableUrl.isEmpty()) {
        msg += "<br>" + tr("Вы используете переносную (Portable) версию. Скачать архив новой версии?");
        if (QMessageBox::question(m_parentWidget, tr("Доступно обновление (Portable)"), msg,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) {
            return;
        }
        const QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString defaultFile = QDir(defaultDir).filePath(QString("OpenKJ-%1-portable.zip").arg(info.version));
        const QString savePath = QFileDialog::getSaveFileName(m_parentWidget,
                                                              tr("Сохранить обновление Portable"),
                                                              defaultFile,
                                                              tr("ZIP-архивы (*.zip)"));
        if (savePath.isEmpty())
            return;
        downloadFile(info.portableUrl, savePath, info.portableSha256,
                     tr("Загрузка архива %1...").arg(info.version), false);
        return;
    }

    if (!info.htmlUrl.isEmpty()) {
        msg += "<br>" + tr("Открыть страницу релиза для скачивания?");
        if (QMessageBox::question(m_parentWidget, tr("Доступно обновление"), msg,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes) {
            QDesktopServices::openUrl(QUrl(info.htmlUrl));
        }
    }
}

void UpdateChecker::downloadFile(const QString &url,
                                 const QString &destPath,
                                 const QString &expectedSha256,
                                 const QString &progressTitle,
                                 bool runSilentInstaller)
{
    auto *progress = new QProgressDialog(progressTitle, tr("Отмена"), 0, 100, m_parentWidget);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);

#ifdef Q_OS_WIN
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });

    QPointer<UpdateChecker> self(this);
    QPointer<QProgressDialog> progPtr(progress);
    QtConcurrent::run([self, progPtr, url, destPath, expectedSha256, runSilentInstaller, cancelled]() {
        QString err;
        const bool ok = winHttpDownloadFile(
                url,
                destPath,
                cancelled,
                [progPtr](qint64 received, qint64 total) {
                    if (!progPtr)
                        return;
                    QMetaObject::invokeMethod(progPtr.data(), [progPtr, received, total]() {
                        if (!progPtr)
                            return;
                        if (total > 0) {
                            progPtr->setMaximum(100);
                            progPtr->setValue(static_cast<int>((received * 100) / total));
                        } else {
                            progPtr->setMaximum(0);
                        }
                    }, Qt::QueuedConnection);
                },
                err);

        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, progPtr, ok, err, destPath, expectedSha256, runSilentInstaller]() {
            if (progPtr) {
                progPtr->close();
                progPtr->deleteLater();
            }
            if (!self)
                return;
            if (!ok) {
                if (err != "CANCELLED") {
                    QMessageBox::warning(self->m_parentWidget, tr("Ошибка загрузки"),
                                         tr("Не удалось скачать обновление:\n%1").arg(err));
                }
                return;
            }
            if (!verifyFileSha256(destPath, expectedSha256)) {
                QFile::remove(destPath);
                QMessageBox::critical(self->m_parentWidget, tr("Ошибка проверки"),
                                      tr("Контрольная сумма SHA-256 загруженного файла не совпадает. Установка отменена."));
                return;
            }
            if (runSilentInstaller) {
                const QStringList args{"/SILENT", "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS"};
                if (!QProcess::startDetached(destPath, args)) {
                    QMessageBox::warning(self->m_parentWidget, tr("Ошибка запуска"),
                                         tr("Не удалось запустить установщик:\n%1").arg(destPath));
                    return;
                }
                QApplication::quit();
            } else {
                QMessageBox::information(self->m_parentWidget, tr("Загрузка завершена"),
                                         tr("Архив с новой версией сохранён:\n%1\n\nРаспакуйте его с заменой файлов после закрытия программы.")
                                                 .arg(QDir::toNativeSeparators(destPath)));
                QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(destPath)});
            }
        }, Qt::QueuedConnection);
    });
#else
    auto *file = new QFile(destPath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        progress->close();
        progress->deleteLater();
        QMessageBox::warning(m_parentWidget, tr("Ошибка обновления"),
                             tr("Не удалось создать файл для записи:\n%1").arg(destPath));
        delete file;
        return;
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString("OpenKJ-Updater/%1").arg(OKJ_VERSION_STRING));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_manager->get(req);
    connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);

    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() {
        if (file->isOpen())
            file->write(reply->readAll());
    });

    connect(reply, &QNetworkReply::downloadProgress, this, [progress](qint64 received, qint64 total) {
        if (total > 0) {
            progress->setMaximum(100);
            progress->setValue(static_cast<int>((received * 100) / total));
        } else {
            progress->setMaximum(0);
        }
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, progress, destPath, expectedSha256, runSilentInstaller]() {
        if (file->isOpen()) {
            file->write(reply->readAll());
            file->flush();
            file->close();
        }
        const bool aborted = (reply->error() == QNetworkReply::OperationCanceledError);
        const bool hasErr = (reply->error() != QNetworkReply::NoError);
        const QString errStr = reply->errorString();
        reply->deleteLater();
        progress->close();
        progress->deleteLater();
        delete file;

        if (hasErr) {
            QFile::remove(destPath);
            if (!aborted) {
                QMessageBox::warning(m_parentWidget, tr("Ошибка загрузки"),
                                     tr("Не удалось скачать обновление:\n%1").arg(errStr));
            }
            return;
        }

        if (!verifyFileSha256(destPath, expectedSha256)) {
            QFile::remove(destPath);
            QMessageBox::critical(m_parentWidget, tr("Ошибка проверки"),
                                  tr("Контрольная сумма SHA-256 загруженного файла не совпадает. Установка отменена."));
            return;
        }

        if (runSilentInstaller) {
            const QStringList args{"/SILENT", "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS"};
            if (!QProcess::startDetached(destPath, args)) {
                QMessageBox::warning(m_parentWidget, tr("Ошибка запуска"),
                                     tr("Не удалось запустить установщик:\n%1").arg(destPath));
                return;
            }
            QApplication::quit();
        } else {
            QMessageBox::information(m_parentWidget, tr("Загрузка завершена"),
                                     tr("Архив с новой версией сохранён:\n%1\n\nРаспакуйте его с заменой файлов после закрытия программы.")
                                             .arg(QDir::toNativeSeparators(destPath)));
            QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(destPath)});
        }
    });
#endif
}
