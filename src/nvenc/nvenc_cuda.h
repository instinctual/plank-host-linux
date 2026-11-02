/**
 * @file src/nvenc/nvenc_cuda.h
 * @brief Linux CUDA input adapter for standalone NVENC.
 */
#pragma once


#include "nvenc_base.h"
#include "nvenc_cuda_factory.h"

namespace NVENC_NAMESPACE {

  class nvenc_cuda final: public nvenc_base {
  public:
    explicit nvenc_cuda(::nvenc::nvenc_cuda_input input);
    ~nvenc_cuda() override;

  protected:
    bool init_library() override;
    bool create_and_register_input_buffer() override;

  private:
    ::nvenc::nvenc_cuda_input input;
    void *library = nullptr;
  };

}  // namespace NVENC_NAMESPACE
