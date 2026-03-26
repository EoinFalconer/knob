#pragma once

/// Start the captive portal WiFi setup. Blocks until credentials are submitted.
void wifi_setup_start();

/// Returns true if WiFi credentials are stored in NVS
bool wifi_setup_has_credentials();
