#include "thread_pool.hh"

#include <gtest/gtest.h>

namespace {
using namespace waxwing::internal;
using namespace waxwing::internal::concurrency;

TEST(ThreadPool, SingleProducer) {
    constexpr const int TASKS = 128;
    std::atomic<int> c = 0;

    {
        ThreadPool pool;

        for (int i = 0; i < TASKS; ++i) {
            pool.async([&c]() { c += 1; });
        }
    }
    EXPECT_EQ(c, TASKS);
}

TEST(ThreadPool, MultipleProducers) {
    constexpr const int TASKS = 256;
    constexpr const int PRODUCERS = 8;
    ASSERT_EQ(TASKS % PRODUCERS, 0);

    std::atomic<int> consumed = 0;
    std::atomic<int> produced = 0;
    {
        ThreadPool pool;
        std::vector<std::jthread> producers;
        for (int i = 0; i < PRODUCERS; ++i) {
            producers.emplace_back([&consumed, &pool, &produced]() {
                for (int i = 0; i < TASKS / PRODUCERS; ++i) {
                    pool.async([&consumed]() { consumed += 1; });
                    produced += 1;
                }
            });
        }

        while (produced != TASKS) {
            std::this_thread::yield();
        }
    }
    EXPECT_EQ(produced, TASKS);
    EXPECT_EQ(consumed, TASKS);
}
}  // namespace