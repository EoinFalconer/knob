#pragma once

#include <cstddef>
#include <cstdint>

void settings_init();

int settings_get_volume();
void settings_set_volume(int volume);

int settings_get_station_index();
void settings_set_station_index(int index);

void settings_get_speaker_ip(char *buf, size_t len);
void settings_set_speaker_ip(const char *ip);

void settings_get_speaker_name(char *buf, size_t len);
void settings_set_speaker_name(const char *name);

bool settings_has_speaker();

void settings_get_wifi_ssid(char *buf, size_t len);
void settings_set_wifi_ssid(const char *ssid);

void settings_get_wifi_pass(char *buf, size_t len);
void settings_set_wifi_pass(const char *pass);

// ─── Multi-network WiFi storage (up to 5 saved networks)
static constexpr int WIFI_MAX_SAVED = 5;

struct WifiEntry {
    char ssid[33];
    char pass[65];
};

// Returns number of saved networks (0..WIFI_MAX_SAVED)
int settings_wifi_count();

// Get the i-th saved network (returns false if index out of range)
bool settings_wifi_get(int index, WifiEntry *entry);

// Save a network. If SSID already exists, updates password and moves to front.
// If full, evicts the oldest (last) entry. Always stores as index 0 (most recent).
void settings_wifi_save(const char *ssid, const char *pass);

// Remove a saved network by SSID. Returns true if found and removed.
bool settings_wifi_remove(const char *ssid);

void settings_get_openai_api_key(char *buf, size_t len);
void settings_set_openai_api_key(const char *key);
