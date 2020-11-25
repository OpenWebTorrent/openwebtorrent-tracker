include(FindPackageHandleStandardArgs)

# Dependencies
find_package(OpenSSL REQUIRED)

add_definitions(-DLIBUS_USE_OPENSSL)

# Get include dir
find_path(
	USOCKETS_INCLUDE_DIR
	NAMES "uWebSockets/uSockets/src/libusockets.h"
)

# Add library
file(GLOB_RECURSE US_H ${USOCKETS_INCLUDE_DIR}/uWebSockets/uSockets/src/*.h)
file(GLOB_RECURSE US_C ${USOCKETS_INCLUDE_DIR}/uWebSockets/uSockets/src/*.c)
file(GLOB_RECURSE US_HPP ${USOCKETS_INCLUDE_DIR}/uWebSockets/uSockets/src/*.hpp)
file(GLOB_RECURSE US_CPP ${USOCKETS_INCLUDE_DIR}/uWebSockets/uSockets/src/*.cpp)
add_library(uSockets STATIC
    ${US_H}
    ${US_C}
    ${US_HPP}
    ${US_CPP}
)
SET_TARGET_PROPERTIES(uSockets PROPERTIES LINKER_LANGUAGE CXX)

# Include and link
TARGET_INCLUDE_DIRECTORIES(uSockets PUBLIC
	${USOCKETS_INCLUDE_DIR}/uWebSockets/uSockets/src
	${OPENSSL_INCLUDE_DIR}
)
TARGET_LINK_LIBRARIES(uSockets PUBLIC
	${OPENSSL_LIBRARIES}
)

# Vars
SET(USOCKETS_INCLUDE_DIR ${USOCKETS_INCLUDE_DIR}/uWebSockets/uSockets/src)
SET(USOCKETS_LIBRARIES uSockets)

find_package_handle_standard_args(
	uSockets
	DEFAULT_MSG
	USOCKETS_INCLUDE_DIR
	USOCKETS_LIBRARIES
)

mark_as_advanced(
	USOCKETS_INCLUDE_DIR
	USOCKETS_LIBRARIES
)
