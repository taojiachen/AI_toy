set(PUBLIC_REQUIREMENTS ${PUBLIC_REQUIREMENTS} "mongoose" CACHE STRING "public requirement for main" FORCE)
message("-- Build component : ${PUBLIC_REQUIREMENTS}")

list(APPEND ADD_INCLUDE
    "."
)

file(GLOB_RECURSE ADD_SRCS
    "./*.c"
)

idf_component_register(
    SRCS ${ADD_SRCS}
    INCLUDE_DIRS ${ADD_INCLUDE}
    REQUIRES lwip esp_wifi esp_event
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-ignored-qualifiers
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MG_ARCH=MG_ARCH_ESP32
    MG_ENABLE_TCPIP=1
    MG_ENABLE_SOCKET=0
    MG_ENABLE_LWIP=1
)