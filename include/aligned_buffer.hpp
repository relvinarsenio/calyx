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
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

class AlignedBuffer {
    struct AlignedDeleter {
        std::size_t alignment {};
        void operator()(std::byte* ptr) const noexcept {
            if (ptr != nullptr) [[likely]] { ::operator delete(ptr, std::align_val_t { alignment }); }
        }
    };

    std::unique_ptr<std::byte[], AlignedDeleter> ptr_;
    std::size_t size_ = 0;

    explicit AlignedBuffer(std::byte* ptr, std::size_t size, std::size_t alignment) noexcept
        : ptr_(ptr, AlignedDeleter { alignment })
        , size_(size) {}

public:
    AlignedBuffer() noexcept
        : ptr_(nullptr, AlignedDeleter { 0 })
        , size_(0) {}

    AlignedBuffer(AlignedBuffer&& other) noexcept
        : ptr_(std::move(other.ptr_))
        , size_(std::exchange(other.size_, 0)) {}

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) [[likely]] {
            ptr_  = std::move(other.ptr_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    [[nodiscard]] std::byte* data() noexcept { return ptr_.get(); }
    [[nodiscard]] const std::byte* data() const noexcept { return ptr_.get(); }

    [[nodiscard]] static std::optional<AlignedBuffer> create(std::size_t size, std::size_t alignment) noexcept {
        if (size == 0) [[unlikely]] { return std::nullopt; }
        if (!std::has_single_bit(alignment)) [[unlikely]] { return std::nullopt; }

        if (void* const raw = ::operator new(size, std::align_val_t { alignment }, std::nothrow)) {
            std::byte* const typed = static_cast<std::byte*>(raw);
            return AlignedBuffer(typed, size, alignment);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::span<std::byte> span() noexcept { return { ptr_.get(), size_ }; }

    [[nodiscard]] std::span<const std::byte> span() const noexcept { return { ptr_.get(), size_ }; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
};
