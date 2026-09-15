#include "InfluxWriter.hpp"

#include "LineProtocolSerializer.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
std::mutex logMutex;

void log(const char *level, const std::string &message)
{
    const std::lock_guard<std::mutex> lock{logMutex};
    std::clog << '[' << level << "] " << message << std::endl;
}

std::string readToken(const std::string &path)
{
    std::ifstream input{path};
    if (!input)
    {
        throw std::runtime_error{
            "unable to read InfluxDB token file"};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad())
    {
        throw std::runtime_error{
            "unable to read InfluxDB token file"};
    }

    std::string token = buffer.str();
    while (!token.empty() &&
           std::isspace(static_cast<unsigned char>(token.back())) != 0)
    {
        token.pop_back();
    }

    std::size_t first = 0;
    while (first < token.size() &&
           std::isspace(static_cast<unsigned char>(token[first])) != 0)
    {
        ++first;
    }
    token.erase(0, first);

    if (token.empty())
    {
        throw std::runtime_error{"InfluxDB token file is empty"};
    }
    return token;
}

void validateConfig(const InfluxConfig &config)
{
    if (config.url.empty())
    {
        throw std::invalid_argument{"InfluxDB URL must not be empty"};
    }
    if (config.org.empty())
    {
        throw std::invalid_argument{
            "InfluxDB organization must not be empty"};
    }
    if (config.bucket.empty())
    {
        throw std::invalid_argument{
            "InfluxDB bucket must not be empty"};
    }
    if (config.tokenFile.empty())
    {
        throw std::invalid_argument{
            "InfluxDB token file must not be empty"};
    }
    if (config.batchSize == 0)
    {
        throw std::invalid_argument{
            "InfluxDB batch size must be greater than zero"};
    }
    if (config.flushInterval.count() <= 0)
    {
        throw std::invalid_argument{
            "InfluxDB flush interval must be greater than zero"};
    }
    if (config.retryMax.count() <= 0)
    {
        throw std::invalid_argument{
            "InfluxDB maximum retry delay must be greater than zero"};
    }
}

void initializeCurl()
{
    static std::once_flag once;
    static CURLcode initializationResult = CURLE_OK;

    std::call_once(
        once,
        []
        {
            initializationResult = curl_global_init(CURL_GLOBAL_DEFAULT);
        });

    if (initializationResult != CURLE_OK)
    {
        throw std::runtime_error{"unable to initialize libcurl"};
    }
}

struct CurlHandleDeleter
{
    void operator()(CURL *handle) const noexcept
    {
        curl_easy_cleanup(handle);
    }
};

struct CurlHeadersDeleter
{
    void operator()(curl_slist *headers) const noexcept
    {
        curl_slist_free_all(headers);
    }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;
using CurlHeaders = std::unique_ptr<curl_slist, CurlHeadersDeleter>;

class CompletionGuard
{
public:
    CompletionGuard(
        std::mutex &mutex,
        std::condition_variable &condition,
        bool &finished)
        : mutex_{mutex},
          condition_{condition},
          finished_{finished}
    {
    }

