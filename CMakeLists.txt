cmake_minimum_required(VERSION 3.20)
project(chatTerminal LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CHAT_ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)

add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
if(CHAT_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g3 -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

set(OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------- common
add_library(chat_common STATIC
  src/common/error.cpp
  src/common/time_util.cpp
  src/common/log.cpp
  src/common/buffer.cpp
  src/common/crc32.cpp
  src/common/sha256.cpp
  src/common/crypto.cpp
  src/common/utf8.cpp
  src/common/protocol.cpp
  src/common/net.cpp
  src/common/fs.cpp
)
target_include_directories(chat_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chat_common PUBLIC pthread)

# ---------------------------------------------------------------- server
add_library(chat_server STATIC
  src/server/config.cpp
  src/server/io_loop.cpp
  src/server/thread_pool.cpp
  src/server/session.cpp
  src/server/auth.cpp
  src/server/router.cpp
  src/server/storage.cpp
  src/server/dispatcher.cpp
  src/server/server.cpp
)
target_include_directories(chat_server PUBLIC ${CMAKE_SOURCE_DIR}/src/server)
target_link_libraries(chat_server PUBLIC chat_common)

add_executable(chatd src/server/main.cpp)
target_link_libraries(chatd PRIVATE chat_server)
set_target_properties(chatd PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

# ---------------------------------------------------------------- signaling
add_executable(chatsig
  src/sig/main.cpp
  src/sig/sig_server.cpp
)
target_link_libraries(chatsig PRIVATE chat_server)
set_target_properties(chatsig PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

# ---------------------------------------------------------------- client
add_executable(chatt
  src/client/main.cpp
  src/client/term.cpp
  src/client/editor.cpp
  src/client/render.cpp
  src/client/net_client.cpp
  src/client/commands.cpp
  src/client/app.cpp
  src/client/p2p_app.cpp
)
# P2P 模式复用 RoomStore（本地分段日志），所以链接 chat_server
target_link_libraries(chatt PRIVATE chat_server)
set_target_properties(chatt PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

# ---------------------------------------------------------------- tests
add_executable(chat_tests
  tests/test_main.cpp
  tests/test_protocol.cpp
  tests/test_buffer.cpp
  tests/test_storage.cpp
  tests/test_editor.cpp
  tests/test_crypto.cpp
  src/client/editor.cpp
)
target_include_directories(chat_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/client ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(chat_tests PRIVATE chat_server)
set_target_properties(chat_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------- tools
add_executable(cht-load tools/cht_load.cpp)
target_link_libraries(cht-load PRIVATE chat_common)
set_target_properties(cht-load PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

add_executable(cht-dump tools/cht_dump.cpp)
target_link_libraries(cht-dump PRIVATE chat_server)
set_target_properties(cht-dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

add_executable(cht-bench tools/cht_bench.cpp)
target_link_libraries(cht-bench PRIVATE chat_server)
set_target_properties(cht-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
