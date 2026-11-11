// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "video_core/pokopia_upscale.h"
TEST_CASE("Pokopia captured cameras require the proven reverse-depth projection", "[pokopia_upscale]") {
const std::array<float,16> frame0{1.3579950333e+00f,0.0000000000e+00f,0.0000000000e+00f,0.0000000000e+00f,0.0000000000e+00f,2.4136774540e+00f,-1.1711016668e-05f,2.1068118513e-02f,0.0000000000e+00f,5.0862934440e-02f,5.5574096041e-04f,-9.9977797270e-01f,0.0000000000e+00f,-1.2255192566e+02f,9.8538131714e+00f,2.7299108887e+02f};
REQUIRE(VideoCommon::PokopiaUpscale::DecodeCamera(frame0));
const std::array<float,16> frame1{0.0000000000e+00f,0.0000000000e+00f,5.5586436065e-04f,-1.0000000000e+00f,0.0000000000e+00f,2.4142134190e+00f,0.0000000000e+00f,0.0000000000e+00f,-1.3579950333e+00f,0.0000000000e+00f,0.0000000000e+00f,0.0000000000e+00f,2.2723917969e+04f,-9.4456103516e+03f,1.1563736916e+01f,-2.8031628418e+03f};
REQUIRE(VideoCommon::PokopiaUpscale::DecodeCamera(frame1));
const std::array<float,16> frame2{1.2935868502e+00f,0.0000000000e+00f,-1.6915827291e-04f,3.0431574583e-01f,0.0000000000e+00f,2.4142131805e+00f,0.0000000000e+00f,0.0000000000e+00f,4.1325926781e-01f,0.0000000000e+00f,5.2950036479e-04f,-9.5257115364e-01f,-2.7716762695e+03f,-9.4456083984e+03f,3.8101291656e-01f,1.7314558594e+04f};
REQUIRE(VideoCommon::PokopiaUpscale::DecodeCamera(frame2));
const std::array<float,16> frame3{1.2935868502e+00f,0.0000000000e+00f,-1.6915827291e-04f,3.0431574583e-01f,0.0000000000e+00f,2.4142131805e+00f,0.0000000000e+00f,0.0000000000e+00f,4.1325926781e-01f,0.0000000000e+00f,5.2950036479e-04f,-9.5257115364e-01f,-2.7716762695e+03f,-9.4456083984e+03f,3.8101291656e-01f,1.7314558594e+04f};
REQUIRE(VideoCommon::PokopiaUpscale::DecodeCamera(frame3));
auto jittered=frame2; for(int c=0;c<4;++c) jittered[c*4]+=0.001f*jittered[c*4+3];
REQUIRE(!VideoCommon::PokopiaUpscale::DecodeCamera(jittered));
auto bad=frame2;bad[0]=std::numeric_limits<float>::quiet_NaN();
REQUIRE(!VideoCommon::PokopiaUpscale::DecodeCamera(bad));
auto scaled=frame2; for(auto& v:scaled) v*=2;
REQUIRE(!VideoCommon::PokopiaUpscale::DecodeCamera(scaled));
auto reflected=frame2;for(int c=0;c<4;++c) reflected[c*4]=-reflected[c*4];
REQUIRE(!VideoCommon::PokopiaUpscale::DecodeCamera(reflected));
auto wrong_depth=frame2;wrong_depth[14]+=1;
REQUIRE(!VideoCommon::PokopiaUpscale::DecodeCamera(wrong_depth));
const auto camera=VideoCommon::PokopiaUpscale::DecodeCamera(frame2).value();
// The captured convention projects the near plane to1 and far plane to0.
REQUIRE(std::abs((-10*camera.projection[10]+camera.projection[11])/10-1)<1e-6);
REQUIRE(std::abs((-18000*camera.projection[10]+camera.projection[11])/18000)<1e-6);
}
