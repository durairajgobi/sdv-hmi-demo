// Ed25519 signature verification (OpenSSL 3 EVP API).
#pragma once

#include <QByteArray>
#include <QString>

namespace sig {

// Verifies `signature` (raw 64 bytes) over `data` with an Ed25519 public key
// in PEM (SubjectPublicKeyInfo) form. Rejects any non-Ed25519 key type.
bool verifyEd25519(const QByteArray &publicKeyPem, const QByteArray &data,
                   const QByteArray &signature, QString *error = nullptr);

} // namespace sig
