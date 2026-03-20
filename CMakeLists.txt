cmake_minimum_required(VERSION 3.20)
project(LemonTea LANGUAGES CXX)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
  asio
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
)
FetchContent_Declare(
  nlohmann_json
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_Declare(
  crow
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://github.com/CrowCpp/Crow/archive/refs/tags/v1.2.1.tar.gz
)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.1
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(asio nlohmann_json crow)
FetchContent_MakeAvailable(libdatachannel)

add_executable(lemontea
  src/main.cpp
)

target_include_directories(lemontea PRIVATE
  ${asio_SOURCE_DIR}/asio/include
  ${nlohmann_json_SOURCE_DIR}/single_include
  ${crow_SOURCE_DIR}/include
)

target_compile_definitions(lemontea PRIVATE ASIO_STANDALONE)
target_link_libraries(lemontea PRIVATE LibDataChannel::LibDataChannel Threads::Threads)
target_link_options(lemontea PRIVATE -pthread)
