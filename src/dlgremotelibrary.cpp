#include "dlgremotelibrary.h"
#include "ui_dlgremotelibrary.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QSqlQuery>
#include <QSqlError>
#include "models/tablemodelkaraokesourcedirs.h"

DlgRemoteLibrary::DlgRemoteLibrary(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::DlgRemoteLibrary)
{
    ui->setupUi(this);
    ui->tableWidgetTracks->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    ui->tableWidgetTracks->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidgetTracks->setColumnWidth(0, 90);
    ui->tableWidgetTracks->setColumnWidth(1, 210);
    ui->tableWidgetTracks->setColumnWidth(2, 260);
    ui->tableWidgetTracks->setColumnWidth(3, 95);
    ui->tableWidgetTracks->setColumnWidth(4, 90);

    connect(ui->btnRefresh, &QPushButton::clicked, this, &DlgRemoteLibrary::onRefreshClicked);
    connect(ui->btnDownload, &QPushButton::clicked, this, &DlgRemoteLibrary::onDownloadSelectedClicked);
    connect(ui->btnCancel, &QPushButton::clicked, this, &DlgRemoteLibrary::onCancelClicked);
    connect(ui->btnClose, &QPushButton::clicked, this, &DlgRemoteLibrary::close);
    connect(ui->lineEditSearch, &QLineEdit::textChanged, this, &DlgRemoteLibrary::onFilterChanged);
    connect(ui->comboBoxType, qOverload<int>(&QComboBox::currentIndexChanged), this, &DlgRemoteLibrary::onFilterChanged);
    connect(ui->checkBoxOnlyMissing, &QCheckBox::toggled, this, &DlgRemoteLibrary::onFilterChanged);

    connect(&m_client, &RemoteLibraryClient::catalogFetched, this, &DlgRemoteLibrary::onCatalogFetched);
    connect(&m_client, &RemoteLibraryClient::downloadProgress, this, &DlgRemoteLibrary::onDownloadProgress);
    connect(&m_client, &RemoteLibraryClient::downloadFinished, this, &DlgRemoteLibrary::onDownloadFinished);

    m_allTracks = RemoteLibraryClient::loadCatalogCache();
    rebuildTable();
    if (!m_allTracks.isEmpty()) {
        ui->labelStatus->setText(tr("Загружен локальный кэш каталога (%1 треков)").arg(m_allTracks.size()));
    } else if (m_settings.remoteLibraryUrl().trimmed().isEmpty()) {
        ui->labelStatus->setText(tr("Укажите URL и Bearer-токен в Настройки → Сеть"));
    }
}

DlgRemoteLibrary::~DlgRemoteLibrary()
{
    m_client.cancelDownload();
    delete ui;
}

QString DlgRemoteLibrary::localPathForTrack(const RemoteTrack &track)
{
    QString baseDir = m_settings.remoteLibraryLocalDir();
    QString subFolder = (track.type == "break") ? "Break" : "Karaoke";
    QString rel = track.relPath.isEmpty() ? track.filename : track.relPath;
    rel.replace('\\', '/');
    return QDir::cleanPath(QDir(baseDir).filePath(subFolder + "/" + rel));
}

bool DlgRemoteLibrary::isTrackDownloaded(const RemoteTrack &track)
{
    QFileInfo fi(localPathForTrack(track));
    return fi.exists() && fi.size() > 0;
}

QString DlgRemoteLibrary::formatSize(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("-");
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1.0)
        return QString::number(mb, 'f', 1) + " МБ";
    return QString::number(bytes / 1024) + " КБ";
}

