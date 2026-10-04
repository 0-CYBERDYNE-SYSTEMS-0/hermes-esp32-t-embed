set(HERMES_CORE_SOURCES
    "src/app.cpp"
    "src/canvas.cpp"
    "src/crypto.cpp"
    "src/font5x7.cpp"
    "src/json.cpp"
    "src/mascot_data.cpp"
    "src/protocol.cpp"
    "src/touch.cpp"
    "src/ui.cpp"
    "src/vad.cpp"
)

idf_component_register(SRCS ${HERMES_CORE_SOURCES} INCLUDE_DIRS "include")
set_target_properties(${COMPONENT_LIB} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti -std=gnu++17)
