// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_sync.hpp
 * @brief Thread-safety primitives: `locked_ptr`, `synchronized`, and
 *        the `userdata` base class.
 *
 * `synchronized<T>` wraps an arbitrary value with a `std::shared_mutex` and
 * exposes RAII-locked accessors (`wlock` / `rlock`) that return a
 * `locked_ptr<T>`.  The lock is held for the lifetime of the returned
 * `locked_ptr`, providing exception-safe, scope-bound access.
 */

#pragma once

#include "src/bison/bison_common.hpp"

namespace bdg::bison {

/**
 * @brief RAII guard that holds a mutex lock and exposes a pointer to the
 *        protected value.
 *
 * Obtained from `synchronized::wlock()` or `synchronized::rlock()`.  The lock
 * is released when the `locked_ptr` is destroyed or `unlock()` is called
 * explicitly.  After `unlock()` the pointer becomes null and must not be
 * dereferenced.
 *
 * @tparam T        Type of the protected value.
 * @tparam LockType RAII lock type (`std::unique_lock` or `std::shared_lock`).
 */
template <typename T, typename LockType>
class locked_ptr {
 public:
  locked_ptr() = default;
  /**
   * @brief Construct with a protected-value pointer and an already-acquired
   *        RAII lock.
   */
  locked_ptr(T* data, LockType lock) : data_(data), lock_(std::move(lock)) {}

  locked_ptr(const locked_ptr&) = delete;
  locked_ptr& operator=(const locked_ptr&) = delete;
  locked_ptr(locked_ptr&&) = default;
  locked_ptr& operator=(locked_ptr&&) = default;

  /** @brief Arrow operator — access members of the protected value. */
  T* operator->() const {
    return data_;
  }

  /** @brief Dereference operator — access the protected value directly. */
  T& operator*() const {
    return *data_;
  }

  /**
   * @brief Release the lock early; sets the internal pointer to null.
   *
   * After calling `unlock()`, dereferencing this `locked_ptr` is undefined
   * behaviour.
   */
  void unlock() {
    lock_.unlock();
    data_ = nullptr;
  }

  /** @brief Return `true` if the lock has been released (pointer is null). */
  bool isNull() const {
    return data_ == nullptr;
  }

  /** @brief Contextual boolean; `false` if the lock has been released. */
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

/**
 * @brief Mutex-protected wrapper for an arbitrary value.
 *
 * Provides RAII-locked access through `locked_ptr` without requiring callers
 * to manage mutexes directly:
 * - `wlock()` acquires an exclusive (`std::unique_lock`) and returns a mutable
 *   `locked_ptr<T>`.
 * - `rlock()` acquires a shared (`std::shared_lock`) and returns a const
 *   `locked_ptr<const T>` (only available when @p Mutex satisfies
 *   `detail::shared_mutex_c`).
 * - `withWLock(fn)` / `withRLock(fn)` are convenience wrappers that invoke @p
 *   fn while holding the appropriate lock.
 *
 * `synchronized` is neither movable nor copyable after construction;
 * use `copy()` to take a snapshot.
 *
 * @tparam T      The type of the protected value.
 * @tparam Mutex  Mutex type; defaults to `std::shared_mutex`.
 */
template <typename T, typename Mutex = std::shared_mutex>
class synchronized {
 public:
  synchronized() = default;
  explicit synchronized(T data) : data_(std::move(data)) {}

  /// @brief In-place constructor: constructs the wrapped value directly.
  ///
  /// This is useful for non-movable, non-copyable types. Instead of
  /// `synchronized(T data)` which requires moving T, this forwards constructor
  /// arguments directly to T's constructor.
  ///
  /// Example: `synchronized<session>(std::in_place, session_id)`.
  template <typename... Args>
  explicit synchronized(std::in_place_t, Args&&... args)
      : data_(std::forward<Args>(args)...) {}

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

  /** @brief Acquire an exclusive write lock and return a mutable `locked_ptr`. */
  auto wlock() {
    return locked_ptr<T, std::unique_lock<Mutex>>{
        &data_, std::unique_lock<Mutex>(mutex_)};
  }

  /** @brief Alias for `wlock()`. */
  auto lock() {
    return wlock();
  }

  /**
   * @brief Invoke @p fn with an exclusive lock held, forwarding the result.
   *
   * @tparam Fn  Callable with signature `R(T&)`.
   * @param  fn  Function to call under the write lock.
   * @return Whatever @p fn returns.
   */
  template <typename Fn>
  auto withWLock(Fn&& fn) {
    auto lp = wlock();
    return std::forward<Fn>(fn)(*lp);
  }

  /** @brief Alias for `withWLock(fn)`. */
  template <typename Fn>
  auto withLock(Fn&& fn) {
    return withWLock(std::forward<Fn>(fn));
  }

  /**
   * @brief Acquire a shared read lock and return a const `locked_ptr`.
   *
   * Only available when @p Mutex satisfies `detail::shared_mutex_c` (e.g.,
   * `std::shared_mutex`).
   */
  auto rlock() const
    requires detail::shared_mutex_c<Mutex>
  {
    return locked_ptr<const T, std::shared_lock<Mutex>>{
        &data_, std::shared_lock<Mutex>(mutex_)};
  }

  /**
   * @brief Invoke @p fn with a shared read lock held, forwarding the result.
   *
   * @tparam Fn  Callable with signature `R(const T&)`.
   * @param  fn  Function to call under the read lock.
   * @return Whatever @p fn returns.
   */
  template <typename Fn>
  auto withRLock(Fn&& fn) const
    requires detail::shared_mutex_c<Mutex>
  {
    auto lp = rlock();
    return std::forward<Fn>(fn)(*lp);
  }

  /**
   * @brief Return a copy of the protected value under the appropriate lock.
   *
   * Uses a shared lock when available, falling back to an exclusive lock for
   * non-shared-mutex types.
   */
  T copy() const {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      auto lp = rlock();
      return *lp;
    } else {
      std::unique_lock<Mutex> lk(mutex_);
      return data_;
    }
  }

  /**
   * @brief Copy the protected value into @p out under the appropriate lock.
   *
   * @param out  Pointer to the destination; must not be null.
   */
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

/**
 * @brief Abstract base class for application-defined userdata attached to a
 *        `dynamic` object.
 *
 * Derive from `userdata` to associate arbitrary C++ state with a `dynamic`
 * instance without polluting its field map.  Instances are always managed
 * through `std::shared_ptr<userdata>`.
 */
class userdata {
 public:
  virtual ~userdata() = default;
};

} // namespace bdg::bison
