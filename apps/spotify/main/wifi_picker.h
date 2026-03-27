#pragma once

/// Result of the WiFi picker flow.
enum class WifiPickerResult {
    Connected,    // User selected a network and we connected successfully
    AddNew,       // User chose "Add new network" — caller should start captive portal
};

/// Run the WiFi picker UI. Scans for networks, shows a list on the display,
/// and lets the user select with the encoder. Blocks until a choice is made
/// and connection succeeds (or user picks "Add new").
///
/// Prerequisites: NVS, display, encoder, and LVGL must be initialized.
/// WiFi must NOT be initialized yet — the picker handles esp_wifi_init itself.
WifiPickerResult wifi_picker_run();
