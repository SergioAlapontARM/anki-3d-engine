// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma anki mutator ORIENTATION 0 1 2 // 0: VERTICAL, 1: HORIZONTAL, 2: BOX
#pragma anki mutator SAMPLE_COUNT 3 5 7 9 11 13 15
#pragma anki mutator COLOR_COMPONENTS 4 3 1

ANKI_SPECIALIZATION_CONSTANT_UVEC2(kTextureSize, 0u);

#include <AnKi/Shaders/Common.glsl>

#if ORIENTATION == 0
#	define VERTICAL 1
#elif ORIENTATION == 1
#	define HORIZONTAL 1
#else
#	define BOX 1
#endif

#if SAMPLE_COUNT < 3
#	error See file
#endif

#if defined(ANKI_COMPUTE_SHADER)
#	define USE_COMPUTE 1
const UVec2 kWorkgroupSize = UVec2(8u, 8u);
#else
#	define USE_COMPUTE 0
#endif

// Define some macros depending on the number of components
#if COLOR_COMPONENTS == 4
#	define COL_TYPE Vec4
#	define TEX_FETCH rgba
#	define TO_VEC4(x_) x_
#elif COLOR_COMPONENTS == 3
#	define COL_TYPE Vec3
#	define TEX_FETCH rgb
#	define TO_VEC4(x_) Vec4(x_, 0.0)
#elif COLOR_COMPONENTS == 1
#	define COL_TYPE F32
#	define TEX_FETCH r
#	define TO_VEC4(x_) Vec4(x_, 0.0, 0.0, 0.0)
#else
#	error See file
#endif

layout(set = 0, binding = 0) uniform sampler u_linearAnyClampSampler;
layout(set = 0, binding = 1) uniform texture2D u_inTex;
layout(set = 0, binding = 2) uniform texture2D u_depthTex;

#if USE_COMPUTE
layout(local_size_x = kWorkgroupSize.x, local_size_y = kWorkgroupSize.y, local_size_z = 1) in;
layout(set = 0, binding = 3) writeonly uniform image2D u_outImg;
#else
layout(location = 0) in Vec2 in_uv;
layout(location = 0) out COL_TYPE out_color;
#endif

F32 computeDepthWeight(F32 refDepth, F32 depth)
{
	const F32 diff = abs(refDepth - depth);
	const F32 weight = 1.0 / (kEpsilonf + diff);
	return sqrt(weight);
}

F32 readDepth(Vec2 uv)
{
	return textureLod(u_depthTex, u_linearAnyClampSampler, uv, 0.0).r;
}

void sampleTex(Vec2 uv, F32 refDepth, inout COL_TYPE col, inout F32 weight)
{
	const COL_TYPE color = textureLod(u_inTex, u_linearAnyClampSampler, uv, 0.0).TEX_FETCH;
	F32 w = computeDepthWeight(refDepth, readDepth(uv));
	col += color * w;
	weight += w;
}

void main()
{
	// Set UVs
#if USE_COMPUTE
	ANKI_BRANCH if(gl_GlobalInvocationID.x >= kTextureSize.x || gl_GlobalInvocationID.y >= kTextureSize.y)
	{
		// Out of bounds
		return;
	}

	const Vec2 uv = (Vec2(gl_GlobalInvocationID.xy) + 0.5) / Vec2(kTextureSize);
#else
	const Vec2 uv = in_uv;
#endif

	const Vec2 TEXEL_SIZE = 1.0 / Vec2(kTextureSize);

	// Sample
	COL_TYPE color = textureLod(u_inTex, u_linearAnyClampSampler, uv, 0.0).TEX_FETCH;
	const F32 refDepth = readDepth(uv);
	F32 weight = 1.0;

#if !defined(BOX)
	// Do seperable

#	if defined(HORIZONTAL)
#		define X_OR_Y x
#	else
#		define X_OR_Y y
#	endif

	Vec2 uvOffset = Vec2(0.0);
	uvOffset.X_OR_Y = 1.5 * TEXEL_SIZE.X_OR_Y;

	ANKI_UNROLL for(U32 i = 0u; i < (U32(SAMPLE_COUNT) - 1u) / 2u; ++i)
	{
		sampleTex(uv + uvOffset, refDepth, color, weight);
		sampleTex(uv - uvOffset, refDepth, color, weight);

		uvOffset.X_OR_Y += 2.0 * TEXEL_SIZE.X_OR_Y;
	}
#else
	// Do box

	const Vec2 OFFSET = 1.5 * TEXEL_SIZE;

	sampleTex(uv + Vec2(+OFFSET.x, +OFFSET.y), refDepth, color, weight);
	sampleTex(uv + Vec2(+OFFSET.x, -OFFSET.y), refDepth, color, weight);
	sampleTex(uv + Vec2(-OFFSET.x, +OFFSET.y), refDepth, color, weight);
	sampleTex(uv + Vec2(-OFFSET.x, -OFFSET.y), refDepth, color, weight);

	sampleTex(uv + Vec2(OFFSET.x, 0.0), refDepth, color, weight);
	sampleTex(uv + Vec2(0.0, OFFSET.y), refDepth, color, weight);
	sampleTex(uv + Vec2(-OFFSET.x, 0.0), refDepth, color, weight);
	sampleTex(uv + Vec2(0.0, -OFFSET.y), refDepth, color, weight);
#endif
	color = color / weight;

	// Write value
#if USE_COMPUTE
	imageStore(u_outImg, IVec2(gl_GlobalInvocationID.xy), TO_VEC4(color));
#else
	out_color = color;
#endif
}
