// OTA release manifest: parsing, validation and version ordering.
#pragma once

#include <QByteArray>
#include <QString>

struct Manifest {
    int schema = 0;
    QString component;
    QString version;
    QString minShellVersion;
    QString url;       // relative to the server base URL
    QString sha256;    // lowercase hex of the bundle archive
    qint64 size = 0;
    QString releaseNotes;

    // Parses and validates; on failure returns false and sets error.
    static bool parse(const QByteArray &json, Manifest &out, QString &error);
};

// Semantic-version style comparison of dotted numeric versions ("1.10.0" > "1.9.3").
// Returns <0, 0, >0. Non-numeric parts compare as 0.
int compareVersions(const QString &a, const QString &b);
