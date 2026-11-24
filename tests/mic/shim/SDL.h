// Minimal SDL stand-in for building the microphone tests where SDL2 is not installed.
// Only what micstreamer.cpp / micsession.cpp use: logging and user events.
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#define SDL_LOG_CATEGORY_APPLICATION 0
#define SDL_USEREVENT 0x8000

typedef struct SDL_UserEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    int32_t code;
    void* data1;
    void* data2;
} SDL_UserEvent;

typedef union SDL_Event {
    uint32_t type;
    SDL_UserEvent user;
} SDL_Event;

static inline void SDL_ShimLog(const char* level, const char* fmt, va_list ap)
{
    fprintf(stderr, "[%s] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static inline void SDL_LogInfo(int, const char* fmt, ...) { va_list ap; va_start(ap, fmt); SDL_ShimLog("info", fmt, ap); va_end(ap); }
static inline void SDL_LogWarn(int, const char* fmt, ...) { va_list ap; va_start(ap, fmt); SDL_ShimLog("warn", fmt, ap); va_end(ap); }
static inline void SDL_LogError(int, const char* fmt, ...) { va_list ap; va_start(ap, fmt); SDL_ShimLog("error", fmt, ap); va_end(ap); }
static inline int SDL_PushEvent(SDL_Event*) { return 1; }