void DlgRemoteLibrary::rebuildTable()
{
    const QString query = ui->lineEditSearch->text().trimmed();
    const int typeIdx = ui->comboBoxType->currentIndex();
    const bool onlyMissing = ui->checkBoxOnlyMissing->isChecked();

    ui->tableWidgetTracks->setSortingEnabled(false);
    ui->tableWidgetTracks->setRowCount(0);

    for (int i = 0; i < m_allTracks.size(); ++i) {
        const RemoteTrack &t = m_allTracks.at(i);
        if (typeIdx == 1 && t.type != "karaoke")
            continue;
        if (typeIdx == 2 && t.type != "break")
            continue;
        const bool downloaded = isTrackDownloaded(t);
        if (onlyMissing && downloaded)
            continue;
        if (!query.isEmpty()) {
            const bool match = t.artist.contains(query, Qt::CaseInsensitive)
                            || t.title.contains(query, Qt::CaseInsensitive)
                            || t.songId.contains(query, Qt::CaseInsensitive)
                            || t.filename.contains(query, Qt::CaseInsensitive);
            if (!match)
                continue;
        }

        const int row = ui->tableWidgetTracks->rowCount();
        ui->tableWidgetTracks->insertRow(row);

        auto *typeItem = new QTableWidgetItem(t.type == "break" ? tr("Фон") : tr("Караоке"));
        typeItem->setData(Qt::UserRole, i);
        auto *artistItem = new QTableWidgetItem(t.artist);
        auto *titleItem = new QTableWidgetItem(t.title);
        auto *idItem = new QTableWidgetItem(t.songId);
        auto *sizeItem = new QTableWidgetItem(formatSize(t.size));
        auto *statusItem = new QTableWidgetItem(downloaded ? tr("Скачано") : tr("На сервере"));
        if (downloaded)
            statusItem->setForeground(QColor(22, 163, 74));

        ui->tableWidgetTracks->setItem(row, 0, typeItem);
        ui->tableWidgetTracks->setItem(row, 1, artistItem);
        ui->tableWidgetTracks->setItem(row, 2, titleItem);
        ui->tableWidgetTracks->setItem(row, 3, idItem);
        ui->tableWidgetTracks->setItem(row, 4, sizeItem);
        ui->tableWidgetTracks->setItem(row, 5, statusItem);
    }

    ui->tableWidgetTracks->setSortingEnabled(true);
}

void DlgRemoteLibrary::onFilterChanged()
{
    rebuildTable();
}

void DlgRemoteLibrary::onRefreshClicked()
{
    const QString url = m_settings.remoteLibraryUrl().trimmed();
    const QString token = m_settings.remoteLibraryToken().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, tr("Удалённая библиотека"),
                             tr("Сначала укажите URL сервера и Bearer-токен в Настройки → Сеть."));
        return;
    }
    setBusyUi(true);
    ui->labelStatus->setText(tr("Загрузка каталога с сервера..."));
    m_client.fetchCatalog(url, token);
}

void DlgRemoteLibrary::onCatalogFetched(bool ok, const QList<RemoteTrack> &tracks, const QString &error)
{
    setBusyUi(false);
    if (!ok) {
        ui->labelStatus->setText(tr("Ошибка: %1").arg(error));
        QMessageBox::warning(this, tr("Удалённая библиотека"), error);
        return;
    }
    m_allTracks = tracks;
    rebuildTable();
    ui->labelStatus->setText(tr("Каталог обновлён: %1 треков").arg(m_allTracks.size()));
}

void DlgRemoteLibrary::onDownloadSelectedClicked()
{
    const auto selectedRows = ui->tableWidgetTracks->selectionModel()->selectedRows();
    if (selectedRows.isEmpty()) {
        QMessageBox::information(this, tr("Удалённая библиотека"),
                                 tr("Выберите один или несколько треков в таблице."));
        return;
    }

    m_downloadQueue.clear();
    for (const QModelIndex &idx : selectedRows) {
        auto *item = ui->tableWidgetTracks->item(idx.row(), 0);
        if (!item)
            continue;
        const int trackIdx = item->data(Qt::UserRole).toInt();
        if (trackIdx >= 0 && trackIdx < m_allTracks.size()) {
            m_downloadQueue.append(m_allTracks.at(trackIdx));
        }
    }
    if (m_downloadQueue.isEmpty())
        return;

    m_queueTotalCount = m_downloadQueue.size();
    m_queueDoneCount = 0;
    m_karaokeDirty = false;
    m_breakDirty = false;
    setBusyUi(true);
    ui->progressBar->setVisible(true);
    ui->progressBar->setValue(0);
    startNextDownload();
}

void DlgRemoteLibrary::startNextDownload()
{
    if (m_downloadQueue.isEmpty()) {
        setBusyUi(false);
        ui->progressBar->setVisible(false);
        ui->labelStatus->setText(tr("Загрузка завершена (%1 из %2)").arg(m_queueDoneCount).arg(m_queueTotalCount));
        rebuildTable();
        if (m_karaokeDirty)
            emit karaokeLibraryUpdated();
        if (m_breakDirty)
            emit breakLibraryUpdated();
        return;
    }

    RemoteTrack next = m_downloadQueue.takeFirst();
    const QString destPath = localPathForTrack(next);
    ui->labelStatus->setText(tr("Скачивание [%1/%2]: %3")
                                 .arg(m_queueDoneCount + 1)
                                 .arg(m_queueTotalCount)
                                 .arg(next.filename));
    ui->progressBar->setValue(0);
    m_client.downloadTrack(next, m_settings.remoteLibraryUrl(), m_settings.remoteLibraryToken(), destPath);
}

void DlgRemoteLibrary::onDownloadProgress(qint64 received, qint64 total)
{
    if (total > 0) {
        ui->progressBar->setRange(0, 100);
        ui->progressBar->setValue(static_cast<int>((received * 100) / total));
    } else {
        ui->progressBar->setRange(0, 0);
    }
}

void DlgRemoteLibrary::onDownloadFinished(bool ok, const RemoteTrack &track, const QString &destPath, const QString &error)
{
    if (!ok) {
        m_downloadQueue.clear();
        setBusyUi(false);
        ui->progressBar->setVisible(false);
        ui->labelStatus->setText(tr("Остановлено: %1").arg(error));
        if (m_karaokeDirty)
            emit karaokeLibraryUpdated();
        if (m_breakDirty)
            emit breakLibraryUpdated();
        if (error != tr("Загрузка отменена"))
            QMessageBox::warning(this, tr("Удалённая библиотека"), error);
        return;
    }

    registerDownloadedTrackInDb(track, destPath);
    m_queueDoneCount++;
    startNextDownload();
}

void DlgRemoteLibrary::onCancelClicked()
{
    m_downloadQueue.clear();
    m_client.cancelDownload();
}

void DlgRemoteLibrary::registerDownloadedTrackInDb(const RemoteTrack &track, const QString &localPath)
{
    const QString artist = track.artist.isEmpty() ? tr("Unknown") : track.artist;
    const QString title = track.title.isEmpty() ? QFileInfo(localPath).completeBaseName() : track.title;
    const QString searchStr = (track.songId + " " + artist + " " + title + " " + track.filename).trimmed();

    if (track.type == "break") {
        QSqlQuery q;
        q.prepare("INSERT OR IGNORE INTO bmsongs (Artist, Title, path, Filename, Duration, searchstring) "
                  "VALUES (:artist, :title, :path, :filename, '', :search)");
        q.bindValue(":artist", artist);
        q.bindValue(":title", title);
        q.bindValue(":path", localPath);
        q.bindValue(":filename", track.filename);
        q.bindValue(":search", searchStr);
        if (q.exec())
            m_breakDirty = true;
    } else {
        const QString karRoot = QDir::cleanPath(QDir(m_settings.remoteLibraryLocalDir()).filePath("Karaoke"));
        TableModelKaraokeSourceDirs srcDirs;
        srcDirs.loadFromDB();
        bool hasRoot = false;
        for (const QString &sd : srcDirs.getSourceDirs()) {
            if (QDir::cleanPath(sd).compare(karRoot, Qt::CaseInsensitive) == 0) {
                hasRoot = true;
                break;
            }
        }
        if (!hasRoot) {
            srcDirs.addSourceDir(karRoot, SourceDir::AT, 0);
        }

        QSqlQuery q;
        q.prepare("INSERT OR IGNORE INTO dbsongs (discid, artist, title, path, filename, duration, searchstring) "
                  "VALUES (:discid, :artist, :title, :path, :filename, 0, :search)");
        q.bindValue(":discid", track.songId);
        q.bindValue(":artist", artist);
        q.bindValue(":title", title);
        q.bindValue(":path", localPath);
        q.bindValue(":filename", track.filename);
        q.bindValue(":search", searchStr);
        if (q.exec())
            m_karaokeDirty = true;
    }
}

void DlgRemoteLibrary::setBusyUi(bool busy)
{
    ui->btnRefresh->setEnabled(!busy);
    ui->btnDownload->setEnabled(!busy);
    ui->btnCancel->setEnabled(busy);
}
