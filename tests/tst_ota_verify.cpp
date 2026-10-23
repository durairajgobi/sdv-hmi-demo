// Security-relevant unit tests: manifest validation, version ordering and
// Ed25519 signature verification (incl. tampering and wrong-key cases).
#include <QtTest>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include "manifest.h"
#include "signature.h"

namespace {
struct Key {
    QByteArray publicPem;
    EVP_PKEY *pkey = nullptr;
    ~Key() { EVP_PKEY_free(pkey); }
};

void makeKey(Key &k, int type = EVP_PKEY_ED25519) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(type, nullptr);
    QVERIFY(ctx);
    QCOMPARE(EVP_PKEY_keygen_init(ctx), 1);
    if (type == EVP_PKEY_EC) QCOMPARE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1), 1);
    QCOMPARE(EVP_PKEY_keygen(ctx, &k.pkey), 1);
    EVP_PKEY_CTX_free(ctx);
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, k.pkey);
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    k.publicPem = QByteArray(data, len);
    BIO_free(bio);
}

QByteArray sign(EVP_PKEY *key, const QByteArray &msg) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
    size_t len = 64;
    QByteArray sig(64, 0);
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(sig.data()), &len,
                   reinterpret_cast<const unsigned char *>(msg.constData()), size_t(msg.size()));
    EVP_MD_CTX_free(ctx);
    return sig;
}

QByteArray manifestJson(const QString &version = "1.1.0", const QString &url = "bundles/hmi-bundle-1.1.0.tar.gz") {
    return QStringLiteral(R"({"schema":1,"component":"cluster-hmi","version":"%1","minShellVersion":"1.0.0",)"
                          R"("url":"%2","sha256":"%3","size":12345,"releaseNotes":"x"})")
        .arg(version, url, QString(64, QLatin1Char('a')))
        .toUtf8();
}
} // namespace

class TestOtaVerify : public QObject {
    Q_OBJECT
private slots:
    void validSignatureAccepted() {
        Key k; makeKey(k);
        const QByteArray m = manifestJson();
        QString err;
        QVERIFY2(sig::verifyEd25519(k.publicPem, m, sign(k.pkey, m), &err), qPrintable(err));
    }
    void tamperedManifestRejected() {
        Key k; makeKey(k);
        const QByteArray m = manifestJson();
        const QByteArray s = sign(k.pkey, m);
        QByteArray evil = m;
        evil.replace("1.1.0", "9.9.9");
        QVERIFY(!sig::verifyEd25519(k.publicPem, evil, s));
    }
    void wrongKeyRejected() {
        Key vehicleTrust, attacker; makeKey(vehicleTrust); makeKey(attacker);
        const QByteArray m = manifestJson();
        QVERIFY(!sig::verifyEd25519(vehicleTrust.publicPem, m, sign(attacker.pkey, m)));
    }
    void nonEd25519KeyRejected() {
        Key ec; makeKey(ec, EVP_PKEY_EC);
        QString err;
        QVERIFY(!sig::verifyEd25519(ec.publicPem, "x", QByteArray(64, 1), &err));
        QVERIFY(err.contains("not Ed25519"));
    }
    void malformedInputsRejected() {
        Key k; makeKey(k);
        QVERIFY(!sig::verifyEd25519(k.publicPem, "x", QByteArray(10, 0)));      // short sig
        QVERIFY(!sig::verifyEd25519("garbage", "x", QByteArray(64, 0)));        // bad PEM
        QVERIFY(!sig::verifyEd25519(k.publicPem, "x", QByteArray()));            // missing sig
    }
    void manifestParses() {
        Manifest m; QString err;
        QVERIFY2(Manifest::parse(manifestJson(), m, err), qPrintable(err));
        QCOMPARE(m.version, QStringLiteral("1.1.0"));
        QCOMPARE(m.size, qint64(12345));
    }
    void manifestRejectsPathTraversal() {
        Manifest m; QString err;
        QVERIFY(!Manifest::parse(manifestJson("1.1.0", "../../etc/passwd"), m, err));
        QVERIFY(!Manifest::parse(manifestJson("1.1.0", "/abs/path.tar.gz"), m, err));
        QVERIFY(!Manifest::parse(manifestJson("1.1.0", "http://evil/x.tgz"), m, err));
    }
    void manifestRejectsBadFields() {
        Manifest m; QString err;
        QVERIFY(!Manifest::parse("not json", m, err));
        QVERIFY(!Manifest::parse(manifestJson("1.1"), m, err));
        QByteArray j = manifestJson();
        j.replace("cluster-hmi", "brake-ecu");
        QVERIFY(!Manifest::parse(j, m, err));
        QVERIFY(err.contains("component"));
    }
    void versionOrdering() {
        QVERIFY(compareVersions("1.10.0", "1.9.9") > 0);
        QVERIFY(compareVersions("1.1.0", "1.1.0") == 0);
        QVERIFY(compareVersions("1.0.9", "1.1.0") < 0);
        QVERIFY(compareVersions("2.0", "1.99.99") > 0);
    }
};

QTEST_APPLESS_MAIN(TestOtaVerify)
#include "tst_ota_verify.moc"
