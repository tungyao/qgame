#pragma once

#if defined(_WIN32)
    #if defined(QGAME_ENGINE_EXPORTS)
        #define QGAME_ENGINE_API __declspec(dllexport)
    #else
        #define QGAME_ENGINE_API __declspec(dllimport)
    #endif
    #if defined(QGAME_BACKEND_EXPORTS)
        #define QGAME_BACKEND_API __declspec(dllexport)
    #else
        #define QGAME_BACKEND_API __declspec(dllimport)
    #endif
#else
    #define QGAME_ENGINE_API __attribute__((visibility("default")))
    #define QGAME_BACKEND_API __attribute__((visibility("default")))
    #define QGAME_PLATFORM_API __attribute__((visibility("default")))
#endif
