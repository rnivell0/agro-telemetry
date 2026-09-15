#pragma once

#include "PersistenceQueue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct InfluxConfig
{
    std::string url;
    std::string org;
    std::string bucket;
    std::string tokenFile;
    std::size_t batchSize;
    std::chrono::milliseconds flushInterval;
    std::chrono::seconds retryMax;
};

struct InfluxWriterStats
{
    std::uint64_t batchesWritten;
    std::uint64_t pointsWritten;
    std::uint64_t batchesDiscarded;
    std::uint64_t pointsDiscarded;
    std::uint64_t retryAttempts;
    std::uint64_t requestFailures;
};

class InfluxWriter
{
public:
    InfluxWriter(InfluxConfig config, PersistenceQueue &queue);
    ~InfluxWriter();

    InfluxWriter(const InfluxWriter &) = delete;
    InfluxWriter &operator=(const InfluxWriter &) = delete;
    InfluxWriter(InfluxWriter &&) = delete;
    InfluxWriter &operator=(InfluxWriter &&) = delete;

    void start();
    void stop();

    InfluxWriterStats stats() const noexcept;

private:
    enum class WriteOutcome
    {
        Success,
        RetryableFailure,
        InvalidPayload,
        AuthorizationFailure,
        PermanentFailure,
        Stopped
    };

    void run() noexcept;
    WriteOutcome writeBatch(
        const std::vector<PersistableTelemetry> &batch);
    std::string makeEndpoint(void *curlHandle) const;
    bool waitForRetry(std::chrono::milliseconds duration);
    void discardBatch(
        const std::vector<PersistableTelemetry> &batch) noexcept;

    static std::size_t discardResponse(
        char *data,
        std::size_t size,
        std::size_t count,
        void *context) noexcept;
    static int transferProgress(
        void *context,
        std::int64_t downloadTotal,
        std::int64_t downloadNow,
        std::int64_t uploadTotal,
        std::int64_t uploadNow) noexcept;

    InfluxConfig config_;
    PersistenceQueue &queue_;
    std::string token_;

    std::atomic<bool> stopRequested_{false};
    std::thread worker_;
    mutable std::mutex lifecycleMutex_;
    std::mutex retryMutex_;
    std::condition_variable retryCondition_;
    std::mutex completionMutex_;
    std::condition_variable completionCondition_;
    bool workerFinished_{true};

    std::atomic<std::uint64_t> batchesWritten_{0};
    std::atomic<std::uint64_t> pointsWritten_{0};
    std::atomic<std::uint64_t> batchesDiscarded_{0};
    std::atomic<std::uint64_t> pointsDiscarded_{0};
    std::atomic<std::uint64_t> retryAttempts_{0};
    std::atomic<std::uint64_t> requestFailures_{0};
};
