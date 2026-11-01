/**
 * @file src/crypto.cpp
 * @brief Definitions for cryptography functions.
 */
// lib includes
#include <openssl/pem.h>
#include <openssl/rsa.h>

// local includes
#include "crypto.h"
#include "utility.h"

namespace crypto {
  using x509_t = util::safe_ptr<X509, X509_free>;
  using bio_t = util::safe_ptr<BIO, BIO_free_all>;
  using pkey_t = util::safe_ptr<EVP_PKEY, EVP_PKEY_free>;
  using pkey_ctx_t = util::safe_ptr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
  using bignum_t = util::safe_ptr<BIGNUM, BN_free>;

  /**
   * @brief OpenSSL ASN.1 string pointer with automatic release.
   */
  using asn1_string_t = util::safe_ptr<ASN1_STRING, ASN1_STRING_free>;
  /**
   * @brief OpenSSL X.509 subject/issuer name pointer with automatic release.
   */
  using x509_name_t = util::safe_ptr<X509_NAME, &X509_NAME_free>;

  /**
   * @brief Serialize an OpenSSL object to PEM text.
   */
  static std::string pem(x509_t &x509) {
    bio_t bio {BIO_new(BIO_s_mem())};

    PEM_write_bio_X509(bio.get(), x509.get());
    BUF_MEM *mem_ptr;
    BIO_get_mem_ptr(bio.get(), &mem_ptr);

    return {mem_ptr->data, mem_ptr->length};
  }

  /**
   * @brief Serialize an OpenSSL object to PEM text.
   */
  static std::string pem(pkey_t &pkey) {
    bio_t bio {BIO_new(BIO_s_mem())};

    PEM_write_bio_PrivateKey(bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
    BUF_MEM *mem_ptr;
    BIO_get_mem_ptr(bio.get(), &mem_ptr);

    return {mem_ptr->data, mem_ptr->length};
  }

  /**
   * @brief Generate a self-signed certificate and private key for TLS.
   */
  creds_t gen_creds(const std::string_view &cn, std::uint32_t key_bits) {
    x509_t x509 {X509_new()};
    pkey_ctx_t ctx {EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    pkey_t pkey;

    EVP_PKEY_keygen_init(ctx.get());
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), key_bits);
    EVP_PKEY_keygen(ctx.get(), &pkey);

    X509_set_version(x509.get(), 2);

    // Generate a real serial number to avoid SEC_ERROR_REUSED_ISSUER_AND_SERIAL with Firefox
    bignum_t serial {BN_new()};
    BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);  // 159 bits to fit in 20 bytes in DER format
    BN_set_negative(serial.get(), 0);  // Serial numbers must be positive
    BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(x509.get()));

    constexpr auto year = 60 * 60 * 24 * 365;
    asn1_string_t not_before {ASN1_STRING_dup(X509_get0_notBefore(x509.get()))};
    asn1_string_t not_after {ASN1_STRING_dup(X509_get0_notAfter(x509.get()))};

    X509_gmtime_adj(not_before.get(), 0);
    X509_gmtime_adj(not_after.get(), 20 * year);

    X509_set1_notBefore(x509.get(), not_before.get());
    X509_set1_notAfter(x509.get(), not_after.get());

    X509_set_pubkey(x509.get(), pkey.get());

    x509_name_t name {X509_NAME_new()};
    X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC, reinterpret_cast<const std::uint8_t *>(cn.data()), (int) cn.size(), -1, 0);

    X509_set_subject_name(x509.get(), name.get());
    X509_set_issuer_name(x509.get(), name.get());
    X509_sign(x509.get(), pkey.get(), EVP_sha256());

    return {pem(x509), pem(pkey)};
  }

}  // namespace crypto
