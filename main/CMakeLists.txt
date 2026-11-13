idf_component_register(SRCS "main.c" "legacy_pool_demo.c" "allocator_tests.c" "experiments.c"
                       INCLUDE_DIRS "."
                       REQUIRES adaptive_heap demo_pool freertos log esp_timer esp_system)
