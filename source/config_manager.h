#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include "ui/theme.h"

// Config file path on SD card
#define CONFIG_DIR  "sdmc:/3ds/StreaMu"
#define CONFIG_PATH "sdmc:/3ds/StreaMu/config.json"

class ConfigManager {
public:
    // Load config from file. Returns defaults on failure
    static bool load(AppConfig& out) {
        out = AppConfig{}; // Init with defaults

        FILE* f = fopen(CONFIG_PATH, "r");
        if (!f) return false;

        char buf[1024];
        size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = '\0';

        std::string json(buf);

        // Manual parse (lightweight, no external library)
        out.mode = (ThemeMode)parse_int(json, "theme_mode", (int)THEME_LIGHT);
        out.accent_hue = parse_int(json, "accent_hue", 220);
        out.palette_index = parse_int(json, "palette_index", 0);
        out.l_action = (LRAction)parse_int(json, "l_action", (int)LR_SKIP_BACK);
        out.r_action = (LRAction)parse_int(json, "r_action", (int)LR_SKIP_FORWARD);
        out.dpad_speed = parse_int(json, "dpad_speed", 3);
        out.quick_access_ids = parse_string(json, "quick_access_ids", "");
        out.wallpaper_file = parse_string(json, "wallpaper_file", "");
        out.server_ip = parse_string(json, "server_ip", "");
        out.language = parse_string(json, "language", "en");
        out.accent_saturation = parse_float(json, "accent_saturation", 0.75f);
        out.accent_brightness = parse_float(json, "accent_brightness", 0.78f);

        // Validation
        if (out.accent_hue < 0 || out.accent_hue > 360) out.accent_hue = 220;
        if ((int)out.mode > 1) out.mode = THEME_LIGHT;
        if ((int)out.l_action > 3) out.l_action = LR_SKIP_BACK;
        if ((int)out.r_action > 3) out.r_action = LR_SKIP_FORWARD;
        if (out.dpad_speed < 1 || out.dpad_speed > 10) out.dpad_speed = 5;
        if (out.language != "ja") out.language = "en";
        if (out.accent_saturation < 0.0f || out.accent_saturation > 1.0f) out.accent_saturation = 0.75f;
        if (out.accent_brightness < 0.0f || out.accent_brightness > 1.0f) out.accent_brightness = 0.78f;

        return true;
    }

    // Save config to file
    static bool save(const AppConfig& cfg) {
        // Create directory if it doesn't exist
        mkdir("sdmc:/3ds", 0777);
        mkdir(CONFIG_DIR, 0777);

        FILE* f = fopen(CONFIG_PATH, "w");
        if (!f) return false;

        // Manual JSON serialize (clarity first)
        fprintf(f, "{\n");
        fprintf(f, "  \"theme_mode\": %d,\n", (int)cfg.mode);
        fprintf(f, "  \"accent_hue\": %d,\n", cfg.accent_hue);
        fprintf(f, "  \"palette_index\": %d,\n", cfg.palette_index);
        fprintf(f, "  \"l_action\": %d,\n", (int)cfg.l_action);
        fprintf(f, "  \"r_action\": %d,\n", (int)cfg.r_action);
        fprintf(f, "  \"dpad_speed\": %d,\n", cfg.dpad_speed);
        fprintf(f, "  \"quick_access_ids\": \"%s\",\n", cfg.quick_access_ids.c_str());
        fprintf(f, "  \"wallpaper_file\": \"%s\",\n", cfg.wallpaper_file.c_str());
        fprintf(f, "  \"server_ip\": \"%s\",\n", cfg.server_ip.c_str());
        fprintf(f, "  \"language\": \"%s\",\n", cfg.language.c_str());
        fprintf(f, "  \"accent_saturation\": %.2f,\n", cfg.accent_saturation);
        fprintf(f, "  \"accent_brightness\": %.2f\n", cfg.accent_brightness);
        fprintf(f, "}\n");

        fclose(f);
        return true;
    }

private:
    // Simple JSON string parser
    // Finds "key": "value" and returns the value
    static std::string parse_string(const std::string& json, const char* key,
                                    const std::string& default_val) {
        std::string search = std::string("\"") + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return default_val;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return default_val;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size() || json[pos] != '"') return default_val;
        pos++; // Skip opening quote
        std::string result;
        while (pos < json.size() && json[pos] != '"') result += json[pos++];
        return result;
    }

    // Simple JSON float parser
    static float parse_float(const std::string& json, const char* key, float default_val) {
        std::string search = std::string("\"") + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return default_val;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return default_val;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

        bool negative = false;
        if (pos < json.size() && json[pos] == '-') { negative = true; pos++; }

        float val = 0.0f;
        bool found_digit = false;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val = val * 10.0f + (float)(json[pos] - '0');
            found_digit = true;
            pos++;
        }
        if (pos < json.size() && json[pos] == '.') {
            pos++;
            float frac = 0.1f;
            while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
                val += (float)(json[pos] - '0') * frac;
                frac *= 0.1f;
                found_digit = true;
                pos++;
            }
        }
        if (!found_digit) return default_val;
        return negative ? -val : val;
    }

    // Simple JSON integer parser
    // Finds "key": 123 and returns the value
    static int parse_int(const std::string& json, const char* key, int default_val) {
        std::string search = std::string("\"") + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return default_val;

        pos = json.find(':', pos);
        if (pos == std::string::npos) return default_val;
        pos++; // After ':'

        // Skip whitespace
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

        // Handle negative numbers
        bool negative = false;
        if (pos < json.size() && json[pos] == '-') {
            negative = true;
            pos++;
        }

        int val = 0;
        bool found_digit = false;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val = val * 10 + (json[pos] - '0');
            found_digit = true;
            pos++;
        }

        if (!found_digit) return default_val;
        return negative ? -val : val;
    }
};
