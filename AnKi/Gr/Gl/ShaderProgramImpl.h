// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/ShaderProgram.h>
#include <AnKi/Gr/gl/GlObject.h>

namespace anki {

/// @addtogroup opengl
/// @{

class ShaderProgramImplReflection
{
public:
	struct Uniform
	{
		I32 m_location;
		U32 m_pushConstantOffset;
		ShaderVariableDataType m_type;
		U8 m_arrSize;
	};

	DynamicArray<Uniform> m_uniforms;
	U32 m_uniformDataSize = 0;
};

/// Shader program implementation.
class ShaderProgramImpl final : public ShaderProgram, public GlObject
{
public:
	ShaderProgramImpl(GrManager* manager, CString name)
		: ShaderProgram(manager, name)
	{
	}

	~ShaderProgramImpl();

	ANKI_USE_RESULT Error initGraphics(ShaderPtr vert, ShaderPtr tessc, ShaderPtr tesse, ShaderPtr geom,
									   ShaderPtr frag);
	ANKI_USE_RESULT Error initCompute(ShaderPtr comp);

	// Do that only when is needed to avoid serializing the thread the driver is using for compilation.
	const ShaderProgramImplReflection& getReflection();

private:
	Array<ShaderPtr, U(ShaderType::kCount)> m_shaders;
	ShaderProgramImplReflection m_refl;
	Bool m_reflInitialized = false;

	ANKI_USE_RESULT Error link(GLuint vert, GLuint frag);
};
/// @}

} // end namespace anki
