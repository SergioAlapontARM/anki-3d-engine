// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Scene/Common.h>
#include <AnKi/Math.h>
#include <AnKi/Collision/Plane.h>
#include <AnKi/Util/WeakArray.h>

namespace anki {

/// @addtogroup scene
/// @{

/// Software rasterizer for visibility tests.
class SoftwareRasterizer
{
public:
	SoftwareRasterizer()
	{
	}

	~SoftwareRasterizer()
	{
		m_zbuffer.destroy(*m_pool);
	}

	/// Initialize.
	void init(BaseMemoryPool* pool)
	{
		ANKI_ASSERT(pool);
		m_pool = pool;
	}

	/// Prepare for rendering. Call it before every draw.
	void prepare(const Mat4& mv, const Mat4& p, U32 width, U32 height);

	/// Render some verts.
	/// @param[in] verts Pointer to the first vertex to draw.
	/// @param vertCount The number of verts to draw.
	/// @param stride The stride (in bytes) of the next vertex.
	/// @param backfaceCulling If true it will do backface culling.
	/// @note It's thread-safe against other draw() invocations only.
	void draw(const F32* verts, U vertCount, U stride, Bool backfaceCulling);

	/// Fill the depth buffer with some values.
	void fillDepthBuffer(ConstWeakArray<F32> depthValues);

	/// Perform visibility tests.
	/// @param aabb The Aabb in of the cs in world space.
	/// @return Return true if it's visible and false otherwise.
	Bool visibilityTest(const Aabb& aabb) const;

private:
	BaseMemoryPool* m_pool = nullptr;
	Mat4 m_mv; ///< ModelView.
	Mat4 m_p; ///< Projection.
	Mat4 m_mvp;
	Array<Plane, 6> m_planesL; ///< In view space.
	Array<Plane, 6> m_planesW; ///< In world space.
	U32 m_width;
	U32 m_height;
	DynamicArray<Atomic<U32>> m_zbuffer;

	/// @param tri In clip space.
	void rasterizeTriangle(const Vec4* tri);

	Bool computeBarycetrinc(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& p, Vec3& uvw) const;

	/// Clip triangle in the near plane.
	/// @note Triangles in view space.
	void clipTriangle(const Vec4* inTriangle, Vec4* outTriangles, U& outTriangleCount) const;

	Bool visibilityTestInternal(const Aabb& aabb) const;
};
/// @}

} // end namespace anki
