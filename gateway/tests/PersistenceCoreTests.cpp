#include "LineProtocolSerializer.hpp"
#include "PersistenceQueue.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string &description)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

PersistableTelemetry sample(
    const std::uint64_t sequence,
    std::string deviceId = "esp32-node-01")
{
    return {
        {std::move(deviceId), sequence, 23.0, 60.0, sequence * 1000, std::nullopt},
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1785362400123}}};
}

template <typename Function>
void expectInvalidArgument(Function function, const std::string &description)
{
    try
    {
        function();
        expect(false, description);
    }
    catch (const std::invalid_argument &)
    {
        expect(true, description);
    }
    catch (...)
    {
        expect(false, description + " (wrong exception type)");
    }
}

void testSerializer()
{
    const LineProtocolSerializer serializer;

    expect(
        serializer.serialize(sample(2035)) ==
            "device_telemetry,device_id=esp32-node-01 "
            "temperature_c=23.0,humidity_pct=60.0,"
            "sequence=2035u,uptime_ms=2035000u 1785362400123",
        "serializer emits the expected line protocol");

    auto withRssi = sample(2035);
    withRssi.telemetry.rssiDbm = -67;
    expect(
        serializer.serialize(withRssi) ==
            "device_telemetry,device_id=esp32-node-01 "
            "temperature_c=23.0,humidity_pct=60.0,"
            "sequence=2035u,uptime_ms=2035000u,rssi_dbm=-67i 1785362400123",
        "serializer emits rssi_dbm as a signed integer when present");

    expect(
        serializer.serialize(sample(1, R"(node a,b=c\d)")).find(
            R"(device_id=node\ a\,b\=c\\d )") != std::string::npos,
        "serializer escapes spaces, commas, equals signs, and backslashes in tag values");

    auto decimal = sample(2);
    decimal.telemetry.temperatureC = 1.25;
    expect(
        serializer.serialize(decimal).find("temperature_c=1.25") != std::string::npos,
        "serializer uses a locale-independent decimal point");

    auto invalid = sample(3);
    invalid.telemetry.temperatureC = std::numeric_limits<double>::infinity();
    expectInvalidArgument(
        [&serializer, &invalid] { static_cast<void>(serializer.serialize(invalid)); },
        "serializer rejects non-finite temperature");

    invalid = sample(4);
    invalid.telemetry.humidityPct = std::numeric_limits<double>::quiet_NaN();
    expectInvalidArgument(
        [&serializer, &invalid] { static_cast<void>(serializer.serialize(invalid)); },
        "serializer rejects non-finite humidity");

    expectInvalidArgument(
        [&serializer] { static_cast<void>(serializer.serialize(sample(5, "node\ninjected"))); },
        "serializer rejects line feeds in tag values");
    expectInvalidArgument(
        [&serializer] { static_cast<void>(serializer.serialize(sample(6, "node\rinjected"))); },
        "serializer rejects carriage returns in tag values");
}

void testQueueDropsOldestAndPreservesOrder()
{
    PersistenceQueue queue(2);

    expect(queue.push(sample(1)) == QueuePushResult::Enqueued, "first push is enqueued");
    expect(queue.push(sample(2)) == QueuePushResult::Enqueued, "second push is enqueued");
    expect(
        queue.push(sample(3)) == QueuePushResult::DroppedOldest,
        "full queue reports dropping the oldest item");
    expect(queue.droppedCount() == 1, "drop counter is incremented");

    const auto batch = queue.waitAndPopBatch(2, 1s);
    expect(batch.size() == 2, "queue returns a full batch");
    if (batch.size() == 2)
    {
        expect(batch[0].telemetry.sequence == 2, "oldest retained item is first");
        expect(batch[1].telemetry.sequence == 3, "newest retained item is last");
    }
}

