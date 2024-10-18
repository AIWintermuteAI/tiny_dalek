idf_component_register(SRCS "llm.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-dsp)

# https://github.com/espressif/esp-idf/issues/11696#issuecomment-1596208414
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-if-conversion) #
component_compile_options(-Wno-error=format= -Wno-format)