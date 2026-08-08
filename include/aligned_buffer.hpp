/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

/**
 * @brief RAII container for over-aligned contiguous memory buffers.
 *
 * Designed specifically for O_DIRECT I/O operations requiring sector-aligned memory.
 * Implements standard contiguous container traits for STL compatibility.
 */
namespace memory {

/**
 * @brief Concepts for enforcing type safety in over-aligned memory operations.
 *
 * Constrains custom deleters and buffers to ensure they are only instantiated
 * for appropriate types (e.g., unbounded arrays or trivially destructible types).
 */
template <typename T>
concept unbounded_array = std::is_unbounded_array_v<T>;

template <typename T>
concept trivial = std::is_trivial_v<T>;

template <typename T>
concept trivially_destructible = std::is_trivially_destructible_v<T>;

template <trivial T> class BasicAlignedBuffer {
public:
    using value_type             = T;
    using size_type              = std::size_t;
    using difference_type        = std::ptrdiff_t;
    using pointer                = value_type*;
    using const_pointer          = const value_type*;
    using reference              = value_type&;
    using const_reference        = const value_type&;
    using iterator               = pointer;
    using const_iterator         = const_pointer;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    constexpr BasicAlignedBuffer() noexcept
        : ptr_(nullptr, AlignedDeleter { alignof(value_type) }) {}
    ~BasicAlignedBuffer() noexcept = default;

    BasicAlignedBuffer(const BasicAlignedBuffer&)            = delete;
    BasicAlignedBuffer& operator=(const BasicAlignedBuffer&) = delete;

    constexpr BasicAlignedBuffer(BasicAlignedBuffer&& other) noexcept
        : ptr_(std::move(other.ptr_))
        , size_(std::exchange(other.size_, 0uz)) {}

    constexpr BasicAlignedBuffer& operator=(BasicAlignedBuffer&& other) noexcept {
        if (this != &other) [[likely]] {
            ptr_  = std::move(other.ptr_);
            size_ = std::exchange(other.size_, 0uz);
        }
        return *this;
    }

    /**
     * @brief Factory method to create an AlignedBuffer safely.
     *
     * Ensures the alignment is valid and delegates memory allocation to make_unique_aligned_nothrow.
     * Returns an empty optional if allocation fails or if parameters are invalid.
     *
     * @param size Number of elements to allocate.
     * @param alignment Required memory alignment (must be a power of two).
     * @return std::optional<AlignedBuffer> containing the buffer, or std::nullopt on failure.
     */
    [[nodiscard]] static std::optional<BasicAlignedBuffer> create(size_type size, size_type alignment) noexcept {
        if (size == 0) [[unlikely]] { return std::nullopt; }
        if (!std::has_single_bit(alignment) || alignment < alignof(value_type)) [[unlikely]] { return std::nullopt; }

        auto smart_ptr = make_unique_aligned_nothrow(size, alignment);
        if (smart_ptr) { return BasicAlignedBuffer(std::move(smart_ptr), size); }
        return std::nullopt;
    }

    constexpr void swap(BasicAlignedBuffer& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(size_, other.size_);
    }

    template <typename Self>
    [[nodiscard]] constexpr auto data(this Self&& self) noexcept
        -> std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_pointer, pointer> {
        return self.ptr_.get();
    }

    template <typename Self>
    [[nodiscard]] constexpr auto begin(this Self&& self) noexcept
        -> std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator> {
        return self.ptr_.get();
    }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return ptr_.get(); }

