// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef ENABLE_ASYNC_EXPORT
#  include <gtest/gtest.h>
TEST(AsyncBatchSpanProcessor, DummyTest)
{
  // For linking
}
#endif

#ifdef ENABLE_ASYNC_EXPORT

#  include "opentelemetry/sdk/trace/async_batch_span_processor.h"
#  include "opentelemetry/sdk/trace/span_data.h"
#  include "opentelemetry/sdk/trace/tracer.h"

#  include <gtest/gtest.h>
#  include <chrono>
#  include <list>
#  include <memory>
#  include <thread>

OPENTELEMETRY_BEGIN_NAMESPACE

/**
 * Returns a mock span exporter meant exclusively for testing only
 */
class MockSpanExporter final : public sdk::trace::SpanExporter
{
public:
  MockSpanExporter(
      std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received,
      std::shared_ptr<std::atomic<bool>> is_shutdown,
      std::shared_ptr<std::atomic<bool>> is_export_completed =
          std::shared_ptr<std::atomic<bool>>(new std::atomic<bool>(false)),
      const std::chrono::milliseconds export_delay = std::chrono::milliseconds(0),
      int callback_count                           = 1) noexcept
      : spans_received_(spans_received),
        is_shutdown_(is_shutdown),
        is_export_completed_(is_export_completed),
        export_delay_(export_delay),
        callback_count_(callback_count)
  {}

  std::unique_ptr<sdk::trace::Recordable> MakeRecordable() noexcept override
  {
    return std::unique_ptr<sdk::trace::Recordable>(new sdk::trace::SpanData);
  }

  sdk::common::ExportResult Export(
      const nostd::span<std::unique_ptr<sdk::trace::Recordable>> &recordables) noexcept override
  {
    *is_export_completed_ = false;

    std::this_thread::sleep_for(export_delay_);

    for (auto &recordable : recordables)
    {
      auto span = std::unique_ptr<sdk::trace::SpanData>(
          static_cast<sdk::trace::SpanData *>(recordable.release()));

      if (span != nullptr)
      {
        spans_received_->push_back(std::move(span));
      }
    }

    *is_export_completed_ = true;
    return sdk::common::ExportResult::kSuccess;
  }

  void Export(const nostd::span<std::unique_ptr<sdk::trace::Recordable>> &records,
              std::function<bool(opentelemetry::sdk::common::ExportResult)>
                  &&result_callback) noexcept override
  {
    // We should keep the order of test records
    auto result = Export(records);
    async_threads_.emplace_back(std::make_shared<std::thread>(
        [this,
         result](std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback) {
          for (int i = 0; i < callback_count_; i++)
          {
            result_callback(result);
          }
        },
        std::move(result_callback)));
  }

  bool Shutdown(
      std::chrono::microseconds timeout = std::chrono::microseconds::max()) noexcept override
  {
    while (!async_threads_.empty())
    {
      std::list<std::shared_ptr<std::thread>> async_threads;
      async_threads.swap(async_threads_);
      for (auto &async_thread : async_threads)
      {
        if (async_thread && async_thread->joinable())
        {
          async_thread->join();
        }
      }
    }
    *is_shutdown_ = true;
    return true;
  }

  bool IsExportCompleted() { return is_export_completed_->load(); }

private:
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received_;
  std::shared_ptr<std::atomic<bool>> is_shutdown_;
  std::shared_ptr<std::atomic<bool>> is_export_completed_;
  // Meant exclusively to test force flush timeout
  const std::chrono::milliseconds export_delay_;
  std::list<std::shared_ptr<std::thread>> async_threads_;
  int callback_count_;
};

/**
 * Fixture Class
 */
class AsyncBatchSpanProcessorTestPeer : public testing::Test
{
public:
  std::unique_ptr<std::vector<std::unique_ptr<sdk::trace::Recordable>>> GetTestSpans(
      std::shared_ptr<sdk::trace::SpanProcessor> processor,
      const int num_spans)
  {
    std::unique_ptr<std::vector<std::unique_ptr<sdk::trace::Recordable>>> test_spans(
        new std::vector<std::unique_ptr<sdk::trace::Recordable>>);

    for (int i = 0; i < num_spans; ++i)
    {
      test_spans->push_back(processor->MakeRecordable());
      static_cast<sdk::trace::SpanData *>(test_spans->at(i).get())
          ->SetName("Span " + std::to_string(i));
    }

    return test_spans;
  }
};

/* ##################################   TESTS   ############################################ */

