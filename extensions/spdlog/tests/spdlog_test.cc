//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <gtest/gtest.h>

#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "znet/ext/spdlog/spdlog.h"

using znet::LogLevel;
using znet::ext::SpdlogBridge;

namespace {

struct Record {
  spdlog::level::level_enum level;
  std::string payload;
};

/** @brief A spdlog sink that keeps records so the test can inspect them. */
class CapturingSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<Record> records;

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    records.push_back(
        Record{msg.level, std::string(msg.payload.data(), msg.payload.size())});
  }
  void flush_() override {}
};

struct Capture {
  std::shared_ptr<CapturingSink> sink = std::make_shared<CapturingSink>();
  std::shared_ptr<spdlog::logger> logger;

  Capture() {
    logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::trace);
  }
};

bool ContainsAnsiEscape(const std::string& text) {
  return text.find('\x1b') != std::string::npos;
}

}  // namespace

TEST(SpdlogBridge, EachLevelArrivesAtItsSpdlogEquivalent) {
  Capture capture;
  {
    SpdlogBridge bridge{capture.logger};
    ZNET_LOG_DEBUG("a debug line");
    ZNET_LOG_INFO("an info line");
    ZNET_LOG_WARN("a warn line");
    ZNET_LOG_ERROR("an error line");
  }

  ASSERT_EQ(capture.sink->records.size(), 4u);
  EXPECT_EQ(capture.sink->records[0].level, spdlog::level::debug);
  EXPECT_EQ(capture.sink->records[1].level, spdlog::level::info);
  EXPECT_EQ(capture.sink->records[2].level, spdlog::level::warn);
  EXPECT_EQ(capture.sink->records[3].level, spdlog::level::err);
}

// the point of routing through the sink rather than the stream: the severity
// arrives as a value, and the payload is the message on its own.
TEST(SpdlogBridge, PayloadIsTheMessageWithoutDecoration) {
  Capture capture;
  {
    SpdlogBridge bridge{capture.logger};
    ZNET_LOG_WARN("disk is at {}%", 91);
  }

  ASSERT_EQ(capture.sink->records.size(), 1u);
  const std::string& payload = capture.sink->records[0].payload;
  EXPECT_EQ(payload, "disk is at 91%");
  EXPECT_FALSE(ContainsAnsiEscape(payload)) << payload;
  EXPECT_EQ(payload.find("[warn"), std::string::npos)
      << "the level should be a value, not text in the message";
}

TEST(SpdlogBridge, FormatsArgumentsAsUsual) {
  Capture capture;
  {
    SpdlogBridge bridge{capture.logger};
    ZNET_LOG_INFO("{} peers, {} bytes", 3, 4096);
  }
  ASSERT_EQ(capture.sink->records.size(), 1u);
  EXPECT_EQ(capture.sink->records[0].payload, "3 peers, 4096 bytes");
}

TEST(SpdlogBridge, FunctionNameIsOptOut) {
  Capture with_function;
  {
    SpdlogBridge bridge{with_function.logger, /*include_function=*/true};
    ZNET_LOG_INFO("hello");
  }
  ASSERT_EQ(with_function.sink->records.size(), 1u);
  EXPECT_NE(with_function.sink->records[0].payload.find("hello"),
            std::string::npos);
  EXPECT_NE(with_function.sink->records[0].payload.find(':'),
            std::string::npos);

  Capture without_function;
  {
    SpdlogBridge bridge{without_function.logger};
    ZNET_LOG_INFO("hello");
  }
  ASSERT_EQ(without_function.sink->records.size(), 1u);
  EXPECT_EQ(without_function.sink->records[0].payload, "hello");
}

// ---------------------------------------------------------------------------
// Installing and standing down
// ---------------------------------------------------------------------------

// once the bridge is gone, logging goes back where it was, rather than
// vanishing or writing through a dangling pointer.
TEST(SpdlogBridge, RestoresTheStreamOnDestruction) {
  Capture capture;
  std::ostringstream stream;
  znet::SetLogStream(stream);

  {
    SpdlogBridge bridge{capture.logger};
    ZNET_LOG_INFO("through spdlog");
  }
  ZNET_LOG_INFO("through the stream");

  EXPECT_EQ(capture.sink->records.size(), 1u);
  EXPECT_NE(stream.str().find("through the stream"), std::string::npos);
  EXPECT_EQ(stream.str().find("through spdlog"), std::string::npos);

  znet::SetLogStream(std::cout);
}

TEST(SpdlogBridge, NestedBridgesUnwindInOrder) {
  Capture outer;
  Capture inner;

  SpdlogBridge outer_bridge{outer.logger};
  ZNET_LOG_INFO("outer one");
  {
    SpdlogBridge inner_bridge{inner.logger};
    ZNET_LOG_INFO("inner");
  }
  ZNET_LOG_INFO("outer two");

  ASSERT_EQ(inner.sink->records.size(), 1u);
  EXPECT_EQ(inner.sink->records[0].payload, "inner");
  ASSERT_EQ(outer.sink->records.size(), 2u);
  EXPECT_EQ(outer.sink->records[0].payload, "outer one");
  EXPECT_EQ(outer.sink->records[1].payload, "outer two");
}

// a bridge that has been superseded must not unhook the one that replaced it
// when it goes out of scope, or destruction order would decide where logs go.
TEST(SpdlogBridge, SupersededBridgeDoesNotUnhookItsReplacement) {
  Capture first;
  Capture second;

  auto first_bridge = std::unique_ptr<SpdlogBridge>(
      new SpdlogBridge{first.logger});
  auto second_bridge = std::unique_ptr<SpdlogBridge>(
      new SpdlogBridge{second.logger});

  first_bridge.reset();  // destroyed out of order, while second is installed
  ZNET_LOG_INFO("still routed");

  EXPECT_TRUE(first.sink->records.empty());
  ASSERT_EQ(second.sink->records.size(), 1u);
  EXPECT_EQ(second.sink->records[0].payload, "still routed");

  second_bridge.reset();
}

// without a bridge the library behaves exactly as before, decoration included.
TEST(SpdlogBridge, DefaultPathIsUntouched) {
  std::ostringstream stream;
  znet::SetLogStream(stream);
  ZNET_LOG_ERROR("plain path");
  znet::SetLogStream(std::cout);

  EXPECT_NE(stream.str().find("plain path"), std::string::npos);
  EXPECT_TRUE(ContainsAnsiEscape(stream.str()))
      << "the stream path still renders its own prefix";
}

TEST(SpdlogBridge, LevelMappingIsTotal) {
  EXPECT_EQ(SpdlogBridge::ToSpdlogLevel(LogLevel::Debug), spdlog::level::debug);
  EXPECT_EQ(SpdlogBridge::ToSpdlogLevel(LogLevel::Info), spdlog::level::info);
  EXPECT_EQ(SpdlogBridge::ToSpdlogLevel(LogLevel::Warn), spdlog::level::warn);
  EXPECT_EQ(SpdlogBridge::ToSpdlogLevel(LogLevel::Error), spdlog::level::err);
}
