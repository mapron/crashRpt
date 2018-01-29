project(CrashSenderApp)

# Create the list of source files
aux_source_directory( . source_files )
file( GLOB header_files *.h )

# Enable usage of precompiled header
#set(srcs_using_precomp ${source_files})
#list(REMOVE_ITEM srcs_using_precomp ./stdafx.cpp ./md5.cpp ./base64.cpp)
#add_msvc_precompiled_header(stdafx.h ./stdafx.cpp srcs_using_precomp)

list(APPEND source_files
	./CrashSender.rc)

# Define _UNICODE (use wide-char encoding)
add_definitions(-D_UNICODE )

fix_default_compiler_settings_()

include_directories( ${CMAKE_BINARY_DIR} )
# Add include dir
include_directories( ${CMAKE_SOURCE_DIR}/include
                     ${CMAKE_SOURCE_DIR}/thirdparty/wtl 
							${CMAKE_SOURCE_DIR}/reporting/crashsender)



# Add executable build target
add_executable(CrashSenderApp WIN32 ${source_files} ${header_files})

# Add input link libraries
target_link_libraries(CrashSenderApp CrashSender)

