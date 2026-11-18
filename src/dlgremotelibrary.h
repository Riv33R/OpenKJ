#ifndef DLGREMOTELIBRARY_H
#define DLGREMOTELIBRARY_H

#include <QDialog>
#include <QList>
#include "remotelibraryclient.h"
#include "settings.h"

namespace Ui {
class DlgRemoteLibrary;
}

class DlgRemoteLibrary : public QDialog
{
    Q_OBJECT

public:
    explicit DlgRemoteLibrary(QWidget *parent = nullptr);
    ~DlgRemoteLibrary() override;

signals:
    void karaokeLibraryUpdated();
    void breakLibraryUpdated();

private slots:
    void onRefreshClicked();
    void onDownloadSelectedClicked();
    void onCancelClicked();
    void onFilterChanged();
    void onCatalogFetched(bool ok, const QList<RemoteTrack> &tracks, const QString &error);
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(bool ok, const RemoteTrack &track, const QString &destPath, const QString &error);

private:
    Ui::DlgRemoteLibrary *ui;
    Settings m_settings;
    RemoteLibraryClient m_client;
    QList<RemoteTrack> m_allTracks;
    QList<RemoteTrack> m_downloadQueue;
    int m_queueTotalCount{0};
    int m_queueDoneCount{0};
    bool m_karaokeDirty{false};
    bool m_breakDirty{false};

    void rebuildTable();
    QString localPathForTrack(const RemoteTrack &track);
    bool isTrackDownloaded(const RemoteTrack &track);
    void startNextDownload();
    void registerDownloadedTrackInDb(const RemoteTrack &track, const QString &localPath);
    void setBusyUi(bool busy);
    static QString formatSize(qint64 bytes);
};

#endif // DLGREMOTELIBRARY_H
