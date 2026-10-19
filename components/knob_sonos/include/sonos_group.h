#pragma once

#include "sonos_config.h"

#include <cstddef>

bool sonos_group_has(const SonosGroup &group, const char *uuid);

// Same rooms, in any order.
bool sonos_group_same_members(const SonosGroup &a, const SonosGroup &b);

// "Kitchen", "Kitchen + Living Room", "Kitchen + 2". Controlled room first.
void sonos_group_label(const SonosGroup &group, char *out, size_t out_len);

enum class GroupToggle : uint8_t { None, Join, Leave };

// What long-pressing `uuid` in the speaker picker should do: rooms outside
// the group join it, rooms inside leave it. The controlled room itself
// leaves only when it shares a group (it then plays on its own).
GroupToggle sonos_group_toggle(const SonosGroup &group, const char *uuid);

// Mirror a toggle locally so the picker reacts before the speakers confirm.
void sonos_group_apply(SonosGroup *group, GroupToggle toggle, const char *uuid,
                       const char *name);
