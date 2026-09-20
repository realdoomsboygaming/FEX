// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/fextl/allocator.h>
#include <FEXCore/fextl/list.h>

#if defined(FEX_IOS_HOST)
#include <cstddef>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#else
#include <memory_resource>
#endif
#include <fmt/format.h>

namespace fextl {
namespace pmr {
#if defined(FEX_IOS_HOST)
  // Apple's libc++ does not ship std::pmr until iOS 17. The headers do not
  // diagnose this when back-deploying, so using std::pmr here would produce a
  // launch-time dyld failure on iOS 16. Keep FEX's small PMR surface local to
  // the process instead of depending on the system C++ runtime.
  class memory_resource {
  public:
    virtual ~memory_resource() = default;

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
      return do_allocate(bytes, alignment);
    }

    void deallocate(void* p, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
      do_deallocate(p, bytes, alignment);
    }

    bool is_equal(const memory_resource& other) const noexcept {
      return this == &other || do_is_equal(other);
    }

  private:
    virtual void* do_allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) = 0;
    virtual bool do_is_equal(const memory_resource& other) const noexcept = 0;
  };

  FEX_DEFAULT_VISIBILITY memory_resource* get_default_resource();

  template<class T>
  class polymorphic_allocator {
  public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using void_pointer = void*;
    using const_void_pointer = const void*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;

    template<class U>
    struct rebind {
      using other = polymorphic_allocator<U>;
    };

    explicit polymorphic_allocator(memory_resource* resource = nullptr) noexcept
      : Resource {resource ? resource : get_default_resource()} {}

    template<class U>
    polymorphic_allocator(const polymorphic_allocator<U>& other) noexcept
      : Resource {other.resource()} {}

    pointer allocate(size_type count) {
      return static_cast<pointer>(Resource->allocate(count * sizeof(T), alignof(T)));
    }

    void deallocate(pointer p, size_type count) noexcept {
      Resource->deallocate(p, count * sizeof(T), alignof(T));
    }

    template<class U, class... Args>
    void construct(U* p, Args&&... args) {
      ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template<class U>
    void destroy(U* p) {
      p->~U();
    }

    memory_resource* resource() const noexcept {
      return Resource;
    }

    template<class U>
    U* allocate_object(size_type count = 1) {
      return static_cast<U*>(Resource->allocate(count * sizeof(U), alignof(U)));
    }

    template<class U, class... Args>
    U* new_object(Args&&... args) {
      U* result = allocate_object<U>();
      construct(result, std::forward<Args>(args)...);
      return result;
    }

    template<class U>
    void delete_object(U* p) {
      destroy(p);
      Resource->deallocate(p, sizeof(U), alignof(U));
    }

  private:
    memory_resource* Resource;

    template<class U>
    friend class polymorphic_allocator;
  };

  template<class T, class U>
  bool operator==(const polymorphic_allocator<T>& lhs, const polymorphic_allocator<U>& rhs) noexcept {
    return lhs.resource()->is_equal(*rhs.resource());
  }

  template<class T, class U>
  bool operator!=(const polymorphic_allocator<T>& lhs, const polymorphic_allocator<U>& rhs) noexcept {
    return !(lhs == rhs);
  }

  template<class T, class Allocator = polymorphic_allocator<T>>
  using list = std::list<T, Allocator>;

  template<class Key, class T, class Compare = std::less<Key>,
           class Allocator = polymorphic_allocator<std::pair<const Key, T>>>
  using map = std::map<Key, T, Compare, Allocator>;
#else
  using memory_resource = std::pmr::memory_resource;
  template<class T>
  using polymorphic_allocator = std::pmr::polymorphic_allocator<T>;
  template<class T>
  using list = std::pmr::list<T>;
  template<class Key, class T, class Compare = std::less<Key>>
  using map = std::pmr::map<Key, T, Compare>;
  FEX_DEFAULT_VISIBILITY memory_resource* get_default_resource();
#endif

  class default_resource : public memory_resource {
  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
      return FEXCore::Allocator::memalign(alignment, bytes);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
      return FEXCore::Allocator::aligned_free(p);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
      return this == &other;
    }
  };

  /**
   * @brief A `std::pmr::monotonic_buffer_resource` compatible class.
   *
   * Allocates internal buffers on page boundaries and names them for buffer tracking.
   */
  class named_monotonic_page_buffer_resource final : public memory_resource {
  public:
    explicit named_monotonic_page_buffer_resource(const char* Name)
      : Name {Name} {}

    void release() noexcept {
      for (auto& Iter : Buffers) {
        FEXCore::Allocator::VirtualFree(Iter.Buffer, Iter.BufferSize);
      }
      Buffers.clear();

      CurrentBufferRemaining = 0;
      CurrentAllocationSize = FEXCore::Utils::FEX_PAGE_SIZE;
    }

  protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
      LOGMAN_THROW_A_FMT(bytes != 0, "Nope");
      LOGMAN_THROW_A_FMT(alignment <= FEXCore::Utils::FEX_PAGE_SIZE, "Nope");

      // Wow, an actual use case of std::align in the wild.
      void* NewPointer = std::align(alignment, bytes, CurrentBuffer, CurrentBufferRemaining);
      if (!NewPointer) [[unlikely]] {
        AllocateNewBuffer(bytes, alignment);
        NewPointer = CurrentBuffer;
      }

      CurrentBuffer = static_cast<char*>(CurrentBuffer) + bytes;
      CurrentBufferRemaining -= bytes;

      return NewPointer;
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {
      // Explicit no-op.
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
      return this == &other;
    }

  private:
    const char* Name;

    // Allocate a new buffer that can at least fit the passed in bytes with alignment.
    void AllocateNewBuffer(std::size_t bytes, std::size_t) {
      bytes = FEXCore::AlignUp(bytes, CurrentAllocationSize);
      void* Ptr = FEXCore::Allocator::VirtualAlloc(bytes);
      if (Name) {
        FEXCore::Allocator::VirtualName(Name, Ptr, bytes);
      }

      Buffers.emplace_back(BufferData {
        .Buffer = Ptr,
        .BufferSize = bytes,
      });

      CurrentBuffer = Ptr;
      CurrentBufferRemaining = bytes;

      // Multiply the allocation size by 1.5 for the next allocation
      // Avoid double math because of ugly conversions.
      CurrentAllocationSize = FEXCore::AlignUp(CurrentAllocationSize + (CurrentAllocationSize >> 1), FEXCore::Utils::FEX_PAGE_SIZE);
    }

    // Current buffer management.
    void* CurrentBuffer {};
    size_t CurrentBufferRemaining {};

    struct BufferData final {
      void* Buffer;
      size_t BufferSize;
    };

    fextl::list<BufferData> Buffers {};

    size_t CurrentAllocationSize = FEXCore::Utils::FEX_PAGE_SIZE;
  };

  /**
   * @brief This is similar to the std::pmr::monotonic_buffer_resource.
   *
   * The difference is that class doesn't have ownership of the backing memory and
   * it also doesn't have any growth factor.
   *
   * If the amount of memory allocated is overrun then this will overwrite memory unless assertions are enabled.
   *
   * Ensure that you know how much memory you're going to use before using this class.
   */
  class fixed_size_monotonic_buffer_resource final : public memory_resource {
  public:
    fixed_size_monotonic_buffer_resource(void* Base, [[maybe_unused]] size_t Size)
      : Ptr {reinterpret_cast<uint64_t>(Base)}
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
      , PtrEnd {reinterpret_cast<uint64_t>(Base) + Size}
      , Size {Size}
#endif
    {
    }
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
      uint64_t NewPtr = FEXCore::AlignUp((uint64_t)Ptr, alignment);
      Ptr = NewPtr + bytes;
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
      if (Ptr >= PtrEnd) {
        LogMan::Msg::AFmt("Fail: Only allocated: {} ({} this time) bytes. Tried allocating at ptr offset: {}.\n", Size, bytes,
                          (uint64_t)(Ptr - (PtrEnd - Size)));
        FEX_TRAP_EXECUTION;
      }
#endif
      return reinterpret_cast<void*>(NewPtr);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
      // noop
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
      return this == &other;
    }
  private:
    uint64_t Ptr;
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    uint64_t PtrEnd;
    size_t Size;
#endif
  };
} // namespace pmr
} // namespace fextl
