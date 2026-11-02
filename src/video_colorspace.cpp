/**
 * @file src/video_colorspace.cpp
 * @brief Definitions for colorspace functions.
 */
// this include
#include "video_colorspace.h"

// local includes
#include "logging.h"
#include "video.h"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace video {

  /**
   * @brief Derive Sunshine colorspace metadata from client stream configuration.
   */
  sunshine_colorspace_t colorspace_from_client_config(const config_t &config) {
    sunshine_colorspace_t colorspace;

    /* See video::config_t declaration for details */

    switch (config.encoderCscMode >> 1) {
      case 0:
        // Rec. 601
        colorspace.colorspace = colorspace_e::rec601;
        break;

      case 1:
        // Rec. 709
        colorspace.colorspace = colorspace_e::rec709;
        break;

      case 2:
        // Rec. 2020
        colorspace.colorspace = colorspace_e::bt2020sdr;
        break;

      case COLORSPACE_IDENTITY_GBR:
        colorspace.colorspace = colorspace_e::identity_gbr;
        break;

      default:
        BOOST_LOG(error) << "Unknown video colorspace in csc, falling back to Rec. 709";
        colorspace.colorspace = colorspace_e::rec709;
        break;
    }

    colorspace.full_range = (config.encoderCscMode & 0x1);

    switch (config.dynamicRange) {
      case 0:
        colorspace.bit_depth = 8;
        break;

      case 1:
        colorspace.bit_depth = 10;
        break;

      default:
        BOOST_LOG(error) << "Unknown dynamicRange value, falling back to 10-bit color depth";
        colorspace.bit_depth = 10;
        break;
    }

    if (colorspace.colorspace == colorspace_e::bt2020sdr && colorspace.bit_depth != 10) {
      BOOST_LOG(error) << "BT.2020 SDR colorspace expects 10-bit color depth, falling back to Rec. 709";
      colorspace.colorspace = colorspace_e::rec709;
    }

    if (colorspace.colorspace == colorspace_e::identity_gbr &&
        (!colorspace.full_range || config.chromaSamplingType != 1)) {
      BOOST_LOG(error) << "Identity GBR requires full-range 4:4:4; falling back to Rec. 709";
      colorspace.colorspace = colorspace_e::rec709;
    }

    return colorspace;
  }

  /**
   * @brief Convert Sunshine colorspace metadata to FFmpeg AVCodec fields.
   */
  avcodec_colorspace_t avcodec_colorspace_from_sunshine_colorspace(const sunshine_colorspace_t &sunshine_colorspace) {
    avcodec_colorspace_t avcodec_colorspace;

    switch (sunshine_colorspace.colorspace) {
      case colorspace_e::rec601:
        // Rec. 601
        avcodec_colorspace.primaries = AVCOL_PRI_SMPTE170M;
        avcodec_colorspace.transfer_function = AVCOL_TRC_SMPTE170M;
        avcodec_colorspace.matrix = AVCOL_SPC_SMPTE170M;
        avcodec_colorspace.software_format = SWS_CS_SMPTE170M;
        break;

      case colorspace_e::rec709:
        // Rec. 709
        avcodec_colorspace.primaries = AVCOL_PRI_BT709;
        avcodec_colorspace.transfer_function = AVCOL_TRC_BT709;
        avcodec_colorspace.matrix = AVCOL_SPC_BT709;
        avcodec_colorspace.software_format = SWS_CS_ITU709;
        break;

      case colorspace_e::bt2020sdr:
        // Rec. 2020
        avcodec_colorspace.primaries = AVCOL_PRI_BT2020;
        assert(sunshine_colorspace.bit_depth == 10);
        avcodec_colorspace.transfer_function = AVCOL_TRC_BT2020_10;
        avcodec_colorspace.matrix = AVCOL_SPC_BT2020_NCL;
        avcodec_colorspace.software_format = SWS_CS_BT2020;
        break;

      case colorspace_e::identity_gbr:
        avcodec_colorspace.primaries = AVCOL_PRI_BT709;
        avcodec_colorspace.transfer_function = AVCOL_TRC_IEC61966_2_1;
        avcodec_colorspace.matrix = AVCOL_SPC_RGB;
        avcodec_colorspace.software_format = SWS_CS_ITU709;
        break;
    }

    const int ffmpeg_range = av_color_range_from_name(
      sunshine_colorspace.full_range ? "pc" : "tv"
    );
    assert(ffmpeg_range >= 0);
    avcodec_colorspace.range = static_cast<AVColorRange>(ffmpeg_range);

    return avcodec_colorspace;
  }

  const color_t *color_vectors_from_colorspace(const sunshine_colorspace_t &colorspace, bool unorm_output) {
    constexpr auto generate_color_vectors = [](const sunshine_colorspace_t &colorspace, bool unorm_output) -> color_t {
      // "Table 4 – Interpretation of matrix coefficients (MatrixCoefficients) value" section of ITU-T H.273
      double Kr;
      double Kb;
      switch (colorspace.colorspace) {
        case colorspace_e::rec601:
          Kr = 0.299;
          Kb = 0.114;
          break;
        case colorspace_e::rec709:
        default:
          Kr = 0.2126;
          Kb = 0.0722;
          break;
        case colorspace_e::bt2020sdr:
          Kr = 0.2627;
          Kb = 0.0593;
          break;
      }
      double Kg = 1.0 - Kr - Kb;

      double y_mult;
      double y_add;
      double uv_mult;
      double uv_add;

      // "8.3 Matrix coefficients" section of ITU-T H.273
      if (colorspace.full_range) {
        y_mult = (1 << colorspace.bit_depth) - 1;
        y_add = 0;
        uv_mult = (1 << colorspace.bit_depth) - 1;
        uv_add = (1 << (colorspace.bit_depth - 1));
      } else {
        y_mult = (1 << (colorspace.bit_depth - 8)) * 219;
        y_add = (1 << (colorspace.bit_depth - 8)) * 16;
        uv_mult = (1 << (colorspace.bit_depth - 8)) * 224;
        uv_add = (1 << (colorspace.bit_depth - 8)) * 128;
      }

      if (unorm_output) {
        const double unorm_range = (1 << colorspace.bit_depth) - 1;
        y_mult /= unorm_range;
        y_add /= unorm_range;
        uv_mult /= unorm_range;
        uv_add /= unorm_range;
      } else {
        // For rounding
        y_add += 0.5f;
        uv_add += 0.5f;
      }

      color_t color_vectors;

      if (colorspace.colorspace == colorspace_e::identity_gbr) {
        const double component_mult = unorm_output ? 1.0 : (1 << colorspace.bit_depth) - 1;
        const double component_add = unorm_output ? 0.0 : 0.5;

        color_vectors.color_vec_y[0] = 0;
        color_vectors.color_vec_y[1] = component_mult;
        color_vectors.color_vec_y[2] = 0;
        color_vectors.color_vec_y[3] = component_add;

        color_vectors.color_vec_u[0] = 0;
        color_vectors.color_vec_u[1] = 0;
        color_vectors.color_vec_u[2] = component_mult;
        color_vectors.color_vec_u[3] = component_add;

        color_vectors.color_vec_v[0] = component_mult;
        color_vectors.color_vec_v[1] = 0;
        color_vectors.color_vec_v[2] = 0;
        color_vectors.color_vec_v[3] = component_add;

        color_vectors.range_y[0] = 1;
        color_vectors.range_y[1] = 0;
        color_vectors.range_uv[0] = 1;
        color_vectors.range_uv[1] = 0;
        return color_vectors;
      }

      color_vectors.color_vec_y[0] = Kr * y_mult;
      color_vectors.color_vec_y[1] = Kg * y_mult;
      color_vectors.color_vec_y[2] = Kb * y_mult;
      color_vectors.color_vec_y[3] = y_add;

      color_vectors.color_vec_u[0] = -0.5 * Kr / (1.0 - Kb) * uv_mult;
      color_vectors.color_vec_u[1] = -0.5 * Kg / (1.0 - Kb) * uv_mult;
      color_vectors.color_vec_u[2] = 0.5 * uv_mult;
      color_vectors.color_vec_u[3] = uv_add;

      color_vectors.color_vec_v[0] = 0.5 * uv_mult;
      color_vectors.color_vec_v[1] = -0.5 * Kg / (1.0 - Kr) * uv_mult;
      color_vectors.color_vec_v[2] = -0.5 * Kb / (1.0 - Kr) * uv_mult;
      color_vectors.color_vec_v[3] = uv_add;

      // Unused
      color_vectors.range_y[0] = 1;
      color_vectors.range_y[1] = 0;
      color_vectors.range_uv[0] = 1;
      color_vectors.range_uv[1] = 0;

      return color_vectors;
    };

    static constexpr color_t colors[] = {
      generate_color_vectors({colorspace_e::rec601, false, 8}, false),
      generate_color_vectors({colorspace_e::rec601, true, 8}, false),
      generate_color_vectors({colorspace_e::rec601, false, 10}, false),
      generate_color_vectors({colorspace_e::rec601, true, 10}, false),
      generate_color_vectors({colorspace_e::rec709, false, 8}, false),
      generate_color_vectors({colorspace_e::rec709, true, 8}, false),
      generate_color_vectors({colorspace_e::rec709, false, 10}, false),
      generate_color_vectors({colorspace_e::rec709, true, 10}, false),
      generate_color_vectors({colorspace_e::bt2020sdr, false, 8}, false),
      generate_color_vectors({colorspace_e::bt2020sdr, true, 8}, false),
      generate_color_vectors({colorspace_e::bt2020sdr, false, 10}, false),
      generate_color_vectors({colorspace_e::bt2020sdr, true, 10}, false),
      generate_color_vectors({colorspace_e::identity_gbr, false, 8}, false),
      generate_color_vectors({colorspace_e::identity_gbr, true, 8}, false),
      generate_color_vectors({colorspace_e::identity_gbr, false, 10}, false),
      generate_color_vectors({colorspace_e::identity_gbr, true, 10}, false),

      generate_color_vectors({colorspace_e::rec601, false, 8}, true),
      generate_color_vectors({colorspace_e::rec601, true, 8}, true),
      generate_color_vectors({colorspace_e::rec601, false, 10}, true),
      generate_color_vectors({colorspace_e::rec601, true, 10}, true),
      generate_color_vectors({colorspace_e::rec709, false, 8}, true),
      generate_color_vectors({colorspace_e::rec709, true, 8}, true),
      generate_color_vectors({colorspace_e::rec709, false, 10}, true),
      generate_color_vectors({colorspace_e::rec709, true, 10}, true),
      generate_color_vectors({colorspace_e::bt2020sdr, false, 8}, true),
      generate_color_vectors({colorspace_e::bt2020sdr, true, 8}, true),
      generate_color_vectors({colorspace_e::bt2020sdr, false, 10}, true),
      generate_color_vectors({colorspace_e::bt2020sdr, true, 10}, true),
      generate_color_vectors({colorspace_e::identity_gbr, false, 8}, true),
      generate_color_vectors({colorspace_e::identity_gbr, true, 8}, true),
      generate_color_vectors({colorspace_e::identity_gbr, false, 10}, true),
      generate_color_vectors({colorspace_e::identity_gbr, true, 10}, true),
    };

    const color_t *result = nullptr;

    switch (colorspace.colorspace) {
      case colorspace_e::rec601:
        result = &colors[0];
        break;
      case colorspace_e::rec709:
      default:
        result = &colors[4];
        break;
      case colorspace_e::bt2020sdr:
        result = &colors[8];
        break;
      case colorspace_e::identity_gbr:
        result = &colors[12];
        break;
    }

    if (colorspace.bit_depth == 10) {
      result += 2;
    }
    if (colorspace.full_range) {
      result += 1;
    }
    if (unorm_output) {
      result += 16;
    }

    return result;
  }
}  // namespace video
