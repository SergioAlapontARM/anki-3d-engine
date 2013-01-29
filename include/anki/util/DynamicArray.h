#ifndef ANKI_UTIL_DYNAMIC_ARRAY_H
#define ANKI_UTIL_DYNAMIC_ARRAY_H

#include "anki/util/Allocator.h"

namespace anki {

/// @addtogroup util
/// @{
/// @addtogroup containers
/// @{

template<typename T, typename Alloc = Allocator<T>>
class DynamicArray
{
public:
	typedef T Value;
	typedef Value* Iterator;
	typedef const Value* ConstIterator;
	typedef Value& Reference;
	typedef const Value& ConstReference;

	/// @name Constructors/destructor
	/// @{

	/// Default
	DynamicArray()
	{}

	/// Copy is deleted
	DynamicArray(const DynamicArray&) = delete;

	/// Move
	DynamicArray(DynamicArray&& other)
		: b(other.e), e(other.e), allocator(other.allocator)
	{
		other.b = nullptr;
	}

	/// Allocate the array
	DynamicArray(const PtrSize n, const Alloc& alloc = Alloc())
		: allocator(alloc)
	{
		resize(n);
	}

	///
	DynamicArray(const Alloc& alloc)
		: allocator(alloc)
	{}

	/// Deallocate
	~DynamicArray()
	{
		clear();
	}
	/// @}

	Reference operator[](const PtrSize n)
	{
		ANKI_ASSERT(b != nullptr);
		ANKI_ASSERT(n < getSize());
		return *(begin + n);
	}

	ConstReference operator[](const PtrSize n) const
	{
		ANKI_ASSERT(b != nullptr);
		ANKI_ASSERT(n < getSize());
		return *(begin + n);
	}

	/// Copy is not permited
	DynamicArray& operator=(const Foo&) = delete;

	/// Move
	DynamicArray& operator=(DynamicArray&& other)
	{
		clear();

		b = other.b;
		e = other.e;
		allocator = b.allocator;

		other.b = nullptr;
		other.e = nullptr;

		return *this;
	}

	/// Make it compatible with the C++11 range based for loop
	Iterator begin()
	{
		ANKI_ASSERT(b != nullptr);
		return b;
	}

	/// Make it compatible with the C++11 range based for loop
	ConstIterator begin() const
	{
		ANKI_ASSERT(b != nullptr);
		return b;
	}

	/// Make it compatible with the C++11 range based for loop
	Iterator end()
	{
		ANKI_ASSERT(b != nullptr);
		return e;
	}

	/// Make it compatible with the C++11 range based for loop
	ConstIterator end() const
	{
		ANKI_ASSERT(b != nullptr);
		return e;
	}

	/// Get the elements count
	PtrSize getSize() const
	{
		return e - b;
	}

	/// Resize the array and call the constructors
	template<typename... Args>
	void resize(const PtrSize n, Args&&... args)
	{
		ANKI_ASSERT(b == nullptr && e == nullptr);
		ANKI_ASSERT(n > 0);

		// Allocate memory
		b = allocator.allocate(n);
		e = b + n;

		// Call the constructors
		for(PtrSize i = 0; i < n; i++)
		{
			allocator.construct(b + i, std::forward<Args>(args)...);
		}
	}

	/// Clear the array
	void clear()
	{
		if(b)
		{
			ANKI_ASSERT(e != nullptr);

			PtrSize n = getSize();
			ANKI_ASSERT(n > 0);

			// Call the destructors
			for(PtrSize i = 0; i < n; i++)
			{
				allocator.destroy(b + i);
			}

			// Deallocate the memory
			allocator.deallocate(b, n);

			// Reset the data
			b = nullptr;
			e = nullptr;
		}
	}

private:
	Value* b = nullptr;
	Value* e = nullptr;
	Alloc allocator;
};

/// @}
/// @}

} // end namespace anki

#endif