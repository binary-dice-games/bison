// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_serialization.hpp
 * @brief Binary serialization and deserialization backends for Bison objects.
 *
 * Provides two families of serializer/deserializer pairs:
 * - **Buffer** (`buffer_serializer` / `buffer_deserializer`) – operates on
 *   in-memory `std::vector<uint8_t>` buffers.
 * - **Stream** (`stream_serializer` / `stream_deserializer`) – wraps
 *   `std::ostream` / `std::istream` for file or network I/O.
 *
 * All values are written in **big-endian** (network) byte order using
 * `byte_swap` from `bison_common.hpp`.  Strings and vectors are prefixed with
 * a `size_t` element count (also big-endian).
 */

#pragma once

#include "src/core/bison_common.hpp"

namespace bdg::bison {

/**
 * @brief In-memory binary serializer that writes big-endian data to an
 *        internal byte buffer.
 *
 * Call `buffer()` to access the accumulated bytes without transferring
 * ownership, or `release()` to move the buffer out.  `buffer_serializer` is
 * move-only (not copyable).
 *
 * All `write` overloads return `*this` for fluent chaining.
 */
class buffer_serializer {
 public:
  /**
   * @brief Construct a serializer with a pre-allocated internal buffer.
   * @param initial_capacity  Number of bytes to reserve upfront (default 256).
   */
  explicit buffer_serializer(size_t initial_capacity = 256) {
    buf_.reserve(initial_capacity);
  }
  buffer_serializer(const buffer_serializer&) = delete;
  buffer_serializer(buffer_serializer&&) = default;

  /** @brief Read-only view of the accumulated serialized bytes. */
  const bdg::bison::buffer& buffer() const {
    return buf_;
  }

  /** @brief Move the internal buffer out, leaving the serializer empty. */
  bdg::bison::buffer release() {
    return std::move(buf_);
  }

  /**
   * @brief Write a scalar value in big-endian byte order.
   * @tparam T  A trivially-copyable scalar type.
   * @param  data  Value to write.
   * @return `*this` for chaining.
   */
  template <typename T>
  buffer_serializer& write(T data) {
    data = byte_swap(data);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&data);
    buf_.insert(buf_.end(), p, p + sizeof(T));
    return *this;
  }

  template <typename T>
  buffer_serializer& write(const std::vector<T>& data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    if constexpr (std::is_same_v<T, bool>) {
      for (bool elem : data) {
        unsigned char val = elem ? 1u : 0u;
        buf_.push_back(static_cast<uint8_t>(val));
      }
    } else if constexpr (sizeof(T) == 1) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size());
    } else if (endian::native == endian::big) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size() * sizeof(T));
    } else {
      for (const auto& elem : data) {
        T value = byte_swap(elem);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        buf_.insert(buf_.end(), p, p + sizeof(T));
      }
    }
    return *this;
  }

  template <typename T>
  buffer_serializer& write(std::span<const T> data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    if constexpr (sizeof(T) == 1) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size());
    } else if (endian::native == endian::big) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size() * sizeof(T));
    } else {
      for (const auto& elem : data) {
        T value = byte_swap(elem);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        buf_.insert(buf_.end(), p, p + sizeof(T));
      }
    }
    return *this;
  }

  buffer_serializer& write(const std::string& data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    buf_.insert(buf_.end(), data.begin(), data.end());
    return *this;
  }

  buffer_serializer& write(std::string_view data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    buf_.insert(buf_.end(), data.begin(), data.end());
    return *this;
  }

  buffer_serializer& write(const char* data, std::streamsize count) {
    auto begin = reinterpret_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), begin, begin + count);
    return *this;
  }

 private:
  bdg::bison::buffer buf_;
};

/**
 * @brief In-memory binary deserializer that reads big-endian data from a
 *        caller-supplied byte range.
 *
 * Constructed from a raw pointer + size, a `buffer`, or a `std::string`.
 * Reading past the end throws `std::runtime_error("buffer_deserializer:
 * buffer underflow")`.  `buffer_deserializer` is move-only (not copyable).
 */
