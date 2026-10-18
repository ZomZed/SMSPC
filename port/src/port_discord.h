// Discord Rich Presence + invites over Discord's local IPC pipe (\.\pipe\discord-ipc-N).
// No Discord SDK: port_discord.cpp speaks the documented RPC framing itself, on its own thread.
// Settings: port_settings.ini [discord] (enabled, client_id, show_stage, allow_join).
#ifndef PORT_DISCORD_H
#define PORT_DISCORD_H

extern "C" {
// Once per presented frame on the game/render thread (port_window.cpp). Starts the integration
// on the first call; does almost nothing while it is idle (disabled, no client id, no Discord).
void port_discord_frame(void);
// One-line status for the settings menu (static buffer; main thread).
const char* port_discord_status(void);
void port_discord_shutdown(void);
}

#endif
