cmake_minimum_required(VERSION 3.20)
project(StarEditor)

add_executable(editor WIN32 MACOSX_BUNDLE
    main.cpp
)

target_link_libraries(editor PRIVATE engine)
target_compile_definitions(editor PRIVATE BUILD_EDITOR)