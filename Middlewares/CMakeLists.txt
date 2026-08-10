include(FetchContent)
 
FetchContent_Declare(
  freertos_kernel
  GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
  GIT_TAG        V11.3.0 # pinned: the CM33 port API is load-bearing for this project
)

add_library(freertos_config INTERFACE)

target_include_directories(
    freertos_config
    SYSTEM INTERFACE
    # (a bare "Inc" used to be listed here; Middlewares/Inc does not exist -- finding F32)
    ${CMAKE_CURRENT_LIST_DIR}/../Inc
)

target_compile_definitions(freertos_config
  INTERFACE
    projCOVERAGE_TEST=0
)

target_compile_options(
  freertos_config
  INTERFACE
  ${cpu_PARAMS}
)

set( FREERTOS_HEAP "4" CACHE STRING "" FORCE)
# Select the native compile PORT
set( FREERTOS_PORT "GCC_POSIX" CACHE STRING "" FORCE)
# Select the cross-compile PORT
if (CMAKE_CROSSCOMPILING)
  set(FREERTOS_PORT "GCC_ARM_CM33_NTZ_NONSECURE" CACHE STRING "" FORCE)
endif()

FetchContent_MakeAvailable(freertos_kernel)

target_include_directories(
  ${CMAKE_PROJECT_NAME} 
  PUBLIC
  ${freertos_kernel_SOURCE_DIR}/include  
  ${freertos_kernel_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure
)

set(device_header "${freertos_kernel_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/portmacro.h" PARENT_SCOPE)

target_link_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR})