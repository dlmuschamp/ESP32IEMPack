# Enforce explicit selection of the build target
if($ENV{IEM_NODE} MATCHES "RX")
    message(STATUS "Build target is set to RECEIVER (rx.c)")
    set(COMPONENT_SRCS "rx.c")
elseif($ENV{IEM_NODE} MATCHES "TX")
    message(STATUS "Build target is set to TRANSMITTER (tx.c)")
    set(COMPONENT_SRCS "tx.c")
elseif($ENV{IEM_NODE} MATCHES "MON")
    message(STATUS "Build target is set to MONITOR (mon.c)")
    set(COMPONENT_SRCS "mon.c")
else()
    # This halts the build immediately if no valid variable is provided
    message(FATAL_ERROR "ABORTED: You must specify a valid target! (e.g., IEM_NODE=RX idf.py build)")
endif()

idf_component_register(SRCS "${COMPONENT_SRCS}"
                       INCLUDE_DIRS "."
                       REQUIRES  driver esp_wifi nvs_flash esp_driver_i2s esp_adc esp_driver_ledc freertos esp_driver_gpio)
