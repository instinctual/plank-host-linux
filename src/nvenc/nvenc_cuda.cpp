/**
 * @file src/nvenc/nvenc_cuda.cpp
 * @brief Linux CUDA input adapter for standalone NVENC.
 */

#include "nvenc_cuda.h"

#include <dlfcn.h>

#include "src/logging.h"

namespace NVENC_NAMESPACE {

  nvenc_cuda::nvenc_cuda(::nvenc::nvenc_cuda_input input):
      nvenc_base(NV_ENC_DEVICE_TYPE_CUDA),
      input(input) {
    device = input.context;
  }

  nvenc_cuda::~nvenc_cuda() {
    destroy_encoder();
    if (library) {
      dlclose(library);
    }
  }

  bool nvenc_cuda::init_library() {
    if (nvenc) {
      return true;
    }

    library = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      BOOST_LOG(error) << "NvEnc: failed to load libnvidia-encode.so.1: " << dlerror();
      return false;
    }

    const auto create_instance = reinterpret_cast<decltype(NvEncodeAPICreateInstance) *>(
      dlsym(library, "NvEncodeAPICreateInstance")
    );
    if (!create_instance) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance symbol is unavailable";
      return false;
    }

    auto functions = std::make_shared<NV_ENCODE_API_FUNCTION_LIST>();
    functions->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (nvenc_failed(create_instance(functions.get()))) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
      return false;
    }

    nvenc = std::move(functions);
    return true;
  }

  bool nvenc_cuda::create_and_register_input_buffer() {
    NV_ENC_REGISTER_RESOURCE registration = {NV_ENC_REGISTER_RESOURCE_VER};
    registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    registration.resourceToRegister = reinterpret_cast<void *>(input.device_pointer);
    registration.width = encoder_params.width;
    registration.height = encoder_params.height;
    registration.pitch = input.pitch;
    registration.bufferFormat = encoder_params.buffer_format;
    registration.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &registration))) {
      BOOST_LOG(error) << "NvEnc: NvEncRegisterResource() failed: " << last_nvenc_error_string;
      return false;
    }

    registered_input_buffer = registration.registeredResource;
    return true;
  }

}  // namespace NVENC_NAMESPACE
