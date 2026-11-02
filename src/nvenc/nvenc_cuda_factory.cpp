/**
 * @file src/nvenc/nvenc_cuda_factory.cpp
 * @brief API-version-specific Linux CUDA NVENC factory.
 */

#include "nvenc_cuda.h"

namespace nvenc {

  std::unique_ptr<nvenc_encoder> make_nvenc_cuda_encoder(nvenc_cuda_input input) {
    return std::make_unique<NVENC_NAMESPACE::nvenc_cuda>(input);
  }

}  // namespace nvenc
