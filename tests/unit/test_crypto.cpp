/**
 * @file tests/unit/test_crypto.cpp
 * @brief Test src/crypto.*.
 */
// test imports
#include <gtest/gtest.h>

// lib imports
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <src/utility.h>

// local imports
#include <src/crypto.h>

TEST(CryptoTest, GeneratedCredentialsHaveMatchingKeyAndValidSelfSignature) {
  constexpr std::string_view common_name = "PLANK Test Host";

  auto creds = crypto::gen_creds(common_name, 2048);
  ASSERT_FALSE(creds.x509.empty());
  ASSERT_FALSE(creds.pkey.empty());

  util::safe_ptr<BIO, BIO_free_all> cert_pem {BIO_new_mem_buf(creds.x509.data(), creds.x509.size())};
  util::safe_ptr<BIO, BIO_free_all> key_pem {BIO_new_mem_buf(creds.pkey.data(), creds.pkey.size())};
  ASSERT_NE(cert_pem.get(), nullptr);
  ASSERT_NE(key_pem.get(), nullptr);
  util::safe_ptr<X509, X509_free> cert {PEM_read_bio_X509(cert_pem.get(), nullptr, nullptr, nullptr)};
  util::safe_ptr<EVP_PKEY, EVP_PKEY_free> pkey {PEM_read_bio_PrivateKey(key_pem.get(), nullptr, nullptr, nullptr)};
  ASSERT_NE(cert.get(), nullptr);
  ASSERT_NE(pkey.get(), nullptr);

  const auto subject = X509_get_subject_name(cert.get());
  ASSERT_NE(subject, nullptr);

  const auto common_name_index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  ASSERT_GE(common_name_index, 0);

  const auto common_name_entry = X509_NAME_get_entry(subject, common_name_index);
  ASSERT_NE(common_name_entry, nullptr);

  const auto common_name_data = X509_NAME_ENTRY_get_data(common_name_entry);
  ASSERT_NE(common_name_data, nullptr);

  const std::string_view parsed_common_name {
    reinterpret_cast<const char *>(ASN1_STRING_get0_data(common_name_data)),
    static_cast<std::size_t>(ASN1_STRING_length(common_name_data))
  };
  ASSERT_EQ(parsed_common_name, common_name);

  ASSERT_EQ(X509_check_private_key(cert.get(), pkey.get()), 1);
  ASSERT_EQ(X509_verify(cert.get(), pkey.get()), 1);
  ASSERT_EQ(EVP_PKEY_get_bits(pkey.get()), 2048);
  ASSERT_LT(X509_cmp_current_time(X509_get0_notBefore(cert.get())), 0);
  ASSERT_GT(X509_cmp_current_time(X509_get0_notAfter(cert.get())), 0);
}
