#pragma once
// Template. Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is gitignored, so your passwords and token stay off GitHub.

// --- WiFi ---
#define WIFI_SSID        "your-wifi-name"
#define WIFI_PASSWORD    "your-wifi-password"

// WPA2-Enterprise (eduroam-style networks: username + password).
// Set to 1 and fill the identity/username; WIFI_PASSWORD is used as the password.
#define WIFI_ENTERPRISE  0
#define WIFI_EAP_IDENTITY "you@school.edu"
#define WIFI_EAP_USERNAME "you@school.edu"

// --- Backend: your Apps Script /exec URL + ?token= (same value as the DASH_TOKEN
// script property in Apps Script: Project Settings > Script properties) ---
#define DASH_URL "https://script.google.com/macros/s/YOUR_DEPLOYMENT_ID/exec?token=YOUR_DASH_TOKEN"
