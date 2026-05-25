cmake_minimum_required(VERSION 3.16)
project(aa-handler CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_WITH_AASDK "Link against libaasdk for real USB Android Auto" OFF)

find_package(Threads REQUIRED)

set(AA_HANDLER_SOURCES
  src/main.cpp
  src/config.cpp
  src/ipc_bridge.cpp
  src/stub_handler.cpp
)

if(BUILD_WITH_AASDK)
  list(APPEND AA_HANDLER_SOURCES
    src/aasdk_runner.cpp
    src/aa/Session.cpp
  )
endif()

add_executable(aa-handler ${AA_HANDLER_SOURCES})
target_include_directories(aa-handler PRIVATE src)
target_link_libraries(aa-handler PRIVATE Threads::Threads)

if(BUILD_WITH_AASDK)
  target_compile_definitions(aa-handler PRIVATE BUILD_WITH_AASDK=1)

  # We rely on aasdk + aap_protobuf being installed to /usr/local (the
  # aasdk-build Docker stage does `make install`). The exported CMake package
  # config has a known broken INSTALL_INTERFACE include path, so we link
  # manually here which is also less surprising to read.
  target_include_directories(aa-handler PRIVATE
    /usr/local/include
    /usr/local/include/aap_protobuf
  )
  target_link_directories(aa-handler PRIVATE /usr/local/lib)
  target_link_libraries(aa-handler PRIVATE
    aasdk
    aap_protobuf
    usb-1.0
    ssl
    crypto
    protobuf
    boost_system
    boost_log
    boost_log_setup
    boost_thread
    boost_filesystem
  )

  # libusb headers are typically under /usr/include/libusb-1.0 on Debian.
  find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
  if(LIBUSB_INCLUDE_DIR)
    target_include_directories(aa-handler PRIVATE ${LIBUSB_INCLUDE_DIR})
  endif()
endif()

install(TARGETS aa-handler RUNTIME DESTINATION bin)
