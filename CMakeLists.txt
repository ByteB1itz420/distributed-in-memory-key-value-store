cmake_minimum_required(VERSION 3.16)
project(kvstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_executable(kvserver
    src/server/main.cpp
    src/server/server.cpp
    src/store/store.cpp
    src/proto/protocol.cpp
    src/repl/backlog.cpp
)

target_include_directories(kvserver PRIVATE src)

add_executable(kvcli
    src/cli/main.cpp
    src/proto/protocol.cpp
)

target_include_directories(kvcli PRIVATE src)

add_executable(kvtests
    tests/test_store.cpp
    src/store/store.cpp
    src/repl/backlog.cpp
)
target_include_directories(kvtests PRIVATE src)

add_executable(kvstress
    bench/stress.cpp
    src/proto/protocol.cpp
)
target_include_directories(kvstress PRIVATE src)

add_executable(kvbench
    bench/benchmark.cpp
    src/proto/protocol.cpp
)
target_include_directories(kvbench PRIVATE src)

add_test(NAME kvstore_tests COMMAND kvtests)
