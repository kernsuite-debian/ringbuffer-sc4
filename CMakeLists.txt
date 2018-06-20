cmake_minimum_required(VERSION 3.0)

project(ringbuffer-sc4 C)

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native")

find_package (psrdada REQUIRED)
find_package (CUDA REQUIRED)

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

# expose some variables to the source code
set (ringbuffer_VERSION_MAJOR 0)
set (ringbuffer_VERSION_MINOR 1)
configure_file ("src/config.h.in" "${PROJECT_BINARY_DIR}/config.h")
include_directories ("${PROJECT_BINARY_DIR}")

add_executable(fill_ringbuffer src/fill_ringbuffer.c src/channel_remapping_sc4.c)
target_link_libraries(fill_ringbuffer m)
target_link_libraries(fill_ringbuffer ${PSRDADA_LIBRARIES})
target_link_libraries(fill_ringbuffer ${CUDA_LIBRARIES})

add_executable(send src/send.c)

add_executable(fake src/fake.c)
target_link_libraries(fake ${PSRDADA_LIBRARIES})
target_link_libraries(fake ${CUDA_LIBRARIES})

install(TARGETS fill_ringbuffer send fake RUNTIME DESTINATION bin)
