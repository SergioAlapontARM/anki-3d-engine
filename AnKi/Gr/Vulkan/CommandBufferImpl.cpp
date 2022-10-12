// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Gr/Vulkan/CommandBufferImpl.h>
#include <AnKi/Gr/GrManager.h>
#include <AnKi/Gr/Vulkan/GrManagerImpl.h>
#include <AnKi/Gr/Framebuffer.h>
#include <AnKi/Gr/Vulkan/GrUpscalerImpl.h>
#include <AnKi/Gr/Vulkan/AccelerationStructureImpl.h>
#include <AnKi/Gr/Vulkan/FramebufferImpl.h>

#if ANKI_DLSS
#	include <ThirdParty/DlssSdk/sdk/include/nvsdk_ngx.h>
#	include <ThirdParty/DlssSdk/sdk/include/nvsdk_ngx_helpers.h>
#	include <ThirdParty/DlssSdk/sdk/include/nvsdk_ngx_vk.h>
#	include <ThirdParty/DlssSdk/sdk/include/nvsdk_ngx_helpers_vk.h>
#endif

// FSR 2.0 related
#include <ThirdParty/FidelityFX/fsr2/ffx-fsr2-api/ffx_fsr2.h>
#include <Anki/Gr/Vulkan/fsr2_vk/vk/AnkiFsr2.h>


#include <algorithm>

namespace anki {

CommandBufferImpl::~CommandBufferImpl()
{
	if(m_empty)
	{
		ANKI_VK_LOGW("Command buffer was empty");
	}

	if(!m_finalized)
	{
		ANKI_VK_LOGW("Command buffer was not flushed");
	}
}

Error CommandBufferImpl::init(const CommandBufferInitInfo& init)
{
	m_tid = Thread::getCurrentThreadId();
	m_flags = init.m_flags;

	ANKI_CHECK(getGrManagerImpl().getCommandBufferFactory().newCommandBuffer(m_tid, m_flags, m_microCmdb));
	m_handle = m_microCmdb->getHandle();

	m_pool = &m_microCmdb->getFastMemoryPool();

	// Store some of the init info for later
	if(!!(m_flags & CommandBufferFlag::kSecondLevel))
	{
		m_activeFb = init.m_framebuffer;
		m_colorAttachmentUsages = init.m_colorAttachmentUsages;
		m_depthStencilAttachmentUsage = init.m_depthStencilAttachmentUsage;
		m_state.beginRenderPass(static_cast<FramebufferImpl*>(m_activeFb.get()));
		m_microCmdb->pushObjectRef(m_activeFb);
	}

	for(DescriptorSetState& state : m_dsetState)
	{
		state.init(m_pool);
	}

	return Error::kNone;
}

void CommandBufferImpl::beginRecording()
{
	// Do the begin
	VkCommandBufferInheritanceInfo inheritance = {};
	inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin.pInheritanceInfo = &inheritance;

	if(!!(m_flags & CommandBufferFlag::kSecondLevel))
	{
		FramebufferImpl& impl = static_cast<FramebufferImpl&>(*m_activeFb);

		// Calc the layouts
		Array<VkImageLayout, kMaxColorRenderTargets> colAttLayouts;
		for(U i = 0; i < impl.getColorAttachmentCount(); ++i)
		{
			const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*impl.getColorAttachment(i));
			colAttLayouts[i] = view.getTextureImpl().computeLayout(m_colorAttachmentUsages[i], 0);
		}

		VkImageLayout dsAttLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
		if(impl.hasDepthStencil())
		{
			const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*impl.getDepthStencilAttachment());
			dsAttLayout = view.getTextureImpl().computeLayout(m_depthStencilAttachmentUsage, 0);
		}

		VkImageLayout sriAttachmentLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
		if(impl.hasSri())
		{
			// Technically it's possible for SRI to be in other layout. Don't bother though
			sriAttachmentLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
		}

		inheritance.renderPass = impl.getRenderPassHandle(colAttLayouts, dsAttLayout, sriAttachmentLayout);
		inheritance.subpass = 0;
		inheritance.framebuffer = impl.getFramebufferHandle();

		begin.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
	}

	vkBeginCommandBuffer(m_handle, &begin);

	// Stats
	if(!!(getGrManagerImpl().getExtensions() & VulkanExtensions::kKHR_pipeline_executable_properties))
	{
		m_state.setEnablePipelineStatistics(true);
	}
}

