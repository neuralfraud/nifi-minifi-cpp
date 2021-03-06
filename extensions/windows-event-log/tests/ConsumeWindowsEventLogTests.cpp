/**
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ConsumeWindowsEventLog.h"

#include "core/ConfigurableComponent.h"
#include "processors/LogAttribute.h"
#include "TestBase.h"

using ConsumeWindowsEventLog = org::apache::nifi::minifi::processors::ConsumeWindowsEventLog;
using LogAttribute = org::apache::nifi::minifi::processors::LogAttribute;
using ConfigurableComponent = org::apache::nifi::minifi::core::ConfigurableComponent;
using IdGenerator = org::apache::nifi::minifi::utils::IdGenerator;

namespace {

core::Relationship Success{"success", "Everything is fine"};

const std::string APPLICATION_CHANNEL = "Application";

constexpr DWORD CWEL_TESTS_OPCODE = 14985;  // random opcode hopefully won't clash with something important

void reportEvent(const std::string& channel, const char* message, WORD log_level = EVENTLOG_INFORMATION_TYPE) {
  auto event_source = RegisterEventSourceA(nullptr, channel.c_str());
  auto deleter = gsl::finally([&event_source](){ DeregisterEventSource(event_source); });
  ReportEventA(event_source, log_level, 0, CWEL_TESTS_OPCODE, nullptr, 1, 0, &message, nullptr);
}

}  // namespace

TEST_CASE("ConsumeWindowsEventLog constructor works", "[create]") {
  TestController test_controller;
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  REQUIRE_NOTHROW(ConsumeWindowsEventLog processor_one("one"));

  REQUIRE_NOTHROW(
    utils::Identifier uuid = utils::IdGenerator::getIdGenerator()->generate();
    ConsumeWindowsEventLog processor_two("two", uuid);
  );  // NOLINT

  REQUIRE_NOTHROW(
    auto processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  );  // NOLINT
}

TEST_CASE("ConsumeWindowsEventLog properties work with default values", "[create][properties]") {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConfigurableComponent>();
  LogTestController::getInstance().setTrace<ConsumeWindowsEventLog>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_controller.runSession(test_plan);

  auto properties_required_or_with_default_value = {
    ConsumeWindowsEventLog::Channel,
    ConsumeWindowsEventLog::Query,
    // ConsumeWindowsEventLog::RenderFormatXML,  // FIXME(fgerlits): not defined, does not exist in NiFi either; should be removed
    ConsumeWindowsEventLog::MaxBufferSize,
    // ConsumeWindowsEventLog::InactiveDurationToReconnect,  // FIXME(fgerlits): obsolete, see definition; should be removed
    ConsumeWindowsEventLog::IdentifierMatcher,
    ConsumeWindowsEventLog::IdentifierFunction,
    ConsumeWindowsEventLog::ResolveAsAttributes,
    ConsumeWindowsEventLog::EventHeader,
    ConsumeWindowsEventLog::OutputFormat,
    ConsumeWindowsEventLog::BatchCommitSize,
    ConsumeWindowsEventLog::BookmarkRootDirectory,  // TODO(fgerlits): obsolete, see definition; remove in a later release
    ConsumeWindowsEventLog::ProcessOldEvents
  };
  for (const core::Property& property : properties_required_or_with_default_value) {
    if (!LogTestController::getInstance().contains("property name " + property.getName() + " value ")) {
      FAIL("Property did not get queried: " << property.getName());
    }
  }

  auto properties_optional_without_default_value = {
    ConsumeWindowsEventLog::EventHeaderDelimiter
  };
  for (const core::Property& property : properties_optional_without_default_value) {
    if (!LogTestController::getInstance().contains("property name " + property.getName() + ", empty value")) {
      FAIL("Optional property did not get queried: " << property.getName());
    }
  }

  REQUIRE(LogTestController::getInstance().contains("Successfully configured CWEL"));
}

TEST_CASE("ConsumeWindowsEventLog onSchedule throws if it cannot create the bookmark", "[create][bookmark]") {
  TestController test_controller;
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(processor, ConsumeWindowsEventLog::Channel.getName(), "NonexistentChannel1234981");

  REQUIRE_THROWS_AS(test_controller.runSession(test_plan), minifi::Exception);
}

TEST_CASE("ConsumeWindowsEventLog can consume new events", "[onTrigger]") {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConsumeWindowsEventLog>();
  LogTestController::getInstance().setDebug<LogAttribute>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto cwel_processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::Channel.getName(), APPLICATION_CHANNEL);

  auto logger_processor = test_plan->addProcessor("LogAttribute", "logger", Success, true);
  test_plan->setProperty(logger_processor, LogAttribute::FlowFilesToLog.getName(), "0");
  test_plan->setProperty(logger_processor, LogAttribute::LogPayload.getName(), "true");
  test_plan->setProperty(logger_processor, LogAttribute::MaxPayloadLineLength.getName(), "1024");

  reportEvent(APPLICATION_CHANNEL, "Event zero");

  test_controller.runSession(test_plan);
  REQUIRE(LogTestController::getInstance().contains("processed 0 Events"));
  // event zero is not reported as the bookmark is created on the first run
  // and we use the default config setting ProcessOldEvents = false
  // later runs will start with a bookmark saved in the state manager

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  SECTION("Read one event") {
    reportEvent(APPLICATION_CHANNEL, "Event one");

    test_controller.runSession(test_plan);
    REQUIRE(LogTestController::getInstance().contains("processed 1 Events"));
    REQUIRE(LogTestController::getInstance().contains("<EventData><Data>Event one</Data></EventData>"));
  }

  SECTION("Read two events") {
    reportEvent(APPLICATION_CHANNEL, "Event two");
    reportEvent(APPLICATION_CHANNEL, "Event three");

    test_controller.runSession(test_plan);
    REQUIRE(LogTestController::getInstance().contains("processed 2 Events"));
    REQUIRE(LogTestController::getInstance().contains("<EventData><Data>Event two</Data></EventData>"));
    REQUIRE(LogTestController::getInstance().contains("<EventData><Data>Event three</Data></EventData>"));
  }
}

TEST_CASE("ConsumeWindowsEventLog bookmarking works", "[onTrigger]") {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConsumeWindowsEventLog>();
  LogTestController::getInstance().setDebug<LogAttribute>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto cwel_processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::Channel.getName(), APPLICATION_CHANNEL);

  auto logger_processor = test_plan->addProcessor("LogAttribute", "logger", Success, true);
  test_plan->setProperty(logger_processor, LogAttribute::FlowFilesToLog.getName(), "0");

  reportEvent(APPLICATION_CHANNEL, "Event zero");

  test_controller.runSession(test_plan);
  REQUIRE(LogTestController::getInstance().contains("processed 0 Events"));

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  SECTION("Read in one go") {
    reportEvent(APPLICATION_CHANNEL, "Event one");
    reportEvent(APPLICATION_CHANNEL, "Event two");
    reportEvent(APPLICATION_CHANNEL, "Event three");

    test_controller.runSession(test_plan);
    REQUIRE(LogTestController::getInstance().contains("processed 3 Events"));
  }

  SECTION("Read in two batches") {
    reportEvent(APPLICATION_CHANNEL, "Event one");

    test_controller.runSession(test_plan);
    REQUIRE(LogTestController::getInstance().contains("processed 1 Events"));

    reportEvent(APPLICATION_CHANNEL, "Event two");
    reportEvent(APPLICATION_CHANNEL, "Event three");

    test_plan->reset();
    LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

    test_controller.runSession(test_plan);
    REQUIRE(LogTestController::getInstance().contains("processed 2 Events"));
  }
}

TEST_CASE("ConsumeWindowsEventLog extracts some attributes by default", "[onTrigger]") {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConsumeWindowsEventLog>();
  LogTestController::getInstance().setDebug<LogAttribute>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto cwel_processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::Channel.getName(), APPLICATION_CHANNEL);

  auto logger_processor = test_plan->addProcessor("LogAttribute", "logger", Success, true);
  test_plan->setProperty(logger_processor, LogAttribute::FlowFilesToLog.getName(), "0");

  // 0th event, only to create a bookmark
  {
    reportEvent(APPLICATION_CHANNEL, "Event zero: this is in the past");

    test_controller.runSession(test_plan);
  }

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  // 1st event, on Info level
  {
    reportEvent(APPLICATION_CHANNEL, "Event one: something interesting happened", EVENTLOG_INFORMATION_TYPE);

    test_controller.runSession(test_plan);

    REQUIRE(LogTestController::getInstance().contains("key:Keywords value:Classic"));
    REQUIRE(LogTestController::getInstance().contains("key:Level value:Information"));
  }

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  // 2st event, on Warning level
  {
    reportEvent(APPLICATION_CHANNEL, "Event two: something fishy happened!", EVENTLOG_WARNING_TYPE);

    test_controller.runSession(test_plan);

    REQUIRE(LogTestController::getInstance().contains("key:Keywords value:Classic"));
    REQUIRE(LogTestController::getInstance().contains("key:Level value:Warning"));
  }
}

namespace {

void outputFormatSetterTestHelper(const std::string &output_format, int expected_num_flow_files) {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConsumeWindowsEventLog>();
  LogTestController::getInstance().setDebug<LogAttribute>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto cwel_processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::Channel.getName(), APPLICATION_CHANNEL);
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::OutputFormat.getName(), output_format);

  auto logger_processor = test_plan->addProcessor("LogAttribute", "logger", Success, true);
  test_plan->setProperty(logger_processor, LogAttribute::FlowFilesToLog.getName(), "0");

  {
    reportEvent(APPLICATION_CHANNEL, "Event zero: this is in the past");

    test_controller.runSession(test_plan);
  }

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  {
    reportEvent(APPLICATION_CHANNEL, "Event one");

    test_controller.runSession(test_plan);

    REQUIRE(LogTestController::getInstance().contains("Logged " + std::to_string(expected_num_flow_files) + " flow files"));
  }
}

}  // namespace

TEST_CASE("ConsumeWindowsEventLog output format can be set", "[create][output_format]") {
  outputFormatSetterTestHelper("XML", 1);
  outputFormatSetterTestHelper("Plaintext", 1);
  outputFormatSetterTestHelper("Both", 2);

  // NOTE(fgerlits): this may be a bug, as I would expect this to throw in onSchedule(),
  // but it starts merrily, just does not write flow files in either format
  outputFormatSetterTestHelper("InvalidValue", 0);
}

// NOTE(fgerlits): I don't know how to unit test this, as my manually published events all result in an empty string if OutputFormat is Plaintext
//                 but it does seem to work, based on manual tests reading system logs
// TEST_CASE("ConsumeWindowsEventLog prints events in plain text correctly", "[onTrigger]")

TEST_CASE("ConsumeWindowsEventLog prints events in XML correctly", "[onTrigger]") {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConsumeWindowsEventLog>();
  LogTestController::getInstance().setDebug<LogAttribute>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto cwel_processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::Channel.getName(), APPLICATION_CHANNEL);
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::OutputFormat.getName(), "XML");

  auto logger_processor = test_plan->addProcessor("LogAttribute", "logger", Success, true);
  test_plan->setProperty(logger_processor, LogAttribute::FlowFilesToLog.getName(), "0");
  test_plan->setProperty(logger_processor, LogAttribute::LogPayload.getName(), "true");
  test_plan->setProperty(logger_processor, LogAttribute::MaxPayloadLineLength.getName(), "1024");

  {
    reportEvent(APPLICATION_CHANNEL, "Event zero: this is in the past");

    test_controller.runSession(test_plan);
  }

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  {
    reportEvent(APPLICATION_CHANNEL, "Event one");

    test_controller.runSession(test_plan);

    REQUIRE(LogTestController::getInstance().contains(R"(<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event"><System><Provider Name="Application"/>)"));
    REQUIRE(LogTestController::getInstance().contains(R"(<EventID Qualifiers="0">14985</EventID><Level>4</Level><Task>0</Task><Keywords>0x80000000000000</Keywords><TimeCreated SystemTime=")"));
    // the timestamp (when the event was published) goes here
    REQUIRE(LogTestController::getInstance().contains(R"("/><EventRecordID>)"));
    // the ID of the event goes here (a number)
    REQUIRE(LogTestController::getInstance().contains(R"(</EventRecordID><Channel>Application</Channel><Computer>)"));
    // the computer name goes here
    REQUIRE(LogTestController::getInstance().contains(R"(</Computer><Security/></System><EventData><Data>Event one</Data></EventData></Event>)"));
  }
}

namespace {

void batchCommitSizeTestHelper(int batch_commit_size, int expected_num_commits) {
  TestController test_controller;
  LogTestController::getInstance().setDebug<ConsumeWindowsEventLog>();
  std::shared_ptr<TestPlan> test_plan = test_controller.createPlan();

  auto cwel_processor = test_plan->addProcessor("ConsumeWindowsEventLog", "cwel");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::Channel.getName(), APPLICATION_CHANNEL);
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::OutputFormat.getName(), "XML");
  test_plan->setProperty(cwel_processor, ConsumeWindowsEventLog::BatchCommitSize.getName(), std::to_string(batch_commit_size));

  {
    reportEvent(APPLICATION_CHANNEL, "Event zero: this is in the past");

    test_controller.runSession(test_plan);
  }

  test_plan->reset();
  LogTestController::getInstance().resetStream(LogTestController::getInstance().log_output);

  {
    reportEvent(APPLICATION_CHANNEL, "Event one");
    reportEvent(APPLICATION_CHANNEL, "Event two");
    reportEvent(APPLICATION_CHANNEL, "Event three");
    reportEvent(APPLICATION_CHANNEL, "Event four");
    reportEvent(APPLICATION_CHANNEL, "Event five");

    test_controller.runSession(test_plan);

    REQUIRE(LogTestController::getInstance().countOccurrences("processQueue commit") == expected_num_commits);
  }
}

}  // namespace

TEST_CASE("ConsumeWindowsEventLog batch commit size works", "[onTrigger]") {
  batchCommitSizeTestHelper(1000, 1);
  batchCommitSizeTestHelper(5, 1);
  batchCommitSizeTestHelper(4, 2);
  batchCommitSizeTestHelper(3, 2);
  batchCommitSizeTestHelper(2, 3);
  batchCommitSizeTestHelper(1, 5);
  batchCommitSizeTestHelper(0, 1);
}
