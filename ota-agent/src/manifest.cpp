#include "manifest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

bool Manifest::parse(const QByteArray &json, Manifest &m, QString &error) {
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("manifest is not a JSON object: %1").arg(pe.errorString());
        return false;
    }
    const auto o = doc.object();
    m.schema = o.value(QLatin1String("schema")).toInt();
    m.component = o.value(QLatin1String("component")).toString();
    m.version = o.value(QLatin1String("version")).toString();
    m.minShellVersion = o.value(QLatin1String("minShellVersion")).toString(QStringLiteral("0.0.0"));
    m.url = o.value(QLatin1String("url")).toString();
    m.sha256 = o.value(QLatin1String("sha256")).toString().toLower();
    m.size = qint64(o.value(QLatin1String("size")).toDouble(-1));
    m.releaseNotes = o.value(QLatin1String("releaseNotes")).toString();

    static const QRegularExpression kVersion(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    static const QRegularExpression kSha(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression kRelPath(QStringLiteral("^[A-Za-z0-9._/-]+$"));

    if (m.schema != 1) error = QStringLiteral("unsupported manifest schema %1").arg(m.schema);
    else if (m.component != QLatin1String("cluster-hmi")) error = QStringLiteral("unexpected component '%1'").arg(m.component);
    else if (!kVersion.match(m.version).hasMatch()) error = QStringLiteral("invalid version '%1'").arg(m.version);
    else if (!kSha.match(m.sha256).hasMatch()) error = QStringLiteral("invalid sha256");
    else if (m.size <= 0 || m.size > 256LL * 1024 * 1024) error = QStringLiteral("implausible size %1").arg(m.size);
    else if (!kRelPath.match(m.url).hasMatch() || m.url.contains(QLatin1String("..")) || m.url.startsWith(QLatin1Char('/')))
        error = QStringLiteral("bundle url must be a plain relative path");
    else return true;
    return false;
}

int compareVersions(const QString &a, const QString &b) {
    const auto pa = a.split(QLatin1Char('.'));
    const auto pb = b.split(QLatin1Char('.'));
    for (int i = 0; i < qMax(pa.size(), pb.size()); ++i) {
        const int x = i < pa.size() ? pa[i].toInt() : 0;
        const int y = i < pb.size() ? pb[i].toInt() : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
