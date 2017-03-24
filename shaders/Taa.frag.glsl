// Copyright (C) 2009-2017, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include "shaders/Functions.glsl"
#include "shaders/Pack.glsl"
#include "shaders/Tonemapping.glsl"

#define YCBCR 1
const float BLEND_FACTOR = 1.0 / 8.0;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec3 out_color;

layout(ANKI_TEX_BINDING(0, 0)) uniform sampler2D u_depthRt;
layout(ANKI_TEX_BINDING(0, 1)) uniform sampler2D u_inputRt;
layout(ANKI_TEX_BINDING(0, 2)) uniform sampler2D u_historyRt;

layout(ANKI_UBO_BINDING(0, 0), std140, row_major) uniform u0_
{
	mat4 u_prevViewProjMatMulInvViewProjMat;
};

#if YCBCR
#define sample(s, uv) rgbToYCbCr(textureLod(s, uv, 0.0).rgb)
#define sampleOffset(s, uv, x, y) rgbToYCbCr(textureLodOffset(s, uv, 0.0, ivec2(x, y)).rgb)
#else
#define sample(s, uv) textureLod(s, uv, 0.0).rgb
#define sampleOffset(s, uv, x, y) textureLodOffset(s, uv, 0.0, ivec2(x, y)).rgb
#endif

void main()
{
	float depth = textureLod(u_depthRt, in_uv, 0.0).r;

	// Get prev uv coords
	vec4 v4 = u_prevViewProjMatMulInvViewProjMat * UV_TO_NDC(vec4(in_uv, depth, 1.0));
	vec2 oldUv = NDC_TO_UV(v4.xy / v4.w);

	// Read textures
	vec3 historyCol = sample(u_historyRt, oldUv);
	vec3 crntCol = sample(u_inputRt, in_uv);

	// Remove ghosting by clamping the history color to neighbour's AABB
	vec3 near0 = sampleOffset(u_inputRt, in_uv, 1, 0);
	vec3 near1 = sampleOffset(u_inputRt, in_uv, 0, 1);
	vec3 near2 = sampleOffset(u_inputRt, in_uv, -1, 0);
	vec3 near3 = sampleOffset(u_inputRt, in_uv, 0, -1);

	vec3 boxMin = min(crntCol, min(near0, min(near1, min(near2, near3))));
	vec3 boxMax = max(crntCol, max(near0, max(near1, max(near2, near3))));

	historyCol = clamp(historyCol, boxMin, boxMax);

// Remove jitter (T. Lottes)
#if YCBCR
	float lum0 = crntCol.r;
	float lum1 = historyCol.r;
	float maxLum = boxMax.r;
#else
	float lum0 = computeLuminance(crntCol);
	float lum1 = computeLuminance(historyCol);
	float maxLum = computeLuminance(boxMax);
#endif

	float diff = abs(lum0 - lum1) / max(lum0, max(lum1, maxLum));
	diff = 1.0 - diff;
	diff = diff * diff;
	float feedback = mix(0.0, BLEND_FACTOR, diff);

// Write result
#if YCBCR
	out_color = yCbCrToRgb(mix(historyCol, crntCol, feedback));
#else
	out_color = mix(historyCol, crntCol, feedback);
#endif
}