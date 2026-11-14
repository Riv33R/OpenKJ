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
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>
#include <spdlog/spdlog.h>

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
    auto *file = new QFile(destPath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(m_parentWidget, tr("Ошибка обновления"),
                             tr("Не удалось создать файл для записи:\n%1").arg(destPath));
        delete file;
        return;
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString("OpenKJ-Updater/%1").arg(OKJ_VERSION_STRING));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_manager->get(req);
    auto *progress = new QProgressDialog(progressTitle, tr("Отмена"), 0, 100, m_parentWidget);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);

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
            progress->setMaximum(0); // indeterminate
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
}
