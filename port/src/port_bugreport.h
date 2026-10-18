// Bug reports: rolling capture of the last ~20 s of picture, sound and log lines, saved on F9,
// from the settings menu or on a crash (port_bugreport.cpp, w12).
#ifndef PORT_BUGREPORT_H
#define PORT_BUGREPORT_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
// From main(), right after port_settings_configure(): starts the log tee and the capture
// threads when [bugreport] enabled = 1 (SMS_PORT_BUGREPORT=0/1 overrides). Off = nothing runs.
void port_bugreport_init(void);
bool port_bugreport_enabled(void);
// Main thread, once per presented frame (port_window.cpp): arms the next picture capture,
// counts the frame rate, runs the test triggers.
void port_bugreport_frame(void);
// port_aurora_sdk.cpp hands over a presented picture (4-byte texels, rows `stride` apart,
// bgra = BGRA8 else RGBA8), from whichever thread completed the readback.
void port_bugreport_push_frame(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t stride, bool bgra,
                               long long presentedQpc);
// port_audio_out.cpp: the final stereo mix (interleaved L/R 16-bit) before the device.
void port_bugreport_audio(const int16_t* lr, uint32_t frames, uint32_t rate);
// Writes bug_reports\<date>_<why>\ (video.mp4, log.txt, the replay) next to the exe. `why` is
// "f9", "menu" or "crash". Returns true if the folder was written (log.txt at least).
// crashText: the backtrace text for a crash (or null). Safe to call from the crash filter.
bool port_bugreport_save(const char* why, const char* crashText);
// From the crash handlers (port_crash.cpp / port_replay.cpp): save with a hard time limit;
// never blocks for more than a few seconds, never recurses.
void port_bugreport_crash(const char* crashText);
// Game state lines for log.txt (port_bugreport_game.cpp; may touch game memory).
void port_bugreport_game_state(char* out, size_t cap);
}

#endif
