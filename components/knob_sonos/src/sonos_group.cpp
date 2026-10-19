#include "sonos_group.h"

#include <cstdio>
#include <cstring>

bool sonos_group_has(const SonosGroup &group, const char *uuid) {
  for (int i = 0; i < group.count; i++) {
    if (strcmp(group.member_uuids[i], uuid) == 0)
      return true;
  }
  return false;
}

bool sonos_group_same_members(const SonosGroup &a, const SonosGroup &b) {
  if (a.count != b.count)
    return false;
  for (int i = 0; i < a.count; i++) {
    if (!sonos_group_has(b, a.member_uuids[i]))
      return false;
  }
  return true;
}

void sonos_group_label(const SonosGroup &group, char *out, size_t out_len) {
  if (group.count <= 0) {
    out[0] = '\0';
    return;
  }
  if (group.count == 1)
    snprintf(out, out_len, "%s", group.member_names[0]);
  else if (group.count == 2)
    snprintf(out, out_len, "%s + %s", group.member_names[0],
             group.member_names[1]);
  else
    snprintf(out, out_len, "%s + %d", group.member_names[0], group.count - 1);
}

GroupToggle sonos_group_toggle(const SonosGroup &group, const char *uuid) {
  if (!uuid || !uuid[0] || !group.room_uuid[0])
    return GroupToggle::None;
  if (strcmp(uuid, group.room_uuid) == 0)
    return group.count > 1 ? GroupToggle::Leave : GroupToggle::None;
  return sonos_group_has(group, uuid) ? GroupToggle::Leave : GroupToggle::Join;
}

static void remove_member(SonosGroup *group, int index) {
  for (int i = index; i < group->count - 1; i++) {
    memcpy(group->member_uuids[i], group->member_uuids[i + 1],
           sizeof(group->member_uuids[i]));
    memcpy(group->member_names[i], group->member_names[i + 1],
           sizeof(group->member_names[i]));
  }
  group->count--;
}

void sonos_group_apply(SonosGroup *group, GroupToggle toggle, const char *uuid,
                       const char *name) {
  if (toggle == GroupToggle::Join && group->count < SONOS_GROUP_MAX &&
      !sonos_group_has(*group, uuid)) {
    snprintf(group->member_uuids[group->count], sizeof(group->member_uuids[0]),
             "%s", uuid);
    snprintf(group->member_names[group->count], sizeof(group->member_names[0]),
             "%s", name);
    group->count++;
    return;
  }
  if (toggle != GroupToggle::Leave)
    return;
  if (strcmp(uuid, group->room_uuid) == 0) {
    // The controlled room walked out: it's now a group of one.
    group->count = 1;
    snprintf(group->coordinator_uuid, sizeof(group->coordinator_uuid), "%s",
             group->room_uuid);
    return;
  }
  for (int i = 0; i < group->count; i++) {
    if (strcmp(group->member_uuids[i], uuid) == 0) {
      remove_member(group, i);
      return;
    }
  }
}
