// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/mario_kart_upscale.h"
#include "video_core/neural_rendering/backend.h"
using namespace VideoCommon::MarioKartUpscale;
static const std::array<float, 44> frame0{
    1.127103269e-01f,  0.000000000e+00f,  -9.936277866e-01f, -6.312568665e+01f, -4.896835238e-02f,
    9.987848401e-01f,  -5.554634146e-03f, -5.201110840e+02f, 9.924206138e-01f,  4.928239062e-02f,
    1.125733927e-01f,  2.445906738e+03f,  9.951736778e-02f,  0.000000000e+00f,  -8.773217797e-01f,
    -5.573670578e+01f, -2.555006444e-01f, 1.558907151e+00f,  -2.898224071e-02f, -1.256674072e+03f,
    -9.926191568e-01f, -4.929224774e-02f, -1.125959083e-01f, -2.448396240e+03f, -9.924206138e-01f,
    -4.928239062e-02f, -1.125733927e-01f, -2.445906738e+03f, 8.829481006e-01f,  0.000000000e+00f,
    0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  1.569685459e+00f,  -1.800000072e-01f,
    0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  -1.000200033e+00f, -2.000200033e+00f,
    0.000000000e+00f,  0.000000000e+00f,  -1.000000000e+00f, 0.000000000e+00f};
static const std::array<float, 44> frame1{
    9.986256361e-01f,  -0.000000000e+00f, 5.241141468e-02f,  2.487330566e+03f,  6.359550753e-04f,
    9.999264479e-01f,  -1.211722754e-02f, -4.091200562e+02f, -5.240755528e-02f, 1.213390380e-02f,
    9.985520840e-01f,  2.693631897e+02f,  8.817346096e-01f,  0.000000000e+00f,  4.627655819e-02f,
    2.196183838e+03f,  1.043161005e-02f,  1.567385912e+00f,  -1.987596154e-01f, -6.906751709e+02f,
    5.241803825e-02f,  -1.213633083e-02f, -9.987518191e-01f, -2.714172974e+02f, 5.240755528e-02f,
    -1.213390380e-02f, -9.985520840e-01f, -2.693631897e+02f, 8.829481006e-01f,  0.000000000e+00f,
    0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  1.569685459e+00f,  -1.800000072e-01f,
    0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  -1.000200033e+00f, -2.000200033e+00f,
    0.000000000e+00f,  0.000000000e+00f,  -1.000000000e+00f, 0.000000000e+00f};
