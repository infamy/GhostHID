// Globals that live in main.cpp on the device; defined here for host tests so
// CommandProcessor's status report links.
#include <stdint.h>
uint32_t g_heapAfterBoot = 0, g_heapAfterWifi = 0, g_heapAfterServer = 0;
bool g_bootComplete = true;
