# Sonic 3 A.I.R. - PSP feasibility test build
cmake_minimum_required(VERSION 3.13)
project(Sonic3AIR_PSP C CXX)

set(CMAKE_CXX_STANDARD 20)
set(WORKSPACE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

set(PSP_FLAGS "-O2 -G0 -fsigned-char -Wno-psabi -Wno-unused-function -Wno-stringop-overflow -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${PSP_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PSP_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

add_definitions(-DENDUSER -DPLATFORM_PSP_BUILD -DSDL_MAIN_HANDLED)

include_directories(${WORKSPACE_DIR}/framework/external/ogg-vorbis/libogg/include)
include_directories(${WORKSPACE_DIR}/framework/external/ogg-vorbis/libvorbis/include)
include_directories(${WORKSPACE_DIR}/framework/external/ogg-vorbis/libvorbis/lib)
include_directories(${WORKSPACE_DIR}/framework/external/zlib/zlib/contrib/minizip)
include_directories(${WORKSPACE_DIR}/librmx/source)
include_directories(${WORKSPACE_DIR}/Oxygen/lemonscript/source)
include_directories(${WORKSPACE_DIR}/Oxygen/oxygenengine/source)
include_directories(${WORKSPACE_DIR}/Oxygen/sonic3air/source)
include_directories(${WORKSPACE_DIR}/Oxygen/sonic3air/source/external)
include_directories(${PSPDEV}/psp/include/SDL2)

# ogg vorbis (bundled, same as desktop)
file(GLOB OGGVORBIS_SOURCES ${WORKSPACE_DIR}/framework/external/ogg-vorbis/libogg/src/*.c
							${WORKSPACE_DIR}/framework/external/ogg-vorbis/libvorbis/lib/*.c)
list(FILTER OGGVORBIS_SOURCES EXCLUDE REGEX ".*/(barkmel|tone|psytune)[.]c$")
add_library(oggvorbis STATIC ${OGGVORBIS_SOURCES})

# minizip
add_library(minizip STATIC ${WORKSPACE_DIR}/framework/external/zlib/zlib/contrib/minizip/ioapi.c
						   ${WORKSPACE_DIR}/framework/external/zlib/zlib/contrib/minizip/unzip.c
						   ${WORKSPACE_DIR}/framework/external/zlib/zlib/contrib/minizip/zip.c)
target_compile_options(minizip PRIVATE -Wno-implicit-function-declaration -Wno-int-conversion -DIOAPI_NO_64 -DUSE_FILE32API)

# rmxbase
file(GLOB_RECURSE RMXBASE_SOURCES ${WORKSPACE_DIR}/librmx/source/rmxbase/*.cpp)
add_library(rmxbase STATIC ${RMXBASE_SOURCES})

# rmxmedia (no OpenGL, no GLEW)
file(GLOB_RECURSE RMXMEDIA_SOURCES ${WORKSPACE_DIR}/librmx/source/rmxmedia/*.cpp)
list(FILTER RMXMEDIA_SOURCES EXCLUDE REGEX ".*/_glew/.*")
add_library(rmxmedia STATIC ${RMXMEDIA_SOURCES})

# rmxext_oggvorbis
file(GLOB RMXEXT_OGGVORBIS_SOURCES ${WORKSPACE_DIR}/librmx/source/rmxext_oggvorbis/*.cpp)
add_library(rmxext_oggvorbis STATIC ${RMXEXT_OGGVORBIS_SOURCES})

# lemonscript
file(GLOB_RECURSE LEMONSCRIPT_SOURCES ${WORKSPACE_DIR}/Oxygen/lemonscript/source/lemon/*.cpp)
add_library(lemonscript STATIC ${LEMONSCRIPT_SOURCES})

# oxygen_netcore
file(GLOB_RECURSE OXYGEN_NETCORE_SOURCES ${WORKSPACE_DIR}/Oxygen/oxygenengine/source/oxygen_netcore/*.cpp)
add_library(oxygen_netcore STATIC ${OXYGEN_NETCORE_SOURCES})

# oxygen
file(GLOB_RECURSE OXYGEN_SOURCES ${WORKSPACE_DIR}/Oxygen/oxygenengine/source/oxygen/*.cpp)
add_library(oxygen STATIC ${OXYGEN_SOURCES})

# sonic3air
file(GLOB_RECURSE SONIC3AIR_SOURCES ${WORKSPACE_DIR}/Oxygen/sonic3air/source/sonic3air/*.cpp)
list(FILTER SONIC3AIR_SOURCES EXCLUDE REGEX ".*/platform/vita/.*")
add_executable(sonic3air ${SONIC3AIR_SOURCES})

target_link_libraries(sonic3air
	-Wl,--start-group
	oxygen oxygen_netcore lemonscript rmxext_oggvorbis rmxmedia rmxbase minizip oggvorbis
	-Wl,--end-group
	SDL2 z
	GL pspvram pspaudio pspvfpu pspdisplay pspgu pspge psphprm pspctrl psppower pspnet_inet pspnet_resolver pspnet pspnet_apctl psprtc
	pspatrac3 psputility pspsdk pthread atomic stdc++ m c
)

create_pbp_file(
	TARGET sonic3air
	ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/ICON0.PNG
	BACKGROUND_PATH NULL
	PREVIEW_PATH NULL
	TITLE "Sonic 3 A.I.R."
	BUILD_PRX
	MEMSIZE 1
)

# Use our own copy of the PRX linker script, which discards the (empty) .rel.dyn section
# Drop unused code and data (the sources are compiled with -ffunction-sections -fdata-sections): every MB of the
# executable is a MB less for the heap. The PSP's module sections are KEEP in linkfile_s3air.prx.
target_link_options(sonic3air PRIVATE -Wl,--gc-sections)

get_target_property(S3AIR_LINK_OPTS sonic3air LINK_OPTIONS)
list(TRANSFORM S3AIR_LINK_OPTS REPLACE "-T.*/linkfile.prx" "-T${CMAKE_CURRENT_SOURCE_DIR}/linkfile_s3air.prx")
set_target_properties(sonic3air PROPERTIES LINK_OPTIONS "${S3AIR_LINK_OPTS}")
