// HmiShell: stable native host that loads the updatable QML bundle.
//
// The C++ shell (SOME/IP, view models) is part of the base image; the QML UI
// is an OTA-updatable bundle living in an A/B slot. On activation the shell
// loads the new bundle in a *fresh* QQmlApplicationEngine and only tears down
// the old UI once the new one has loaded -- a broken bundle never blanks the
// cluster. After the first rendered frame plus a stability window it reports
// "healthy" to the OTA agent; load errors are reported as boot failures so the
// agent can roll back.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

#include <vector>

class QQmlApplicationEngine;
class QQuickWindow;
class VehicleState;
class OtaClient;

class HmiShell : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString bundleVersion READ bundleVersion NOTIFY bundleChanged)
    Q_PROPERTY(QString bundleSlot READ bundleSlot NOTIFY bundleChanged)
    Q_PROPERTY(QString previousVersion READ previousVersion NOTIFY bundleChanged)
    Q_PROPERTY(QString shellVersion READ shellVersion CONSTANT)
    Q_PROPERTY(int reloadCount READ reloadCount NOTIFY bundleChanged)
public:
    HmiShell(QString bundleRoot, VehicleState *vehicle, OtaClient *ota, QObject *parent = nullptr);
    ~HmiShell() override;

    bool loadActiveBundle();
    void setSnapshots(const QString &dir, int intervalMs);
    void setHealthWindowMs(int ms) { healthWindowMs_ = ms; }
    void setCockpit(QObject *cockpit) { cockpit_ = cockpit; }

    QString bundleVersion() const { return version_; }
    QString bundleSlot() const { return slot_; }
    QString previousVersion() const { return previousVersion_; }
    QString shellVersion() const;
    int reloadCount() const { return reloadCount_; }

    Q_INVOKABLE bool saveSnapshot(const QString &path);

signals:
    void bundleChanged();
    void uiRendering();  // first frame of the current bundle presented

private:
    struct BundleInfo {
        QString dir, version, slot;
    };
    bool resolveActive(BundleInfo &out, QString &error) const;
    void onOtaStatus();
    void armHealthCheck();
    void takePeriodicSnapshot();
    void recordFrame();
    void reportFrameStats();

    QString root_;
    VehicleState *vehicle_;
    OtaClient *ota_;
    QQmlApplicationEngine *engine_ = nullptr;
    QPointer<QQuickWindow> window_;
    QString version_, slot_, previousVersion_, lastError_;
    QSet<QString> failedVersions_;
    bool healthy_ = false;
    int reloadCount_ = 0;
    int healthWindowMs_ = 3000;
    QTimer healthTimer_;
    QTimer snapTimer_;
    QString snapDir_;
    QObject *cockpit_ = nullptr;
    QTimer kpiTimer_;
    QElapsedTimer frameClock_;
    std::vector<double> intervalsMs_;
    QTimer latencyProbe_;
    QElapsedTimer probeClock_;
    std::vector<double> latencyMs_;
    int snapIndex_ = 0;
};
