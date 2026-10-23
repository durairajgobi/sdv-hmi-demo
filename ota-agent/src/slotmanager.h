// A/B slot management for the HMI UI bundle.
//
//   <root>/slotA/            bundle (bundle.json + qml/)
//   <root>/slotB/
//   <root>/current -> slotA  active slot (switched atomically via rename(2))
//   <root>/ota-state.json    persisted update transaction (survives power loss)
#pragma once

#include <QString>
#include <QStringList>

struct OtaPersistentState {
    QString committedSlot;   // last slot confirmed healthy
    QString pendingSlot;     // slot activated but not yet confirmed ("" if none)
    QString pendingVersion;
    QStringList rejectedVersions;  // failed health check; never auto-offered again
    int pendingAttempts = 0;
};

class SlotManager {
public:
    explicit SlotManager(QString root);

    QString root() const { return root_; }
    QString activeSlot() const;          // "A" / "B" ("" if unprovisioned)
    QString inactiveSlot() const;
    QString versionOf(const QString &slot) const;
    QString slotDir(const QString &slot) const;

    // Extracts a .tar.gz bundle into the inactive slot (old content removed
    // first) and checks bundle.json declares `expectedVersion`.
    bool installToInactive(const QString &archive, const QString &expectedVersion, QString &error);

    // Atomically points `current` at `slot`.
    bool activate(const QString &slot, QString &error);

    OtaPersistentState loadState() const;
    bool saveState(const OtaPersistentState &s) const;

private:
    QString root_;
};
