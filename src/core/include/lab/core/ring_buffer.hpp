#pragma once

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lab::core {

// RingBuffer itself is intentionally not synchronized. A single owner can use it
// without overhead; shared owners (such as TimeSeriesStore) define locking policy.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : storage_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("RingBuffer capacity must be positive");
        }
    }

    void push(const T& value) {
        storage_[next_] = value;
        advance();
    }

    void push(T&& value) {
        storage_[next_] = std::move(value);
        advance();
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] const T& front() const {
        if (empty()) {
            throw std::out_of_range("RingBuffer::front on empty buffer");
        }
        const auto first = size_ == storage_.size() ? next_ : 0;
        return storage_[first];
    }

    [[nodiscard]] const T& back() const {
        if (empty()) {
            throw std::out_of_range("RingBuffer::back on empty buffer");
        }
        const auto index = (next_ + storage_.size() - 1) % storage_.size();
        return storage_[index];
    }

    void clear() noexcept {
        next_ = 0;
        size_ = 0;
    }

    [[nodiscard]] std::vector<T> snapshot() const {
        std::vector<T> result;
        result.reserve(size_);
        const auto first = size_ == storage_.size() ? next_ : 0;
        for (std::size_t index = 0; index < size_; ++index) {
            result.push_back(storage_[(first + index) % storage_.size()]);
        }
        return result;
    }

private:
    void advance() noexcept {
        next_ = (next_ + 1) % storage_.size();
        if (size_ < storage_.size()) {
            ++size_;
        }
    }

    std::vector<T> storage_;
    std::size_t next_{};
    std::size_t size_{};
};

}  // namespace lab::core