class buffer_deserializer {
 public:
  buffer_deserializer(const char* data, size_t size)
      : begin_(reinterpret_cast<const uint8_t*>(data)),
        end_(begin_ + size),
        pos_(begin_) {}

  buffer_deserializer(const uint8_t* data, size_t size)
      : begin_(data), end_(data + size), pos_(data) {}

  explicit buffer_deserializer(const bdg::bison::buffer& buf)
      : buffer_deserializer(buf.data(), buf.size()) {}

  explicit buffer_deserializer(const std::string& buf)
      : buffer_deserializer(buf.data(), buf.size()) {}

  buffer_deserializer(const buffer_deserializer&) = delete;
  buffer_deserializer(buffer_deserializer&&) = default;

  /**
   * @brief Read and return a scalar value, advancing the read position.
   *
   * @tparam T  A trivially-copyable scalar type.
   * @return The deserialized value (byte-swapped from big-endian).
   * @throws std::runtime_error on buffer underflow.
   */
  template <typename T>
  T read() {
    if (pos_ + sizeof(T) > end_) {
      throw std::runtime_error("buffer_deserializer: buffer underflow");
    }
    T data{};
    std::memcpy(&data, pos_, sizeof(T));
    pos_ += sizeof(T);
    return byte_swap(data);
  }

  template <typename T>
  buffer_deserializer& read(T& data) {
    data = read<T>();
    return *this;
  }

  template <typename T>
  buffer_deserializer& read(std::vector<T>& data) {
    const size_t count = read<size_t>();
    data.resize(count);
    if constexpr (std::is_same_v<T, bool>) {
      for (size_t idx = 0; idx < count; ++idx) {
        data[idx] = (read<unsigned char>() != 0u);
      }
    } else if constexpr (sizeof(T) == 1) {
      if (pos_ + count > end_) {
        throw std::runtime_error("buffer_deserializer: buffer underflow");
      }
      std::memcpy(data.data(), pos_, count);
      pos_ += count;
    } else if (endian::native == endian::big) {
      const size_t bytes = count * sizeof(T);
      if (pos_ + bytes > end_) {
        throw std::runtime_error("buffer_deserializer: buffer underflow");
      }
      std::memcpy(data.data(), pos_, bytes);
      pos_ += bytes;
    } else {
      for (size_t idx = 0; idx < count; ++idx) {
        data[idx] = read<T>();
      }
    }
    return *this;
  }

  template <typename T>
  buffer_deserializer& read(std::span<T> data) {
    const size_t count = read<size_t>();
    if (count != data.size()) {
      throw std::runtime_error("buffer_deserializer: Invalid span size");
    }
    for (size_t idx = 0; idx < count; ++idx) {
      data[idx] = read<T>();
    }
    return *this;
  }

  buffer_deserializer& read(std::string& data) {
    const size_t count = read<size_t>();
    if (pos_ + count > end_) {
      throw std::runtime_error("buffer_deserializer: buffer underflow");
    }
    data.assign(reinterpret_cast<const char*>(pos_), count);
    pos_ += count;
    return *this;
  }

  buffer_deserializer& read(std::string_view& view, std::string& storage) {
    read(storage);
    view = storage;
    return *this;
  }

  buffer_deserializer& read(char* data, std::streamsize count) {
    if (pos_ + count > end_) {
      throw std::runtime_error("buffer_deserializer: buffer underflow");
    }
    std::memcpy(data, pos_, count);
    pos_ += count;
    return *this;
  }

 private:
  const uint8_t* begin_;
  const uint8_t* end_;
  const uint8_t* pos_;
};

/**
 * @brief Streaming binary serializer that writes big-endian data to a
 *        `std::ostream`.
 *
 * Internally delegates vector and string writes through a `buffer_serializer`
 * to reuse the endian-swapping logic.  Neither copyable nor movable.
 *
 * All `write` overloads return `*this` for fluent chaining.
 */
