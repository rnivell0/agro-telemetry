#include "PersistenceQueue.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

PersistenceQueue::PersistenceQueue(const std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0)
    {
        throw std::invalid_argument("persistence queue capacity must be greater than zero");
    }
}

QueuePushResult PersistenceQueue::push(PersistableTelemetry value)
{
    QueuePushResult result = QueuePushResult::Enqueued;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
        {
            return QueuePushResult::RejectedStopped;
        }

        if (queue_.size() == capacity_)
        {
            queue_.pop_front();
            ++droppedCount_;
            result = QueuePushResult::DroppedOldest;
        }

        queue_.push_back(std::move(value));
    }

    dataAvailable_.notify_one();
    return result;
}

std::vector<PersistableTelemetry> PersistenceQueue::waitAndPopBatch(
    const std::size_t maxItems,
    const std::chrono::milliseconds flushInterval)
{
    if (maxItems == 0)
    {
        throw std::invalid_argument("batch size must be greater than zero");
    }
    if (flushInterval.count() < 0)
    {
        throw std::invalid_argument("flush interval cannot be negative");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    dataAvailable_.wait(lock, [this] { return stopped_ || !queue_.empty(); });

    if (queue_.empty())
    {
        return {};
    }

    if (!stopped_ && queue_.size() < maxItems && flushInterval.count() > 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + flushInterval;
        dataAvailable_.wait_until(
            lock,
            deadline,
            [this, maxItems] { return stopped_ || queue_.size() >= maxItems; });
    }

    const auto batchSize = std::min(maxItems, queue_.size());
    std::vector<PersistableTelemetry> batch;
    batch.reserve(batchSize);

    for (std::size_t index = 0; index < batchSize; ++index)
    {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }

    return batch;
}

void PersistenceQueue::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    dataAvailable_.notify_all();
}

std::uint64_t PersistenceQueue::droppedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedCount_;
}

bool PersistenceQueue::stopped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

bool PersistenceQueue::stoppedAndEmpty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_ && queue_.empty();
}
