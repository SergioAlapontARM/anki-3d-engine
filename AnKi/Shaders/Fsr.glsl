// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma anki mutator SHARPEN 0 1
#pragma anki mutator FSR_QUALITY 0 1

#include <AnKi/Shaders/Functions.glsl>

layout(set = 0, binding = 0) uniform sampler u_linearAnyClampSampler;
layout(set = 0, binding = 1) uniform ANKI_RP texture2D u_tex;
#if defined(ANKI_COMPUTE_SHADER)
layout(set = 0, binding = 2) writeonly uniform ANKI_RP image2D u_outImg;
layout(local_size_x = 8, local_size_y = 8) in;
#else
layout(location = 0) out ANKI_RP Vec3 out_color;
#endif

layout(push_constant, std430) uniform b_pc
{
	UVec4 u_fsrConsts0;
	UVec4 u_fsrConsts1;
	UVec4 u_fsrConsts2;
	UVec4 u_fsrConsts3;
	UVec2 u_viewportSize;
	UVec2 u_padding;
};

// FSR begin
#define A_GPU 1
#define A_GLSL 1
#define A_HALF 1
#include <ThirdParty/FidelityFX/fsr1/ffx_a.h>

#if SHARPEN
#	define FSR_RCAS_H 1

AH4 FsrRcasLoadH(ASW2 p)
{
	return AH4(texelFetch(sampler2D(u_tex, u_linearAnyClampSampler), ASU2(p), 0));
}

void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b)
{
}

#else // !SHARPEN
#	define FSR_EASU_H 1

AH4 FsrEasuRH(AF2 p)
{
	return AH4(textureGather(sampler2D(u_tex, u_linearAnyClampSampler), p, 0));
}

AH4 FsrEasuGH(AF2 p)
{
	return AH4(textureGather(sampler2D(u_tex, u_linearAnyClampSampler), p, 1));
}

AH4 FsrEasuBH(AF2 p)
{
	return AH4(textureGather(sampler2D(u_tex, u_linearAnyClampSampler), p, 2));
}

AH3 FsrEasuSampleH(AF2 p)
{
	return AH3(textureLod(u_tex, u_linearAnyClampSampler, p, 0.0).xyz);
}
#endif

#include <ThirdParty/FidelityFX/fsr1/ffx_fsr1.h>
// FSR end

void main()
{
#if defined(ANKI_COMPUTE_SHADER)
	if(skipOutOfBoundsInvocations(UVec2(8u), u_viewportSize))
	{
		return;
	}

	const UVec2 uv = gl_GlobalInvocationID.xy;
#else
	const UVec2 uv = UVec2(gl_FragCoord.xy);
#endif

	HVec3 color;
#if SHARPEN
	FsrRcasH(color.r, color.g, color.b, uv, u_fsrConsts0);
#elif FSR_QUALITY == 0
	FsrEasuL(color, uv, u_fsrConsts0, u_fsrConsts1, u_fsrConsts2, u_fsrConsts3);
#else
	FsrEasuH(color, uv, u_fsrConsts0, u_fsrConsts1, u_fsrConsts2, u_fsrConsts3);
#endif

#if defined(ANKI_COMPUTE_SHADER)
	imageStore(u_outImg, IVec2(gl_GlobalInvocationID.xy), Vec4(color, 0.0));
#else
	out_color = Vec3(color);
#endif
}
