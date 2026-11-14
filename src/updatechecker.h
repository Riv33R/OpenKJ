#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QWidget>
#include "okjversion.h"
#include "settings.h"

struct UpdateInfo
{
    QString version;
    QString notes;
    QString installerUrl;
    QString installerSha256;
    QString portableUrl;
    QString portableSha256;
    QString htmlUrl;

    [[nodiscard]] bool isValid() const { return !version.isEmpty(); }
};

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QWidget *parentWidget = nullptr);

    void checkForUpdates(bool interactive = false);

    static UpdateInfo parseUpdatePayload(const QByteArray &data, bool is64Bit);
    static bool isNewerVersion(const QString &available, const QString &current);
    static bool verifyFileSha256(const QString &filePath, const QString &expectedHex);
    static bool isInstalledBuild();

signals:
    void newVersionAvailable(QString version);
    void checkFinished(bool updateFound, QString errorMessage);

private:
    QNetworkAccessManager *m_manager{nullptr};
    QWidget *m_parentWidget{nullptr};
    Settings m_settings;
    bool m_checkInProgress{false};

    void handleManifestReply(QNetworkReply *reply, bool interactive);
    void promptAndUpdate(const UpdateInfo &info);
    void downloadFile(const QString &url,
                      const QString &destPath,
                      const QString &expectedSha256,
                      const QString &progressTitle,
                      bool runSilentInstaller);
};

#endif // UPDATECHECKER_H
