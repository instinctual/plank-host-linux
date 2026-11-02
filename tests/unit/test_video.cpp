/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <tuple>
#include <utility>

// local includes
#include <src/config.h>
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
  #include <src/platform/linux/cuda.h>
#endif
#include <src/video.h>
#include <src/video_colorspace.h>

extern "C" {
#include <libavutil/pixdesc.h>
}

using namespace std::literals;

TEST(VideoColorspaceTest, IdentityGbrUsesExactPlaneMapping) {
  const video::sunshine_colorspace_t colorspace {
    video::colorspace_e::identity_gbr,
    true,
    10,
  };

  const auto *unorm = video::color_vectors_from_colorspace(colorspace, true);
  EXPECT_FLOAT_EQ(unorm->color_vec_y[0], 0.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_y[1], 1.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_y[2], 0.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_u[0], 0.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_u[1], 0.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_u[2], 1.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_v[0], 1.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_v[1], 0.0f);
  EXPECT_FLOAT_EQ(unorm->color_vec_v[2], 0.0f);

  const auto *integer = video::color_vectors_from_colorspace(colorspace, false);
  EXPECT_FLOAT_EQ(integer->color_vec_y[1], 1023.0f);
  EXPECT_FLOAT_EQ(integer->color_vec_u[2], 1023.0f);
  EXPECT_FLOAT_EQ(integer->color_vec_v[0], 1023.0f);
}

TEST(VideoColorspaceTest, IdentityGbrUsesMatrixZeroMetadata) {
  const video::sunshine_colorspace_t colorspace {
    video::colorspace_e::identity_gbr,
    true,
    10,
  };

  const auto avcodec = video::avcodec_colorspace_from_sunshine_colorspace(colorspace);
  EXPECT_EQ(avcodec.primaries, AVCOL_PRI_BT709);
  EXPECT_EQ(avcodec.transfer_function, AVCOL_TRC_IEC61966_2_1);
  EXPECT_EQ(avcodec.matrix, AVCOL_SPC_RGB);
  EXPECT_EQ(avcodec.range, AVCOL_RANGE_JPEG);
  EXPECT_STREQ(av_color_range_name(avcodec.range), "pc");
  EXPECT_FALSE(video::colorspace_is_hdr(colorspace));
}

TEST(VideoColorspaceTest, EightBitExpansionUsesFullTenBitRange) {
  EXPECT_EQ(video::expand_8bit_to_10bit(0), 0);
  EXPECT_EQ(video::expand_8bit_to_10bit(43), 173);
  EXPECT_EQ(video::expand_8bit_to_10bit(128), 514);
  EXPECT_EQ(video::expand_8bit_to_10bit(255), 1023);
}

TEST(VideoColorspaceTest, IdentityGbrRequiresFullRange444) {
  video::config_t config {};
  config.encoderCscMode = COLORSPACE_IDENTITY_GBR << 1;
  config.dynamicRange = 1;
  config.chromaSamplingType = 1;
  EXPECT_EQ(video::colorspace_from_client_config(config, false).colorspace, video::colorspace_e::rec709);

  config.encoderCscMode |= 1;
  config.dynamicRange = 0;
  EXPECT_EQ(video::colorspace_from_client_config(config, false).colorspace, video::colorspace_e::identity_gbr);

  config.dynamicRange = 1;
  config.chromaSamplingType = 0;
  EXPECT_EQ(video::colorspace_from_client_config(config, false).colorspace, video::colorspace_e::rec709);

  config.chromaSamplingType = 1;
  EXPECT_EQ(video::colorspace_from_client_config(config, false).colorspace, video::colorspace_e::identity_gbr);
}

TEST(VideoColorspaceTest, H264TenBit422RemainsSdrRec709OnHdrDisplay) {
  video::config_t config {};
  config.videoFormat = 0;
  config.encoderCscMode = (1 << 1) | 1;
  config.dynamicRange = 1;
  config.chromaSamplingType = 2;

  const auto colorspace = video::colorspace_from_client_config(config, true);
  EXPECT_EQ(colorspace.colorspace, video::colorspace_e::rec709);
  EXPECT_TRUE(colorspace.full_range);
  EXPECT_EQ(colorspace.bit_depth, 10);
}

