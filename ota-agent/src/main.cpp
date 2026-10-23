// sdv-ota-agent: on-vehicle OTA client for the cluster HMI bundle.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>

#include "otaagent.h"

static QString envOr(const char *name, const QString &def) {
    const QByteArray v = qgetenv(name);
    return v.isEmpty() ? def : QString::fromLocal8Bit(v);
}

int main(int argc, char *argv[]) {
    qSetMessagePattern(QStringLiteral("[ota-agent] %{time hh:mm:ss.zzz} %{message}"));
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sdv-ota-agent"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SDV_SHELL_VERSION));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Signed A/B OTA agent for the SDV cluster HMI"));
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption server("server", "OTA server base URL", "url", envOr("OTA_SERVER_URL", "http://127.0.0.1:8080/"));
    QCommandLineOption root("bundle-root", "A/B bundle root", "dir", envOr("HMI_BUNDLE_ROOT", "/var/lib/sdv/hmi"));
    QCommandLineOption key("public-key", "Ed25519 release-signing public key (PEM)", "file",
                           envOr("OTA_PUBLIC_KEY", "/etc/sdv/ota_signing_pub.pem"));
    QCommandLineOption work("work-dir", "download directory", "dir", envOr("OTA_WORK_DIR", QDir::tempPath() + "/sdv-ota"));
    QCommandLineOption poll("poll-interval", "server poll interval (ms)", "ms", envOr("OTA_POLL_INTERVAL_MS", "10000"));
    QCommandLineOption health("health-timeout", "max wait for HMI health confirmation (ms)", "ms",
                              envOr("OTA_HEALTH_TIMEOUT_MS", "20000"));
    p.addOptions({server, root, key, work, poll, health});
    p.process(app);

    AgentConfig cfg;
    QString url = p.value(server);
    if (!url.endsWith('/')) url += '/';
    cfg.serverUrl = QUrl(url);
    cfg.bundleRoot = p.value(root);
    cfg.publicKeyPath = p.value(key);
    cfg.workDir = p.value(work);
    cfg.pollIntervalMs = p.value(poll).toInt();
    cfg.healthTimeoutMs = p.value(health).toInt();

    OtaAgent agent(cfg);
    QString error;
    if (!agent.start(&error)) {
        qCritical("fatal: %s", qPrintable(error));
        return 1;
    }
    return app.exec();
}
