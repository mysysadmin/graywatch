cmake_minimum_required(VERSION 3.20)
project(graywatch VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GRAYWATCH_STATIC "Link the core Graywatch executables statically" OFF)

add_library(graywatch_core
    src/block_stats.cpp
    src/cluster.cpp
    src/detector.cpp
    src/event.cpp
    src/io_health.cpp
    src/json.cpp
    src/memory_stats.cpp
    src/network_stats.cpp
    src/process.cpp
    src/psi.cpp
    src/report.cpp
    src/task_stats.cpp
    src/user_lookup.cpp
)
target_include_directories(graywatch_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(graywatch_core PUBLIC Threads::Threads)
target_compile_options(graywatch_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(graywatchctl src/main.cpp)
target_link_libraries(graywatchctl PRIVATE graywatch_core)
target_compile_options(graywatchctl PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(graywatch-report src/report_main.cpp)
target_link_libraries(graywatch-report PRIVATE graywatch_core)
target_compile_options(graywatch-report PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(graywatchd src/daemon.cpp)
target_link_libraries(graywatchd PRIVATE graywatch_core)
target_compile_options(graywatchd PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(GRAYWATCH_STATIC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "GRAYWATCH_STATIC is supported only on Linux")
    endif()
    target_link_options(graywatchctl PRIVATE -static)
    target_link_options(graywatchd PRIVATE -static)
    target_link_options(graywatch-report PRIVATE -static)
endif()

install(TARGETS graywatchctl graywatch-report
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS graywatchd
    RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR}
)
install(TARGETS graywatch_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/graywatch
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(GRAYWATCH_SYSTEMD_UNIT_DIR "lib/systemd/system"
    CACHE PATH "systemd system unit installation directory")
set(GRAYWATCH_SYSUSERS_DIR "lib/sysusers.d"
    CACHE PATH "systemd-sysusers configuration directory")
configure_file(packaging/graywatchd.service.in graywatchd.service @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/graywatchd.service"
    DESTINATION "${GRAYWATCH_SYSTEMD_UNIT_DIR}")
install(FILES packaging/graywatch.conf
    DESTINATION "${GRAYWATCH_SYSUSERS_DIR}")
install(FILES README.md LICENSE
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")

include(CTest)
if(BUILD_TESTING)
    add_executable(block_stats_test tests/block_stats_test.cpp)
    target_link_libraries(block_stats_test PRIVATE graywatch_core)
    target_compile_options(block_stats_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME block_stats COMMAND block_stats_test)

    add_executable(psi_test tests/psi_test.cpp)
    target_link_libraries(psi_test PRIVATE graywatch_core)
    target_compile_options(psi_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME psi_parser COMMAND psi_test)

    add_executable(detector_test tests/detector_test.cpp)
    target_link_libraries(detector_test PRIVATE graywatch_core)
    target_compile_options(detector_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME degradation_detector COMMAND detector_test)

    add_executable(task_stats_test tests/task_stats_test.cpp)
    target_link_libraries(task_stats_test PRIVATE graywatch_core)
    target_compile_options(task_stats_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME task_stats COMMAND task_stats_test)

    add_executable(event_test tests/event_test.cpp)
    target_link_libraries(event_test PRIVATE graywatch_core)
    target_compile_options(event_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME event_serialization COMMAND event_test)

    add_executable(io_health_test tests/io_health_test.cpp)
    target_link_libraries(io_health_test PRIVATE graywatch_core)
    target_compile_options(io_health_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME io_health COMMAND io_health_test)

    add_executable(network_stats_test tests/network_stats_test.cpp)
    target_link_libraries(network_stats_test PRIVATE graywatch_core)
    target_compile_options(network_stats_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME network_stats COMMAND network_stats_test)

    add_executable(memory_stats_test tests/memory_stats_test.cpp)
    target_link_libraries(memory_stats_test PRIVATE graywatch_core)
    target_compile_options(memory_stats_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME memory_stats COMMAND memory_stats_test)

    add_executable(cluster_test tests/cluster_test.cpp)
    target_link_libraries(cluster_test PRIVATE graywatch_core)
    target_compile_options(cluster_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME cluster_summary COMMAND cluster_test)

    add_executable(report_test tests/report_test.cpp)
    target_link_libraries(report_test PRIVATE graywatch_core)
    target_compile_options(report_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME support_report COMMAND report_test)

    add_executable(user_lookup_test tests/user_lookup_test.cpp)
    target_link_libraries(user_lookup_test PRIVATE graywatch_core)
    target_compile_options(user_lookup_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME user_lookup COMMAND user_lookup_test)
endif()
