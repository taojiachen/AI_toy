set(PUBLIC_REQUIREMENTS ${PUBLIC_REQUIREMENTS} "ogg" CACHE STRING "public requirement for main" FORCE)
message("-- Build component : ${PUBLIC_REQUIREMENTS}")

list(APPEND ADD_INCLUDE
    "."
)

file(GLOB_RECURSE ADD_SRCS
    "./*.c"
)

set(OGG_COMPILE_OPTIONS)

if(CONFIG_USE_DYNAMIC_CALCULATION)
list(APPEND OGG_COMPILE_OPTIONS       
    -DDISABLE_CRC
    )
endif()

idf_component_register(SRCS ${ADD_SRCS}
                    INCLUDE_DIRS ${ADD_INCLUDE})
target_compile_options(${COMPONENT_LIB} PRIVATE ${OGG_COMPILE_OPTIONS})