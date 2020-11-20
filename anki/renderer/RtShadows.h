// Copyright (C) 2009-2020, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <anki/Gr.h>
#include <anki/renderer/RendererObject.h>
#include <anki/resource/TextureResource.h>

namespace anki
{

/// @addtogroup renderer
/// @{

/// Similar to ShadowmapsResolve but it's using ray tracing.
class RtShadows : public RendererObject
{
public:
	RtShadows(Renderer* r)
		: RendererObject(r)
	{
		registerDebugRenderTarget("RtShadows");
	}

	~RtShadows();

	ANKI_USE_RESULT Error init(const ConfigSet& cfg);

	void populateRenderGraph(RenderingContext& ctx);

	void getDebugRenderTarget(CString rtName, RenderTargetHandle& handle) const override
	{
		ANKI_ASSERT(rtName == "RtShadows");
		handle = m_runCtx.m_renderRt;
	}

	RenderTargetHandle getRt() const
	{
		return m_runCtx.m_renderRt;
	}

public:
	ShaderProgramPtr m_grProg;
	Array<TexturePtr, 2> m_rtTextures;

	class
	{
	public:
		RenderingContext* m_ctx = nullptr;

		RenderTargetHandle m_renderRt;
		RenderTargetHandle m_historyRt;
		Bool m_rtsImportedOnce = false;

		BufferPtr m_sbtBuffer;
		PtrSize m_sbtOffset;
		U32 m_hitGroupCount = 0;
	} m_runCtx;

	U32 m_sbtRecordSize = 256;

	ANKI_USE_RESULT Error initInternal(const ConfigSet& cfg);

	void run(RenderPassWorkContext& rgraphCtx);

	void buildSbt();
};
/// @}

} // namespace anki