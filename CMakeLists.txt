cmake_minimum_required(VERSION 3.20)
project(graywatch VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(graywatch_core
    src/block_stats.cpp
    src/detector.cpp
    src/event.cpp
    src/io_health.cpp
    src/psi.cpp
    src/task_stats.cpp
)
target_include_directories(graywatch_core PUBLIC include)
target_compile_options(graywatch_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(graywatchctl src/main.cpp)
target_link_libraries(graywatchctl PRIVATE graywatch_core)
target_compile_options(graywatchctl PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(graywatchd src/daemon.cpp)
target_link_libraries(graywatchd PRIVATE graywatch_core)
target_compile_options(graywatchd PRIVATE -Wall -Wextra -Wpedantic -Werror)

install(TARGETS graywatchctl
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
endif()