    ~CompletionGuard()
    {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            finished_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex &mutex_;
    std::condition_variable &condition_;
    bool &finished_;
};

std::string escapeQueryValue(CURL *handle, const std::string &value)
{
    if (value.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::runtime_error{
            "InfluxDB query parameter is too long"};
    }

    char *encoded = curl_easy_escape(
        handle,
        value.data(),
        static_cast<int>(value.size()));
    if (encoded == nullptr)
    {
        throw std::runtime_error{
            "unable to encode InfluxDB query parameter"};
    }

    std::string result{encoded};
    curl_free(encoded);
    return result;
}

std::chrono::milliseconds nextBackoff(
    const std::chrono::milliseconds current,
    const std::chrono::milliseconds maximum)
{
    if (current >= maximum)
    {
        return maximum;
    }
    if (current.count() > maximum.count() / 2)
    {
        return maximum;
    }
    return std::min(current * 2, maximum);
}
}

InfluxWriter::InfluxWriter(
    InfluxConfig config,
    PersistenceQueue &queue)
    : config_{std::move(config)},
      queue_{queue}
{
    validateConfig(config_);
}

InfluxWriter::~InfluxWriter()
{
    stop();
}

void InfluxWriter::start()
{
    const std::lock_guard<std::mutex> lock{lifecycleMutex_};
    if (worker_.joinable())
    {
        return;
    }
    if (queue_.stopped())
    {
        throw std::logic_error{
            "cannot start InfluxDB writer with a stopped queue"};
    }

    initializeCurl();
    token_ = readToken(config_.tokenFile);
    stopRequested_.store(false);
    {
        const std::lock_guard<std::mutex> completionLock{
            completionMutex_};
        workerFinished_ = false;
    }

    try
    {
        worker_ = std::thread{&InfluxWriter::run, this};
    }
    catch (...)
    {
        {
            const std::lock_guard<std::mutex> completionLock{
                completionMutex_};
            workerFinished_ = true;
        }
        completionCondition_.notify_all();
        std::fill(token_.begin(), token_.end(), '\0');
        token_.clear();
        throw;
    }
}

void InfluxWriter::stop()
{
    std::unique_lock<std::mutex> lock{lifecycleMutex_};
    if (!worker_.joinable())
    {
        return;
    }

    queue_.stop();

    constexpr auto gracefulDrainTimeout =
        std::chrono::seconds{5};
    {
        std::unique_lock<std::mutex> completionLock{
            completionMutex_};
        if (!completionCondition_.wait_for(
                completionLock,
                gracefulDrainTimeout,
                [this] { return workerFinished_; }))
        {
            log(
                "WARN",
                "InfluxDB graceful drain timed out; "
                "stopping writer");
            stopRequested_.store(true);
            retryCondition_.notify_all();
        }
    }

    std::thread worker = std::move(worker_);
    lock.unlock();
    worker.join();
    lock.lock();

    std::fill(token_.begin(), token_.end(), '\0');
    token_.clear();
}

InfluxWriterStats InfluxWriter::stats() const noexcept
{
    return InfluxWriterStats{
        batchesWritten_.load(),
        pointsWritten_.load(),
        batchesDiscarded_.load(),
        pointsDiscarded_.load(),
        retryAttempts_.load(),
        requestFailures_.load(),
    };
}

void InfluxWriter::run() noexcept
{
    const CompletionGuard completionGuard{
        completionMutex_,
        completionCondition_,
        workerFinished_,
    };

    try
    {
        const auto retryMaximum =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                config_.retryMax);
        std::chrono::milliseconds retryDelay{
            std::min<std::int64_t>(1000, retryMaximum.count())};
        bool awaitingRestoration = false;

        while (!stopRequested_.load())
        {
            std::vector<PersistableTelemetry> batch =
                queue_.waitAndPopBatch(
                    config_.batchSize,
                    config_.flushInterval);

            if (batch.empty())
            {
                if (queue_.stoppedAndEmpty())
                {
                    break;
                }
                continue;
            }

            while (!stopRequested_.load())
            {
                const WriteOutcome outcome = writeBatch(batch);

                if (outcome == WriteOutcome::Success)
                {
                    ++batchesWritten_;
                    pointsWritten_.fetch_add(batch.size());
                    log(
                        "INFO",
                        "InfluxDB batch written points=" +
                            std::to_string(batch.size()));

                    if (awaitingRestoration)
                    {
                        log("INFO", "InfluxDB connection restored");
                    }
                    awaitingRestoration = false;
                    retryDelay = std::chrono::milliseconds{
                        std::min<std::int64_t>(
                            1000,
                            retryMaximum.count())};
                    break;
                }

                if (outcome == WriteOutcome::Stopped)
                {
                    return;
                }

                if (outcome != WriteOutcome::RetryableFailure)
                {
                    discardBatch(batch);
                    break;
                }

                ++requestFailures_;
                ++retryAttempts_;
                awaitingRestoration = true;
                log(
                    "WARN",
                    "InfluxDB unavailable retry_in_ms=" +
                        std::to_string(retryDelay.count()));

                if (waitForRetry(retryDelay))
                {
                    return;
                }
                retryDelay = nextBackoff(
                    retryDelay,
                    retryMaximum);
            }
        }
    }
    catch (const std::exception &error)
    {
        log(
            "ERROR",
            std::string{"InfluxDB writer stopped unexpectedly: "} +
                error.what());
    }
    catch (...)
    {
        log(
            "ERROR",
            "InfluxDB writer stopped unexpectedly");
    }
}

