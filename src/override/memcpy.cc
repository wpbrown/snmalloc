// Core implementation of snmalloc independent of the configuration mode
#include "../snmalloc_core.h"

#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
#  include "../backend/globalconfig.h"
// The default configuration for snmalloc is used if alternative not defined
namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::Globals>;
} // namespace snmalloc
#endif

// User facing API surface, needs to know what `Alloc` is.
#include "../snmalloc_front.h"

#include <errno.h>
#include <string.h>

using namespace snmalloc;

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif
#ifdef SNMALLOC_STATIC_LIBRARY_PREFIX
#  define __SN_CONCAT(a, b) a##b
#  define __SN_EVALUATE(a, b) __SN_CONCAT(a, b)
#  define SNMALLOC_NAME_MANGLE(a) \
    __SN_EVALUATE(SNMALLOC_STATIC_LIBRARY_PREFIX, a)
#elif !defined(SNMALLOC_NAME_MANGLE)
#  define SNMALLOC_NAME_MANGLE(a) a
#endif

// glibc lacks snprintf_l
#ifdef __linux__
#define snprintf_l(buf, size, loc, msg, ...) \
          snprintf(buf, size, msg, __VA_ARGS__)
#endif

namespace
{
  /**
   * Should we check loads?  This defaults to on in debug builds, off in
   * release (store-only checks)
   */
  static constexpr bool CheckReads =
#ifdef SNMALLOC_CHECK_LOADS
    SNMALLOC_CHECK_LOADS
#else
#  ifdef NDEBUG
    false
#  else
    true
#  endif
#endif
    ;

  /**
   * Should we fail fast when we encounter an error?  With this set to true, we
   * just issue a trap instruction and crash the process once we detect an
   * error. With it set to false we print a helpful error message and then crash
   * the process.  The process may be in an undefined state by the time the
   * check fails, so there are potentially security implications to turning this
   * off. It defaults to true for debug builds, false for release builds.
   */
  static constexpr bool FailFast =
#ifdef SNMALLOC_FAIL_FAST
    SNMALLOC_FAIL_FAST
#else
#  ifdef NDEBUG
    true
#  else
    false
#  endif
#endif
    ;

  /**
   * The largest register size that we can use for loads and stores.  These
   * types are expected to work for overlapping copies: we can always load them
   * into a register and store them.  Note that this is at the C abstract
   * machine level: the compiler may spill temporaries to the stack, just not
   * to the source or destination object.
   */
  static constexpr size_t LargestRegisterSize =
#ifdef __AVX__
    32
#elif defined(__SSE__)
    16
#else
    sizeof(uint64_t)
#endif
    ;

  /**
   * Helper that maps from a size to a type.
   */
  template<size_t Size>
  struct TypeForSizeHelper
  {
  private:
    /**
     * A vector type that is wide enough for the size.
     */
    using vector = __attribute__((vector_size(Size))) uint8_t;

  public:
    /**
     * If this vector size exists in our target, expose it, otherwise this will
     * expand to `void` and uses will fail.
     */
    using type = std::conditional_t<
      bits::is_pow2(Size) && (Size <= LargestRegisterSize),
      vector,
      void>;
  };

  /**
   * Concrete specialisation: 1 => uint8_t.
   */
  template<>
  struct TypeForSizeHelper<1>
  {
    using type = uint8_t;
  };

  /**
   * Concrete specialisation: 2 => uint16_t.
   */
  template<>
  struct TypeForSizeHelper<2>
  {
    using type = uint16_t;
  };

  /**
   * Concrete specialisation: 4 => uint32_t.
   */
  template<>
  struct TypeForSizeHelper<4>
  {
    using type = uint32_t;
  };

  /**
   * Concrete specialisation: 8 => uint64_t.
   */
  template<>
  struct TypeForSizeHelper<8>
  {
    using type = uint64_t;
  };

  /**
   * Wrapper that uses `TypeForSizeHelper` to map from a size in char units to
   * a type.
   */
  template<size_t Size>
  using TypeForSize = typename TypeForSizeHelper<Size>::type;

  /**
   * Copy a single element of a specified type.
   */
  template<typename T>
  SNMALLOC_FAST_PATH void copy_one(void* dst, const void* src)
  {
    *static_cast<T*>(dst) = *static_cast<const T*>(src);
  }

  /**
   * Check whether a pointer + length is in the same object as the pointer.
   * Fail with the error message from the third argument if not.
   *
   * The template parameter indicates whether this is a read.  If so, this
   * function is a no-op when `CheckReads` is false.
   */
  template<bool IsRead = false>
  SNMALLOC_FAST_PATH void
  check_bounds(const void* ptr, size_t len, const char* msg = "")
  {
    if constexpr (!IsRead || CheckReads)
    {
      if (unlikely(!call_is_initialised<snmalloc::Globals>(nullptr, 0)))
      {
        return;
      }

      auto& alloc = ThreadAlloc::get();
      void* p = const_cast<void*>(ptr);

      // FIXME: Overflow checking.
      if (likely(
            pointer_offset(ptr, len) > alloc.external_pointer<OnePastEnd>(p)))
      {
        if constexpr (FailFast)
        {
          __builtin_trap();
        }
        else
        {
          // We're going to crash the program now, but try to avoid heap
          // allocations if possible, since the heap may be in an undefined
          // state.
          std::array<char, 1024> buffer;
          snprintf_l(
            buffer.data(),
            buffer.size(),
            /* Force C locale */ nullptr,
            "%s: %p is in allocation %p--%p, offset 0x%zx is past the end.\n",
            msg,
            p,
            alloc.external_pointer<Start>(p),
            alloc.external_pointer<OnePastEnd>(p),
            len);
          Pal::error(buffer.data());
        }
      }
    }
  }