void CommandBufferImpl::beginRenderPassInternal(
	const FramebufferPtr& fb, const Array<TextureUsageBit, kMaxColorRenderTargets>& colorAttachmentUsages,
	TextureUsageBit depthStencilAttachmentUsage, U32 minx, U32 miny, U32 width, U32 height)
{
	commandCommon();
	ANKI_ASSERT(!insideRenderPass());

	m_rpCommandCount = 0;
	m_activeFb = fb;

	FramebufferImpl& fbimpl = static_cast<FramebufferImpl&>(*fb);

	U32 fbWidth, fbHeight;
	fbimpl.getAttachmentsSize(fbWidth, fbHeight);
	m_fbSize[0] = fbWidth;
	m_fbSize[1] = fbHeight;

	ANKI_ASSERT(minx < fbWidth && miny < fbHeight);

	const U32 maxx = min<U32>(minx + width, fbWidth);
	const U32 maxy = min<U32>(miny + height, fbHeight);
	width = maxx - minx;
	height = maxy - miny;
	ANKI_ASSERT(minx + width <= fbWidth && miny + height <= fbHeight);

	m_renderArea[0] = minx;
	m_renderArea[1] = miny;
	m_renderArea[2] = width;
	m_renderArea[3] = height;

	m_colorAttachmentUsages = colorAttachmentUsages;
	m_depthStencilAttachmentUsage = depthStencilAttachmentUsage;

	m_microCmdb->pushObjectRef(fb);

	m_subpassContents = VK_SUBPASS_CONTENTS_MAX_ENUM;

	// Re-set the viewport and scissor because sometimes they are set clamped
	m_viewportDirty = true;
	m_scissorDirty = true;
}

void CommandBufferImpl::beginRenderPassInternal()
{
	FramebufferImpl& impl = static_cast<FramebufferImpl&>(*m_activeFb);

	m_state.beginRenderPass(&impl);

	VkRenderPassBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	bi.clearValueCount = impl.getAttachmentCount();
	bi.pClearValues = impl.getClearValues();
	bi.framebuffer = impl.getFramebufferHandle();

	// Calc the layouts
	Array<VkImageLayout, kMaxColorRenderTargets> colAttLayouts;
	for(U i = 0; i < impl.getColorAttachmentCount(); ++i)
	{
		const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*impl.getColorAttachment(i));
		colAttLayouts[i] = view.getTextureImpl().computeLayout(m_colorAttachmentUsages[i], 0);
	}

	VkImageLayout dsAttLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
	if(impl.hasDepthStencil())
	{
		const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*impl.getDepthStencilAttachment());
		dsAttLayout = view.getTextureImpl().computeLayout(m_depthStencilAttachmentUsage, 0);
	}

	VkImageLayout sriAttachmentLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
	if(impl.hasSri())
	{
		// Technically it's possible for SRI to be in other layout. Don't bother though
		sriAttachmentLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	}

	bi.renderPass = impl.getRenderPassHandle(colAttLayouts, dsAttLayout, sriAttachmentLayout);

	const Bool flipvp = flipViewport();
	bi.renderArea.offset.x = m_renderArea[0];
	if(flipvp)
	{
		ANKI_ASSERT(m_renderArea[3] <= m_fbSize[1]);
	}
	bi.renderArea.offset.y = (flipvp) ? m_fbSize[1] - (m_renderArea[1] + m_renderArea[3]) : m_renderArea[1];
	bi.renderArea.extent.width = m_renderArea[2];
	bi.renderArea.extent.height = m_renderArea[3];

	getGrManagerImpl().beginMarker(m_handle, impl.getName(), Vec3(0.0f, 1.0f, 0.0f));

#if !ANKI_PLATFORM_MOBILE
	// nVidia SRI cache workaround
	if(impl.hasSri())
	{
		VkMemoryBarrier memBarrier = {};
		memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memBarrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;

		const VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
		const VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;

		vkCmdPipelineBarrier(m_handle, srcStages, dstStages, 0, 1, &memBarrier, 0, nullptr, 0, nullptr);
	}
#endif

	VkSubpassBeginInfo subpassBeginInfo = {};
	subpassBeginInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
	subpassBeginInfo.contents = m_subpassContents;

	vkCmdBeginRenderPass2KHR(m_handle, &bi, &subpassBeginInfo);

	m_renderedToDefaultFb = m_renderedToDefaultFb || impl.hasPresentableTexture();
}

void CommandBufferImpl::endRenderPassInternal()
{
	commandCommon();
	ANKI_ASSERT(insideRenderPass());
	if(m_rpCommandCount == 0)
	{
		// Empty pass
		m_subpassContents = VK_SUBPASS_CONTENTS_INLINE;
		beginRenderPassInternal();
	}

	VkSubpassEndInfo subpassEndInfo = {};
	subpassEndInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;

	vkCmdEndRenderPass2KHR(m_handle, &subpassEndInfo);
	getGrManagerImpl().endMarker(m_handle);

	m_activeFb.reset(nullptr);
	m_state.endRenderPass();

	// After pushing second level command buffers the state is undefined. Reset the tracker and rebind the dynamic state
	if(m_subpassContents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS)
	{
		m_state.reset();
		rebindDynamicState();
	}
}