TEST(VideoEncoderTest, NativeX264RgbRequiresEightBitIdentity444OutsideCudaBackend) {
  video::config_t config {};
  config.videoFormat = 0;
  config.dynamicRange = 0;
  config.chromaSamplingType = 1;
  const video::sunshine_colorspace_t identity_8bit {
    video::colorspace_e::identity_gbr,
    true,
    8,
  };

  EXPECT_TRUE(video::use_native_x264rgb("software"sv, "libx264"sv, config, identity_8bit));
  EXPECT_FALSE(video::use_native_x264rgb("software-cuda"sv, "libx264"sv, config, identity_8bit));

  config.dynamicRange = 1;
  EXPECT_FALSE(video::use_native_x264rgb("software"sv, "libx264"sv, config, identity_8bit));
  config.dynamicRange = 0;
  config.chromaSamplingType = 0;
  EXPECT_FALSE(video::use_native_x264rgb("software"sv, "libx264"sv, config, identity_8bit));
  config.chromaSamplingType = 1;

  auto rec709 = identity_8bit;
  rec709.colorspace = video::colorspace_e::rec709;
  EXPECT_FALSE(video::use_native_x264rgb("software"sv, "libx264"sv, config, rec709));
  EXPECT_FALSE(video::use_native_x264rgb("software"sv, "h264_nvenc"sv, config, identity_8bit));
}

TEST(VideoEncoderTest, SoftwareRateControlAllowsBoundedSceneCutBursts) {
  const auto rate_control = video::software_rate_control(78988000, 60, 200, 4);

  EXPECT_EQ(rate_control.average_rate, 78988000);
  EXPECT_EQ(rate_control.peak_rate, 157976000);
  EXPECT_EQ(rate_control.buffer_size, 5265866);
}

TEST(VideoOutputTopologyTest, ResolvesOpaqueIdentityWithoutEnumerationIndex) {
  const std::vector<platf::display_info_t> outputs {
    {.id = "x11:DP-1", .name = "DP-1", .capture_name = "DP-1"},
    {.id = "x11:DP-2", .name = "DP-2", .capture_name = "DP-2"},
  };

  EXPECT_EQ(video::resolve_output_capture_name(outputs, "x11:DP-2"), "DP-2");
  EXPECT_FALSE(video::resolve_output_capture_name(outputs, "x11:missing"));
  EXPECT_FALSE(video::resolve_output_capture_name(outputs, "1"));
}

TEST(VideoOutputTopologyTest, GenerationTracksTopologyNotEnumerationOrder) {
  const std::vector<platf::display_info_t> outputs {
    {.id = "x11:DP-1", .name = "DP-1", .capture_name = "DP-1", .x = 3840,
     .y = 0, .width = 1280, .height = 2160, .rotation = 0,
     .refresh_millihz = 59930, .primary = false},
    {.id = "x11:DP-2", .name = "DP-2", .capture_name = "DP-2", .x = 0,
     .y = 0, .width = 3840, .height = 2160, .rotation = 0,
     .refresh_millihz = 59970, .primary = true},
  };
  auto reordered = outputs;
  std::reverse(reordered.begin(), reordered.end());
  EXPECT_EQ(video::output_topology_generation(outputs),
            video::output_topology_generation(reordered));

  auto changed = outputs;
  changed.front().x = 2560;
  EXPECT_NE(video::output_topology_generation(outputs),
            video::output_topology_generation(changed));
  changed = outputs;
  changed.front().primary = true;
  EXPECT_NE(video::output_topology_generation(outputs),
            video::output_topology_generation(changed));
}

TEST(VideoEncoderTest, SoftwareRateControlRetainsLegacyBufferSizingWhenDisabled) {
  const auto rate_control = video::software_rate_control(78988000, 60, 100, 0);

  EXPECT_EQ(rate_control.average_rate, rate_control.peak_rate);
  EXPECT_EQ(rate_control.buffer_size, 0);
}

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
TEST(VideoColorspaceTest, IdentityGbrCudaKernelProducesExact8BitPlanes) {
  EXPECT_TRUE(cuda::test_identity_gbr_8bit_conversion());
}

