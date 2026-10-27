cmake_minimum_required(VERSION 3.25)
project(calyx_uring_tests LANGUAGES CXX)

# Run separately from the application build, using its prepared liburing.
# cmake -S tests -B build-tests -DLIBURING_INCLUDE_DIR=<liburing>/src/include \
#   -DLIBURING_LIBRARY=<build>/liburing.a -DCMAKE_CXX_COMPILER=clang++ \
#   -DCMAKE_LINKER_TYPE=LLD
find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
find_library(LIBURING_LIBRARY NAMES uring REQUIRED)
find_package(Threads REQUIRED)

add_executable(uring_timeout uring_timeout.cpp)
target_compile_features(uring_timeout PRIVATE cxx_std_23)
set_target_properties(uring_timeout PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(uring_timeout PRIVATE .. ../include)
target_include_directories(uring_timeout SYSTEM PRIVATE "${LIBURING_INCLUDE_DIR}")
target_compile_options(uring_timeout PRIVATE -Wall -Wextra -Werror -Wpedantic -ffunction-sections -fdata-sections)
target_link_options(uring_timeout PRIVATE
    -Wl,--gc-sections
    -Wl,--wrap=io_uring_submit
    -Wl,--wrap=io_uring_wait_cqe_timeout
)
target_link_libraries(uring_timeout PRIVATE "${LIBURING_LIBRARY}" Threads::Threads)

enable_testing()
foreach(mode legacy modern batch)
    add_test(NAME uring_timeout_${mode} COMMAND uring_timeout ${mode})
    set_tests_properties(uring_timeout_${mode} PROPERTIES TIMEOUT 5)
endforeach()