void testQueueFlushAndStop()
{
    PersistenceQueue flushingQueue(4);
    flushingQueue.push(sample(10));

    auto delayedBatch = std::async(std::launch::async, [&flushingQueue] {
        return flushingQueue.waitAndPopBatch(4, 25ms);
    });
    expect(
        delayedBatch.wait_for(500ms) == std::future_status::ready,
        "partial batch is released after the flush interval");
    if (delayedBatch.wait_for(0ms) == std::future_status::ready)
    {
        expect(delayedBatch.get().size() == 1, "flush returns the available partial batch");
    }

    PersistenceQueue stoppedQueue(4);
    auto waitingBatch = std::async(std::launch::async, [&stoppedQueue] {
        return stoppedQueue.waitAndPopBatch(4, 1s);
    });
    std::this_thread::sleep_for(20ms);
    stoppedQueue.stop();
    expect(
        waitingBatch.wait_for(500ms) == std::future_status::ready,
        "stop wakes a consumer waiting on an empty queue");
    if (waitingBatch.wait_for(0ms) == std::future_status::ready)
    {
        expect(waitingBatch.get().empty(), "stopped empty queue returns an empty batch");
    }
    expect(stoppedQueue.stoppedAndEmpty(), "stopped empty queue reports drained");
    expect(
        stoppedQueue.push(sample(11)) == QueuePushResult::RejectedStopped,
        "push is rejected after stop");

    PersistenceQueue drainingQueue(4);
    drainingQueue.push(sample(20));
    drainingQueue.push(sample(21));
    drainingQueue.stop();
    expect(!drainingQueue.stoppedAndEmpty(), "stopped queue is not drained while data remains");
    expect(
        drainingQueue.waitAndPopBatch(1, 1s).size() == 1,
        "stopped queue still returns its first queued item");
    expect(
        drainingQueue.waitAndPopBatch(4, 1s).size() == 1,
        "stopped queue drains its remaining item");
    expect(drainingQueue.stoppedAndEmpty(), "stopped queue reports drained after final batch");
}

void testQueueRejectsInvalidConfiguration()
{
    expectInvalidArgument(
        [] { PersistenceQueue queue(0); },
        "queue rejects zero capacity");

    PersistenceQueue queue(1);
    expectInvalidArgument(
        [&queue] { static_cast<void>(queue.waitAndPopBatch(0, 1ms)); },
        "queue rejects zero batch size");
    expectInvalidArgument(
        [&queue] { static_cast<void>(queue.waitAndPopBatch(1, -1ms)); },
        "queue rejects a negative flush interval");
}

void testConcurrentProducers()
{
    constexpr std::size_t producerCount = 4;
    constexpr std::size_t itemsPerProducer = 250;
    constexpr std::size_t totalItems = producerCount * itemsPerProducer;

    PersistenceQueue queue(totalItems);
    std::atomic<std::size_t> rejectedPushes{0};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (std::size_t producer = 0; producer < producerCount; ++producer)
    {
        producers.emplace_back([producer, &queue, &rejectedPushes] {
            for (std::size_t item = 0; item < itemsPerProducer; ++item)
            {
                const auto sequence = producer * itemsPerProducer + item;
                if (queue.push(sample(sequence)) != QueuePushResult::Enqueued)
                {
                    ++rejectedPushes;
                }
            }
        });
    }

    for (auto &producer : producers)
    {
        producer.join();
    }

    const auto batch = queue.waitAndPopBatch(totalItems, 0ms);
    expect(rejectedPushes == 0, "concurrent producers enqueue without unexpected rejection");
    expect(batch.size() == totalItems, "concurrent producers do not lose measurements");

    std::unordered_set<std::uint64_t> sequences;
    for (const auto &value : batch)
    {
        sequences.insert(value.telemetry.sequence);
    }
    expect(sequences.size() == totalItems, "concurrent producer measurements remain distinct");
}
}

int main()
{
    testSerializer();
    testQueueDropsOldestAndPreservesOrder();
    testQueueFlushAndStop();
    testQueueRejectsInvalidConfiguration();
    testConcurrentProducers();

    if (failures == 0)
    {
        std::cout << "All persistence core tests passed\n";
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
