/**
 *
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

#undef NDEBUG
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "core/Core.h"
#include "core/repository/AtomicRepoEntries.h"
#include "core/RepositoryFactory.h"
#include "FlowFileRecord.h"
#include "provenance/Provenance.h"
#include "properties/Configure.h"
#include "../unit/ProvenanceTestHelper.h"
#include "../TestBase.h"
#include "YamlConfiguration.h"
#include "CustomProcessors.h"
#include "TestControllerWithFlow.h"

const char* yamlConfig =
    R"(
Flow Controller:
    name: MiNiFi Flow
    id: 2438e3c8-015a-1000-79ca-83af40ec1990
Processors:
  - name: Generator
    id: 2438e3c8-015a-1000-79ca-83af40ec1991
    class: org.apache.nifi.processors.standard.TestFlowFileGenerator
    max concurrent tasks: 1
    scheduling strategy: TIMER_DRIVEN
    scheduling period: 100 ms
    penalization period: 300 ms
    yield period: 100 ms
    run duration nanos: 0
    auto-terminated relationships list:
    Properties:
      Batch Size: 3
  - name: TestProcessor
    id: 2438e3c8-015a-1000-79ca-83af40ec1992
    class: org.apache.nifi.processors.standard.TestProcessor
    max concurrent tasks: 1
    scheduling strategy: TIMER_DRIVEN
    scheduling period: 100 ms
    penalization period: 3 sec
    yield period: 1 sec
    run duration nanos: 0
    auto-terminated relationships list:
      - apple
      - banana
Connections:
  - name: Gen
    id: 2438e3c8-015a-1000-79ca-83af40ec1997
    source name: Generator
    source id: 2438e3c8-015a-1000-79ca-83af40ec1991
    source relationship name: success
    destination name: TestProcessor
    destination id: 2438e3c8-015a-1000-79ca-83af40ec1992
    max work queue size: 0
    max work queue data size: 1 MB
    flowfile expiration: 60 sec
Remote Processing Groups:
)";

TEST_CASE("Flow shutdown drains connections", "[TestFlow1]") {
  TestControllerWithFlow testController(yamlConfig);
  auto controller = testController.controller_;
  auto root = testController.root_;

  testController.configuration_->set(minifi::Configure::nifi_flowcontroller_drain_timeout, "100 ms");

  auto sinkProc = std::static_pointer_cast<minifi::processors::TestProcessor>(root->findProcessor("TestProcessor"));
  // prevent execution of the consumer processor
  sinkProc->yield(10000);

  std::map<std::string, std::shared_ptr<minifi::Connection>> connectionMap;

  root->getConnections(connectionMap);
  // adds the single connection to the map both by name and id
  REQUIRE(connectionMap.size() == 2);

  testController.startFlow();

  // wait for the generator to create some files
  std::this_thread::sleep_for(std::chrono::milliseconds{1000});

  for (auto& it : connectionMap) {
    REQUIRE(it.second->getQueueSize() > 10);
  }

  controller->stop(true);

  REQUIRE(sinkProc->trigger_count == 0);

  for (auto& it : connectionMap) {
    REQUIRE(it.second->isEmpty());
  }
}

TEST_CASE("Flow shutdown waits for a while", "[TestFlow2]") {
  TestControllerWithFlow testController(yamlConfig);
  auto controller = testController.controller_;
  auto root = testController.root_;

  testController.configuration_->set(minifi::Configure::nifi_flowcontroller_drain_timeout, "10 s");

  auto sourceProc = std::static_pointer_cast<minifi::processors::TestFlowFileGenerator>(root->findProcessor("Generator"));
  auto sinkProc = std::static_pointer_cast<minifi::processors::TestProcessor>(root->findProcessor("TestProcessor"));

  // prevent the initial trigger
  // in case the source got triggered
  // and the scheduler triggers the sink
  // before we could initiate the shutdown
  sinkProc->yield(100);

  testController.startFlow();

  // wait for the source processor to enqueue its flowFiles
  int tryCount = 0;
  while (tryCount++ < 10 && root->getTotalFlowFileCount() != 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  REQUIRE(root->getTotalFlowFileCount() == 3);
  REQUIRE(sourceProc->trigger_count.load() == 1);
  REQUIRE(sinkProc->trigger_count.load() == 0);

  controller->stop(true);

  REQUIRE(sourceProc->trigger_count.load() == 1);
  REQUIRE(sinkProc->trigger_count.load() == 3);
}

TEST_CASE("Flow stopped after grace period", "[TestFlow3]") {
  TestControllerWithFlow testController(yamlConfig);
  auto controller = testController.controller_;
  auto root = testController.root_;

  testController.configuration_->set(minifi::Configure::nifi_flowcontroller_drain_timeout, "1000 ms");

  auto sourceProc = std::static_pointer_cast<minifi::processors::TestFlowFileGenerator>(root->findProcessor("Generator"));
  auto sinkProc = std::static_pointer_cast<minifi::processors::TestProcessor>(root->findProcessor("TestProcessor"));

  // prevent the initial trigger
  // in case the source got triggered
  // and the scheduler triggers the sink
  sinkProc->yield(100);

  sinkProc->onTriggerCb_ = [&]{
    static std::atomic<bool> first_onTrigger{true};
    bool isFirst = true;
    // sleep only on the first trigger
    if (first_onTrigger.compare_exchange_strong(isFirst, false)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    }
  };

  testController.startFlow();

  // wait for the source processor to enqueue its flowFiles
  int tryCount = 0;
  while (tryCount++ < 10 && root->getTotalFlowFileCount() != 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  REQUIRE(root->getTotalFlowFileCount() == 3);
  REQUIRE(sourceProc->trigger_count.load() == 1);
  REQUIRE(sinkProc->trigger_count.load() == 0);

  controller->stop(true);

  REQUIRE(sourceProc->trigger_count.load() == 1);
  REQUIRE(sinkProc->trigger_count.load() == 1);
}

TEST_CASE("Extend the waiting period during shutdown", "[TestFlow4]") {
  TestControllerWithFlow testController(yamlConfig);
  auto controller = testController.controller_;
  auto root = testController.root_;

  int timeout_ms = 1000;

  testController.configuration_->set(minifi::Configure::nifi_flowcontroller_drain_timeout, std::to_string(timeout_ms) + " ms");

  auto sourceProc = std::static_pointer_cast<minifi::processors::TestFlowFileGenerator>(root->findProcessor("Generator"));
  auto sinkProc = std::static_pointer_cast<minifi::processors::TestProcessor>(root->findProcessor("TestProcessor"));

  // prevent the initial trigger
  // in case the source got triggered
  // and the scheduler triggers the sink
  sinkProc->yield(100);

  sinkProc->onTriggerCb_ = [&]{
    static std::atomic<bool> first_onTrigger{true};
    bool isFirst = true;
    // sleep only on the first trigger
    if (first_onTrigger.compare_exchange_strong(isFirst, false)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    }
  };

  testController.startFlow();

  // wait for the source processor to enqueue its flowFiles
  int tryCount = 0;
  while (tryCount++ < 10 && root->getTotalFlowFileCount() != 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  REQUIRE(root->getTotalFlowFileCount() == 3);
  REQUIRE(sourceProc->trigger_count.load() == 1);
  REQUIRE(sinkProc->trigger_count.load() == 0);

  std::thread shutdownThread([&]{
    controller->stop(true);
  });

  while (controller->isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    timeout_ms += 500;
    testController.configuration_->set(minifi::Configure::nifi_flowcontroller_drain_timeout, std::to_string(timeout_ms) + " ms");
  }

  shutdownThread.join();

  REQUIRE(sourceProc->trigger_count.load() == 1);
  REQUIRE(sinkProc->trigger_count.load() == 3);
}
