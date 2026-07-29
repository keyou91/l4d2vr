#pragma once

#include "d3d9_include.h"

#include <atomic>
#include <thread>

namespace dxvk {

  /**
   * \brief Device lock
   *
   * Lightweight RAII wrapper that implements
   * a subset of the functionality provided by
   * \c std::unique_lock, with the goal of being
   * cheaper to construct and destroy.
   */
  class D3D9DeviceLock {

  public:

    D3D9DeviceLock() = default;

    D3D9DeviceLock(sync::RecursiveSpinlock& mutex)
      : m_mutex(&mutex) {
      mutex.lock();
    }

    static D3D9DeviceLock Activity(std::atomic<uint32_t>& activity) {
      D3D9DeviceLock result;
      result.m_activity = &activity;
      return result;
    }

    static D3D9DeviceLock Exclusive(
            sync::RecursiveSpinlock& mutex,
            std::atomic<uint32_t>& activity,
            std::atomic<uint32_t>& exclusiveRequests) {
      D3D9DeviceLock result;
      result.m_mutex = &mutex;
      result.m_exclusiveRequests = &exclusiveRequests;

      // Publish the writer request before taking the recursive lock. New ordinary
      // D3D9 calls then join the lock, while calls already executing remain visible
      // through the activity count below.
      exclusiveRequests.fetch_add(1, std::memory_order_acq_rel);
      mutex.lock();
      while (activity.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();

      return result;
    }

    D3D9DeviceLock(D3D9DeviceLock&& other)
      : m_mutex(other.m_mutex),
        m_activity(other.m_activity),
        m_exclusiveRequests(other.m_exclusiveRequests) {
      other.Clear();
    }

    D3D9DeviceLock& operator = (D3D9DeviceLock&& other) {
      Release();
      m_mutex = other.m_mutex;
      m_activity = other.m_activity;
      m_exclusiveRequests = other.m_exclusiveRequests;
      other.Clear();
      return *this;
    }

    ~D3D9DeviceLock() {
      Release();
    }

  private:

    void Clear() {
      m_mutex = nullptr;
      m_activity = nullptr;
      m_exclusiveRequests = nullptr;
    }

    void Release() {
      if (m_activity != nullptr)
        m_activity->fetch_sub(1, std::memory_order_release);
      if (m_mutex != nullptr)
        m_mutex->unlock();
      if (m_exclusiveRequests != nullptr)
        m_exclusiveRequests->fetch_sub(1, std::memory_order_release);
      Clear();
    }

    sync::RecursiveSpinlock* m_mutex = nullptr;
    std::atomic<uint32_t>* m_activity = nullptr;
    std::atomic<uint32_t>* m_exclusiveRequests = nullptr;

  };


  /**
   * \brief D3D9 context lock
   */
  class D3D9Multithread {

  public:

    D3D9Multithread(
      BOOL                  Protected);

    D3D9DeviceLock AcquireLock() {
      // Source's material worker owns the device for the complete queued render
      // command interval. Calls made by that owner are already serialized by the
      // outer frame lock, so avoid reader registration on every draw/state call.
      if (SourceFrameOwner() == this)
        return D3D9DeviceLock();

      if (m_protected)
        return D3D9DeviceLock(m_mutex);

      for (;;) {
        if (m_sourceFrameActive.load(std::memory_order_acquire)
         || m_exclusiveRequests.load(std::memory_order_acquire) != 0)
          return AcquireExclusiveLock();

        m_unprotectedActivity.fetch_add(1, std::memory_order_acq_rel);
        if (!m_sourceFrameActive.load(std::memory_order_acquire)
         && m_exclusiveRequests.load(std::memory_order_acquire) == 0)
          return D3D9DeviceLock::Activity(m_unprotectedActivity);

        // A writer arrived between the first check and activity publication. Back
        // out and join the exclusive side; the writer waits for this decrement.
        m_unprotectedActivity.fetch_sub(1, std::memory_order_release);
      }
    }

    D3D9DeviceLock AcquireExclusiveLock() {
      if (m_protected)
        return D3D9DeviceLock(m_mutex);

      return D3D9DeviceLock::Exclusive(
        m_mutex,
        m_unprotectedActivity,
        m_exclusiveRequests);
    }

    bool BeginSourceFrameOwnership() {
      D3D9Multithread*& owner = SourceFrameOwner();
      uint32_t& depth = SourceFrameDepth();

      if (owner == this) {
        depth += 1;
        return true;
      }

      // A thread cannot execute two devices' Source frame command streams at the
      // same time. Treat that as a marker mismatch instead of unlocking the wrong
      // recursive spinlock later.
      if (owner != nullptr)
        return false;

      if (m_protected) {
        m_mutex.lock();
      } else {
        // Close the reader-registration race before publishing frame ownership.
        // Existing ordinary calls drain; later calls join the recursive device lock.
        m_exclusiveRequests.fetch_add(1, std::memory_order_acq_rel);
        m_mutex.lock();
        while (m_unprotectedActivity.load(std::memory_order_acquire) != 0)
          std::this_thread::yield();
        m_sourceFrameActive.store(true, std::memory_order_release);
        m_exclusiveRequests.fetch_sub(1, std::memory_order_release);
      }

      owner = this;
      depth = 1;
      return true;
    }

    bool EndSourceFrameOwnership() {
      D3D9Multithread*& owner = SourceFrameOwner();
      uint32_t& depth = SourceFrameDepth();
      if (owner != this || depth == 0)
        return false;

      if (--depth != 0)
        return true;

      owner = nullptr;
      if (!m_protected)
        m_sourceFrameActive.store(false, std::memory_order_release);
      m_mutex.unlock();
      return true;
    }

  private:

    static D3D9Multithread*& SourceFrameOwner() {
      static thread_local D3D9Multithread* owner = nullptr;
      return owner;
    }

    static uint32_t& SourceFrameDepth() {
      static thread_local uint32_t depth = 0;
      return depth;
    }

    BOOL            m_protected;

    sync::RecursiveSpinlock m_mutex;
    std::atomic<uint32_t> m_unprotectedActivity { 0 };
    std::atomic<uint32_t> m_exclusiveRequests { 0 };
    std::atomic<bool> m_sourceFrameActive { false };

  };

}
