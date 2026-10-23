#include "signature.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <memory>

namespace sig {

namespace {
struct BioFree { void operator()(BIO *b) const { BIO_free(b); } };
struct PkeyFree { void operator()(EVP_PKEY *k) const { EVP_PKEY_free(k); } };
struct CtxFree { void operator()(EVP_MD_CTX *c) const { EVP_MD_CTX_free(c); } };

bool fail(QString *error, const char *msg) {
    if (error) *error = QString::fromLatin1(msg);
    return false;
}
} // namespace

bool verifyEd25519(const QByteArray &publicKeyPem, const QByteArray &data,
                   const QByteArray &signature, QString *error) {
    if (signature.size() != 64) return fail(error, "signature must be 64 bytes");

    std::unique_ptr<BIO, BioFree> bio(BIO_new_mem_buf(publicKeyPem.constData(), int(publicKeyPem.size())));
    if (!bio) return fail(error, "out of memory");
    std::unique_ptr<EVP_PKEY, PkeyFree> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
    if (!key) return fail(error, "cannot parse public key");
    if (EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519) return fail(error, "public key is not Ed25519");

    std::unique_ptr<EVP_MD_CTX, CtxFree> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return fail(error, "verify init failed");

    const int rc = EVP_DigestVerify(ctx.get(),
                                    reinterpret_cast<const unsigned char *>(signature.constData()),
                                    size_t(signature.size()),
                                    reinterpret_cast<const unsigned char *>(data.constData()),
                                    size_t(data.size()));
    if (rc != 1) return fail(error, "signature does not match");
    return true;
}

} // namespace sig
