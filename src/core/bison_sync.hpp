// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#pragma once

#include "src/core/bison_common.hpp"

namespace bdg::bison {

template <typename T, typename LockType>
class locked_ptr {
 public:
  locked_ptr() = default;
  locked_ptr(T* data, LockType lock) : data_(data), lock_(std::move(lock)) {}

  locked_ptr(const locked_ptr&) = delete;
  locked_ptr& operator=(const locked_ptr&) = delete;
  locked_ptr(locked_ptr&&) = default;
  locked_ptr& operator=(locked_ptr&&) = default;

  T* operator->() const {
    return data_;
  }

  T& operator*() const {
    return *data_;
  }

  void unlock() {
    lock_.unlock();
    data_ = nullptr;
  }

  bool isNull() const {
    return data_ == nullptr;
  }

  explicit operator bool() const {
    return data_ != nullptr;
  }

 private:
  T* data_ = nullptr;
  LockType lock_;
};

namespace detail {

template <typename Mutex>
concept shared_mutex_c = requires(Mutex& m) {
  m.lock_shared();
  m.unlock_shared();
};

} // namespace detail

template <typename T, typename Mutex = std::shared_mutex>
class synchronized {
 public:
  synchronized() = default;
  explicit synchronized(T data) : data_(std::move(data)) {}

  synchronized(const synchronized& other) {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      std::shared_lock lk(other.mutex_);
      data_ = other.data_;
    } else {
      std::unique_lock lk(other.mutex_);
      data_ = other.data_;
    }
  }

  synchronized& operator=(const synchronized& other) {
    if (this != &other) {
      T tmp;
      if constexpr (detail::shared_mutex_c<Mutex>) {
        std::shared_lock lk(other.mutex_);
        tmp = other.data_;
      } else {
        std::unique_lock lk(other.mutex_);
        tmp = other.data_;
      }
      std::unique_lock lk(mutex_);
      data_ = std::move(tmp);
    }
    return *this;
  }

  synchronized& operator=(T val) {
    std::unique_lock lk(mutex_);
    data_ = std::move(val);
    return *this;
  }

  synchronized(synchronized&&) = delete;
  synchronized& operator=(synchronized&&) = delete;

  auto wlock() {
    return locked_ptr<T, std::unique_lock<Mutex>>{
        &data_, std::unique_lock<Mutex>(mutex_)};
  }

  auto lock() {
    return wlock();
  }

  template <typename Fn>
  auto withWLock(Fn&& fn) {
    auto lp = wlock();
    return std::forward<Fn>(fn)(*lp);
  }

  template <typename Fn>
  auto withLock(Fn&& fn) {
    return withWLock(std::forward<Fn>(fn));
  }

  auto rlock() const
    requires detail::shared_mutex_c<Mutex>
  {
    return locked_ptr<const T, std::shared_lock<Mutex>>{
        &data_, std::shared_lock<Mutex>(mutex_)};
  }

  template <typename Fn>
  auto withRLock(Fn&& fn) const
    requires detail::shared_mutex_c<Mutex>
  {
    auto lp = rlock();
    return std::forward<Fn>(fn)(*lp);
  }

  T copy() const {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      auto lp = rlock();
      return *lp;
    } else {
      std::unique_lock<Mutex> lk(mutex_);
      return data_;
    }
  }

  void copy(T* out) const {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      auto lp = rlock();
      *out = *lp;
    } else {
      std::unique_lock<Mutex> lk(mutex_);
      *out = data_;
    }
  }

 private:
  mutable Mutex mutex_;
  T data_;
};

class userdata {
 public:
  virtual ~userdata() = default;
};

} // namespace bdg::bison