void CommandBufferImpl::endRecording()
{
	commandCommon();

	ANKI_ASSERT(!m_finalized);
	ANKI_ASSERT(!m_empty);

	ANKI_VK_CHECKF(vkEndCommandBuffer(m_handle));
	m_finalized = true;

#if ANKI_EXTRA_CHECKS
	static Atomic<U32> messagePrintCount(0);
	constexpr U32 MAX_PRINT_COUNT = 10;

	CString message;
	if(!!(m_flags & CommandBufferFlag::kSmallBatch))
	{
		if(m_commandCount > kCommandBufferSmallBatchMaxCommands * 4)
		{
			message = "Command buffer has too many commands%s: %u";
		}
	}
	else
	{
		if(m_commandCount <= kCommandBufferSmallBatchMaxCommands / 4)
		{
			message = "Command buffer has too few commands%s: %u";
		}
	}

	if(!message.isEmpty())
	{
		const U32 count = messagePrintCount.fetchAdd(1) + 1;
		if(count < MAX_PRINT_COUNT)
		{
			ANKI_VK_LOGW(message.cstr(), "", m_commandCount);
		}
		else if(count == MAX_PRINT_COUNT)
		{
			ANKI_VK_LOGW(message.cstr(), " (will ignore further warnings)", m_commandCount);
		}
	}
#endif
}

void CommandBufferImpl::generateMipmaps2dInternal(const TextureViewPtr& texView)
{
	commandCommon();

	const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*texView);
	const TextureImpl& tex = view.getTextureImpl();
	ANKI_ASSERT(tex.getTextureType() != TextureType::k3D && "Not for 3D");
	ANKI_ASSERT(tex.isSubresourceGoodForMipmapGeneration(view.getSubresource()));

	const U32 blitCount = tex.getMipmapCount() - 1u;
	if(blitCount == 0)
	{
		// Nothing to be done, flush the previous commands though because you may batch (and sort) things you shouldn't
		return;
	}

	const DepthStencilAspectBit aspect = view.getSubresource().m_depthStencilAspect;
	const U32 face = view.getSubresource().m_firstFace;
	const U32 layer = view.getSubresource().m_firstLayer;

	for(U32 i = 0; i < blitCount; ++i)
	{
		// Transition source
		// OPT: Combine the 2 barriers
		if(i > 0)
		{
			VkImageSubresourceRange range;
			tex.computeVkImageSubresourceRange(TextureSubresourceInfo(TextureSurfaceInfo(i, 0, face, layer), aspect),
											   range);

			setImageBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
							VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
							VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.m_imageHandle,
							range);
		}

		// Transition destination
		{
			VkImageSubresourceRange range;
			tex.computeVkImageSubresourceRange(
				TextureSubresourceInfo(TextureSurfaceInfo(i + 1, 0, face, layer), aspect), range);

			setImageBarrier(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
							VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
							VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, tex.m_imageHandle, range);
		}

		// Setup the blit struct
		I32 srcWidth = tex.getWidth() >> i;
		I32 srcHeight = tex.getHeight() >> i;

		I32 dstWidth = tex.getWidth() >> (i + 1);
		I32 dstHeight = tex.getHeight() >> (i + 1);

		ANKI_ASSERT(srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0);

		U32 vkLayer = 0;
		switch(tex.getTextureType())
		{
		case TextureType::k2D:
		case TextureType::k2DArray:
			break;
		case TextureType::kCube:
			vkLayer = face;
			break;
		case TextureType::kCubeArray:
			vkLayer = layer * 6 + face;
			break;
		default:
			ANKI_ASSERT(0);
			break;
		}

		VkImageBlit blit;
		blit.srcSubresource.aspectMask = convertImageAspect(aspect);
		blit.srcSubresource.baseArrayLayer = vkLayer;
		blit.srcSubresource.layerCount = 1;
		blit.srcSubresource.mipLevel = i;
		blit.srcOffsets[0] = {0, 0, 0};
		blit.srcOffsets[1] = {srcWidth, srcHeight, 1};

		blit.dstSubresource.aspectMask = convertImageAspect(aspect);
		blit.dstSubresource.baseArrayLayer = vkLayer;
		blit.dstSubresource.layerCount = 1;
		blit.dstSubresource.mipLevel = i + 1;
		blit.dstOffsets[0] = {0, 0, 0};
		blit.dstOffsets[1] = {dstWidth, dstHeight, 1};

		vkCmdBlitImage(m_handle, tex.m_imageHandle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.m_imageHandle,
					   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
					   (!!aspect) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	}

	// Hold the reference
	m_microCmdb->pushObjectRef(texView);
}

