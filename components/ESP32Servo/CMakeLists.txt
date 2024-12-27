idf_component_register(SRCS "src/ESP32Servo.cpp"
                    "src/ESP32PWM.cpp"
                    INCLUDE_DIRS "src"
                    REQUIRES arduino-esp32)

# https://github.com/espressif/esp-idf/issues/11696#issuecomment-1596208414
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-if-conversion) #
component_compile_options(-Wno-error=format= -Wno-format)