cmake_minimum_required(VERSION 3.20)
project(graywatch VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(graywatch_core
    src/psi.cpp
)
target_include_directories(graywatch_core PUBLIC include)
target_compile_options(graywatch_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(graywatchctl src/main.cpp)
target_link_libraries(graywatchctl PRIVATE graywatch_core)
target_compile_options(graywatchctl PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
    add_executable(psi_test tests/psi_test.cpp)
    target_link_libraries(psi_test PRIVATE graywatch_core)
    target_compile_options(psi_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME psi_parser COMMAND psi_test)
endif()