    template <typename Self>
    [[nodiscard]] constexpr auto end(this Self&& self) noexcept
        -> std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator> {
        return self.ptr_.get() + self.size_;
    }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return ptr_.get() + size_; }

    template <typename Self> [[nodiscard]] constexpr auto rbegin(this Self&& self) noexcept {
        using IteratorType
            = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator>;
        return std::reverse_iterator<IteratorType>(self.end());
    }
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }

    template <typename Self> [[nodiscard]] constexpr auto rend(this Self&& self) noexcept {
        using IteratorType
            = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator>;
        return std::reverse_iterator<IteratorType>(self.begin());
    }
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin()); }

    template <typename Self>
    [[nodiscard]] constexpr auto operator[](this Self&& self, size_type pos) noexcept
        -> std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_reference, reference> {
        return self.ptr_[pos];
    }

    template <typename Self>
    [[nodiscard]] constexpr auto front(this Self&& self) noexcept
        -> std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_reference, reference> {
        return self.ptr_[0];
    }

    template <typename Self>
    [[nodiscard]] constexpr auto back(this Self&& self) noexcept
        -> std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_reference, reference> {
        return self.ptr_[self.size_ - 1];
    }

    [[nodiscard]] constexpr explicit operator bool(this auto&& self) noexcept { return self.ptr_ != nullptr; }

    [[nodiscard]] constexpr auto span(this auto&& self) noexcept {
        using ElementType = std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(self)>>,
            const value_type, value_type>;
        return std::span<ElementType> { self.ptr_.get(), self.size_ };
    }

    [[nodiscard]] constexpr size_type size(this auto&& self) noexcept { return self.size_; }
    [[nodiscard]] constexpr size_type max_size(this auto&&) noexcept {
        return size_type { std::numeric_limits<difference_type>::max() } / sizeof(value_type);
    }
    [[nodiscard]] constexpr bool empty(this auto&& self) noexcept { return self.size_ == 0uz; }

private:
    /**
     * @brief Custom deleter for over-aligned pointers.
     *
     * Ensures memory allocated via over-aligned new is correctly deallocated.
     */
    struct AlignedDeleter {
        size_type alignment { alignof(value_type) };
        void operator()(value_type* ptr) const noexcept {
            if (ptr != nullptr) [[likely]] {
                ::operator delete(static_cast<void*>(ptr), std::align_val_t { alignment });
            }
        }
    };

    /**
     * @brief Instantiates an over-aligned array with value-initialization semantics.
     *
     * Mirrors std::make_unique behavior but supports custom alignment and guarantees
     * zero-initialization of memory to prevent data leaks from uninitialized RAM.
     *
     * @param size Number of elements to allocate.
     * @param alignment Required memory alignment boundary (must be a power of two).
     * @return unique_ptr managing the aligned memory, or nullptr on allocation failure.
     */
    [[nodiscard]] static std::unique_ptr<value_type[], AlignedDeleter> make_unique_aligned_nothrow(
        size_type size, size_type alignment) noexcept {

        using ElementType = value_type;

        static constexpr size_type max_size
            = size_type { std::numeric_limits<difference_type>::max() } / sizeof(ElementType);

        if (size > max_size) [[unlikely]] { return { nullptr, AlignedDeleter { alignment } }; }

        const size_type total_bytes = size * sizeof(ElementType);

        if (void* const raw = ::operator new(total_bytes, std::align_val_t { alignment }, std::nothrow)) {
            std::uninitialized_value_construct_n(static_cast<ElementType*>(raw), size);
            return { static_cast<ElementType*>(raw), AlignedDeleter { alignment } };
        }
        return { nullptr, AlignedDeleter { alignment } };
    }

    constexpr explicit BasicAlignedBuffer(std::unique_ptr<value_type[], AlignedDeleter> ptr, size_type size) noexcept
        : ptr_(std::move(ptr))
        , size_(size) {}

    std::unique_ptr<value_type[], AlignedDeleter> ptr_;
    size_type size_ = 0uz;
};

template <trivial T> constexpr void swap(BasicAlignedBuffer<T>& lhs, BasicAlignedBuffer<T>& rhs) noexcept {
    lhs.swap(rhs);
}

using AlignedBuffer = BasicAlignedBuffer<std::byte>;

} // namespace memory
