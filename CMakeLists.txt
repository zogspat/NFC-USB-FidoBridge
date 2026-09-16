cmake_minimum_required(VERSION 3.13)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(Fido2Authenticator C CXX ASM)

# Initialize the Pico SDK
pico_sdk_init()

# Add executable. Default name is fido2_authenticator
add_executable(fido2_authenticator
    main.c
    pn532_i2c.c
    ctaphid.c
    usb_descriptors.c
)

add_executable(factory_reset
    factory_reset.c
    pn532_i2c.c
    usb_descriptors.c
)

# Pull in standard library features
target_link_libraries(fido2_authenticator pico_stdlib tinyusb_device tinyusb_board hardware_i2c)
target_include_directories(fido2_authenticator PRIVATE ${CMAKE_CURRENT_LIST_DIR})
pico_add_extra_outputs(fido2_authenticator)

target_link_libraries(factory_reset pico_stdlib tinyusb_device tinyusb_board hardware_i2c)
target_include_directories(factory_reset PRIVATE ${CMAKE_CURRENT_LIST_DIR})
pico_add_extra_outputs(factory_reset)
