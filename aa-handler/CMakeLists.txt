cmake_minimum_required(VERSION 3.16)
project(aa-handler CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_WITH_AASDK "Link against libaasdk for USB Android Auto" OFF)

find_package(Threads REQUIRED)

set(AA_HANDLER_SOURCES
  src/main.cpp
  src/config.cpp
  src/ipc_bridge.cpp
  src/stub_handler.cpp
)

if(BUILD_WITH_AASDK)
  list(APPEND AA_HANDLER_SOURCES src/aasdk_runner.cpp)
endif()

add_executable(aa-handler ${AA_HANDLER_SOURCES})
target_include_directories(aa-handler PRIVATE src)
target_link_libraries(aa-handler PRIVATE Threads::Threads)

if(BUILD_WITH_AASDK)
  target_compile_definitions(aa-handler PRIVATE BUILD_WITH_AASDK=1)
  target_include_directories(aa-handler PRIVATE /usr/local/include)
  link_directories(/usr/local/lib)
  target_link_libraries(aa-handler PRIVATE aasdk usb-1.0 ssl crypto protobuf boost_system boost_log)
endif()

install(TARGETS aa-handler RUNTIME DESTINATION bin)