void CommandBufferImpl::copyBufferToTextureViewInternal(const BufferPtr& buff, PtrSize offset,
														[[maybe_unused]] PtrSize range, const TextureViewPtr& texView)
{
	commandCommon();

	const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*texView);
	const TextureImpl& tex = view.getTextureImpl();
	ANKI_ASSERT(tex.usageValid(TextureUsageBit::kTransferDestination));
	ANKI_ASSERT(tex.isSubresourceGoodForCopyFromBuffer(view.getSubresource()));
	const VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	const Bool is3D = tex.getTextureType() == TextureType::k3D;
	const VkImageAspectFlags aspect = convertImageAspect(view.getSubresource().m_depthStencilAspect);

	const TextureSurfaceInfo surf(view.getSubresource().m_firstMipmap, view.getSubresource().m_firstFace, 0,
								  view.getSubresource().m_firstLayer);
	const TextureVolumeInfo vol(view.getSubresource().m_firstMipmap);

	// Compute the sizes of the mip
	const U32 width = tex.getWidth() >> surf.m_level;
	const U32 height = tex.getHeight() >> surf.m_level;
	ANKI_ASSERT(width && height);
	const U32 depth = (is3D) ? (tex.getDepth() >> surf.m_level) : 1u;

	if(!is3D)
	{
		ANKI_ASSERT(range == computeSurfaceSize(width, height, tex.getFormat()));
	}
	else
	{
		ANKI_ASSERT(range == computeVolumeSize(width, height, depth, tex.getFormat()));
	}

	// Copy
	VkBufferImageCopy region;
	region.imageSubresource.aspectMask = aspect;
	region.imageSubresource.baseArrayLayer = (is3D) ? tex.computeVkArrayLayer(vol) : tex.computeVkArrayLayer(surf);
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = surf.m_level;
	region.imageOffset = {0, 0, 0};
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = depth;
	region.bufferOffset = offset;
	region.bufferImageHeight = 0;
	region.bufferRowLength = 0;

	vkCmdCopyBufferToImage(m_handle, static_cast<const BufferImpl&>(*buff).getHandle(), tex.m_imageHandle, layout, 1,
						   &region);

	m_microCmdb->pushObjectRef(texView);
	m_microCmdb->pushObjectRef(buff);
}

void CommandBufferImpl::rebindDynamicState()
{
	m_viewportDirty = true;
	m_lastViewport = {};
	m_scissorDirty = true;
	m_lastScissor = {};
	m_vrsRateDirty = true;
	m_vrsRate = VrsRate::k1x1;

	// Rebind the stencil compare mask
	if(m_stencilCompareMasks[0] == m_stencilCompareMasks[1])
	{
		vkCmdSetStencilCompareMask(m_handle, VK_STENCIL_FACE_FRONT_BIT | VK_STENCIL_FACE_BACK_BIT,
								   m_stencilCompareMasks[0]);
	}
	else
	{
		vkCmdSetStencilCompareMask(m_handle, VK_STENCIL_FACE_FRONT_BIT, m_stencilCompareMasks[0]);
		vkCmdSetStencilCompareMask(m_handle, VK_STENCIL_FACE_BACK_BIT, m_stencilCompareMasks[1]);
	}

	// Rebind the stencil write mask
	if(m_stencilWriteMasks[0] == m_stencilWriteMasks[1])
	{
		vkCmdSetStencilWriteMask(m_handle, VK_STENCIL_FACE_FRONT_BIT | VK_STENCIL_FACE_BACK_BIT,
								 m_stencilWriteMasks[0]);
	}
	else
	{
		vkCmdSetStencilWriteMask(m_handle, VK_STENCIL_FACE_FRONT_BIT, m_stencilWriteMasks[0]);
		vkCmdSetStencilWriteMask(m_handle, VK_STENCIL_FACE_BACK_BIT, m_stencilWriteMasks[1]);
	}

	// Rebind the stencil reference
	if(m_stencilReferenceMasks[0] == m_stencilReferenceMasks[1])
	{
		vkCmdSetStencilReference(m_handle, VK_STENCIL_FACE_FRONT_BIT | VK_STENCIL_FACE_BACK_BIT,
								 m_stencilReferenceMasks[0]);
	}
	else
	{
		vkCmdSetStencilReference(m_handle, VK_STENCIL_FACE_FRONT_BIT, m_stencilReferenceMasks[0]);
		vkCmdSetStencilReference(m_handle, VK_STENCIL_FACE_BACK_BIT, m_stencilReferenceMasks[1]);
	}
}

