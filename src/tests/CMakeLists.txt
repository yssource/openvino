# Copyright (C) 2018-2022 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

set(IE_TESTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()

if(NOT MSVC)
    ie_add_compiler_flags(-Wno-missing-declarations)
    ie_add_compiler_flags(-Wno-sign-compare)
    ie_add_compiler_flags(-Wno-odr)
    ie_add_compiler_flags(-Wno-all)
endif()

add_subdirectory(ngraph_helpers)
add_subdirectory(unit)
add_subdirectory(ie_test_utils)

if(ENABLE_FUNCTIONAL_TESTS)
    add_subdirectory(functional)
endif()
