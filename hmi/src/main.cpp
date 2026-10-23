// sdv-cluster-hmi: Qt 6 digital cluster for the SDV / OTA demo.
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QTimer>

#include "cockpitlink.h"
#include "hmishell.h"
#include "ivisurfaceitem.h"
#include "otaclient.h"
#include "someip/sd.h"
#include "someip/services.h"
#include "someipclient.h"
#include "vehiclestate.h"

static QString envOr(const char *name, const QString &def) {
    const QByteArray v = qgetenv(name);
    return v.isEmpty() ? def : QString::fromLocal8Bit(v);
}

int main(int argc, char *argv[]) {
    qSetMessagePattern(QStringLiteral("[hmi] %{time hh:mm:ss.zzz} %{if-category}%{category}: %{endif}%{message}"));
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("sdv-cluster-hmi"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SDV_SHELL_VERSION));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("SDV digital cluster with OTA-updatable QML bundle"));
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption bundleRoot("bundle-root", "A/B bundle root (contains slotA, slotB, current)", "dir",
                                  envOr("HMI_BUNDLE_ROOT", "/var/lib/sdv/hmi"));
    QCommandLineOption vehHost("vehicle-host", "VehicleSignals provider host", "host",
                               envOr("VEHICLE_HOST", "127.0.0.1"));
    QCommandLineOption vehSd("vehicle-sd-port", "provider SOME/IP-SD port", "port",
                             envOr("VEHICLE_SD_PORT", QString::number(someip::sd::kDefaultPort)));
    QCommandLineOption otaHost("ota-host", "OtaUpdate provider host", "host", envOr("OTA_AGENT_HOST", "127.0.0.1"));
    QCommandLineOption otaSd("ota-sd-port", "OTA agent SOME/IP-SD port", "port",
                             envOr("OTA_AGENT_SD_PORT", QString::number(sdv::ota::kSdPort)));
    QCommandLineOption snapDir("snap-dir", "save periodic screenshots into dir", "dir", envOr("HMI_SNAP_DIR", ""));
    QCommandLineOption snapMs("snap-interval", "screenshot interval ms", "ms", envOr("HMI_SNAP_INTERVAL", "2000"));
    QCommandLineOption healthMs("health-window", "stable-run time before confirming an update", "ms",
                                envOr("HMI_HEALTH_WINDOW_MS", "3000"));
    p.addOptions({bundleRoot, vehHost, vehSd, otaHost, otaSd, snapDir, snapMs, healthMs});
    p.process(app);

    ServiceConfig vcfg;
    vcfg.name = QStringLiteral("VehicleSignals");
    vcfg.host = p.value(vehHost);
    vcfg.sdPort = p.value(vehSd).toUShort();
    vcfg.servicePort = sdv::vehicle::kPort;
    vcfg.serviceId = sdv::vehicle::kServiceId;
    vcfg.instanceId = sdv::vehicle::kInstanceId;
    vcfg.majorVersion = sdv::vehicle::kMajor;
    vcfg.eventgroupId = sdv::vehicle::kEventgroup;

    ServiceConfig ocfg;
    ocfg.name = QStringLiteral("OtaUpdate");
    ocfg.host = p.value(otaHost);
    ocfg.sdPort = p.value(otaSd).toUShort();
    ocfg.servicePort = sdv::ota::kPort;
    ocfg.serviceId = sdv::ota::kServiceId;
    ocfg.instanceId = sdv::ota::kInstanceId;
    ocfg.majorVersion = sdv::ota::kMajor;
    ocfg.eventgroupId = sdv::ota::kEventgroup;
    ocfg.livenessTimeoutMs = 3500;  // status is published at 1 Hz

    SomeIpClient vehicleClient(vcfg, 0x0101);
    SomeIpClient otaClient(ocfg, 0x0101);
    VehicleState vehicle(&vehicleClient);
    OtaClient ota(&otaClient);

    // Enums usable from the dynamically loaded bundle: VehicleState.LeftIndicator, OtaClient.Activating
    qmlRegisterUncreatableType<VehicleState>("Sdv", 1, 0, "VehicleState", "provided by the shell");
    qmlRegisterUncreatableType<OtaClient>("Sdv", 1, 0, "OtaClient", "provided by the shell");
    qmlRegisterUncreatableType<HmiShell>("Sdv", 1, 0, "HmiShell", "provided by the shell");
    qmlRegisterUncreatableType<CockpitLink>("Sdv", 1, 0, "CockpitLink", "provided by the shell");
    qmlRegisterType<IviSurfaceItem>("Sdv", 1, 0, "IviSurface");

    // Cluster partition's end of the inter-VM channel (active under sdv-hypervisor)
    CockpitLink cockpit;
    QObject::connect(&vehicle, &VehicleState::speedChanged, &cockpit,
                     [&] { cockpit.setSpeed(vehicle.speed(), vehicle.gear()); });

    // Demo/CI policy: approve offered updates automatically ("park" = only in P, "any" = always)
    const QByteArray autoApprove = qgetenv("HMI_AUTO_APPROVE");
    QString approvedFor;
    if (!autoApprove.isEmpty()) {
        auto tryApprove = [&] {
            const bool gearOk = autoApprove == "any" || vehicle.gear() == QLatin1String("P");
            if (ota.state() == OtaClient::UpdateAvailable && gearOk && approvedFor != ota.availableVersion()) {
                approvedFor = ota.availableVersion();
                // optional pause so the "update available" card is visible in demos
                QTimer::singleShot(qEnvironmentVariableIntValue("HMI_AUTO_APPROVE_DELAY_MS"), &ota, [&ota] {
                    if (ota.state() == OtaClient::UpdateAvailable) ota.approveInstall();
                });
            }
        };
        QObject::connect(&ota, &OtaClient::statusChanged, &app, tryApprove);
        QObject::connect(&vehicle, &VehicleState::gearChanged, &app, tryApprove);
    }

    HmiShell shell(p.value(bundleRoot), &vehicle, &ota);
    shell.setHealthWindowMs(p.value(healthMs).toInt());
    shell.setCockpit(&cockpit);
    QObject::connect(&shell, &HmiShell::uiRendering, &cockpit, [&] { cockpit.setUiRendering(true); });
    if (!p.value(snapDir).isEmpty()) shell.setSnapshots(p.value(snapDir), p.value(snapMs).toInt());

    vehicleClient.start();
    otaClient.start();
    cockpit.start();
    shell.loadActiveBundle();  // failure is reported to the agent, which rolls back

    return app.exec();
}