void CommandBufferImpl::buildAccelerationStructureInternal(const AccelerationStructurePtr& as)
{
	commandCommon();

	// Get objects
	const AccelerationStructureImpl& asImpl = static_cast<AccelerationStructureImpl&>(*as);

	// Create the scrach buffer
	BufferInitInfo bufferInit;
	bufferInit.m_usage = PrivateBufferUsageBit::kAccelerationStructureBuildScratch;
	bufferInit.m_size = asImpl.getBuildScratchBufferSize();
	BufferPtr scratchBuff = getManager().newBuffer(bufferInit);

	// Create the build info
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
	VkAccelerationStructureBuildRangeInfoKHR rangeInfo;
	asImpl.generateBuildInfo(scratchBuff->getGpuAddress(), buildInfo, rangeInfo);

	// Do the command
	Array<const VkAccelerationStructureBuildRangeInfoKHR*, 1> pRangeInfos = {&rangeInfo};
	vkCmdBuildAccelerationStructuresKHR(m_handle, 1, &buildInfo, &pRangeInfos[0]);

	// Push refs
	m_microCmdb->pushObjectRef(as);
	m_microCmdb->pushObjectRef(scratchBuff);
}

#if ANKI_DLSS
/// Utility function to get the NGX's resource structure for a texture
/// @param[in] tex the texture to generate the NVSDK_NGX_Resource_VK from
static NVSDK_NGX_Resource_VK getNGXResourceFromAnkiTexture(const TextureViewImpl& view)
{
	const TextureImpl& tex = view.getTextureImpl();

	const VkImageView imageView = view.getHandle();
	const VkFormat format = tex.m_vkFormat;
	const VkImage image = tex.m_imageHandle;
	const VkImageSubresourceRange subresourceRange = view.getVkImageSubresourceRange();
	const Bool isUAV = !!(tex.m_vkUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT);

	// TODO Not sure if I should pass the width,height of the image or the view
	return NVSDK_NGX_Create_ImageView_Resource_VK(imageView, image, subresourceRange, format, tex.getWidth(),
												  tex.getHeight(), isUAV);
}
#endif

#if ANKI_DLSS
void upscaleDLSS(const VkCommandBuffer cmdbuff, const anki::GrUpscalerPtr& upscaler, const anki::TextureViewPtr& inColor,
				 const anki::TextureViewPtr& outUpscaledColor, const anki::TextureViewPtr& motionVectors,
				 const anki::TextureViewPtr& depth, const anki::TextureViewPtr& exposure,
				 const anki::Vec2& jitterOffset, const anki::Bool& resetAccumulation,
				 const anki::Vec2& motionVectorsScale)
{
	ANKI_ASSERT(getGrManagerImpl().getDeviceCapabilities().m_dlss);
	ANKI_ASSERT(upscaler->getUpscalerType() == GrUpscalerType::kDlss2);

	commandCommon();

	const GrUpscalerImpl& upscalerImpl = static_cast<const GrUpscalerImpl&>(*upscaler);

	const TextureViewImpl& srcViewImpl = static_cast<const TextureViewImpl&>(*inColor);
	const TextureViewImpl& dstViewImpl = static_cast<const TextureViewImpl&>(*outUpscaledColor);
	const TextureViewImpl& mvViewImpl = static_cast<const TextureViewImpl&>(*motionVectors);
	const TextureViewImpl& depthViewImpl = static_cast<const TextureViewImpl&>(*depth);
	const TextureViewImpl& exposureViewImpl = static_cast<const TextureViewImpl&>(*exposure);

	NVSDK_NGX_Resource_VK srcResVk = getNGXResourceFromAnkiTexture(srcViewImpl);
	NVSDK_NGX_Resource_VK dstResVk = getNGXResourceFromAnkiTexture(dstViewImpl);
	NVSDK_NGX_Resource_VK mvResVk = getNGXResourceFromAnkiTexture(mvViewImpl);
	NVSDK_NGX_Resource_VK depthResVk = getNGXResourceFromAnkiTexture(depthViewImpl);
	NVSDK_NGX_Resource_VK exposureResVk = getNGXResourceFromAnkiTexture(exposureViewImpl);

	const U32 mipLevel = srcViewImpl.getSubresource().m_firstMipmap;
	const NVSDK_NGX_Coordinates renderingOffset = {0, 0};
	const NVSDK_NGX_Dimensions renderingSize = {srcViewImpl.getTextureImpl().getWidth() >> mipLevel,
												srcViewImpl.getTextureImpl().getHeight() >> mipLevel};

	NVSDK_NGX_VK_DLSS_Eval_Params vkDlssEvalParams;
	memset(&vkDlssEvalParams, 0, sizeof(vkDlssEvalParams));
	vkDlssEvalParams.Feature.pInColor = &srcResVk;
	vkDlssEvalParams.Feature.pInOutput = &dstResVk;
	vkDlssEvalParams.pInDepth = &depthResVk;
	vkDlssEvalParams.pInMotionVectors = &mvResVk;
	vkDlssEvalParams.pInExposureTexture = &exposureResVk;
	vkDlssEvalParams.InJitterOffsetX = jitterOffset.x();
	vkDlssEvalParams.InJitterOffsetY = jitterOffset.y();
	vkDlssEvalParams.InReset = resetAccumulation;
	vkDlssEvalParams.InMVScaleX = motionVectorsScale.x();
	vkDlssEvalParams.InMVScaleY = motionVectorsScale.y();
	vkDlssEvalParams.InColorSubrectBase = renderingOffset;
	vkDlssEvalParams.InDepthSubrectBase = renderingOffset;
	vkDlssEvalParams.InTranslucencySubrectBase = renderingOffset;
	vkDlssEvalParams.InMVSubrectBase = renderingOffset;
	vkDlssEvalParams.InRenderSubrectDimensions = renderingSize;

	NVSDK_NGX_Parameter* dlssParameters = &upscalerImpl.getParameters();
	NVSDK_NGX_Handle* dlssFeature = &upscalerImpl.getFeature();
	const NVSDK_NGX_Result result =
		NGX_VULKAN_EVALUATE_DLSS_EXT(cmdbuff, dlssFeature, dlssParameters, &vkDlssEvalParams);
	

	if(NVSDK_NGX_FAILED(result))
	{
		ANKI_VK_LOGF("Failed to NVSDK_NGX_VULKAN_EvaluateFeature for DLSS, code = 0x%08x, info: %ls", result,
					 GetNGXResultAsString(result));
	}
}
#endif // end ANKI_DLSS

void upscaleFsr2(const CommandBufferPtr& cmdbuff, const anki::GrUpscalerPtr& upscaler,
				 const anki::TextureViewPtr& inColor, const anki::TextureViewPtr& outUpscaledColor,
				 const anki::TextureViewPtr& motionVectors, const anki::TextureViewPtr& depth,
				 const anki::TextureViewPtr& exposure, const anki::Vec2& jitterOffset,
				 const anki::Bool& resetAccumulation, const anki::Vec2& motionVectorsScale, const F32 cameraNear,
				 const F32 cameraFar, const F32 cameraFovV)
{
	const GrUpscalerImpl& upscalerImpl = static_cast<const GrUpscalerImpl&>(*upscaler);

	const TextureViewImpl& srcViewImpl = static_cast<const TextureViewImpl&>(*inColor);
	const TextureViewImpl& dstViewImpl = static_cast<const TextureViewImpl&>(*outUpscaledColor);
	const TextureViewImpl& mvViewImpl = static_cast<const TextureViewImpl&>(*motionVectors);
	const TextureViewImpl& depthViewImpl = static_cast<const TextureViewImpl&>(*depth);
	const TextureViewImpl& exposureViewImpl = static_cast<const TextureViewImpl&>(*exposure);

	const U32 mipLevel = srcViewImpl.getSubresource().m_firstMipmap;
	const UVec2 renderingSize = {srcViewImpl.getTextureImpl().getWidth() >> mipLevel,
								 srcViewImpl.getTextureImpl().getHeight() >> mipLevel};
	const UVec2 upscaledSize = {dstViewImpl.getTextureImpl().getWidth() >> mipLevel,
								dstViewImpl.getTextureImpl().getHeight() >> mipLevel};

	FfxFsr2Context& fsrCtx = upscalerImpl.getFsr2Ctx();

	FfxFsr2DispatchDescription params{};

	params.commandList = ffxGetCommandList(cmdbuff);

	params.color = ffxGetTextureResource(srcViewImpl.getTexture(), inColor, renderingSize.x(), renderingSize.y(),
										 L"FSR2_InputColor");
	params.depth = ffxGetTextureResource(depthViewImpl.getTexture(), depth, renderingSize.x(), renderingSize.y(),
										 L"FSR2_InputDepth");
	params.motionVectors = ffxGetTextureResource(mvViewImpl.getTexture(), motionVectors, renderingSize.x(),
												 renderingSize.y(), L"FSR2_InputMotionVectors");
	params.exposure = ffxGetTextureResource(exposureViewImpl.getTexture(), exposure, 1, 1, L"FSR2_InputExposure",
											FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	params.output =
		ffxGetTextureResource(dstViewImpl.getTexture(), outUpscaledColor, upscaledSize.x(), upscaledSize.y(),
							  L"FSR2_OutputUpscaledColor", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

	// Do not use reactive or transparencyAndComposite masks, sub-optimal path (quality/performance tradeoff)
	params.reactive.resource = nullptr;
	params.transparencyAndComposition.resource = nullptr;

	params.jitterOffset.x = jitterOffset.x();
	params.jitterOffset.y = jitterOffset.y();
	params.motionVectorScale.x = motionVectorsScale.x();
	params.motionVectorScale.y = motionVectorsScale.y();
	params.reset = resetAccumulation;
	params.enableSharpening = false; // We will do sharpening later
	params.sharpness = 0.0f;
	params.frameTimeDelta = 0.0f; // This should be only used for auto-exposure (which we don't use)
	params.preExposure = 1.0f;
	params.renderSize.width = renderingSize.x();
	params.renderSize.height = renderingSize.y();
	params.cameraFar = cameraFar;
	params.cameraNear = cameraNear;
	params.cameraFovAngleVertical = cameraFovV;

	FfxErrorCode errorCode = ffxFsr2ContextDispatch(&fsrCtx, &params);
	FFX_ASSERT(errorCode == FFX_OK);

	// Move ExposureAndAvgLum1x1 back to the original layout
	const TextureSurfaceInfo surf(0, 0, 0, 0);

	Array<TextureBarrierInfo, 1> textureBarriers;
	textureBarriers[0] = {exposureViewImpl.getTexture().get(), TextureUsageBit::kSampledCompute,
						  TextureUsageBit::kImageComputeRead};

	cmdbuff->setPipelineBarrier(ConstWeakArray<TextureBarrierInfo>(textureBarriers),
								ConstWeakArray<BufferBarrierInfo>(nullptr, 0),
								ConstWeakArray<AccelerationStructureBarrierInfo>(nullptr, 0));
}

void CommandBufferImpl::upscaleInternal(const GrUpscalerPtr& upscaler, const TextureViewPtr& inColor,
										const TextureViewPtr& outUpscaledColor, const TextureViewPtr& motionVectors,
										const TextureViewPtr& depth, const TextureViewPtr& exposure,
										const Bool resetAccumulation, const Vec2& jitterOffset,
										const Vec2& motionVectorsScale, [[maybe_unused]] const F32 cameraNear,
										[[maybe_unused]] const F32 cameraFar, [[maybe_unused]] const F32 cameraFovV)
{
	commandCommon();
	
	if(upscaler->getUpscalerType() == GrUpscalerType::kDlss2)
	{
#if ANKI_DLSS
		ANKI_ASSERT(getGrManagerImpl().getDeviceCapabilities().m_dlss);

		getGrManagerImpl().beginMarker(m_handle, "DLSS");
		upscaleDLSS(m_handle, upscaler, inColor, outUpscaledColor, motionVectors, depth, exposure, jitterOffset,
					resetAccumulation, motionVectorsScale);
		getGrManagerImpl().endMarker(m_handle);

#else
		ANKI_ASSERT(0 && "Not supported");
		(void)upscaler;
		(void)inColor;
		(void)outUpscaledColor;
		(void)motionVectors;
		(void)depth;
		(void)exposure;
		(void)resetAccumulation;
		(void)jitterOffset;
		(void)motionVectorsScale;
#endif
	}

	if(upscaler->getUpscalerType() == GrUpscalerType::kFsr2)
	{
		getGrManagerImpl().beginMarker(m_handle, "FSR2");
		upscaleFsr2(CommandBufferPtr(this), upscaler, inColor, outUpscaledColor, motionVectors, depth, exposure,
					jitterOffset,
					resetAccumulation, motionVectorsScale, cameraNear, cameraFar, cameraFovV);
		getGrManagerImpl().endMarker(m_handle);
	}
}

void CommandBufferImpl::setPipelineBarrierInternal(
	ConstWeakArray<TextureBarrierInfo> textures, ConstWeakArray<BufferBarrierInfo> buffers,
	ConstWeakArray<AccelerationStructureBarrierInfo> accelerationStructures)
{
	commandCommon();

	DynamicArrayRaii<VkImageMemoryBarrier> imageBarriers(m_pool);
	DynamicArrayRaii<VkBufferMemoryBarrier> bufferBarriers(m_pool);
	DynamicArrayRaii<VkMemoryBarrier> genericBarriers(m_pool);
	VkPipelineStageFlags srcStageMask = 0;
	VkPipelineStageFlags dstStageMask = 0;

	for(const TextureBarrierInfo& barrier : textures)
	{
		ANKI_ASSERT(barrier.m_texture);
		const TextureImpl& impl = static_cast<const TextureImpl&>(*barrier.m_texture);
		const TextureUsageBit nextUsage = barrier.m_nextUsage;
		const TextureUsageBit prevUsage = barrier.m_previousUsage;
		TextureSubresourceInfo subresource = barrier.m_subresource;

		ANKI_ASSERT(impl.usageValid(prevUsage));
		ANKI_ASSERT(impl.usageValid(nextUsage));
		ANKI_ASSERT(((nextUsage & TextureUsageBit::kGenerateMipmaps) == TextureUsageBit::kGenerateMipmaps
					 || (nextUsage & TextureUsageBit::kGenerateMipmaps) == TextureUsageBit::kNone)
					&& "GENERATE_MIPMAPS should be alone");
		ANKI_ASSERT(impl.isSubresourceValid(subresource));

		if(ANKI_UNLIKELY(subresource.m_firstMipmap > 0 && nextUsage == TextureUsageBit::kGenerateMipmaps))
		{
			// This transition happens inside CommandBufferImpl::generateMipmapsX. No need to do something
			continue;
		}

		if(ANKI_UNLIKELY(nextUsage == TextureUsageBit::kGenerateMipmaps))
		{
			// The transition of the non zero mip levels happens inside CommandBufferImpl::generateMipmapsX so limit the
			// subresource

			ANKI_ASSERT(subresource.m_firstMipmap == 0 && subresource.m_mipmapCount == 1);
		}

		VkImageMemoryBarrier& inf = *imageBarriers.emplaceBack();
		inf = {};
		inf.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		inf.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		inf.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		inf.image = impl.m_imageHandle;

		impl.computeVkImageSubresourceRange(subresource, inf.subresourceRange);

		VkPipelineStageFlags srcStage;
		VkPipelineStageFlags dstStage;
		impl.computeBarrierInfo(prevUsage, nextUsage, inf.subresourceRange.baseMipLevel, srcStage, inf.srcAccessMask,
								dstStage, inf.dstAccessMask);
		inf.oldLayout = impl.computeLayout(prevUsage, inf.subresourceRange.baseMipLevel);
		inf.newLayout = impl.computeLayout(nextUsage, inf.subresourceRange.baseMipLevel);

		srcStageMask |= srcStage;
		dstStageMask |= dstStage;

		m_microCmdb->pushObjectRef(barrier.m_texture);
	}

	for(const BufferBarrierInfo& barrier : buffers)
	{
		ANKI_ASSERT(barrier.m_buffer);
		const BufferImpl& impl = static_cast<const BufferImpl&>(*barrier.m_buffer);

		const BufferUsageBit prevUsage = barrier.m_previousUsage;
		const BufferUsageBit nextUsage = barrier.m_nextUsage;

		VkBufferMemoryBarrier& inf = *bufferBarriers.emplaceBack();
		inf = {};
		inf.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		inf.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		inf.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		inf.buffer = impl.getHandle();

		ANKI_ASSERT(barrier.m_offset < impl.getSize());
		inf.offset = barrier.m_offset;

		if(barrier.m_size == kMaxPtrSize)
		{
			inf.size = VK_WHOLE_SIZE;
		}
		else
		{
			ANKI_ASSERT(barrier.m_size > 0);
			ANKI_ASSERT(barrier.m_offset + barrier.m_size <= impl.getSize());
			inf.size = barrier.m_size;
		}

		VkPipelineStageFlags srcStage;
		VkPipelineStageFlags dstStage;
		impl.computeBarrierInfo(prevUsage, nextUsage, srcStage, inf.srcAccessMask, dstStage, inf.dstAccessMask);

		srcStageMask |= srcStage;
		dstStageMask |= dstStage;

		m_microCmdb->pushObjectRef(barrier.m_buffer);
	}

	for(const AccelerationStructureBarrierInfo& barrier : accelerationStructures)
	{
		ANKI_ASSERT(barrier.m_as);

		VkMemoryBarrier& inf = *genericBarriers.emplaceBack();
		inf = {};
		inf.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;

		VkPipelineStageFlags srcStage;
		VkPipelineStageFlags dstStage;
		AccelerationStructureImpl::computeBarrierInfo(barrier.m_previousUsage, barrier.m_nextUsage, srcStage,
													  inf.srcAccessMask, dstStage, inf.dstAccessMask);

		srcStageMask |= srcStage;
		dstStageMask |= dstStage;

		m_microCmdb->pushObjectRef(barrier.m_as);
	}

	vkCmdPipelineBarrier(m_handle, srcStageMask, dstStageMask, 0, genericBarriers.getSize(),
						 (genericBarriers.getSize()) ? &genericBarriers[0] : nullptr, bufferBarriers.getSize(),
						 (bufferBarriers.getSize()) ? &bufferBarriers[0] : nullptr, imageBarriers.getSize(),
						 (imageBarriers.getSize()) ? &imageBarriers[0] : nullptr);

	ANKI_TRACE_INC_COUNTER(VK_PIPELINE_BARRIERS, 1);
}

} // end namespace anki
