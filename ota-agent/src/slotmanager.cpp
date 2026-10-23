#include "slotmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>

#include <cstdio>
#include <unistd.h>

SlotManager::SlotManager(QString root) : root_(std::move(root)) {}

QString SlotManager::slotDir(const QString &slot) const {
    return QDir(root_).filePath(QStringLiteral("slot") + slot);
}

QString SlotManager::activeSlot() const {
    const QString target = QFileInfo(QDir(root_).filePath(QStringLiteral("current"))).symLinkTarget();
    const QString name = QFileInfo(target).fileName();
    return name.startsWith(QLatin1String("slot")) ? name.mid(4) : QString();
}

QString SlotManager::inactiveSlot() const {
    return activeSlot() == QLatin1String("A") ? QStringLiteral("B") : QStringLiteral("A");
}

QString SlotManager::versionOf(const QString &slot) const {
    QFile f(QDir(slotDir(slot)).filePath(QStringLiteral("bundle.json")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("version")).toString();
}

bool SlotManager::installToInactive(const QString &archive, const QString &expectedVersion, QString &error) {
    const QString slot = inactiveSlot();
    const QString dest = slotDir(slot);
    const QString staging = dest + QStringLiteral(".staging");

    QDir(staging).removeRecursively();
    QDir().mkpath(staging);

    // Refuse absolute paths / traversal inside the archive before extracting.
    QProcess list;
    list.start(QStringLiteral("tar"), {QStringLiteral("-tzf"), archive});
    if (!list.waitForFinished(30000) || list.exitCode() != 0) {
        error = QStringLiteral("archive listing failed: %1").arg(QString::fromLocal8Bit(list.readAllStandardError()));
        return false;
    }
    for (const auto &entry : QString::fromLocal8Bit(list.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (entry.startsWith(QLatin1Char('/')) || entry.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
            error = QStringLiteral("unsafe path in archive: %1").arg(entry);
            return false;
        }
    }

    QProcess tar;
    tar.start(QStringLiteral("tar"), {QStringLiteral("--no-same-owner"), QStringLiteral("-xzf"), archive,
                                      QStringLiteral("-C"), staging});
    if (!tar.waitForFinished(60000) || tar.exitCode() != 0) {
        error = QStringLiteral("extract failed: %1").arg(QString::fromLocal8Bit(tar.readAllStandardError()));
        return false;
    }

    QFile meta(QDir(staging).filePath(QStringLiteral("bundle.json")));
    if (!meta.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("bundle.json missing from archive");
        return false;
    }
    const QString declared = QJsonDocument::fromJson(meta.readAll()).object().value(QLatin1String("version")).toString();
    if (declared != expectedVersion) {
        error = QStringLiteral("bundle declares version %1, manifest says %2").arg(declared, expectedVersion);
        return false;
    }
    if (!QFileInfo::exists(QDir(staging).filePath(QStringLiteral("qml/Main.qml")))) {
        error = QStringLiteral("bundle has no qml/Main.qml entry point");
        return false;
    }

    QDir(dest).removeRecursively();
    if (!QDir().rename(staging, dest)) {
        error = QStringLiteral("cannot move staging dir into slot %1").arg(slot);
        return false;
    }
    ::sync();
    return true;
}

bool SlotManager::activate(const QString &slot, QString &error) {
    const QByteArray tmp = QFile::encodeName(QDir(root_).filePath(QStringLiteral(".current.tmp")));
    const QByteArray cur = QFile::encodeName(QDir(root_).filePath(QStringLiteral("current")));
    const QByteArray target = QFile::encodeName(QStringLiteral("slot") + slot);
    ::unlink(tmp.constData());
    if (::symlink(target.constData(), tmp.constData()) != 0 ||
        std::rename(tmp.constData(), cur.constData()) != 0) {  // atomic replace
        error = QStringLiteral("symlink switch to slot %1 failed").arg(slot);
        return false;
    }
    ::sync();
    return true;
}

OtaPersistentState SlotManager::loadState() const {
    OtaPersistentState s;
    QFile f(QDir(root_).filePath(QStringLiteral("ota-state.json")));
    if (f.open(QIODevice::ReadOnly)) {
        const auto o = QJsonDocument::fromJson(f.readAll()).object();
        s.committedSlot = o.value(QLatin1String("committedSlot")).toString();
        s.pendingSlot = o.value(QLatin1String("pendingSlot")).toString();
        s.pendingVersion = o.value(QLatin1String("pendingVersion")).toString();
        s.pendingAttempts = o.value(QLatin1String("pendingAttempts")).toInt();
        for (const auto &v : o.value(QLatin1String("rejectedVersions")).toArray())
            s.rejectedVersions << v.toString();
    }
    if (s.committedSlot.isEmpty()) s.committedSlot = activeSlot();
    return s;
}

bool SlotManager::saveState(const OtaPersistentState &s) const {
    QJsonObject o{
        {QStringLiteral("committedSlot"), s.committedSlot},
        {QStringLiteral("pendingSlot"), s.pendingSlot},
        {QStringLiteral("pendingVersion"), s.pendingVersion},
        {QStringLiteral("pendingAttempts"), s.pendingAttempts},
        {QStringLiteral("rejectedVersions"), QJsonArray::fromStringList(s.rejectedVersions)},
    };
    QSaveFile f(QDir(root_).filePath(QStringLiteral("ota-state.json")));  // write-temp + rename
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson());
    return f.commit();
}
