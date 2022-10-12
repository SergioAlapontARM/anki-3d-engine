// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/Texture.h>
#include <AnKi/Gr/gl/GlObject.h>
#include <AnKi/Gr/Utils/Functions.h>
#include <AnKi/Util/HashMap.h>

namespace anki {

/// @addtogroup opengl
/// @{

/// Small wrapper on top of a texture view and its aspect.
struct MicroTextureView
{
	GLuint m_glName;
	DepthStencilAspectBit m_aspect;
};

/// Texture container.
class TextureImpl final : public Texture, public GlObject
{
public:
	GLenum m_target = GL_NONE; ///< GL_TEXTURE_2D, GL_TEXTURE_3D... etc.
	GLenum m_internalFormat = GL_NONE; ///< GL_COMPRESSED_RED, GL_RGB16 etc.
	GLenum m_glFormat = GL_NONE;
	GLenum m_glType = GL_NONE;
	U32 m_surfaceCountPerLevel = 0;
	U8 m_faceCount = 0; ///< 6 for cubes and 1 for the rest.
	Bool m_compressed = false;

	TextureImpl(GrManager* manager, CString name)
		: Texture(manager, name)
	{
	}

	~TextureImpl();

	/// Init some stuff.
	void preInit(const TextureInitInfo& init);

	/// Create the texture storage.
	void init(const TextureInitInfo& init);

	/// Write texture data.
	void copyFromBuffer(const TextureSubresourceInfo& subresource, GLuint pbo, PtrSize offset, PtrSize dataSize) const;

	/// Generate mipmaps.
	void generateMipmaps2d(const TextureViewImpl& view) const;

	void bind() const;

	void clear(const TextureSubresourceInfo& subresource, const ClearValue& clearValue) const;

	U computeSurfaceIdx(const TextureSurfaceInfo& surf) const;

	MicroTextureView getOrCreateView(const TextureSubresourceInfo& subresource) const;

	TextureType computeNewTexTypeOfSubresource(const TextureSubresourceInfo& subresource) const
	{
		ANKI_ASSERT(isSubresourceValid(subresource));
		return (textureTypeIsCube(m_texType) && subresource.m_faceCount != 6) ? TextureType::k2D : m_texType;
	}

private:
	mutable HashMap<TextureSubresourceInfo, MicroTextureView> m_viewsMap;
	mutable Mutex m_viewsMapMtx;
};
/// @}

} // end namespace anki
