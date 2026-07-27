cmake_minimum_required(VERSION 3.24)
project(faraday LANGUAGES CXX)

option(FARADAY_BUILD_TESTS "Build the Catch2 test binary" ON)

include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

add_library(faraday INTERFACE)
target_include_directories(faraday INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(faraday INTERFACE cxx_std_20)
target_link_libraries(faraday INTERFACE nlohmann_json::nlohmann_json)

add_executable(faraday_cli tools/faraday_cli.cpp)
target_link_libraries(faraday_cli PRIVATE faraday)

if(FARADAY_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)

    add_executable(test_faraday
        tests/test_sexpr.cpp
        tests/test_importer.cpp
        tests/test_tline.cpp
        tests/test_screener.cpp
        tests/test_real_boards.cpp
        tests/test_hyp.cpp
        tests/test_ipc.cpp)
    target_link_libraries(test_faraday PRIVATE faraday Catch2::Catch2WithMain)
    target_compile_definitions(test_faraday PRIVATE
        FARADAY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()