TEST_F(AsyncBatchSpanProcessorTestPeer, TestAsyncShutdown)
{
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received(
      new std::vector<std::unique_ptr<sdk::trace::SpanData>>);
  std::shared_ptr<std::atomic<bool>> is_shutdown(new std::atomic<bool>(false));

  sdk::trace::AsyncBatchSpanProcessorOptions options{};
  options.max_export_async = 5;

  auto batch_processor =
      std::shared_ptr<sdk::trace::AsyncBatchSpanProcessor>(new sdk::trace::AsyncBatchSpanProcessor(
          std::unique_ptr<MockSpanExporter>(new MockSpanExporter(spans_received, is_shutdown)),
          options));
  const int num_spans = 2048;

  auto test_spans = GetTestSpans(batch_processor, num_spans);

  for (int i = 0; i < num_spans; ++i)
  {
    batch_processor->OnEnd(std::move(test_spans->at(i)));
  }

  EXPECT_TRUE(batch_processor->Shutdown(std::chrono::milliseconds(5000)));
  // It's safe to shutdown again
  EXPECT_TRUE(batch_processor->Shutdown());

  EXPECT_EQ(num_spans, spans_received->size());
  for (int i = 0; i < num_spans; ++i)
  {
    EXPECT_EQ("Span " + std::to_string(i), spans_received->at(i)->GetName());
  }

  EXPECT_TRUE(is_shutdown->load());
}

TEST_F(AsyncBatchSpanProcessorTestPeer, TestAsyncShutdownNoCallback)
{
  std::shared_ptr<std::atomic<bool>> is_export_completed(new std::atomic<bool>(false));
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received(
      new std::vector<std::unique_ptr<sdk::trace::SpanData>>);
  const std::chrono::milliseconds export_delay(0);
  std::shared_ptr<std::atomic<bool>> is_shutdown(new std::atomic<bool>(false));

  sdk::trace::AsyncBatchSpanProcessorOptions options{};
  options.max_export_async = 8;

  auto batch_processor =
      std::shared_ptr<sdk::trace::AsyncBatchSpanProcessor>(new sdk::trace::AsyncBatchSpanProcessor(
          std::unique_ptr<MockSpanExporter>(new MockSpanExporter(
              spans_received, is_shutdown, is_export_completed, export_delay, 0)),
          options));
  const int num_spans = 2048;

  auto test_spans = GetTestSpans(batch_processor, num_spans);

  for (int i = 0; i < num_spans; ++i)
  {
    batch_processor->OnEnd(std::move(test_spans->at(i)));
  }

  // Shutdown should never block for ever and return on timeout
  EXPECT_TRUE(batch_processor->Shutdown(std::chrono::milliseconds(5000)));
  // It's safe to shutdown again
  EXPECT_TRUE(batch_processor->Shutdown());

  EXPECT_TRUE(is_shutdown->load());
}

TEST_F(AsyncBatchSpanProcessorTestPeer, TestAsyncForceFlush)
{
  std::shared_ptr<std::atomic<bool>> is_shutdown(new std::atomic<bool>(false));
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received(
      new std::vector<std::unique_ptr<sdk::trace::SpanData>>);

  sdk::trace::AsyncBatchSpanProcessorOptions options{};

  auto batch_processor =
      std::shared_ptr<sdk::trace::AsyncBatchSpanProcessor>(new sdk::trace::AsyncBatchSpanProcessor(
          std::unique_ptr<MockSpanExporter>(new MockSpanExporter(spans_received, is_shutdown)),
          options));
  const int num_spans = 2048;

  auto test_spans = GetTestSpans(batch_processor, num_spans);

  for (int i = 0; i < num_spans; ++i)
  {
    batch_processor->OnEnd(std::move(test_spans->at(i)));
  }

  // Give some time to export
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(batch_processor->ForceFlush());

  EXPECT_EQ(num_spans, spans_received->size());
  for (int i = 0; i < num_spans; ++i)
  {
    EXPECT_EQ("Span " + std::to_string(i), spans_received->at(i)->GetName());
  }

  // Create some more spans to make sure that the processor still works
  auto more_test_spans = GetTestSpans(batch_processor, num_spans);
  for (int i = 0; i < num_spans; ++i)
  {
    batch_processor->OnEnd(std::move(more_test_spans->at(i)));
  }

  // Give some time to export the spans
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(batch_processor->ForceFlush());

  EXPECT_EQ(num_spans * 2, spans_received->size());
  for (int i = 0; i < num_spans; ++i)
  {
    EXPECT_EQ("Span " + std::to_string(i % num_spans),
              spans_received->at(num_spans + i)->GetName());
  }
}