TEST_CASE("MKW captured perspective retains its intentional vertical offset", "[mkw_upscale]") {
    for (const auto& words : {frame0, frame1}) {
        const auto camera = DecodeCamera(words);
        REQUIRE(camera);
        REQUIRE(camera->projection[6] == -0.18f);
        const auto& p = camera->projection;
        REQUIRE(std::abs((-p[10] + p[11]) + 1) < 1e-6);
        REQUIRE(std::abs((-10000 * p[10] + p[11]) / 10000 - 1) < 1e-6);
    }
    auto bad = frame0;
    bad[28 + 6] = 0;
    REQUIRE_FALSE(DecodeCamera(bad));
    bad = frame0;
    bad[28 + 2] = 0.001f;
    REQUIRE_FALSE(DecodeCamera(bad));
    bad = frame0;
    bad[28 + 11] = 0;
    REQUIRE_FALSE(DecodeCamera(bad));
    bad = frame0;
    bad[12 + 3] += 1;
    REQUIRE_FALSE(DecodeCamera(bad));
    bad = frame0;
    bad[0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(DecodeCamera(bad));
}
TEST_CASE("MKW changing bend centre reprojects stationary world geometry", "[mkw_upscale]") {
    const auto previous = DecodeCurvature({-2445.721923828125f, -340.9564514160156f, 500, 20000});
    const auto current = DecodeCurvature({-2469.534912109375f, -404.2950744628906f, 500, 20000});
    REQUIRE(previous);
    REQUIRE(current);
    double largest_naive_error = 0;
    for (const std::array<double, 3> world : {std::array<double, 3>{-2445, 400, -340},
                                              {-1945, 400, -340},
                                              {2000, 400, 4000},
                                              {-9000, 300, -4000},
                                              {2000, -500, 12000}}) {
        const auto current_curved = Bend(world, *current);
        REQUIRE(current_curved);
        const auto reconstructed = Unbend(*current_curved, *current);
        REQUIRE(reconstructed);
        const auto actual_previous = Bend(*reconstructed, *previous);
        const auto expected_previous = Bend(world, *previous);
        REQUIRE(actual_previous);
        REQUIRE(expected_previous);
        double naive_squared = 0;
        for (size_t k = 0; k < 3; ++k) {
            REQUIRE(std::abs((*reconstructed)[k] - world[k]) < 1e-8);
            REQUIRE(std::abs((*actual_previous)[k] - (*expected_previous)[k]) < 1e-8);
            naive_squared += std::pow((*current_curved)[k] - (*expected_previous)[k], 2);
        }
        largest_naive_error = std::max(largest_naive_error, std::sqrt(naive_squared));
    }
    REQUIRE(largest_naive_error > 1);
    REQUIRE_FALSE(DecodeCurvature({0, 0, 500, 0}));
    REQUIRE_FALSE(DecodeCurvature({0, 0, 500, std::numeric_limits<float>::infinity()}));
    REQUIRE_FALSE(Bend({1e7, 0, 1e7}, *current));
    REQUIRE_FALSE(Unbend({1e7, -20001, 1e7}, *current));
}

TEST_CASE("MKW captured DRS and full-native scene extents retain the fixed output bound",
          "[mkw_upscale]") {
    REQUIRE(InputExtentSupported(1664, 932));
    REQUIRE(InputExtentSupported(1704, 956));
    REQUIRE(InputExtentSupported(2560, 1440));
    REQUIRE_FALSE(InputExtentSupported(2568, 1440));
    REQUIRE_FALSE(InputExtentSupported(2560, 1448));
    REQUIRE_FALSE(InputExtentSupported(2560, 1080));
    REQUIRE_FALSE(InputExtentSupported(592, 588));
    REQUIRE_FALSE(InputExtentSupported(1280, 0));
}

TEST_CASE("MKW recorded HDR exposures normalize NR without changing the native shoulder",
          "[mkw_upscale]") {
    std::array<float, 24> cb{};
    cb[0] = .25f;
    cb[2] = .15f;
    cb[3] = 1;
    cb[4] = .75f;
    cb[5] = -5.7707800865f;
    cb[6] = 2.3280851841f;
    cb[23] = 1;
    const auto day = VideoCommon::MarioKartUpscale::DecodeTransfer(cb);
    REQUIRE(day);
    CHECK(1.f / day->luminance[0] == 4.f);
    cb[0] = 1;
    const auto sunset = VideoCommon::MarioKartUpscale::DecodeTransfer(cb);
    REQUIRE(sunset);
    CHECK(1.f / sunset->luminance[0] == 1.f);
    CHECK(day->shoulder == sunset->shoulder);
    cb[0] = 0;
    CHECK_FALSE(VideoCommon::MarioKartUpscale::DecodeTransfer(cb));
    cb[0] = .5f; // Lighting interpolates between captured daytime/sunset exposures.
    CHECK(VideoCommon::MarioKartUpscale::DecodeTransfer(cb));
    cb[0] = 1;
    cb[0] = .2992350161f;
    cb[1] = .1294686794f; // Captured Bowser exposure requires the optional nonlinear proxy.
    CHECK(VideoCommon::MarioKartUpscale::DecodeTransfer(cb));
    cb[1] = -0.1f;
    CHECK_FALSE(VideoCommon::MarioKartUpscale::DecodeTransfer(cb));
    cb[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(VideoCommon::MarioKartUpscale::DecodeTransfer(cb));
    cb[1] = 0;
    cb[5] = 0;
    CHECK_FALSE(VideoCommon::MarioKartUpscale::DecodeTransfer(cb));
}

TEST_CASE("NR optional reference exposure validates an invertible finite domain", "[mkw_upscale]") {
    using VideoCore::NeuralRendering::ValidReferenceExposure;
    CHECK(ValidReferenceExposure({}));
    CHECK(ValidReferenceExposure({.2992350161f, .1294686794f, .15f}));
    CHECK(ValidReferenceExposure({.25f, 0.f, 0.f}));
    CHECK_FALSE(ValidReferenceExposure({0.f, 1.f, .15f}));
    CHECK_FALSE(ValidReferenceExposure({.25f, -1.f, .15f}));
    CHECK_FALSE(ValidReferenceExposure({.25f, 1.f, -1.f}));
    CHECK_FALSE(ValidReferenceExposure({20.f, 1.f, .15f}));
    CHECK_FALSE(ValidReferenceExposure({.25f, 1.f, 65505.f}));
    for (size_t i = 0; i < 3; ++i) {
        auto value = std::array{.25f, .5f, .15f};
        value[i] = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(ValidReferenceExposure(value));
        value[i] = std::numeric_limits<float>::infinity();
        CHECK_FALSE(ValidReferenceExposure(value));
    }
}