InfluxWriter::WriteOutcome InfluxWriter::writeBatch(
    const std::vector<PersistableTelemetry> &batch)
{
    LineProtocolSerializer serializer;
    std::string payload;
    try
    {
        for (std::size_t index = 0; index < batch.size(); ++index)
        {
            if (index != 0)
            {
                payload.push_back('\n');
            }
            payload += serializer.serialize(batch[index]);
        }
    }
    catch (const std::exception &)
    {
        log(
            "WARN",
            "Unable to serialize telemetry; discarding batch points=" +
                std::to_string(batch.size()));
        return WriteOutcome::InvalidPayload;
    }

    CurlHandle handle{curl_easy_init()};
    if (!handle)
    {
        return WriteOutcome::RetryableFailure;
    }

    const std::string endpoint = makeEndpoint(handle.get());
    std::string authorization = "Authorization: Token " + token_;

    curl_slist *rawHeaders = nullptr;
    rawHeaders = curl_slist_append(
        rawHeaders,
        authorization.c_str());
    if (rawHeaders == nullptr)
    {
        std::fill(
            authorization.begin(),
            authorization.end(),
            '\0');
        return WriteOutcome::RetryableFailure;
    }

    curl_slist *withContentType = curl_slist_append(
        rawHeaders,
        "Content-Type: text/plain; charset=utf-8");
    if (withContentType == nullptr)
    {
        curl_slist_free_all(rawHeaders);
        std::fill(
            authorization.begin(),
            authorization.end(),
            '\0');
        return WriteOutcome::RetryableFailure;
    }
    CurlHeaders headers{withContentType};

    curl_easy_setopt(
        handle.get(),
        CURLOPT_URL,
        endpoint.c_str());
    curl_easy_setopt(
        handle.get(),
        CURLOPT_HTTPHEADER,
        headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(
        handle.get(),
        CURLOPT_POSTFIELDS,
        payload.data());
    curl_easy_setopt(
        handle.get(),
        CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(
        handle.get(),
        CURLOPT_WRITEFUNCTION,
        &InfluxWriter::discardResponse);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        handle.get(),
        CURLOPT_XFERINFOFUNCTION,
        &InfluxWriter::transferProgress);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, this);

    std::fill(
        authorization.begin(),
        authorization.end(),
        '\0');

    const CURLcode result = curl_easy_perform(handle.get());
    if (stopRequested_.load())
    {
        return WriteOutcome::Stopped;
    }
    if (result != CURLE_OK)
    {
        return WriteOutcome::RetryableFailure;
    }

    long status = 0;
    if (curl_easy_getinfo(
            handle.get(),
            CURLINFO_RESPONSE_CODE,
            &status) != CURLE_OK)
    {
        return WriteOutcome::RetryableFailure;
    }

    if (status == 204)
    {
        return WriteOutcome::Success;
    }
    if (status == 400)
    {
        log(
            "WARN",
            "InfluxDB rejected invalid payload; discarding batch points=" +
                std::to_string(batch.size()));
        return WriteOutcome::InvalidPayload;
    }
    if (status == 401 || status == 403)
    {
        log(
            "ERROR",
            "InfluxDB authorization/configuration error http_status=" +
                std::to_string(status) +
                "; discarding batch points=" +
                std::to_string(batch.size()));
        return WriteOutcome::AuthorizationFailure;
    }
    if (status == 429 || status >= 500)
    {
        return WriteOutcome::RetryableFailure;
    }

    log(
        "WARN",
        "InfluxDB non-retryable response http_status=" +
            std::to_string(status) +
            "; discarding batch points=" +
            std::to_string(batch.size()));
    return WriteOutcome::PermanentFailure;
}

std::string InfluxWriter::makeEndpoint(void *curlHandle) const
{
    CURL *handle = static_cast<CURL *>(curlHandle);
    std::string base = config_.url;
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }

    return base + "/api/v2/write?org=" +
           escapeQueryValue(handle, config_.org) +
           "&bucket=" +
           escapeQueryValue(handle, config_.bucket) +
           "&precision=ms";
}

bool InfluxWriter::waitForRetry(
    const std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock{retryMutex_};
    return retryCondition_.wait_for(
        lock,
        duration,
        [this]
        {
            return stopRequested_.load();
        });
}

void InfluxWriter::discardBatch(
    const std::vector<PersistableTelemetry> &batch) noexcept
{
    ++batchesDiscarded_;
    pointsDiscarded_.fetch_add(batch.size());
}

std::size_t InfluxWriter::discardResponse(
    char *,
    const std::size_t size,
    const std::size_t count,
    void *) noexcept
{
    return size * count;
}

int InfluxWriter::transferProgress(
    void *context,
    std::int64_t,
    std::int64_t,
    std::int64_t,
    std::int64_t) noexcept
{
    const auto *writer = static_cast<const InfluxWriter *>(context);
    return writer->stopRequested_.load() ? 1 : 0;
}
