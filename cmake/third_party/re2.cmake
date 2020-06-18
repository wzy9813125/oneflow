include(ExternalProject)

SET(RE2_PROJECT re2)

SET(RE2_INSTALL_DIR ${THIRD_PARTY_DIR}/re2)

SET(RE2_INCLUDE_DIR ${RE2_INSTALL_DIR}/include CACHE PATH "" FORCE)
SET(RE2_LIBRARY_DIR ${RE2_INSTALL_DIR}/lib CACHE PATH "" FORCE)
SET(RE2_LIBRARIES ${RE2_LIBRARY_DIR}/libre2.a)

if (THIRD_PARTY)
    ExternalProject_Add(${RE2_PROJECT}
        PREFIX re2
        URL ${THIRD_PARTY_SUBMODULE_DIR}/re2/src/re2
        UPDATE_COMMAND ""
        CMAKE_ARGS
          -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
          -DBUILD_SHARED_LIBS:BOOL=OFF
          -DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}
          -DCMAKE_CXX_FLAGS_DEBUG:STRING=${CMAKE_CXX_FLAGS_DEBUG}
          -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        CMAKE_CACHE_ARGS
          -DCMAKE_INSTALL_PREFIX:PATH=${RE2_INSTALL_DIR}
          -DCMAKE_INSTALL_LIBDIR:PATH=${RE2_LIBRARY_DIR}
          -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
          -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE})
endif (THIRD_PARTY)
