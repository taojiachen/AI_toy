set(PUBLIC_REQUIREMENTS ${PUBLIC_REQUIREMENTS} "mbedtls-2.28" CACHE STRING "public requirement for main" FORCE)
message("-- Build component : ${PUBLIC_REQUIREMENTS}")

cmake_minimum_required(VERSION 3.5)

list(APPEND ADD_INCLUDE
    "."
    "library"
    "include"
    "configs"
)

file(GLOB_RECURSE ADD_SRCS
    "./*.c"
    "library/*.c"
    "include/*.c"
    "configs/*.c"
)

idf_component_register(
    SRCS ${ADD_SRCS}
    INCLUDE_DIRS ${ADD_INCLUDE}
)

# 2. 配置编译选项（保留官方核心flags，适配ESP-GCC）
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wwrite-strings -Wformat=2 -Wno-format-nonliteral")

# 针对ESP-GCC（基于GCC）补充优化与警告
if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    # 优化级别：ESP默认-O2，可根据需求调整（调试时用-O0）
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    # 禁用不必要的警告（避免ESP环境下误报）
    target_compile_options(${COMPONENT_LIB} PRIVATE 
        -Wno-unused-parameter 
        -Wno-sign-compare 
        -Wno-implicit-fallthrough
    )
endif()

# 3. 保留mbedTLS核心配置能力（自定义config.h）
# 若用户设置了MBEDTLS_CONFIG_FILE，传递编译定义（如简化版config.h）
if(DEFINED MBEDTLS_CONFIG_FILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC 
        MBEDTLS_CONFIG_FILE="${MBEDTLS_CONFIG_FILE}"
    )
endif()

# 若用户设置了MBEDTLS_USER_CONFIG_FILE（追加配置）
if(DEFINED MBEDTLS_USER_CONFIG_FILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC 
        MBEDTLS_USER_CONFIG_FILE="${MBEDTLS_USER_CONFIG_FILE}"
    )
endif()

# 4. 关闭ESP不需要的功能（避免编译冗余代码）
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MBEDTLS_THREADING_C          # 启用线程安全（ESP支持多线程）
    MBEDTLS_THREADING_PTHREAD    # 适配ESP的pthread（若用FreeRTOS，可改MBEDTLS_THREADING_FREERTOS）
)

# 6. 适配ESP的熵源（可选：若MBEDTLS_NO_PLATFORM_ENTROPY不够，可添加ESP特定熵源）
# 示例：若需要用ESP的硬件随机数（可选，mbedTLS自带的entropy_poll已适配部分）
# target_sources(${COMPONENT_LIB} PRIVATE "library/entropy_poll_esp32.c")  # 需自行实现