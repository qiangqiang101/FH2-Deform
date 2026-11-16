cmake_minimum_required(VERSION 3.20)
project(FH2Deform VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The FH2 Mod Platform submodule, for its headers (fh2mod_api). Its DLL (DWrite.dll) isn't built here.
add_subdirectory(platform EXCLUDE_FROM_ALL)

# The DLL's version info (Properties > Details), made from project(VERSION) above
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/version.rc @ONLY CONTENT [[
#include <winver.h>

VS_VERSION_INFO VERSIONINFO
 FILEVERSION     @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
 PRODUCTVERSION  @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
 FILEFLAGSMASK   0x3fL
 FILEFLAGS       0x0L
 FILEOS          VOS_NT_WINDOWS32
 FILETYPE        VFT_DLL
 FILESUBTYPE     VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "FileDescription",  "More car deformation for Forza Horizon 2"
            VALUE "FileVersion",      "@PROJECT_VERSION@"
            VALUE "InternalName",     "FH2Deform"
            VALUE "LegalCopyright",   "MIT License"
            VALUE "OriginalFilename", "FH2Deform.dll"
            VALUE "ProductName",      "FH2 Deform"
            VALUE "ProductVersion",   "@PROJECT_VERSION@"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
]])

add_library(FH2Deform SHARED
    src/deform_mod.cpp
    src/dxbc_patch.cpp
    src/zip_read.cpp
    ${CMAKE_BINARY_DIR}/version.rc
)

target_link_libraries(FH2Deform PRIVATE fh2mod_api)
set_target_properties(FH2Deform PROPERTIES OUTPUT_NAME FH2Deform PREFIX "")
target_compile_definitions(FH2Deform PRIVATE
    FH2DEFORM_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    # One C++ runtime per process is enough: link it statically, as the other platform mods do.
    set_property(TARGET FH2Deform PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(FH2Deform PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(FH2Deform PRIVATE -Wall -Wextra)
    target_link_options(FH2Deform PRIVATE -static)
endif()

# A check run outside the game: the shader patch over all of Shaders.zip (not shipped)
add_executable(morph_scan EXCLUDE_FROM_ALL tools/morph_scan.cpp src/dxbc_patch.cpp src/zip_read.cpp)
target_compile_definitions(morph_scan PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(morph_scan PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
