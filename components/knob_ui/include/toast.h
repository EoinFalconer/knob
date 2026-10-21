#pragma once

#include "primitives.h"

#include <cstdint>

enum class ToastKind : uint8_t { Success, Failure, Info };

void toast_attach(const IconPill &pill, void (*restore)());
void toast_show(ToastKind kind, const char *text, const char *icon = nullptr,
                uint32_t ms = 0);
bool toast_active();
void toast_cancel();