  /**
   * Copy a block using the specified type.  This copies as many complete
   * elements of type `T` as are possible from `len`.
   */
  template<typename T>
  SNMALLOC_FAST_PATH void block_copy(void* dst, const void* src, size_t len)
  {
    // Rounds down
    size_t count = len / sizeof(T);
    auto s = static_cast<const T*>(src);
    auto d = static_cast<T*>(dst);
    for (size_t i = 0; i < count; i += 1)
    {
      d[i] = s[i];
    }
  }

  /**
   * Perform an overlapping copy of the end.  This will copy one (potentially
   * unaligned) `T` from the end of the source to the end of the destination.
   * This may overlap other bits of the copy.
   */
  template<typename T>
  SNMALLOC_FAST_PATH void copy_end(void* dst, const void* src, size_t len)
  {
    copy_one<T>(
      pointer_offset(dst, len - sizeof(T)),
      pointer_offset(src, len - sizeof(T)));
  }

  /**
   * Predicate indicating whether the source and destination are sufficiently
   * aligned to be copied as aligned chunks of `Size` bytes.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH bool is_aligned_memcpy(void* dst, const void* src)
  {
    return (pointer_align_down<Size>(const_cast<void*>(src)) == src) &&
      (pointer_align_down<Size>(dst) == dst);
  }
}

extern "C"
{
  /**
   * Snmalloc checked memcpy.
   */
  SNMALLOC_EXPORT void* memcpy(void* dst, const void* src, size_t len)
  {
    // 0 is a very common size for memcpy and we don't need to do external
    // pointer checks if we hit it.  It's also the fastest case, to encourage
    // the compiler to favour the other cases.
    if (unlikely(len == 0))
      return dst;
    // Check the bounds of the arguments.
    check_bounds(
      dst, len, "memcpy with destination out of bounds of heap allocation");
    check_bounds<true>(
      src, len, "memcpy with destination out of bounds of heap allocation");
    // Handle some small common sizes with a jump table.
    switch (len)
    {
      case 1:
        copy_one<TypeForSize<1>>(dst, src);
        return dst;
      case 2:
        copy_one<TypeForSize<2>>(dst, src);
        return dst;
      case 4:
        copy_one<TypeForSize<4>>(dst, src);
        return dst;
      case 8:
        copy_one<TypeForSize<8>>(dst, src);
        return dst;
      case 16:
        // Only enable this and the larger vector sizes if we have a type that
        // handles them.
        if constexpr (LargestRegisterSize >= 16)
        {
          if (is_aligned_memcpy<16>(dst, src))
          {
            copy_one<TypeForSize<16>>(dst, src);
            return dst;
          }
        }
        break;
      case 32:
        if constexpr (LargestRegisterSize >= 32)
        {
          if (is_aligned_memcpy<32>(dst, src))
          {
            copy_one<TypeForSize<32>>(dst, src);
            return dst;
          }
        }
        break;
      case 64:
        if constexpr (LargestRegisterSize >= 64)
        {
          if (is_aligned_memcpy<64>(dst, src))
          {
            copy_one<TypeForSize<64>>(dst, src);
            return dst;
          }
        }
        break;
    }
    // If this is a small but weird size, do byte-by-byte copies.
    if (len < sizeof(uint64_t))
    {
      block_copy<uint8_t>(dst, src, len);
      return dst;
    }
    // If we have a useful vector size, try using it.
    if constexpr (LargestRegisterSize > sizeof(uint64_t))
    {
      // TODO: We're only copying strongly aligned things with vector
      // instructions.  We could do better by aligning the start and end.
      if (is_aligned_memcpy<LargestRegisterSize>(dst, src) && 0)
      {
        block_copy<TypeForSize<LargestRegisterSize>>(dst, src, len);
        size_t unaligned_tail = bits::align_down(len, LargestRegisterSize);
        void* dst_tail = pointer_offset(dst, unaligned_tail);
        void* src_tail = pointer_offset(src, unaligned_tail);
        size_t len_tail = len - unaligned_tail;
        block_copy<uint64_t>(dst_tail, src_tail, len_tail);
        copy_end<uint64_t>(dst_tail, src_tail, len_tail);
        return dst;
      }
    }
    // Copy in a loop of 8-byte copies.
    block_copy<uint64_t>(dst, src, len);
    // Branchless copy of the last 0-7 bytes.
    copy_end<uint64_t>(dst, src, len);
    return dst;
  }
}
