set(app_sources "main.c" "legacy_pool_demo.c" "allocator_tests.c" "experiments.c")
if(CONFIG_APP_INTERACTIVE_HEAP_LAB)
    list(APPEND app_sources "heap_lab.c")
endif()
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES adaptive_heap demo_pool freertos log esp_timer esp_system
                       PRIV_REQUIRES esp_driver_uart)
