#include "config.h"

void config_set_defaults(Config *config) {
    config->enabled = TRUE;
    config->valid = TRUE;
    config->target_fps = 90;
    config->logging = TRUE;
    config->log_history = FALSE;
    config->debug_motion_frames = 0;
    config->debug_frame_times = 0;
    config->debug_trail_draws = 0;
    config->debug_apt_calls = FALSE;
    config->debug_spawn_frames = 0;
    config->debug_laser_draws = 0;
    config->adaptive_fps = TRUE;
    config->profiler = FALSE;
    config->profiler_report_seconds = 60;
    config->multicore_particle_index = TRUE;
    config->multicore_verify = FALSE;
    config->multicore_trail_fix = TRUE;
}

BOOL config_load(Config *config, const wchar_t *path) {
    DWORD attributes;
    int fps;
    if (config == NULL || path == NULL) return FALSE;
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FALSE;
    }

    /* A TRUE return means the INI existed, not that every setting was valid. */
    config->enabled = GetPrivateProfileIntW(L"fps_patch", L"enabled", config->enabled, path) != 0;
    fps = (int)GetPrivateProfileIntW(L"fps_patch", L"target_fps", config->target_fps, path);
    /* Ratios 3..6 are the complete-client schedules available above 30 FPS. */
    /*
     * Up to 90 FPS the schedules need a whole number of client frames per
     * 15 Hz logic tick; above 90 the tick length alternates (any rate).
     */
    config->valid = (fps >= 45 && fps <= 90 && fps % 15 == 0) || (fps > 90 && fps <= 240);
    config->target_fps = config->valid ? (u32)fps : 0u;
    config->logging = GetPrivateProfileIntW(L"fps_patch", L"logging", config->logging, path) != 0;
    config->log_history =
        GetPrivateProfileIntW(L"fps_patch", L"log_history", config->log_history, path) != 0;
    config->debug_motion_frames = GetPrivateProfileIntW(
        L"fps_patch", L"debug_motion_frames", config->debug_motion_frames, path);
    config->debug_frame_times = GetPrivateProfileIntW(
        L"fps_patch", L"debug_frame_times", config->debug_frame_times, path);
    config->debug_trail_draws = GetPrivateProfileIntW(
        L"fps_patch", L"debug_trail_draws", config->debug_trail_draws, path);
    config->debug_apt_calls = GetPrivateProfileIntW(
        L"fps_patch", L"debug_apt_calls", config->debug_apt_calls, path);
    config->debug_spawn_frames = GetPrivateProfileIntW(
        L"fps_patch", L"debug_spawn_frames", config->debug_spawn_frames, path);
    {
        wchar_t wide[512];
        DWORD length = GetPrivateProfileStringW(L"fps_patch", L"debug_disable", L"", wide,
                                                ARRAY_COUNT(wide), path);
        DWORD i;
        for (i = 0; i < length && i + 1u < sizeof(config->debug_disable); ++i) {
            wchar_t c = wide[i];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            config->debug_disable[i] = (c < 128) ? (char)c : '?';
        }
        config->debug_disable[i] = '\0';
    }
    config->debug_laser_draws = GetPrivateProfileIntW(
        L"fps_patch", L"debug_laser_draws", config->debug_laser_draws, path);
    config->adaptive_fps = GetPrivateProfileIntW(
        L"fps_patch", L"adaptive_fps", config->adaptive_fps, path) != 0;
    config->profiler = GetPrivateProfileIntW(L"fps_patch", L"profiler", config->profiler, path) != 0;
    config->profiler_report_seconds = GetPrivateProfileIntW(
        L"fps_patch", L"profiler_report_seconds", config->profiler_report_seconds, path);
    if (config->profiler_report_seconds < 5u) config->profiler_report_seconds = 5u;
    config->multicore_particle_index = GetPrivateProfileIntW(
        L"fps_patch", L"multicore_particle_index", config->multicore_particle_index, path) != 0;
    config->multicore_verify = GetPrivateProfileIntW(
        L"fps_patch", L"multicore_verify", config->multicore_verify, path) != 0;
    config->multicore_trail_fix = GetPrivateProfileIntW(
        L"fps_patch", L"multicore_trail_fix", config->multicore_trail_fix, path) != 0;
    return TRUE;
}
