find_package(GLM)
find_package(LIBLAS QUIET)
find_package(LASZIP QUIET)
find_package(ZLIB)

add_subdirectory(ohm2ply)
add_subdirectory(ohmcmp)
add_subdirectory(ohmfilter)
add_subdirectory(ohmheightmap)
add_subdirectory(ohminfo)
add_subdirectory(ohmpop)
add_subdirectory(ohmprob)
add_subdirectory(ohmquery)
add_subdirectory(ohmsubmap)

if(OHM_BUILD_HEIGHTMAP_IMAGE)
  add_subdirectory(ohmhm2img)
endif(OHM_BUILD_HEIGHTMAP_IMAGE)
