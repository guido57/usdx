#pragma once

#include <Arduino.h>

struct TaskProfiler
{
    const char* name;
    uint8_t core;
    uint64_t busy_us;
    uint32_t loops;
    uint64_t last_busy_us = 0;
};


extern TaskProfiler profilers[];
extern size_t profilerCount;

