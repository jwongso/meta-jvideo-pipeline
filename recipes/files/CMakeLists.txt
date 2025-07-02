cmake_minimum_required(VERSION 3.16)
project(jvideo-services)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENCV REQUIRED opencv4)
pkg_check_modules(ZMQ REQUIRED libzmq)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# Find nlohmann-json
find_package(nlohmann_json REQUIRED)

# Frame Publisher executable
add_executable(frame-publisher-cpp frame_publisher.cpp)
target_include_directories(frame-publisher-cpp PRIVATE
    ${OPENCV_INCLUDE_DIRS}
    ${ZMQ_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
)
target_link_libraries(frame-publisher-cpp
    ${OPENCV_LIBRARIES}
    ${ZMQ_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)
target_compile_options(frame-publisher-cpp PRIVATE
    ${OPENCV_CFLAGS_OTHER}
    ${ZMQ_CFLAGS_OTHER}
    ${HIREDIS_CFLAGS_OTHER}
)

# Frame Resizer executable
add_executable(frame-resizer-cpp frame_resizer.cpp)
target_include_directories(frame-resizer-cpp PRIVATE
    ${OPENCV_INCLUDE_DIRS}
    ${ZMQ_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
)
target_link_libraries(frame-resizer-cpp
    ${OPENCV_LIBRARIES}
    ${ZMQ_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)
target_compile_options(frame-resizer-cpp PRIVATE
    ${OPENCV_CFLAGS_OTHER}
    ${ZMQ_CFLAGS_OTHER}
    ${HIREDIS_CFLAGS_OTHER}
)

# Frame Saver executable
add_executable(frame-saver-cpp frame_saver.cpp)
target_include_directories(frame-saver-cpp PRIVATE
    ${OPENCV_INCLUDE_DIRS}
    ${ZMQ_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
)
target_link_libraries(frame-saver-cpp
    ${OPENCV_LIBRARIES}
    ${ZMQ_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
    stdc++fs  # For std::filesystem
)
target_compile_options(frame-saver-cpp PRIVATE
    ${OPENCV_CFLAGS_OTHER}
    ${ZMQ_CFLAGS_OTHER}
    ${HIREDIS_CFLAGS_OTHER}
)

install(TARGETS frame-publisher-cpp frame-resizer-cpp frame-saver-cpp RUNTIME DESTINATION bin)
