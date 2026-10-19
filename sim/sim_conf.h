#pragma once

// Render with the firmware's own LVGL Kconfig (color depth, gradient stops,
// caches, widgets) so screenshots and redraw counts match the device. Needs one
// `idf.py build` of the app to generate its build/config/sdkconfig.h.
#include "sdkconfig.h"
