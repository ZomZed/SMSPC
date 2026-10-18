// Game data location for the PC port: a disc image (.iso/.gcm, .ciso) read in place, or an
// extracted disc folder (files/). Used by sdk_dvd.cpp's FST builder. (w9)
#ifndef PORT_DISC_H
#define PORT_DISC_H

#include <stdint.h>

#include <string>
#include <vector>

struct PortDiscEntry {
	std::string name; // as on disc
	bool isDir;
	int32_t parent; // directories: parent entry
	int32_t next;   // directories: index after the last descendant; files: index + 1
	uint32_t offset; // files: byte offset on the disc
	uint32_t size;   // files: byte length
};

// Finds the game data. Order: a disc path on the command line, %SMS_PORT_DISC%, the
// settings-file `disc =` entry, auto-detection (next to the exe, ./files, walking up
// from the exe for files/, dolphin_test/files, *.iso/*.gcm/*.ciso), then a message box and
// a file-open dialog (the choice is saved to the settings file).
// Returns true for a disc image: its FST is in `entries` (disc order, entry 0 = root) and
// port_disc_read() reads it. Returns false for an extracted folder in `folder`.
// Exits with a message box if no usable US (GMSE01) game data is found.
bool port_disc_locate(std::string* folder, std::vector<PortDiscEntry>* entries);

// Reads `length` bytes at disc offset `offset` of the image (thread-safe). Returns the
// number of bytes read (short at the end of the disc), or -1 on an I/O error.
int32_t port_disc_read(uint32_t offset, void* dst, int32_t length);

// What the game runs from, for replay files: 'image <ID> <hash> <file name>' or 'folder [<ID>]' (no paths).
const char* port_disc_identity(void);

// w13 (BOOTLOG "Regional behaviour (w13)"): every exe is built for one disc region
// (sms_port.exe US 'E', sms_port_jp.exe 'J', sms_port_eu.exe 'P'). port_disc_locate accepts
// GMSE01, GMSJ01 and GMSP01; a disc of another region is handed to the sibling exe
// (same arguments, the chosen disc in %SMS_PORT_DISC%), which this process waits for and
// whose exit code it returns. port_build_region() is this exe's region letter.
char port_build_region(void);
const char* port_build_game_id(void); // "GMSE01" / "GMSJ01" / "GMSP01"
// Called by port_main.cpp before Aurora starts (settings loaded): hands a disc of another
// region to the sibling exe before any window opens. Returns if nothing needs handing over.
void port_disc_early_handoff(void);
// Game behaviour (port_behaviour.cpp): the SMS_REGION_* a PortSettings::behaviour value
// selects on this exe, whether the European build's pal_video / pal_language settings differ
// from what this run uses, and the settings menu's restart (the next pumpEvents shuts the
// game down and starts it again with the same arguments; a hand-off child asks its parent
// to do it with exit code PORT_EXIT_RESTART).
int port_behaviour_for_setting(int setting);
bool port_pal_settings_changed(void);
void port_restart_request(void);
bool port_restart_pending(void);
void port_restart_now(void); // port_main.cpp pumpEvents; does not return
#define PORT_EXIT_RESTART 0x52535452u // "RSTR"

// Compressed Dolphin images (port_disc_wia.cpp): RVZ, WIA and GCZ, read as the plain disc.
class PortDiscReader {
public:
	virtual ~PortDiscReader() {}
	virtual uint64_t size() const = 0; // plain disc size
	// Thread-safe. Returns bytes read (short at the end of the disc) or -1 on an error.
	virtual int32_t read(uint64_t offset, void* dst, int32_t length) = 0;
};

// Opens `path` (UTF-8) if it is an RVZ, WIA or GCZ file. Returns null if it is not one
// (*err empty), or if it is one that cannot be read (*err says why: Wii disc, damaged,
// unsupported compression).
PortDiscReader* port_disc_open_compressed(const std::string& path, std::string* err);

#endif
