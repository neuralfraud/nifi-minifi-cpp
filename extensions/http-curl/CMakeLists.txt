#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

include(${CMAKE_SOURCE_DIR}/extensions/ExtensionHeader.txt)
include_directories(protocols client processors sitetosite)

file(GLOB SOURCES  "*.cpp" "protocols/*.cpp" "client/*.cpp" "processors/*.cpp" "sitetosite/*.cpp")

add_library(minifi-http-curl STATIC ${SOURCES})
set_property(TARGET minifi-http-curl PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(minifi-http-curl ${LIBMINIFI} Threads::Threads)
target_link_libraries(minifi-http-curl CURL::libcurl RapidJSON)
target_link_libraries(minifi-http-curl minifi-civet-extensions)

SET (HTTP-CURL minifi-http-curl PARENT_SCOPE)
register_extension(minifi-http-curl)
