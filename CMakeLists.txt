cmake_minimum_required(VERSION 3.16)
project(pdftomd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-cpp)

add_library(pdf2md_lib STATIC src/pdf2md.cc)
target_include_directories(pdf2md_lib PUBLIC src)
target_link_libraries(pdf2md_lib PUBLIC PkgConfig::POPPLER)

add_executable(pdftomd src/main.cc)
target_link_libraries(pdftomd PRIVATE pdf2md_lib)

include(GNUInstallDirs)
install(TARGETS pdftomd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

find_package(GTest REQUIRED)

add_executable(pdftomd_tests tests/test_pdf2md.cc)
target_link_libraries(pdftomd_tests PRIVATE pdf2md_lib GTest::gtest_main GTest::gtest)
target_include_directories(pdftomd_tests PRIVATE tests)
if(EXISTS ${CMAKE_SOURCE_DIR}/nouriva_woche_1_quellen.pdf)
    target_compile_definitions(pdftomd_tests PRIVATE
        REAL_PDF="${CMAKE_SOURCE_DIR}/nouriva_woche_1_quellen.pdf")
endif()

add_test(NAME pdftomd_tests COMMAND pdftomd_tests)
