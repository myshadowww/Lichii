// Native-host-only exposure normalization. Vendor NrParams and shaders retain their ABI.
layout(std140, set = 0, binding = 7) uniform NativeExposure {
  vec4 nativeExposure; // base, extra, luminance cap; base zero disables
};

float nativeExposureLuminance(vec3 color) {
  return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 nativeExposureForward(vec3 color) {
  float y = nativeExposureLuminance(color);
  float weight = y > 0.0 ? clamp(nativeExposure.z / y, 0.0, 1.0) : 1.0;
  return color * (nativeExposure.x + weight * nativeExposure.y);
}

vec3 nativeExposureInverse(vec3 color) {
  // NR may move luminance across the knee; invert its new value, not the input's scale.
  color = max(color, vec3(0.0));
  float y = nativeExposureLuminance(color);
  if (y <= 0.0) return vec3(0.0);
  float lowSlope = nativeExposure.x + nativeExposure.y;
  float originalY = y <= lowSlope * nativeExposure.z
      ? y / lowSlope : (y - nativeExposure.y * nativeExposure.z) / nativeExposure.x;
  // The native scene surface is half-float; retain finite representable HDR on writeback.
  return clamp(color * (originalY / y), vec3(0.0), vec3(65504.0));
}
