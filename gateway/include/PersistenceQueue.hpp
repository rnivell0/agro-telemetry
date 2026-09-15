#pragma once

#include "PersistableTelemetry.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

enum class QueuePushResult
{
    Enqueued,
    DroppedOldest,
    RejectedStopped
};

class PersistenceQueue
{
public:
    explicit PersistenceQueue(std::size_t capacity = 1000);

    PersistenceQueue(const PersistenceQueue &) = delete;
    PersistenceQueue &operator=(const PersistenceQueue &) = delete;

    QueuePushResult push(PersistableTelemetry value);

    std::vector<PersistableTelemetry> waitAndPopBatch(
        std::size_t maxItems,
        std::chrono::milliseconds flushInterval);

    void stop();

    std::uint64_t droppedCount() const;
    bool stopped() const;
    bool stoppedAndEmpty() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable dataAvailable_;
    std::deque<PersistableTelemetry> queue_;
    std::uint64_t droppedCount_{0};
    bool stopped_{false};
};
