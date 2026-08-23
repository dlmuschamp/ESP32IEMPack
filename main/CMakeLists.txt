# Enforce explicit selection of the build target (exact names).
set(IEM_COMPILE_DEFS "")
set(IEM_NODE_SEL "$ENV{IEM_NODE}")

if(IEM_NODE_SEL STREQUAL "RX_DAC_TEST")
    message(STATUS "Build target: RX DAC PROFILE TEST (debugging/rx_dac_profile_test.c)")
    set(COMPONENT_SRCS "debugging/rx_dac_profile_test.c" "rig_shared.c")
elseif(IEM_NODE_SEL STREQUAL "TX_TONE")
    message(STATUS "Build target: TX TONE BYPASS (debugging/tx_tone_bypass.c)")
    set(COMPONENT_SRCS "debugging/tx_tone_bypass.c" "rig_shared.c")
    list(APPEND IEM_COMPILE_DEFS TX_TONE_BYPASS_ADC=1)
elseif(IEM_NODE_SEL STREQUAL "TX_BUZZ_DEBUG")
    message(STATUS "Build target: TX BUZZ/PEAK DEBUG (debugging/tx_buzz_debug.c)")
    set(COMPONENT_SRCS "debugging/tx_buzz_debug.c" "rig_shared.c")
elseif(IEM_NODE_SEL STREQUAL "RX")
    message(STATUS "Build target: RECEIVER (rx.c)")
    set(COMPONENT_SRCS "rx.c" "rig_shared.c")
elseif(IEM_NODE_SEL STREQUAL "TX")
    message(STATUS "Build target: TRANSMITTER (tx.c)")
    set(COMPONENT_SRCS "tx.c" "rig_shared.c")
else()
    message(FATAL_ERROR
        "ABORTED: Set IEM_NODE to TX, RX, TX_TONE, RX_DAC_TEST, or TX_BUZZ_DEBUG "
        "(e.g. IEM_NODE=RX idf.py build). Got '${IEM_NODE_SEL}'.")
endif()

idf_component_register(SRCS "${COMPONENT_SRCS}"
                       INCLUDE_DIRS "." "debugging"
                       REQUIRES driver esp_wifi nvs_flash esp_driver_i2s esp_adc
                                esp_driver_ledc freertos esp_driver_gpio
                                esp_timer)

if(IEM_COMPILE_DEFS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${IEM_COMPILE_DEFS})
endif()