TEST_F(AsyncBatchSpanProcessorTestPeer, TestManySpansLoss)
{
  /* Test that when exporting more than max_queue_size spans, some are most likely lost*/

  std::shared_ptr<std::atomic<bool>> is_shutdown(new std::atomic<bool>(false));
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received(
      new std::vector<std::unique_ptr<sdk::trace::SpanData>>);

  const int max_queue_size = 4096;

  auto batch_processor =
      std::shared_ptr<sdk::trace::AsyncBatchSpanProcessor>(new sdk::trace::AsyncBatchSpanProcessor(
          std::unique_ptr<MockSpanExporter>(new MockSpanExporter(spans_received, is_shutdown)),
          sdk::trace::AsyncBatchSpanProcessorOptions()));

  auto test_spans = GetTestSpans(batch_processor, max_queue_size);

  for (int i = 0; i < max_queue_size; ++i)
  {
    batch_processor->OnEnd(std::move(test_spans->at(i)));
  }

  // Give some time to export the spans
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  EXPECT_TRUE(batch_processor->ForceFlush());

  // Span should be exported by now
  EXPECT_GE(max_queue_size, spans_received->size());
}

TEST_F(AsyncBatchSpanProcessorTestPeer, TestManySpansLossLess)
{
  /* Test that no spans are lost when sending max_queue_size spans */

  std::shared_ptr<std::atomic<bool>> is_shutdown(new std::atomic<bool>(false));
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received(
      new std::vector<std::unique_ptr<sdk::trace::SpanData>>);

  const int num_spans = 2048;

  auto batch_processor =
      std::shared_ptr<sdk::trace::AsyncBatchSpanProcessor>(new sdk::trace::AsyncBatchSpanProcessor(
          std::unique_ptr<MockSpanExporter>(new MockSpanExporter(spans_received, is_shutdown)),
          sdk::trace::AsyncBatchSpanProcessorOptions()));

  auto test_spans = GetTestSpans(batch_processor, num_spans);

  for (int i = 0; i < num_spans; ++i)
  {
    batch_processor->OnEnd(std::move(test_spans->at(i)));
  }

  // Give some time to export the spans
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(batch_processor->ForceFlush());

  EXPECT_EQ(num_spans, spans_received->size());
  for (int i = 0; i < num_spans; ++i)
  {
    EXPECT_EQ("Span " + std::to_string(i), spans_received->at(i)->GetName());
  }
}

TEST_F(AsyncBatchSpanProcessorTestPeer, TestScheduleDelayMillis)
{
  /* Test that max_export_batch_size spans are exported every schedule_delay_millis
     seconds */

  std::shared_ptr<std::atomic<bool>> is_shutdown(new std::atomic<bool>(false));
  std::shared_ptr<std::atomic<bool>> is_export_completed(new std::atomic<bool>(false));
  std::shared_ptr<std::vector<std::unique_ptr<sdk::trace::SpanData>>> spans_received(
      new std::vector<std::unique_ptr<sdk::trace::SpanData>>);
  const std::chrono::milliseconds export_delay(0);
  const size_t max_export_batch_size = 512;
  sdk::trace::AsyncBatchSpanProcessorOptions options{};
  options.schedule_delay_millis = std::chrono::milliseconds(2000);

  auto batch_processor =
      std::shared_ptr<sdk::trace::AsyncBatchSpanProcessor>(new sdk::trace::AsyncBatchSpanProcessor(
          std::unique_ptr<MockSpanExporter>(
              new MockSpanExporter(spans_received, is_shutdown, is_export_completed, export_delay)),
          options));

  auto test_spans = GetTestSpans(batch_processor, max_export_batch_size);

  for (size_t i = 0; i < max_export_batch_size; ++i)
  {
    batch_processor->OnEnd(std::move(test_spans->at(i)));
  }

  // Sleep for schedule_delay_millis milliseconds
  std::this_thread::sleep_for(options.schedule_delay_millis);

  // small delay to give time to export
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Spans should be exported by now
  EXPECT_TRUE(is_export_completed->load());
  EXPECT_EQ(max_export_batch_size, spans_received->size());
  for (size_t i = 0; i < max_export_batch_size; ++i)
  {
    EXPECT_EQ("Span " + std::to_string(i), spans_received->at(i)->GetName());
  }
}

OPENTELEMETRY_END_NAMESPACE

#endif