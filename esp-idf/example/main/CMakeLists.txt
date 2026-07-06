idf_component_register(
    SRCS "bench_main.c" "../../../embedded/bench/bench_core.c"
    INCLUDE_DIRS "."
    REQUIRES glint esp_timer
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE GLINT_NO_THREADS)
