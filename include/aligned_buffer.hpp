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
namespace aligned_buffer_impl {

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

/**
 * @brief Custom deleter for over-aligned pointers.
 *
 * Ensures memory allocated via over-aligned new is correctly deallocated.
 */
template <unbounded_array T> struct AlignedDeleter;

/**
 * @brief Specialization of AlignedDeleter for unbounded arrays (T[]), used by unique_ptr<T[]>.
 */
template <trivially_destructible T> struct AlignedDeleter<T[]> {
    std::size_t alignment {};
    void operator()(T* ptr) const noexcept {
        if (ptr != nullptr) [[likely]] { ::operator delete(static_cast<void*>(ptr), std::align_val_t { alignment }); }
    }
};

/**
 * @brief Instantiates an over-aligned array with value-initialization semantics.
 *
 * Mirrors std::make_unique behavior but supports custom alignment and guarantees
 * zero-initialization of memory to prevent data leaks from uninitialized RAM.
 *
 * @tparam T Unbounded array type to allocate (e.g., std::byte[]).
 * @param size Number of elements to allocate.
 * @param alignment Required memory alignment boundary (must be a power of two).
 * @return unique_ptr managing the aligned memory, or nullptr on allocation failure.
 */
template <unbounded_array T>
    requires trivial<std::remove_extent_t<T>>
[[nodiscard]] std::unique_ptr<T, AlignedDeleter<T>> make_unique_aligned_nothrow(
    std::size_t size, std::size_t alignment) noexcept {

    using ElementType = std::remove_extent_t<T>;

    static constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max() / sizeof(ElementType);

    if (size > max_size) [[unlikely]] { return { nullptr, AlignedDeleter<T> { alignment } }; }

    const std::size_t total_bytes = size * sizeof(ElementType);

    if (void* const raw = ::operator new(total_bytes, std::align_val_t { alignment }, std::nothrow)) {
        std::uninitialized_value_construct_n(static_cast<ElementType*>(raw), size);
        return { static_cast<ElementType*>(raw), AlignedDeleter<T> { alignment } };
    }
    return { nullptr, AlignedDeleter<T> { alignment } };
}

} // namespace aligned_buffer_impl

template <aligned_buffer_impl::trivial T> class BasicAlignedBuffer {
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

private:
    std::unique_ptr<value_type[], aligned_buffer_impl::AlignedDeleter<value_type[]>> ptr_;
    size_type size_ = 0;

    constexpr explicit BasicAlignedBuffer(
        std::unique_ptr<value_type[], aligned_buffer_impl::AlignedDeleter<value_type[]>> ptr, size_type size) noexcept
        : ptr_(std::move(ptr))
        , size_(size) {}

public:
    constexpr BasicAlignedBuffer() noexcept
        : ptr_(nullptr, aligned_buffer_impl::AlignedDeleter<value_type[]> { alignof(value_type) }) {}

    constexpr BasicAlignedBuffer(BasicAlignedBuffer&& other) noexcept
        : ptr_(std::move(other.ptr_))
        , size_(std::exchange(other.size_, 0)) {}

    constexpr BasicAlignedBuffer& operator=(BasicAlignedBuffer&& other) noexcept {
        if (this != &other) [[likely]] {
            ptr_  = std::move(other.ptr_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    constexpr void swap(BasicAlignedBuffer& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(size_, other.size_);
    }

    BasicAlignedBuffer(const BasicAlignedBuffer&)            = delete;
    BasicAlignedBuffer& operator=(const BasicAlignedBuffer&) = delete;

    [[nodiscard]] constexpr pointer data() noexcept { return ptr_.get(); }
    [[nodiscard]] constexpr const_pointer data() const noexcept { return ptr_.get(); }

    [[nodiscard]] constexpr iterator begin() noexcept { return ptr_.get(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return ptr_.get(); }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return ptr_.get(); }

    [[nodiscard]] constexpr iterator end() noexcept { return ptr_.get() + size_; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return ptr_.get() + size_; }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return ptr_.get() + size_; }

    [[nodiscard]] constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }

    [[nodiscard]] constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin()); }

    [[nodiscard]] constexpr reference operator[](size_type pos) noexcept { return ptr_[pos]; }
    [[nodiscard]] constexpr const_reference operator[](size_type pos) const noexcept { return ptr_[pos]; }

    [[nodiscard]] constexpr reference front() noexcept { return ptr_[0]; }
    [[nodiscard]] constexpr const_reference front() const noexcept { return ptr_[0]; }

    [[nodiscard]] constexpr reference back() noexcept { return ptr_[size_ - 1]; }
    [[nodiscard]] constexpr const_reference back() const noexcept { return ptr_[size_ - 1]; }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

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

        auto smart_ptr = aligned_buffer_impl::make_unique_aligned_nothrow<value_type[]>(size, alignment);
        if (smart_ptr) { return BasicAlignedBuffer(std::move(smart_ptr), size); }
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::span<value_type> span() noexcept { return { ptr_.get(), size_ }; }

    [[nodiscard]] constexpr std::span<const value_type> span() const noexcept { return { ptr_.get(), size_ }; }

    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(value_type);
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
};

template <aligned_buffer_impl::trivial T>
constexpr void swap(BasicAlignedBuffer<T>& lhs, BasicAlignedBuffer<T>& rhs) noexcept {
    lhs.swap(rhs);
}

using AlignedBuffer = BasicAlignedBuffer<std::byte>;

} // namespace memory
