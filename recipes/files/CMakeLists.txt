cmake_minimum_required(VERSION 3.16)
project(jvideo-services)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(SYSTEMD libsystemd)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# Find MessagePack
find_path(MSGPACK_INCLUDE_DIRS msgpack.hpp
    PATHS /usr/include /usr/local/include
    PATH_SUFFIXES msgpack
)

if(NOT MSGPACK_INCLUDE_DIRS)
    # Try pkg-config as fallback
    pkg_check_modules(MSGPACK msgpack)
    if(NOT MSGPACK_FOUND)
        message(FATAL_ERROR "MessagePack not found")
    endif()
endif()

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections")

# Common libraries - Removed Boost dependencies
set(COMMON_LIBS
    ${OpenCV_LIBS}
    ${ZMQ_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    nlohmann_json::nlohmann_json
    Threads::Threads
    systemd
    rt
)

# Common include directories - Removed Boost includes
set(COMMON_INCLUDES
    ${OPENCV_INCLUDE_DIRS}
    ${ZMQ_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${MSGPACK_INCLUDE_DIRS}
)

if(SYSTEMD_FOUND)
    list(APPEND COMMON_LIBS ${SYSTEMD_LIBRARIES})
    add_compile_definitions(HAS_SYSTEMD=1)
endif()

# Frame Publisher executable
add_executable(frame-publisher-cpp frame_publisher.cpp)
target_include_directories(frame-publisher-cpp PRIVATE
    ${COMMON_INCLUDES}
)
target_link_libraries(frame-publisher-cpp
    ${COMMON_LIBS}
)
target_compile_options(frame-publisher-cpp PRIVATE
    ${OPENCV_CFLAGS_OTHER}
    ${ZMQ_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
)

# Frame Resizer executable
add_executable(frame-resizer-cpp frame_resizer.cpp)
target_include_directories(frame-resizer-cpp PRIVATE
    ${COMMON_INCLUDES}
)
target_link_libraries(frame-resizer-cpp
    ${COMMON_LIBS}
)
target_compile_options(frame-resizer-cpp PRIVATE
    ${OPENCV_CFLAGS_OTHER}
    ${ZMQ_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
)

# Frame Saver executable
add_executable(frame-saver-cpp frame_saver.cpp)
target_include_directories(frame-saver-cpp PRIVATE
    ${COMMON_INCLUDES}
)
target_link_libraries(frame-saver-cpp
    ${COMMON_LIBS}
)
target_compile_options(frame-saver-cpp PRIVATE
    ${OPENCV_CFLAGS_OTHER}
    ${ZMQ_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
)

# Queue Monitor executable
add_executable(queue-monitor-cpp queue_monitor.cpp)
target_include_directories(queue-monitor-cpp PRIVATE
    ${COMMON_INCLUDES}
)
target_link_libraries(queue-monitor-cpp
    ${COMMON_LIBS}
)
target_compile_options(queue-monitor-cpp PRIVATE
    ${ZMQ_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
)

install(TARGETS frame-publisher-cpp frame-resizer-cpp frame-saver-cpp queue-monitor-cpp RUNTIME DESTINATION bin)