TEST(VideoColorspaceTest, IdentityGbrCudaKernelProducesExact10BitPlanes) {
  EXPECT_TRUE(cuda::test_identity_gbr_10bit_conversion());
}
#endif

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    BaseTest::SetUp();
    auto &encoder = *GetParam();
    // Use the same codec eligibility as the product's backend probes.
    const bool probe_hevc = encoder.name == "nvenc-direct";
    if (!video::validate_encoder(encoder, false, probe_hevc, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    &video::software_cuda,
    &video::nvenc_direct

#endif
  ),
  [](const auto &info) {
    auto name = std::string(info.param->name);
    std::ranges::replace(name, '-', '_');
    return name;
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
  if (GetParam() == &video::software_cuda) {
    EXPECT_TRUE(video::software_cuda.h264[video::encoder_t::YUV444]);
    EXPECT_TRUE(video::software_cuda.h264[video::encoder_t::DYNAMIC_RANGE_YUV444]);
  }
#endif
}
#endif

/**
 * @brief Parameterized coverage for effective H.264 profile selection.
 */
struct H264ProfileTest: testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(H264ProfileTest, SelectProfile) {
  const auto &[chroma_sampling_type, expected_profile] = GetParam();
  video::config_t config {};
  config.chromaSamplingType = chroma_sampling_type;

  EXPECT_EQ(expected_profile, video::select_h264_profile(config));
}

INSTANTIATE_TEST_SUITE_P(
  H264ProfileTests,
  H264ProfileTest,
  testing::Values(
    std::make_tuple(0, AV_PROFILE_H264_HIGH),
    std::make_tuple(1, AV_PROFILE_H264_HIGH_444_PREDICTIVE),
    std::make_tuple(2, AV_PROFILE_H264_HIGH_422)
  )
);


struct FramerateX100Test: BaseTest, testing::WithParamInterface<std::tuple<std::int32_t, AVRational>> {};

TEST_P(FramerateX100Test, Run) {
  const auto &[x100, expected] = GetParam();
  auto res = video::framerateX100_to_rational(x100);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, AVRational {24000, 1001}),
    std::make_tuple(2398, AVRational {24000, 1001}),
    std::make_tuple(2500, AVRational {25, 1}),
    std::make_tuple(2997, AVRational {30000, 1001}),
    std::make_tuple(3000, AVRational {30, 1}),
    std::make_tuple(5994, AVRational {60000, 1001}),
    std::make_tuple(6000, AVRational {60, 1}),
    std::make_tuple(11988, AVRational {120000, 1001}),
    std::make_tuple(23976, AVRational {240000, 1001}),  // future NTSC 240hz?
    std::make_tuple(9498, AVRational {4749, 50})  // from my LG 27GN950
  )
);

struct FramerateToRationalTest: testing::TestWithParam<std::tuple<int, int, AVRational>> {};

TEST_P(FramerateToRationalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  auto res = video::framerate_to_rational(config);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateToRationalTests,
  FramerateToRationalTest,
  testing::Values(
    std::make_tuple(60, 0, AVRational {60, 1}),  // no X100 value, fall back to integer framerate
    std::make_tuple(60, 5994, AVRational {60000, 1001}),
    std::make_tuple(120, 11988, AVRational {120000, 1001}),
    std::make_tuple(24, 2398, AVRational {24000, 1001})
  )
);

struct CaptureFrameIntervalTest: testing::TestWithParam<std::tuple<int, int, std::chrono::nanoseconds>> {};

TEST_P(CaptureFrameIntervalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  ASSERT_EQ(expected, video::capture_frame_interval(config));
}

INSTANTIATE_TEST_SUITE_P(
  CaptureFrameIntervalTests,
  CaptureFrameIntervalTest,
  testing::Values(
    std::make_tuple(60, 0, std::chrono::nanoseconds {16666666}),
    std::make_tuple(60, 5994, std::chrono::nanoseconds {16683333}),  // 1e9 * 1001 / 60000
    std::make_tuple(120, 11988, std::chrono::nanoseconds {8341666})  // 1e9 * 1001 / 120000
  )
);
