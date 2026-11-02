/**
 * @file src/nvenc/nvenc_utils.h
 * @brief Declarations for NVENC utilities.
 */
#pragma once

// plafform includes

// local includes
#include "nvenc_colorspace.h"
#include "nvenc_sdk.h"
#include "src/platform/common.h"
#include "src/video_colorspace.h"

namespace NVENC_NAMESPACE {


  /**
   * @brief Convert a Sunshine pixel format to the matching NVENC buffer format.
   *
   * @param format Pixel, audio, or protocol format being converted.
   * @return NVENC buffer format compatible with the Sunshine pixel format.
   */
  NV_ENC_BUFFER_FORMAT nvenc_format_from_sunshine_format(platf::pix_fmt_e format);

  /**
   * @brief Convert Sunshine colorspace metadata to NVENC VUI metadata.
   *
   * @param sunshine_colorspace Sunshine colorspace.
   * @return NVENC colorspace and VUI fields for the Sunshine metadata.
   */
  nvenc_colorspace_t nvenc_colorspace_from_sunshine_colorspace(const video::sunshine_colorspace_t &sunshine_colorspace);

}  // namespace NVENC_NAMESPACE