class stream_serializer {
 public:
  /**
   * @brief Construct a serializer that writes to @p out.
   * @param out  Output stream; must remain valid for the lifetime of this
   *             serializer.
   */
  stream_serializer(std::ostream& out) : out_(out) {}
  stream_serializer(const stream_serializer& that) = delete;
  stream_serializer(stream_serializer&& that) = delete;

  template <typename T>
  stream_serializer& write(T data) {
    data = byte_swap(data);
    out_.write(reinterpret_cast<const char*>(&data), sizeof(T));
    return *this;
  }

  template <typename T>
  stream_serializer& write(const std::vector<T>& data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  template <typename T>
  stream_serializer& write(std::span<const T> data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  stream_serializer& write(const std::string& data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  stream_serializer& write(std::string_view data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  stream_serializer& write(const char* data, std::streamsize count) {
    out_.write(data, count);
    return *this;
  }

 private:
  std::ostream& out_;
};

/**
 * @brief Streaming binary deserializer that reads big-endian data from a
 *        `std::istream`.
 *
 * Internally delegates vector and span reads through `buffer_deserializer`.
 * Neither copyable nor movable.
 */
class stream_deserializer {
 public:
  /**
   * @brief Construct a deserializer that reads from @p in.
   * @param in  Input stream; must remain valid for the lifetime of this
   *            deserializer.
   */
  stream_deserializer(std::istream& in) : in_(in) {}
  stream_deserializer(const stream_deserializer& that) = delete;
  stream_deserializer(stream_deserializer&& that) = delete;

  template <typename T>
  T read() {
    T data{};
    in_.read(reinterpret_cast<char*>(&data), sizeof(T));
    data = byte_swap(data);
    return data;
  }

  template <typename T>
  stream_deserializer& read(T& data) {
    in_.read(reinterpret_cast<char*>(&data), sizeof(T));
    data = byte_swap(data);
    return *this;
  }

  template <typename T>
  stream_deserializer& read(std::vector<T>& data) {
    size_t count_be = 0;
    in_.read(reinterpret_cast<char*>(&count_be), sizeof(size_t));
    const size_t count = byte_swap(count_be);

    size_t payload_size = 0;
    if constexpr (std::is_same_v<T, bool>) {
      payload_size = count;
    } else {
      payload_size = count * sizeof(T);
    }

    buffer chunk(sizeof(size_t) + payload_size);
    std::memcpy(chunk.data(), &count_be, sizeof(size_t));
    if (payload_size > 0) {
      in_.read(
          reinterpret_cast<char*>(chunk.data() + sizeof(size_t)),
          static_cast<std::streamsize>(payload_size));
    }

    buffer_deserializer buffered(chunk);
    buffered.read(data);
    return *this;
  }

  template <typename T>
  stream_deserializer& read(std::span<T> data) {
    size_t count_be = 0;
    in_.read(reinterpret_cast<char*>(&count_be), sizeof(size_t));
    const size_t count = byte_swap(count_be);
    if (count != data.size()) {
      throw std::runtime_error("Invalid span size");
    }

    const size_t payload_size = count * sizeof(T);
    buffer chunk(sizeof(size_t) + payload_size);
    std::memcpy(chunk.data(), &count_be, sizeof(size_t));
    if (payload_size > 0) {
      in_.read(
          reinterpret_cast<char*>(chunk.data() + sizeof(size_t)),
          static_cast<std::streamsize>(payload_size));
    }

    buffer_deserializer buffered(chunk);
    buffered.read(data);
    return *this;
  }

  stream_deserializer& read(std::string& data) {
    size_t count_be = 0;
    in_.read(reinterpret_cast<char*>(&count_be), sizeof(size_t));
    const size_t count = byte_swap(count_be);
    data.resize(count);
    if (count > 0) {
      in_.read(data.data(), static_cast<std::streamsize>(count));
    }
    return *this;
  }

  stream_deserializer& read(std::string_view& view, std::string& storage) {
    read(storage);
    view = storage;
    return *this;
  }

  stream_deserializer& read(char* data, std::streamsize count) {
    in_.read(data, count);
    return *this;
  }

 private:
  std::istream& in_;
};

} // namespace bdg::bison
