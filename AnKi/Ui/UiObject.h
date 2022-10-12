// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Ui/Common.h>

namespace anki {

/// @addtogroup ui
/// @{

/// The base of all UI objects.
class UiObject
{
public:
	UiObject(UiManager* manager)
		: m_manager(manager)
	{
		ANKI_ASSERT(manager);
	}

	virtual ~UiObject() = default;

	HeapMemoryPool& getMemoryPool() const;

	void retain() const
	{
		m_refcount.fetchAdd(1);
	}

	I32 release() const
	{
		return m_refcount.fetchSub(1);
	}

	/// Set the global IMGUI allocator.
	void setImAllocator(BaseMemoryPool* pool = nullptr)
	{
		pool = (pool) ? pool : &getMemoryPool();

		auto allocCallback = [](size_t size, void* userData) -> void* {
			BaseMemoryPool* pool = static_cast<BaseMemoryPool*>(userData);
			return pool->allocate(size, 16);
		};

		auto freeCallback = [](void* ptr, void* userData) -> void {
			if(ptr)
			{
				BaseMemoryPool* pool = static_cast<BaseMemoryPool*>(userData);
				pool->free(ptr);
			}
		};

		ImGui::SetAllocatorFunctions(allocCallback, freeCallback, pool);
	}

	/// Unset the global IMGUI allocator.
	static void unsetImAllocator()
	{
		ImGui::SetAllocatorFunctions(nullptr, nullptr, nullptr);
	}

protected:
	UiManager* m_manager;
	mutable Atomic<I32> m_refcount = {0};
};
/// @}

} // end namespace anki
