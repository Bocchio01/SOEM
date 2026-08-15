# This software is dual-licensed under GPLv3 and a commercial
# license. See the file LICENSE.md distributed with this software for
# full license information.

if(USE_XENOMAI_EVL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EVL REQUIRED IMPORTED_TARGET evl)
    if(NOT EXISTS "/dev/evl/core")
        message(WARNING "libevl found, but the EVL kernel core is not currently running (/dev/evl/core missing). The compiled binaries will not execute correctly until the system is rebooted into the EVL kernel.")
    endif()
endif()

target_sources(soem PRIVATE
    osal/linux/osal.c
    osal/linux/osal_defs.h
    oshw/linux/oshw.c
    oshw/linux/oshw.h
    oshw/linux/nicdrv.c
    oshw/linux/nicdrv.h
)

target_include_directories(soem PUBLIC
    $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/osal/linux>
    $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/oshw/linux>
    $<INSTALL_INTERFACE:include/soem>
)

foreach(target IN ITEMS
    soem
    ec_sample
    eepromtool
    eni_test
    eoe_test
    firm_update
    simple_ng
    slaveinfo)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
        )
    endif()
endforeach()

target_link_libraries(soem PUBLIC pthread rt)
if(USE_XENOMAI_EVL)
    target_compile_definitions(soem PUBLIC
        USE_XENOMAI_EVL
        EVL_ECAT_FILTER_PATH="oshw/linux/evl_ecat_filter.o"
    )
    target_link_libraries(soem PRIVATE PkgConfig::EVL)
endif()

install(FILES
    osal/linux/osal_defs.h
    oshw/linux/nicdrv.h
    DESTINATION include/soem
)


