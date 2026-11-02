/**
 * @file src/nvenc/nvenc_sdk.h
 * @brief Imports the qualified NVENC API 13 into its implementation namespace.
 */
#pragma once

#ifdef DOXYGEN
  /**
   * @def NVENC_NAMESPACE
   * @brief Namespace used for the documented NVENC implementation.
   */
  #define NVENC_NAMESPACE nvenc

  /**
   * @def NVENC_SDK_VERSION
   * @brief NVENC SDK version used while generating documentation.
   */
  // Doxygen must select the same preprocessor interface used by versioned build targets.
  #define NVENC_SDK_VERSION 1300  // NOSONAR(cpp:S5028)
#endif

#ifndef NVENC_NAMESPACE
  #error NVENC_NAMESPACE must identify the version-specific implementation namespace
#endif

#ifndef NVENC_SDK_VERSION
  #error NVENC_SDK_VERSION must identify the version-specific NVENC SDK
#endif

#if NVENC_SDK_VERSION != 1300
  #error PLANK requires its pinned NVENC API 13 headers
#endif

// Include common platform and integer declarations outside the SDK namespace.
#include <cstddef>
#include <cstdint>

// nvEncodeAPI.h includes these C headers itself. Load them in the global
// namespace first so isolating the SDK declarations below does not
// accidentally place libc and libstdc++ declarations in NVENC_NAMESPACE.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// The version-specific namespace is populated by the SDK headers included below.
namespace NVENC_NAMESPACE {  // NOSONAR(cpp:S3261)
#ifndef DOXYGEN
  #include <ffnvcodec/dynlink_cuda.h>
  #include <ffnvcodec/nvEncodeAPI.h>

  #if NVENCAPI_MAJOR_VERSION * 100 + NVENCAPI_MINOR_VERSION != NVENC_SDK_VERSION
    #error NVENC_SDK_VERSION does not match the selected nv-codec-headers package
  #endif
#endif
}  // namespace NVENC_NAMESPACE
