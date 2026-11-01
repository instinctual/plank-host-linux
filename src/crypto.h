/**
 * @file src/crypto.h
 * @brief Generate the Linux Host's HTTPS certificate and private key.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace crypto {
  /**
   * @brief PEM-encoded certificate and private key pair.
   */
  struct creds_t {
    std::string x509;  ///< PEM-encoded X.509 certificate.
    std::string pkey;  ///< PEM-encoded private key.
  };

  /**
   * @brief Generate a self-signed certificate and private key for TLS.
   * @param cn Common name to place in the generated certificate.
   * @param key_bits Size in bits of the generated RSA key.
   * @return PEM-encoded certificate and private key.
   */
  creds_t gen_creds(const std::string_view &cn, std::uint32_t key_bits);
}  // namespace crypto
