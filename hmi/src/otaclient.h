// OtaClient: QML-facing proxy for the OtaUpdate SOME/IP service (ota-agent).
// The HMI never touches update files itself: it only shows status, collects
// driver consent and reports whether a newly activated bundle is healthy.
#pragma once

#include <QObject>
#include <QString>

class SomeIpClient;

class OtaClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(int state READ state NOTIFY statusChanged)
    Q_PROPERTY(QString stateName READ stateName NOTIFY statusChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY statusChanged)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY statusChanged)
    Q_PROPERTY(int progress READ progress NOTIFY statusChanged)
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY statusChanged)
    Q_PROPERTY(QString activeSlot READ activeSlot NOTIFY statusChanged)
public:
    enum State {
        Idle = 0, Checking, UpdateAvailable, Downloading, Verifying,
        Installing, Activating, Success, RolledBack, Failed
    };
    Q_ENUM(State)

    explicit OtaClient(SomeIpClient *client, QObject *parent = nullptr);

    bool connected() const;
    int state() const { return state_; }
    QString stateName() const;
    QString installedVersion() const { return installed_; }
    QString availableVersion() const { return available_; }
    int progress() const { return progress_; }
    QString message() const { return message_; }
    QString releaseNotes() const { return notes_; }
    QString activeSlot() const { return slot_; }

    Q_INVOKABLE void approveInstall();
    Q_INVOKABLE void defer();
    Q_INVOKABLE void checkNow();
    void confirmHealthy(const QString &version);
    void reportBootFailure(const QString &version, const QString &reason);

signals:
    void connectedChanged();
    void statusChanged();

private:
    void onEvent(quint16 id, const QByteArray &payload);
    SomeIpClient *client_;
    int state_ = Idle;
    QString installed_, available_, message_, notes_, slot_;
    int progress_ = 0;
};
