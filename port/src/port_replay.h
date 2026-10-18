// Input recording and replay (port_replay.cpp, w9).
#ifndef PORT_REPLAY_H
#define PORT_REPLAY_H

#include <dolphin/pad.h>
#include <stdint.h>

#include <aurora/aurora.h>

extern "C" {
// From port_settings_configure(): starts recording (SMS_PORT_RECORD / [debug] record) or
// loads SMS_PORT_REPLAY=<file> (isolated memory card copy in config->userPath).
void port_replay_init(AuroraConfig* config);
// From port_input_read(): records, or (replay) replaces, the pad status the game reads.
void port_replay_pads(PADStatus* status);
bool port_replay_active(void);   // replaying
bool port_replay_recording(void);
// Recording or replaying: timing-dependent results (EFB readbacks) are delivered at fixed
// game-frame delays (port_efb.cpp).
bool port_replay_deterministic(void);
// Writes the recording as sms_replay_<date>_<time>.bin next to the exe. Returns false if
// nothing is recorded. `why` goes into the file ("hotkey", "crash", ...).
bool port_replay_save(const char* why);
// w12: the same into folder `dir` (null = next to the exe); bug reports (port_bugreport.cpp).
bool port_replay_save_dir(const char* why, const char* dir);
// Short status for the menu.
const char* port_replay_status(void);
// Quality-of-life state (16 bytes, port_qol.cpp): recorded on change, replaced while replaying.
void port_replay_qol(uint8_t* state16);
void port_replay_qol_initial(uint8_t* state16);
}

#endif
