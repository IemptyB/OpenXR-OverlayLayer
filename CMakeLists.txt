#
# Copyright 2019 LunarG Inc. and PlutoVR Inc.
# 
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
# NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
# THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Author: Brad Grantham <brad@lunarg.com>
# Author: Dave Houlton <daveh@lunarg.com>
#

cmake_minimum_required(VERSION 3.12.2)

find_package(PythonInterp 3)

# find openxr - OPENXR_SDK_SOURCE_ROOT
# TODO: move this as well as library related vars to a find module
if(NOT DEFINED OPENXR_SDK_SOURCE_ROOT)
    message(
        FATAL_ERROR
            "Set OPENXR_SDK_SOURCE_ROOT to root of OpenXR-SDK-Source 1.0.9 tree with built header and global_generaed_files"
    )
endif()

if(NOT OPENXR_LIB_DIR)
    message(
        FATAL_ERROR
            "Set OPENXR_LIB_DIR to desired built openxr loader"
    )
endif()
set(OPENXR_INCLUDE_DIR ${OPENXR_SDK_SOURCE_ROOT}/include/openxr)

project(XR_overlay)

add_subdirectory(overlay-sample)
add_subdirectory(api-layer)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake targets")

