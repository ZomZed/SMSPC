// Multiplayer transport and session layer (w10). See port/MULTIPLAYER.md and port_mp.h.
//
// One UDP socket (Winsock), one network thread. The host owns the player table and relays
// snapshots and reliable messages between clients. Every packet is validated (magic,
// protocol version, length, connection id + address) and dropped silently otherwise.
// Nothing here touches game state: the game glue (port_mp_game.cpp) and w15's puppet code
// poll the API.
#ifdef SMS_PORT_AURORA

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#include "port_mp.h"
#include "port_disc.h" // port_build_region (the version stamp)

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(PortMpSnapshot) == 128, "PortMpSnapshot is a wire format");
static_assert(sizeof(PortMpCharacter) == 32, "PortMpCharacter is a wire format");
static_assert(sizeof(PortMpSessionSettings) == 16, "PortMpSessionSettings is a wire format");

// port_mp_game.cpp: ini load/save of the multiplayer settings
extern "C" void port_mp_ini_load(char* name, int nameSize, int* port, int* enabled);
extern "C" void port_mp_ini_server(char* spec, int size); // [multiplayer] server
// port_mp_menu.cpp: on-screen notification
extern "C" void port_mp_toast(const char* text);

namespace {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

const u32 kMagic          = 0x31504D53; // "SMP1" little-endian
const u32 kJoinTag        = 0x4E494F4A; // "JOIN": first word of a CONNECT body
const int kHeaderSize     = 16;
const int kMaxPacket      = 1400;
const double kTimeoutMs   = 20000.0; // a peer silent this long is gone (short drops reconnect)
const double kLagMs       = 3000.0;  // silent this long: "reconnecting" (status / toast)
const double kConnectMs   = 6000.0;
const double kJoinNameMs  = 12000.0; // join by name: lookup + punching + relay
const double kPunchMs     = 2500.0;  // direct attempts before asking for the relay
const int kHistory        = 8;
const int kDiscoveryPorts = 16;

enum PacketType : u8 {
	PKT_DISCOVER = 1,
	PKT_LOBBY_INFO,
	PKT_CONNECT,
	PKT_ACCEPT,
	PKT_REJECT,
	PKT_RELIABLE,
	PKT_ACK,
	PKT_SNAPSHOT,
	PKT_PING,
	PKT_PONG,
	PKT_DISCONNECT,
	PKT_ROSTER,
	PKT_APP, // unreliable app message (port_mp_send)
	PKT_PUNCH, // host -> client endpoint from the lobby server (opens the NAT path; ignored)
	PKT_COUNT
};

enum MsgKind : u8 {
	MSG_PLAYER_JOIN = 1,
	MSG_PLAYER_LEAVE,
	MSG_PLAYER_INFO,
	MSG_SETTINGS,
	MSG_STAGE,
	MSG_CHAT,
	MSG_OBJECTIVE,
	MSG_APP, // reliable app message (port_mp_send)
	MSG_PEERS,    // host -> clients: every player's endpoint as the host sees it (migration)
	MSG_LOBBYKEY, // host -> clients: lobby-server id + token (a new host takes the name over)
};

enum RejectReason : u8 { REJ_VERSION = 1, REJ_FULL, REJ_CLOSED, REJ_BADNAME, REJ_BADPASS };
enum LeaveReason : u8 { LEAVE_QUIT = 1, LEAVE_TIMEOUT, LEAVE_KICKED, LEAVE_HOST_CLOSED };

// ---------------------------------------------------------------------------------
double nowMs()
{
	static LARGE_INTEGER sFreq, sStart;
	if (sFreq.QuadPart == 0) {
		QueryPerformanceFrequency(&sFreq);
		QueryPerformanceCounter(&sStart);
	}
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return (double)(q.QuadPart - sStart.QuadPart) * 1000.0 / (double)sFreq.QuadPart;
}

bool sVerbose;
void mplog(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "[mp] %s\n", buf);
}

// Byte writer / bounds-checked reader (little-endian).
struct Writer {
	u8 buf[kMaxPacket];
	int n = 0;
	bool ok = true;
	void put(const void* p, int len)
	{
		if (n + len > kMaxPacket) {
			ok = false;
			return;
		}
		std::memcpy(buf + n, p, len);
		n += len;
	}
	void u8_(u8 v) { put(&v, 1); }
	void u16_(u16 v) { put(&v, 2); }
	void u32_(u32 v) { put(&v, 4); }
	void f64_(double v) { put(&v, 8); }
	void str(const char* s, int size) // fixed-size field, NUL padded
	{
		char tmp[64] = {};
		std::strncpy(tmp, s, size - 1);
		put(tmp, size);
	}
};

struct Reader {
	const u8* p;
	int n, i = 0;
	bool ok = true;
	Reader(const u8* d, int len) : p(d), n(len) {}
	bool get(void* out, int len)
	{
		if (!ok || i + len > n) {
			ok = false;
			std::memset(out, 0, len);
			return false;
		}
		std::memcpy(out, p + i, len);
		i += len;
		return true;
	}
	u8 u8_() { u8 v; get(&v, 1); return v; }
	u16 u16_() { u16 v; get(&v, 2); return v; }
	u32 u32_() { u32 v; get(&v, 4); return v; }
	double f64_() { double v; get(&v, 8); return v; }
	void str(char* out, int size)
	{
		get(out, size);
		out[size - 1] = '\0';
	}
	int left() const { return n - i; }
};

// Keeps printable characters only, trims, never empty. Fits `size` (player names
// PORT_MP_NAME_MAX, lobby names PORT_MP_LOBBY_NAME_MAX; w15: lobby names were cut to 23).
void sanitizeName(char* s, int size, const char* fallback)
{
	char out[PORT_MP_LOBBY_NAME_MAX > PORT_MP_NAME_MAX ? PORT_MP_LOBBY_NAME_MAX : PORT_MP_NAME_MAX] = {};
	const int cap = (size < (int)sizeof(out) ? size : (int)sizeof(out)) - 1;
	int o = 0;
	for (int i = 0; i < size && s[i] != '\0' && o < cap;) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) {
			i++;
			if (c < 0x20 || c == 0x7F || (c == ' ' && o == 0))
				continue;
			out[o++] = (char)c;
			continue;
		}
		// UTF-8: keep only well-formed sequences, others become '?'
		int need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
		bool ok  = need > 0 && i + need <= size;
		for (int k = 1; ok && k < need; k++)
			ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
		if (!ok) {
			out[o++] = '?';
			i++;
			continue;
		}
		if (o + need > cap)
			break;
		for (int k = 0; k < need; k++)
			out[o++] = s[i + k];
		i += need;
	}
	while (o > 0 && out[o - 1] == ' ')
		out[--o] = '\0';
	// do not end on a cut UTF-8 sequence
	int lead = o;
	while (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80)
		lead--;
	if (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0xC0) {
		unsigned char c = (unsigned char)out[lead - 1];
		int need        = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
		if (o - (lead - 1) < need)
			out[o = lead - 1] = '\0';
	}
	if (o == 0)
		std::snprintf(out, sizeof(out), "%s", fallback);
	std::memset(s, 0, size);
	std::snprintf(s, size, "%s", out);
}

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b)
{
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string addrStr(const sockaddr_in& a)
{
	char ip[64];
	inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
	char out[80];
	std::snprintf(out, sizeof(out), "%s:%u", ip, ntohs(a.sin_port));
	return out;
}

// wrap-aware id compare
inline bool idLE(u16 a, u16 b) { return (int16_t)(a - b) <= 0; }

// ---------------------------------------------------------------------------------
struct OutMsg {
	u16 id;
	u8 kind;
	std::vector<u8> data;
	double lastSend = -1e9;
	int sends       = 0;
};

struct Conn {
	bool used = false;
	sockaddr_in addr{};
	u32 connId   = 0;
	u32 nonce    = 0;
	int slot     = -1;
	double lastRecv = 0, lastPing = 0, lastAck = 0;
	u16 txSeq    = 0;
	u16 nextTxId = 1;
	std::deque<OutMsg> outq;
	u16 rxNext = 1;
	std::map<u16, std::pair<u8, std::vector<u8>>> rxBuf;
	bool ackDue  = false;
	bool established = false; // a packet other than CONNECT arrived (the path is fixed)
	bool lagging     = false; // silent for kLagMs (shown as reconnecting)
	double rttMs = 100.0;
	// stats
	u32 snapRx = 0, snapTx = 0, dropped = 0, resent = 0;
	double relayAt[PORT_MP_MAX_PLAYERS] = {}; // host: last relay of slot -> this conn
};

struct Player {
	bool used = false;
	bool isHost = false;
	u32 serial = 0;
	char name[PORT_MP_NAME_MAX] = {};
	PortMpCharacter character{};
	u8 stage = 0xFF, scenario = 0xFF;
	u16 pingMs = 0;
	double rejoinBy = 0; // new host after a migration: dropped unless back by then
	PortMpSnapshotRx hist[kHistory];
	int histCount = 0;
};

std::mutex gMu;
std::atomic<bool> gThreadRunning{ false };
std::thread gThread;
SOCKET gSock      = INVALID_SOCKET;
int gSockGen      = 0;
bool gWsaUp       = false;
bool gInit        = false;
int gEnabled      = 1;
int gState        = PORT_MP_OFF;
bool gIsHost      = false;
int gLocalSlot    = -1;
char gStatus[256] = "Not connected.";
char gLocalName[PORT_MP_NAME_MAX] = "Player";
PortMpCharacter gLocalChar{};

// Our game's version in the character (PortMpCharacter.reserved[4..5], see port_mp.h): the game
// behaviour (SMSGetBehaviour + 1) and the video rate (1 = 60 Hz, 2 = 50 Hz, 0 = unknown). Players
// whose games differ desync; port_mp_game.cpp warns everyone (port_mp_version_check).
extern "C" int SMSGetBehaviour(void);   // port_behaviour.cpp
extern "C" int port_settings_pal60(void); // port_behaviour.cpp
void stampGame(PortMpCharacter& c)
{
	c.reserved[4] = (uint8_t)(SMSGetBehaviour() + 1);
	const int pal = port_build_region() == 'P' ? port_settings_pal60() : 1;
	c.reserved[5] = (uint8_t)(pal == 0 ? 2 : pal == 1 ? 1 : 0); // "ask at boot": unknown
}
int gDefaultPort = PORT_MP_DEFAULT_PORT;
int gHostPort    = 0;
char gLobbyName[PORT_MP_LOBBY_NAME_MAX] = "Delfino Lobby";
PortMpSessionSettings gSettings{};
Player gPlayers[PORT_MP_MAX_PLAYERS];
u32 gSerialCounter = 1;
u8 gLocalStage = 0xFF, gLocalScenario = 0xFF;

// local snapshot
PortMpSnapshot gLocalSnap{};
bool gHaveLocalSnap = false;
u32 gSentTick       = 0xFFFFFFFF;
double gLastSnapSend = 0;

// host: one Conn per client slot; client: gConns[0] is the host
Conn gConns[PORT_MP_MAX_PLAYERS];
double gConnectStart = 0, gLastConnect = 0;
u32 gConnectNonce    = 0;
sockaddr_in gHostAddr{};
double gLastRoster = 0, gLastStats = 0;

// Shared objectives (MULTIPLAYER.md). Ids: port_mp.h PORT_MP_OBJ_*.
enum ObjOp : u8 { OBJ_COLLECT = 1, OBJ_COLLECTED, OBJ_SET };
std::vector<u32> gObjPersistent;                 // host: shines, blue coins
std::map<u32, std::vector<u32>> gObjVisit;       // host: (stage<<8|scenario) -> coins, red coins
struct ObjEvent {
	u32 id;
	u8 slot;
};
std::deque<ObjEvent> gObjEvents;                 // for the game thread (port_mp_objective_poll)

// App messages (port_mp_send / port_mp_recv), one queue per channel.
struct AppMsg {
	u8 from;
	std::vector<u8> data;
};
std::deque<AppMsg> gAppQ[256];

// LAN discovery
std::vector<PortMpLobbyInfo> gLobbies;
u32 gDiscoverToken  = 0;
double gDiscoverAt  = 0;

void setStatus(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(gStatus, sizeof(gStatus), fmt, ap);
	va_end(ap);
	mplog("%s", gStatus);
}

u32 rnd32()
{
	static u32 s = 0;
	if (s == 0)
		s = (u32)GetTickCount64() ^ (u32)GetCurrentProcessId() * 2654435761u ^ (u32)(nowMs() * 1000.0);
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s ? s : 0x1234567;
}

// ---------------------------------------------------------------------------------
// Socket
void closeSock()
{
	if (gSock != INVALID_SOCKET) {
		closesocket(gSock);
		gSock = INVALID_SOCKET;
		gSockGen++;
	}
}

bool openSock(int port)
{
	closeSock();
	if (!gWsaUp) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			setStatus("Network unavailable (WSAStartup failed).");
			return false;
		}
		gWsaUp = true;
	}
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		setStatus("Could not create a UDP socket (error %d).", WSAGetLastError());
		return false;
	}
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
	BOOL yes = TRUE;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
	// Windows reports an ICMP "port unreachable" as WSAECONNRESET on the next recvfrom;
	// turn that off so one dead peer does not disturb the others.
	DWORD noReset = FALSE, bytes = 0;
	WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &noReset, sizeof(noReset), nullptr, 0, &bytes,
	         nullptr, nullptr);
	int rcv = 1 << 20;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));
	sockaddr_in a{};
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port        = htons((u_short)port);
	if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
		int e = WSAGetLastError();
		closesocket(s);
		if (e == WSAEADDRINUSE)
			setStatus("Port %d is already in use (another lobby on this PC?). Pick another port.", port);
		else
			setStatus("Could not open UDP port %d (error %d).", port, e);
		return false;
	}
	gSock = s;
	gSockGen++;
	return true;
}

// SMS_PORT_MP_DROP=<percent> (debug): drop that share of sent and received datagrams.
int sDropPct = -1;
bool dropNow()
{
	if (sDropPct < 0) {
		const char* e = std::getenv("SMS_PORT_MP_DROP");
		sDropPct      = e != nullptr ? std::atoi(e) : 0;
	}
	// SMS_PORT_MP_BLACKOUT=<start s>,<duration s> (debug): nothing in or out in that window of
	// process time (a Wi-Fi drop), to test reconnection.
	static double sBoStart = -2, sBoEnd = -1;
	if (sBoStart == -2) {
		sBoStart = -1;
		if (const char* e = std::getenv("SMS_PORT_MP_BLACKOUT")) {
			double a = 0, b = 0;
			if (std::sscanf(e, "%lf,%lf", &a, &b) == 2) {
				sBoStart = a * 1000.0;
				sBoEnd   = (a + b) * 1000.0;
			}
		}
	}
	if (sBoStart >= 0) {
		const double t = nowMs();
		if (t >= sBoStart && t < sBoEnd)
			return true;
	}
	return sDropPct > 0 && (int)(rnd32() % 100) < sDropPct;
}

// ---------------------------------------------------------------------------------
// Lobby server (port/server/sms_lobby_server.py): registry, rendezvous, relay.
// Its packets: u32 'SMLS', u8 version, u8 type, body (MULTIPLAYER.md "Lobby server").
const u32 kSrvMagic = 0x534C4D53;
const u8 kSrvProto  = 1;
enum SrvType : u8 {
	S_REGISTER = 1, S_REGISTERED, S_REJECT, S_HEARTBEAT, S_UNREGISTER, S_LOOKUP, S_FOUND, S_PUNCH,
	S_RELAY_OPEN, S_RELAY_OK, S_RELAY_DATA, S_STATUS, S_STATUS_OK, S_TAKEOVER, S_LIST, S_LIST_OK
};
enum SrvReject : u8 {
	SR_VERSION = 1, SR_NAME_TAKEN, SR_BAD_NAME, SR_LIMIT, SR_NOT_FOUND, SR_BAD_PASSWORD, SR_FULL, SR_RATE, SR_NO_RELAY
};

char gServerSpec[128] = "";   // "host[:port]" from the ini / SMS_PORT_MP_SERVER
sockaddr_in gServerAddr{};
bool gServerResolved = false;
double gServerResolveAt = -1e9;
int gServerState      = PORT_MP_SERVER_UNKNOWN;
int gServerLobbies    = 0;
bool gServerRelay     = false;
double gServerPingAt = -1e9, gServerPongAt = -1e9;
u32 gServerPingNonce  = 0;
char gPublicAddr[48]  = ""; // our endpoint as the server sees it ("a.b.c.d:port")
u32 gServerListNonce = 0;   // the public lobby list (S_LIST, sent with each status ping)
std::vector<PortMpLobbyInfo> gServerList;

int gConnType   = PORT_MP_CONN_DIRECT; // of the current session
u64 gPwHash     = 0;                    // host: password hash (0 = none); client: what we send
// host registration
u32 gLobbyId = 0, gLobbyToken = 0, gRegNonce = 0;
bool gRegistered = false;
double gRegSentAt = -1e9, gHeartbeatAt = -1e9, gRegStart = 0;
// client join by name
bool gJoinByName   = false;
int gJoinPhase     = 0; // 0 lookup, 1 connecting to candidates, 2 relay
char gJoinName[PORT_MP_LOBBY_NAME_MAX] = "";
u32 gLookupNonce   = 0;
double gLookupAt = -1e9, gFoundAt = 0, gRelayAskAt = -1e9;
u32 gFoundLobby    = 0;
std::vector<sockaddr_in> gCandidates; // host endpoints to try (public, LAN, LAN-discovered)
bool gForceRelay   = false;
// host migration (client side): the others' endpoints and the lobby key, from the host
struct Peer {
	bool known;
	sockaddr_in addr;
};
Peer gPeers[PORT_MP_MAX_PLAYERS];
u32 gKeyLobbyId = 0, gKeyToken = 0;
bool gMigrating   = false; // re-joining the new host (the session goes on meanwhile)
u8 gRejoinSlot    = 0xFF;  // CONNECT extension: the slot / serial we had
u32 gRejoinSerial = 0;
double gRegTolerateUntil = 0; // a new host's REGISTER: "name taken" means the old entry, retry
// relay sessions: index i <-> virtual address 240.0.0.(i+1):1
std::vector<u32> gRelayIds;

u64 pwHash(const char* name, const char* password)
{
	if (password == nullptr || password[0] == '\0')
		return 0;
	// FNV-1a 64 of "sms|<lowercase name>|<password>" (casual lobby passwords, not secrets)
	u64 h = 1469598103934665603ull;
	auto mix = [&](const char* s, bool lower) {
		for (; *s; s++) {
			unsigned char c = (unsigned char)*s;
			if (lower && c < 0x80)
				c = (unsigned char)tolower(c);
			h ^= c;
			h *= 1099511628211ull;
		}
	};
	mix("sms|", false);
	mix(name, true);
	mix("|", false);
	mix(password, false);
	return h ? h : 1;
}

bool nameEq(const char* a, const char* b) { return _stricmp(a, b) == 0; } // ASCII case-insensitive

bool isRelayAddr(const sockaddr_in& a) { return (ntohl(a.sin_addr.s_addr) >> 24) == 240; }

sockaddr_in relayAddr(u32 rid)
{
	size_t i = 0;
	for (; i < gRelayIds.size(); i++)
		if (gRelayIds[i] == rid)
			break;
	if (i == gRelayIds.size())
		gRelayIds.push_back(rid);
	sockaddr_in a{};
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = htonl(0xF0000000u | (u32)(i + 1));
	a.sin_port        = htons(1);
	return a;
}

void srvSend(u8 type, const Writer& body)
{
	if (gSock == INVALID_SOCKET || !gServerResolved)
		return;
	u8 pkt[kMaxPacket + 16];
	u32 m = kSrvMagic;
	std::memcpy(pkt, &m, 4);
	pkt[4] = kSrvProto;
	pkt[5] = type;
	std::memcpy(pkt + 6, body.buf, body.n);
	sendto(gSock, (const char*)pkt, 6 + body.n, 0, (const sockaddr*)&gServerAddr, sizeof(gServerAddr));
}

void relaySend(const sockaddr_in& to, const u8* pkt, int len)
{
	const u32 i = (ntohl(to.sin_addr.s_addr) & 0xFFFFFF) - 1;
	if (i >= gRelayIds.size() || !gServerResolved)
		return;
	u8 buf[kMaxPacket + 16];
	u32 m = kSrvMagic;
	std::memcpy(buf, &m, 4);
	buf[4] = kSrvProto;
	buf[5] = S_RELAY_DATA;
	std::memcpy(buf + 6, &gRelayIds[i], 4);
	std::memcpy(buf + 10, pkt, len);
	sendto(gSock, (const char*)buf, 10 + len, 0, (const sockaddr*)&gServerAddr, sizeof(gServerAddr));
}

std::string ip4(u32 ipBE, u16 portBE)
{
	in_addr a;
	a.s_addr = ipBE;
	char s[32];
	inet_ntop(AF_INET, &a, s, sizeof(s));
	char out[48];
	std::snprintf(out, sizeof(out), "%s:%u", s, ntohs(portBE));
	return out;
}

void sendRaw(const sockaddr_in& to, u8 type, u8 slot, u32 connId, u16 seq, const u8* payload, int len)
{
	if (gSock == INVALID_SOCKET || len < 0 || len + kHeaderSize > kMaxPacket)
		return;
	if (dropNow())
		return;
	u8 pkt[kMaxPacket];
	u32 magic = kMagic;
	u16 proto = PORT_MP_PROTOCOL_VERSION;
	u16 plen  = (u16)len;
	std::memcpy(pkt + 0, &magic, 4);
	std::memcpy(pkt + 4, &proto, 2);
	pkt[6] = type;
	pkt[7] = slot;
	std::memcpy(pkt + 8, &connId, 4);
	std::memcpy(pkt + 12, &seq, 2);
	std::memcpy(pkt + 14, &plen, 2);
	if (len > 0)
		std::memcpy(pkt + kHeaderSize, payload, len);
	if (isRelayAddr(to)) { // through the lobby server's relay
		relaySend(to, pkt, kHeaderSize + len);
		return;
	}
	sendto(gSock, (const char*)pkt, kHeaderSize + len, 0, (const sockaddr*)&to, sizeof(to));
}

void sendConn(Conn& c, u8 type, const Writer& w)
{
	const u8 slot = gLocalSlot >= 0 ? (u8)gLocalSlot : 0xFF;
	sendRaw(c.addr, type, slot, c.connId, c.txSeq++, w.buf, w.n);
}

// ---------------------------------------------------------------------------------
// Reliable channel
void queueMsg(Conn& c, u8 kind, const Writer& w)
{
	if (!c.used)
		return;
	OutMsg m;
	m.id   = c.nextTxId++;
	m.kind = kind;
	m.data.assign(w.buf, w.buf + w.n);
	c.outq.push_back(std::move(m));
}

void writeAcks(Conn& c, Writer& w)
{
	u16 cum  = (u16)(c.rxNext - 1);
	u32 bits = 0;
	for (auto& kv : c.rxBuf) {
		int d = (int16_t)(kv.first - (u16)(cum + 2));
		if (d >= 0 && d < 32)
			bits |= 1u << d;
	}
	w.u16_(cum);
	w.u32_(bits);
}

void flushReliable(Conn& c, double now)
{
	if (!c.used)
		return;
	const double rto = c.rttMs * 1.5 > 100.0 ? c.rttMs * 1.5 : 100.0;
	size_t i = 0;
	bool sentAny = false;
	while (i < c.outq.size()) {
		Writer w;
		writeAcks(c, w);
		int countPos = w.n;
		w.u8_(0);
		int count = 0;
		for (; i < c.outq.size(); i++) {
			OutMsg& m = c.outq[i];
			if (now - m.lastSend < rto)
				continue;
			if (w.n + 5 + (int)m.data.size() > kMaxPacket - kHeaderSize - 8)
				break;
			w.u16_(m.id);
			w.u8_(m.kind);
			w.u16_((u16)m.data.size());
			if (!m.data.empty())
				w.put(m.data.data(), (int)m.data.size());
			if (m.sends > 0)
				c.resent++;
			m.lastSend = now;
			m.sends++;
			if (++count == 255) {
				i++;
				break;
			}
		}
		if (count == 0)
			break;
		w.buf[countPos] = (u8)count;
		sendConn(c, PKT_RELIABLE, w);
		sentAny = true;
	}
	if (sentAny) {
		c.ackDue  = false;
		c.lastAck = now;
	} else if (c.ackDue && now - c.lastAck > 20.0) {
		Writer w;
		writeAcks(c, w);
		sendConn(c, PKT_ACK, w);
		c.ackDue  = false;
		c.lastAck = now;
	}
}

void readAcks(Conn& c, Reader& r)
{
	u16 cum  = r.u16_();
	u32 bits = r.u32_();
	if (!r.ok)
		return;
	for (auto it = c.outq.begin(); it != c.outq.end();) {
		int d = (int16_t)(it->id - (u16)(cum + 2));
		bool acked = idLE(it->id, cum) || (d >= 0 && d < 32 && (bits >> d) & 1);
		if (acked) {
			if (it->sends == 1 && it->lastSend > 0) {
				double sample = nowMs() - it->lastSend;
				c.rttMs = c.rttMs * 0.875 + sample * 0.125;
			}
			it = c.outq.erase(it);
		} else {
			++it;
		}
	}
}

// ---------------------------------------------------------------------------------
// Player table
void resetPlayers()
{
	for (auto& p : gPlayers)
		p = Player();
}

void writePlayer(Writer& w, int slot)
{
	const Player& p = gPlayers[slot];
	w.u8_((u8)slot);
	w.u32_(p.serial);
	w.u8_(p.isHost ? 1 : 0);
	w.str(p.name, PORT_MP_NAME_MAX);
	w.put(&p.character, sizeof(PortMpCharacter));
	w.u8_(p.stage);
	w.u8_(p.scenario);
}

bool readPlayer(Reader& r, int* slotOut)
{
	int slot = r.u8_();
	Player tmp;
	tmp.used   = true;
	tmp.serial = r.u32_();
	tmp.isHost = r.u8_() != 0;
	r.str(tmp.name, PORT_MP_NAME_MAX);
	r.get(&tmp.character, sizeof(PortMpCharacter));
	tmp.stage    = r.u8_();
	tmp.scenario = r.u8_();
	if (!r.ok || slot >= PORT_MP_MAX_PLAYERS)
		return false;
	sanitizeName(tmp.name, PORT_MP_NAME_MAX, "Player");
	Player& p = gPlayers[slot];
	if (!p.used || p.serial != tmp.serial) {
		p = tmp; // new occupant: fresh snapshot history
	} else {
		std::memcpy(p.name, tmp.name, sizeof(p.name));
		p.character = tmp.character;
		p.isHost    = tmp.isHost;
		p.stage     = tmp.stage;
		p.scenario  = tmp.scenario;
	}
	*slotOut = slot;
	return true;
}

void uniqueName(char* name, int skipSlot)
{
	char base[PORT_MP_NAME_MAX];
	std::snprintf(base, sizeof(base), "%s", name);
	for (int n = 2; n < 100; n++) {
		bool clash = false;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			if (i != skipSlot && gPlayers[i].used && _stricmp(gPlayers[i].name, name) == 0)
				clash = true;
		if (!clash)
			return;
		char suffix[8];
		std::snprintf(suffix, sizeof(suffix), " %d", n);
		int room = PORT_MP_NAME_MAX - 1 - (int)std::strlen(suffix);
		std::snprintf(name, PORT_MP_NAME_MAX, "%.*s%s", room, base, suffix);
	}
}

int playerCount()
{
	int n = 0;
	for (auto& p : gPlayers)
		n += p.used;
	return n;
}

void storeSnapshot(int slot, const PortMpSnapshot& s)
{
	Player& p = gPlayers[slot];
	if (!p.used)
		return;
	if (p.histCount > 0 && (int32_t)(s.tick - p.hist[0].snap.tick) <= 0)
		return; // old or duplicate
	int n = p.histCount < kHistory ? p.histCount : kHistory - 1;
	std::memmove(&p.hist[1], &p.hist[0], sizeof(PortMpSnapshotRx) * n);
	p.hist[0].snap = s;
	p.hist[0].rxMs = nowMs();
	p.histCount    = n + 1;
	if (s.flags & PORT_MP_SNAP_VALID) {
		p.stage    = s.stage;
		p.scenario = s.scenario;
	}
}

// ---------------------------------------------------------------------------------
// Session teardown
void resetServerSession()
{
	gMigrating = false;
	for (auto& p : gPeers)
		p.known = false;
	gKeyLobbyId = gKeyToken = 0;
	gRegistered = false;
	gLobbyId = gLobbyToken = 0;
	gJoinByName = false;
	gJoinPhase  = 0;
	gCandidates.clear();
	gRelayIds.clear();
	gFoundLobby = 0;
}

void endSession(const char* why)
{
	if (gState == PORT_MP_OFF)
		return;
	if (gRegistered) { // free the name on the lobby server now
		Writer w;
		w.u32_(gLobbyId);
		w.u32_(gLobbyToken);
		srvSend(S_UNREGISTER, w);
		srvSend(S_UNREGISTER, w);
	}
	// polite goodbye (3 copies, unreliable)
	for (auto& c : gConns)
		if (c.used) {
			Writer w;
			w.u8_(gIsHost ? LEAVE_HOST_CLOSED : LEAVE_QUIT);
			for (int i = 0; i < 3; i++)
				sendConn(c, PKT_DISCONNECT, w);
		}
	for (auto& c : gConns)
		c = Conn();
	resetPlayers();
	gObjPersistent.clear();
	gObjVisit.clear();
	gObjEvents.clear();
	resetServerSession();
	for (auto& q : gAppQ)
		q.clear();
	gIsHost    = false;
	gLocalSlot = -1;
	gHaveLocalSnap = false;
	gSentTick  = 0xFFFFFFFF;
	gState     = PORT_MP_OFF;
	if (why != nullptr)
		setStatus("%s", why);
	closeSock();
}

void failClient(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (auto& c : gConns)
		c = Conn();
	resetPlayers();
	gObjPersistent.clear();
	gObjVisit.clear();
	gObjEvents.clear();
	resetServerSession();
	for (auto& q : gAppQ)
		q.clear();
	gIsHost    = false;
	gLocalSlot = -1;
	gState     = PORT_MP_FAILED;
	closeSock();
	setStatus("%s", buf);
	port_mp_toast(buf); // also on screen (the menu may be closed: host left, connection lost)
}

// host migration (defined further down)
void hostSendPeers();
void hostSendLobbyKey(int toSlot);
void startMigration(const char* why);

// host: tell everyone (except `except`) a reliable message
void broadcastMsg(u8 kind, const Writer& w, int exceptSlot)
{
	for (auto& c : gConns)
		if (c.used && c.slot != exceptSlot)
			queueMsg(c, kind, w);
}

// ---------------------------------------------------------------------------------
// Shared objectives (host-authoritative)
bool objPersistent(u32 id)
{
	const u32 kind = id >> 29;
	return kind == PORT_MP_OBJ_SHINE || kind == PORT_MP_OBJ_BLUE;
}
u32 objVisitKey(u32 id) { return (id >> 13) & 0xFFFF; } // stage<<8 | scenario

bool hasId(const std::vector<u32>& v, u32 id)
{
	for (u32 x : v)
		if (x == id)
			return true;
	return false;
}

void sendObjList(Conn& c, u8 op, u8 slot, const std::vector<u32>& ids)
{
	for (size_t i = 0; i < ids.size(); i += 200) { // 200 ids = 806 bytes per message
		Writer w;
		w.u8_(op);
		w.u8_(slot);
		const size_t n = ids.size() - i < 200 ? ids.size() - i : 200;
		w.u16_((u16)n);
		for (size_t k = 0; k < n; k++)
			w.u32_(ids[i + k]);
		queueMsg(c, MSG_OBJECTIVE, w);
	}
}

// Drops the coin sets of stages nobody is in any more (a revisit respawns them, as on GC).
void objCleanupVisits()
{
	for (auto it = gObjVisit.begin(); it != gObjVisit.end();) {
		bool someone = false;
		for (auto& p : gPlayers)
			if (p.used && ((u32)p.stage << 8 | p.scenario) == it->first)
				someone = true;
		if (!someone)
			it = gObjVisit.erase(it);
		else
			++it;
	}
}

// A player collected `id` (host). The first claim wins; it is broadcast to every client.
void hostObjCollect(u32 id, int slot)
{
	std::vector<u32>& set = objPersistent(id) ? gObjPersistent : gObjVisit[objVisitKey(id)];
	if (hasId(set, id))
		return; // somebody was first
	set.push_back(id);
	std::vector<u32> one(1, id);
	for (auto& c : gConns)
		if (c.used)
			sendObjList(c, OBJ_COLLECTED, (u8)slot, one);
	if (slot != gLocalSlot && gObjEvents.size() < 4096)
		gObjEvents.push_back(ObjEvent{ id, (u8)slot });
	if (sVerbose)
		mplog("objective %08x collected by %d (%s)", id, slot, gPlayers[slot].name);
}

void hostDropClient(Conn& c, u8 reason)
{
	const int slot = c.slot;
	mplog("player %d (%s) left: %s", slot, gPlayers[slot].name,
	      reason == LEAVE_TIMEOUT ? "timed out" : reason == LEAVE_KICKED ? "kicked" : "quit");
	char t[96];
	std::snprintf(t, sizeof(t), "%s left", gPlayers[slot].name);
	port_mp_toast(t);
	c = Conn();
	gPlayers[slot] = Player();
	Writer w;
	w.u8_((u8)slot);
	w.u8_(reason);
	broadcastMsg(MSG_PLAYER_LEAVE, w, -1);
	objCleanupVisits();
	hostSendPeers();
}

// ---------------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------------
// App messages: [u8 channel][u8 to (0xFF = everyone)][u8 from][data]. Clients send to the
// host; the host delivers to itself and forwards (reliable or not, as sent).
void appQueue(u8 ch, u8 from, const u8* data, int len)
{
	std::deque<AppMsg>& q = gAppQ[ch];
	if (q.size() >= 256)
		q.pop_front();
	AppMsg m;
	m.from = from;
	m.data.assign(data, data + len);
	q.push_back(std::move(m));
}

void appWrite(Writer& w, u8 ch, u8 to, u8 from, const u8* data, int len)
{
	w.u8_(ch);
	w.u8_(to);
	w.u8_(from);
	if (len > 0)
		w.put(data, len);
}

// host: forward to the clients concerned (never back to `except`)
void appForward(u8 ch, u8 to, u8 from, const u8* data, int len, bool reliable, int except)
{
	for (auto& c : gConns) {
		if (!c.used || c.slot == except || (to != 0xFF && c.slot != to))
			continue;
		Writer w;
		appWrite(w, ch, to, from, data, len);
		if (!w.ok)
			return;
		if (reliable)
			queueMsg(c, MSG_APP, w);
		else
			sendConn(c, PKT_APP, w);
	}
}

void appReceive(Conn& c, Reader& r, bool reliable)
{
	const u8 ch = r.u8_(), to = r.u8_();
	u8 from     = r.u8_();
	if (!r.ok || ch == 0)
		return;
	const u8* data = r.p + r.i;
	const int len  = r.left();
	if (gIsHost) {
		from = (u8)c.slot; // a client speaks only for itself
		if (to == 0xFF || to == gLocalSlot)
			appQueue(ch, from, data, len);
		if (to != gLocalSlot)
			appForward(ch, to, from, data, len, reliable, c.slot);
	} else if (from < PORT_MP_MAX_PLAYERS && from != gLocalSlot) {
		appQueue(ch, from, data, len);
	}
}
void handleMsg(Conn& c, u8 kind, Reader& r)
{
	switch (kind) {
	case MSG_PLAYER_JOIN: { // host -> client
		if (gIsHost)
			return;
		int slot;
		if (readPlayer(r, &slot)) {
			mplog("player %d joined: %s", slot, gPlayers[slot].name);
			char t[96];
			std::snprintf(t, sizeof(t), "%s joined", gPlayers[slot].name);
			port_mp_toast(t);
		}
		break;
	}
	case MSG_PLAYER_LEAVE: {
		if (gIsHost)
			return;
		int slot = r.u8_();
		if (r.ok && slot < PORT_MP_MAX_PLAYERS && slot != gLocalSlot) {
			mplog("player %d left: %s", slot, gPlayers[slot].name);
			char t[96];
			std::snprintf(t, sizeof(t), "%s left", gPlayers[slot].name);
			port_mp_toast(t);
			gPlayers[slot] = Player();
		}
		break;
	}
	case MSG_PLAYER_INFO: {
		int slot = r.u8_();
		char name[PORT_MP_NAME_MAX];
		r.str(name, sizeof(name));
		PortMpCharacter ch;
		r.get(&ch, sizeof(ch));
		if (!r.ok || slot >= PORT_MP_MAX_PLAYERS)
			return;
		if (gIsHost) {
			if (slot != c.slot)
				return; // a client may only change itself
			sanitizeName(name, sizeof(name), "Player");
			uniqueName(name, slot);
		}
		Player& p = gPlayers[slot];
		if (!p.used)
			return;
		std::memcpy(p.name, name, sizeof(p.name));
		p.character = ch;
		if (gIsHost) {
			Writer w;
			w.u8_((u8)slot);
			w.str(p.name, PORT_MP_NAME_MAX);
			w.put(&p.character, sizeof(PortMpCharacter));
			broadcastMsg(MSG_PLAYER_INFO, w, -1); // back to the sender too (unique name)
		}
		break;
	}
	case MSG_SETTINGS: {
		if (gIsHost)
			return;
		PortMpSessionSettings s;
		r.get(&s, sizeof(s));
		if (r.ok) {
			gSettings = s;
			mplog("settings: mode %d, shared objectives %d", s.gameMode, s.sharedObjectives);
		}
		break;
	}
	case MSG_STAGE: {
		int slot = r.u8_();
		u8 st = r.u8_(), sc = r.u8_();
		if (!r.ok || slot >= PORT_MP_MAX_PLAYERS)
			return;
		if (gIsHost && slot != c.slot)
			return;
		Player& p = gPlayers[slot];
		if (!p.used)
			return;
		p.stage    = st;
		p.scenario = sc;
		if (sVerbose)
			mplog("player %d (%s) is in stage %d/%d", slot, p.name, st, sc);
		if (gIsHost) {
			Writer w;
			w.u8_((u8)slot);
			w.u8_(st);
			w.u8_(sc);
			broadcastMsg(MSG_STAGE, w, slot);
			// what is already collected in that stage (shared objectives)
			objCleanupVisits();
			auto it = gObjVisit.find((u32)st << 8 | sc);
			if (it != gObjVisit.end() && !it->second.empty())
				sendObjList(c, OBJ_SET, 0xFF, it->second);
		}
		break;
	}
	case MSG_APP:
		appReceive(c, r, true);
		break;
	case MSG_OBJECTIVE: {
		const u8 op   = r.u8_();
		const u8 from = r.u8_();
		const int n   = r.u16_();
		for (int i = 0; i < n && r.ok; i++) {
			const u32 id = r.u32_();
			if (!r.ok)
				break;
			if (gIsHost) {
				if (op == OBJ_COLLECT)
					hostObjCollect(id, c.slot); // the claim is the sender's, whatever it says
			} else if ((op == OBJ_COLLECTED && from != gLocalSlot) || op == OBJ_SET) {
				if (gObjEvents.size() < 4096)
					gObjEvents.push_back(ObjEvent{ id, op == OBJ_SET ? (u8)0xFF : from });
			}
		}
		break;
	}
	case MSG_PEERS: { // host -> clients (migration)
		if (gIsHost)
			return;
		for (auto& p : gPeers)
			p.known = false;
		const int n = r.u8_();
		for (int i = 0; i < n && r.ok; i++) {
			const int s = r.u8_();
			u32 ip;
			u16 port;
			r.get(&ip, 4);
			r.get(&port, 2);
			if (r.ok && s < PORT_MP_MAX_PLAYERS && ip != 0) {
				gPeers[s].known                 = true;
				gPeers[s].addr                  = sockaddr_in{};
				gPeers[s].addr.sin_family       = AF_INET;
				gPeers[s].addr.sin_addr.s_addr  = ip;
				gPeers[s].addr.sin_port         = port;
			}
		}
		break;
	}
	case MSG_LOBBYKEY: {
		if (gIsHost)
			return;
		const u32 id = r.u32_(), tok = r.u32_();
		if (r.ok) {
			gKeyLobbyId = id;
			gKeyToken   = tok;
		}
		break;
	}
	default:
		break; // unknown kinds are ignored (newer peers never get here: version check)
	}
}

void receiveReliable(Conn& c, Reader& r)
{
	readAcks(c, r);
	int count = r.u8_();
	for (int k = 0; k < count && r.ok; k++) {
		u16 id   = r.u16_();
		u8 kind  = r.u8_();
		u16 len  = r.u16_();
		if (!r.ok || len > r.left()) {
			c.dropped++;
			return;
		}
		const u8* data = r.p + r.i;
		r.i += len;
		c.ackDue = true;
		if (id == c.rxNext) {
			Reader mr(data, len);
			handleMsg(c, kind, mr);
			if (!c.used)
				return; // the message ended the connection
			c.rxNext++;
			// deliver buffered successors
			for (;;) {
				auto it = c.rxBuf.find(c.rxNext);
				if (it == c.rxBuf.end())
					break;
				std::vector<u8> d = std::move(it->second.second);
				u8 kd             = it->second.first;
				c.rxBuf.erase(it);
				Reader br(d.data(), (int)d.size());
				handleMsg(c, kd, br);
				if (!c.used)
					return;
				c.rxNext++;
			}
		} else if (!idLE(id, c.rxNext) && (int16_t)(id - c.rxNext) < 256) {
			if (c.rxBuf.size() < 256)
				c.rxBuf[id] = std::make_pair(kind, std::vector<u8>(data, data + len));
		} // else: duplicate of something delivered; the ack covers it
	}
}

// ---------------------------------------------------------------------------------
void hostHandleConnect(const sockaddr_in& from, u16 proto, Reader& r)
{
	if (r.u32_() != kJoinTag)
		return; // not a CONNECT from this game
	u32 nonce = r.u32_();
	char name[PORT_MP_NAME_MAX];
	r.str(name, sizeof(name));
	PortMpCharacter ch;
	r.get(&ch, sizeof(ch));
	const u64 pw = r.left() >= 8 ? ((u64)r.u32_() | ((u64)r.u32_() << 32)) : 0;
	u8 rejoinSlot = 0xFF;
	u32 rejoinSerial = 0;
	if (r.left() >= 5) {
		rejoinSlot   = r.u8_();
		rejoinSerial = r.u32_();
	}
	if (!r.ok)
		return;
	auto reject = [&](u8 why) {
		Writer w;
		w.u32_(nonce);
		w.u8_(why);
		w.u16_(PORT_MP_PROTOCOL_VERSION);
		sendRaw(from, PKT_REJECT, (u8)gLocalSlot, 0, 0, w.buf, w.n);
	};
	if (proto != PORT_MP_PROTOCOL_VERSION) {
		mplog("refused %s: protocol %u, ours %u", addrStr(from).c_str(), proto, PORT_MP_PROTOCOL_VERSION);
		reject(REJ_VERSION);
		return;
	}
	if (gPwHash != 0 && pw != gPwHash) {
		reject(REJ_BADPASS);
		return;
	}
	Conn* c = nullptr;
	for (auto& k : gConns)
		if (k.used && k.nonce == nonce && !k.established) { // the same attempt by another path
			k.addr = from;
			c      = &k;
		}
	if (c == nullptr)
		for (auto& k : gConns)
			if (k.used && sameAddr(k.addr, from))
				c = &k;
	if (c != nullptr && c->nonce != nonce) {
		// same address, new attempt (client restarted): drop the stale connection
		hostDropClient(*c, LEAVE_QUIT);
		c = nullptr;
	}
	bool fresh = false;
	// after a host migration: a player of this lobby coming back keeps its slot
	if (c == nullptr && rejoinSlot < PORT_MP_MAX_PLAYERS && gPlayers[rejoinSlot].used
	    && gPlayers[rejoinSlot].serial == rejoinSerial && !gConns[rejoinSlot].used && rejoinSlot != gLocalSlot) {
		c         = &gConns[rejoinSlot];
		*c        = Conn();
		c->used   = true;
		c->addr   = from;
		c->slot   = rejoinSlot;
		c->nonce  = nonce;
		c->connId = rnd32();
		c->lastRecv = c->lastPing = nowMs();
		gPlayers[rejoinSlot].rejoinBy = 0;
		mplog("player %d (%s) re-joined after the host change", rejoinSlot, gPlayers[rejoinSlot].name);
		hostSendPeers();
		hostSendLobbyKey(rejoinSlot);
	}
	if (c == nullptr) {
		int slot = -1;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS && i < gSettings.maxPlayers; i++)
			if (!gPlayers[i].used) {
				slot = i;
				break;
			}
		if (slot < 0) {
			reject(REJ_FULL);
			return;
		}
		c         = &gConns[slot];
		*c        = Conn();
		c->used   = true;
		c->addr   = from;
		c->slot   = slot;
		c->nonce  = nonce;
		c->connId = rnd32();
		c->lastRecv = c->lastPing = nowMs();
		Player& p = gPlayers[slot];
		p         = Player();
		p.used    = true;
		p.serial  = gSerialCounter++;
		sanitizeName(name, sizeof(name), "Player");
		uniqueName(name, slot);
		std::memcpy(p.name, name, sizeof(p.name));
		p.character = ch;
		fresh       = true;
	}
	c->lastRecv = nowMs();
	// ACCEPT (unreliable; repeated on every CONNECT copy)
	Writer w;
	w.u32_(nonce);
	w.u8_((u8)c->slot);
	w.u32_(c->connId);
	w.put(&gSettings, sizeof(gSettings));
	w.str(gLobbyName, PORT_MP_LOBBY_NAME_MAX);
	int count = 0;
	for (auto& p : gPlayers)
		count += p.used;
	w.u8_((u8)count);
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (gPlayers[i].used)
			writePlayer(w, i);
	sendRaw(from, PKT_ACCEPT, (u8)gLocalSlot, c->connId, c->txSeq++, w.buf, w.n);
	if (fresh) {
		mplog("player %d joined: %s from %s", c->slot, gPlayers[c->slot].name, addrStr(from).c_str());
		char t[96];
		std::snprintf(t, sizeof(t), "%s joined", gPlayers[c->slot].name);
		port_mp_toast(t);
		Writer j;
		writePlayer(j, c->slot);
		broadcastMsg(MSG_PLAYER_JOIN, j, c->slot);
		// shared objectives: shines and blue coins collected so far in this session
		sendObjList(*c, OBJ_SET, 0xFF, gObjPersistent);
		hostSendPeers();
		hostSendLobbyKey(c->slot);
	}
}

void clientHandleAccept(const sockaddr_in& from, Reader& r)
{
	if (gState != PORT_MP_CONNECTING)
		return;
	u32 nonce = r.u32_();
	int slot  = r.u8_();
	u32 connId = r.u32_();
	PortMpSessionSettings s;
	r.get(&s, sizeof(s));
	char lobby[PORT_MP_LOBBY_NAME_MAX];
	r.str(lobby, sizeof(lobby));
	int count = r.u8_();
	if (!r.ok || nonce != gConnectNonce || slot >= PORT_MP_MAX_PLAYERS || connId == 0)
		return;
	resetPlayers();
	for (int i = 0; i < count; i++) {
		int ps;
		if (!readPlayer(r, &ps))
			return;
	}
	if (!gPlayers[slot].used)
		return;
	Conn& c   = gConns[0];
	c         = Conn();
	c.used    = true;
	c.addr    = from;
	c.connId  = connId;
	c.slot    = 0; // index of the host in our table is found by isHost
	c.lastRecv = c.lastPing = nowMs();
	gSettings  = s;
	gLocalSlot = slot;
	std::snprintf(gLobbyName, sizeof(gLobbyName), "%s", lobby);
	std::memcpy(gLocalName, gPlayers[slot].name, sizeof(gLocalName)); // host may have renamed us
	gState = PORT_MP_CONNECTED;
	for (auto& p : gPeers)
		p.known = false;
	if (gMigrating) {
		gMigrating = false;
		port_mp_toast("Back in the lobby with the new host.");
	}
	setStatus("Joined \"%s\" as %s (slot %d, %d players).", gLobbyName, gLocalName, slot, playerCount());
}

// ---------------------------------------------------------------------------------
// Join by name: candidate endpoints of the host
bool isCandidate(const sockaddr_in& a)
{
	for (auto& c : gCandidates)
		if (sameAddr(c, a))
			return true;
	return false;
}

void addCandidate(const sockaddr_in& a, const char* what)
{
	if (isCandidate(a) || gCandidates.size() >= 8)
		return;
	gCandidates.push_back(a);
	if (gJoinPhase == 0) {
		gJoinPhase = 1;
		gFoundAt   = nowMs();
	}
	gLastConnect = -1e9; // send CONNECT at once
	mplog("join \"%s\": trying %s %s", gJoinName, what, addrStr(a).c_str());
}

void handlePacket(const u8* d, int len, const sockaddr_in& from);

// Our own LAN IPv4 endpoints for REGISTER (so peers behind the same NAT connect directly).
int localEndpoints(u32* ipsBE, int max);

void serverWriteRegister(Writer& w)
{
	w.u32_(gRegNonce);
	w.u16_(PORT_MP_PROTOCOL_VERSION);
	w.u8_(gSettings.maxPlayers);
	w.u8_((u8)playerCount());
	w.u8_(gSettings.gameMode);
	w.u8_(gPwHash != 0 ? 1 : 0);
	w.u32_((u32)gPwHash);
	w.u32_((u32)(gPwHash >> 32));
	w.str(gLobbyName, 32);
	w.str(gLocalName, 24);
	u32 ips[4];
	const int n = localEndpoints(ips, 4);
	w.u8_((u8)n);
	for (int i = 0; i < n; i++) {
		w.put(&ips[i], 4); // network order
		u16 portBE = htons((u16)gHostPort);
		w.put(&portBE, 2);
	}
}

void hostSendLobbyKey(int toSlot); // host migration (below)

// One datagram from the lobby server (address already checked).
void handleServerPacket(const u8* d, int len)
{
	if (len < 6 || d[4] != kSrvProto)
		return;
	const u8 type = d[5];
	Reader r(d + 6, len - 6);
	const double now = nowMs();
	auto readObserved = [&]() {
		if (r.left() >= 6) {
			u32 ip;
			u16 port;
			r.get(&ip, 4);
			r.get(&port, 2);
			std::snprintf(gPublicAddr, sizeof(gPublicAddr), "%s", ip4(ip, port).c_str());
		}
	};
	switch (type) {
	case S_STATUS_OK: {
		u32 nonce = r.u32_();
		u16 lob   = r.u16_();
		u8 relay  = r.u8_();
		r.u8_(); // server protocol
		if (!r.ok || nonce != gServerPingNonce)
			return;
		readObserved();
		gServerState   = PORT_MP_SERVER_ONLINE;
		gServerLobbies = lob;
		gServerRelay   = relay != 0;
		gServerPongAt  = now;
		break;
	}
	case S_LIST_OK: {
		u32 nonce = r.u32_();
		r.u16_(); // lobbies open in all
		const int cnt = r.u8_();
		if (!r.ok || nonce != gServerListNonce)
			return;
		std::vector<PortMpLobbyInfo> list;
		for (int i = 0; i < cnt && r.left() >= 60; i++) {
			PortMpLobbyInfo l{};
			r.str(l.name, PORT_MP_LOBBY_NAME_MAX);
			r.str(l.hostName, PORT_MP_NAME_MAX);
			l.players     = r.u8_();
			l.maxPlayers  = r.u8_();
			l.gameMode    = r.u8_();
			const u8 fl   = r.u8_();
			l.hasPassword = (fl & 1) != 0;
			l.protocolOk  = (fl & 2) != 0;
			l.connType    = PORT_MP_CONN_SERVER;
			l.seenMs      = now;
			list.push_back(l);
		}
		gServerList.swap(list);
		break;
	}
	case S_REGISTERED: {
		u32 nonce = r.u32_(), id = r.u32_(), token = r.u32_();
		if (!r.ok || nonce != gRegNonce || gState != PORT_MP_HOSTING)
			return;
		readObserved();
		const bool first = !gRegistered && gLobbyId == 0;
		gLobbyId       = id;
		gLobbyToken    = token;
		gRegistered    = true;
		gServerState   = PORT_MP_SERVER_ONLINE;
		gServerPongAt  = now;
		gHeartbeatAt   = now;
		if (first)
			setStatus("Lobby \"%s\" is open: players join by typing its name.", gLobbyName);
		hostSendLobbyKey(-1); // so any player can take the name over if we leave
		break;
	}
	case S_REJECT: {
		u8 reason = r.u8_(), req = r.u8_();
		u32 nonce = r.u32_();
		if (!r.ok)
			return;
		if (req == S_TAKEOVER && nonce == gRegNonce && gState == PORT_MP_HOSTING) {
			// the old entry is gone (the old host unregistered or it expired): register the name anew
			gLobbyId   = 0;
			gRegNonce  = rnd32();
			gRegSentAt = -1e9;
		} else if (req == S_REGISTER && nonce == gRegNonce && gState == PORT_MP_HOSTING && reason == SR_NAME_TAKEN
		           && now < gRegTolerateUntil) {
			// a new host while the old host's entry is still alive: retry until it expires
		} else if (req == S_REGISTER && nonce == gRegNonce && gState == PORT_MP_HOSTING && gConnType == PORT_MP_CONN_SERVER) {
			const char* why = reason == SR_NAME_TAKEN ? "is already taken"
			                  : reason == SR_BAD_NAME ? "cannot be used"
			                  : reason == SR_LIMIT    ? "was refused (server limit reached)"
			                  : reason == SR_VERSION  ? "was refused (the server runs another version)"
			                                          : "was refused";
			char msg[160];
			std::snprintf(msg, sizeof(msg), "The lobby name \"%s\" %s. Pick another name.", gLobbyName, why);
			endSession(nullptr);
			gState = PORT_MP_FAILED;
			setStatus("%s", msg);
		} else if (req == S_HEARTBEAT && gState == PORT_MP_HOSTING && nonce == gLobbyId) {
			gRegistered = false; // server restarted or expired us: register again
			gRegSentAt  = -1e9;
		} else if (req == S_LOOKUP && nonce == gLookupNonce && gState == PORT_MP_CONNECTING && gJoinPhase == 0) {
			if (reason == SR_BAD_PASSWORD)
				failClient("Wrong password for \"%s\".", gJoinName);
			else if (reason == SR_FULL)
				failClient("The lobby \"%s\" is full.", gJoinName);
			else if (reason == SR_VERSION)
				failClient("The lobby \"%s\" runs a different game version.", gJoinName);
			else if (reason == SR_NOT_FOUND)
				gFoundLobby = 0xFFFFFFFF; // keep looking on the LAN until the timeout
		} else if (req == S_RELAY_OPEN && nonce == gConnectNonce && gState == PORT_MP_CONNECTING) {
			if (gCandidates.empty() || gForceRelay)
				failClient("Could not reach the host, and the server cannot relay.");
		}
		break;
	}
	case S_FOUND: { // client
		u32 nonce = r.u32_(), lobby = r.u32_();
		u32 hip;
		u16 hport;
		r.get(&hip, 4);
		r.get(&hport, 2);
		int nloc = r.u8_();
		if (!r.ok || nonce != gLookupNonce || gState != PORT_MP_CONNECTING || !gJoinByName)
			return;
		gFoundLobby = lobby;
		gServerState = PORT_MP_SERVER_ONLINE;
		if (!gForceRelay) {
			for (int i = 0; i < nloc && i < 4; i++) {
				u32 lip;
				u16 lport;
				r.get(&lip, 4);
				r.get(&lport, 2);
				if (!r.ok)
					break;
				sockaddr_in a{};
				a.sin_family      = AF_INET;
				a.sin_addr.s_addr = lip;
				a.sin_port        = lport;
				addCandidate(a, "LAN endpoint");
			}
			sockaddr_in a{};
			a.sin_family      = AF_INET;
			a.sin_addr.s_addr = hip;
			a.sin_port        = hport;
			addCandidate(a, "public endpoint");
		} else if (gJoinPhase == 0) {
			gJoinPhase = 1;
			gFoundAt   = now - 1e6; // straight to the relay
		}
		break;
	}
	case S_PUNCH: { // host: a client is about to send to us; open our NAT towards it
		r.u32_();
		u32 cip;
		u16 cport;
		r.get(&cip, 4);
		r.get(&cport, 2);
		if (!r.ok || gState != PORT_MP_HOSTING)
			return;
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_addr.s_addr = cip;
		a.sin_port        = cport;
		for (int i = 0; i < 3; i++)
			sendRaw(a, PKT_PUNCH, (u8)gLocalSlot, 0, 0, nullptr, 0);
		if (sVerbose)
			mplog("punch towards a joining player");
		break;
	}
	case S_RELAY_OK: { // client
		u32 nonce = r.u32_(), rid = r.u32_();
		if (!r.ok || nonce != gConnectNonce || gState != PORT_MP_CONNECTING || gJoinPhase == 2)
			return;
		gHostAddr  = relayAddr(rid);
		gJoinPhase = 2;
		gCandidates.push_back(gHostAddr);
		gLastConnect = -1e9;
		mplog("join \"%s\": relaying through the lobby server", gJoinName);
		break;
	}
	case S_RELAY_DATA: {
		if (len < 10 + kHeaderSize)
			return;
		u32 rid;
		std::memcpy(&rid, d + 6, 4);
		handlePacket(d + 10, len - 10, relayAddr(rid));
		break;
	}
	default:
		break;
	}
}

// Validates the header and routes one datagram.
void handlePacket(const u8* d, int len, const sockaddr_in& from)
{
	if (len < kHeaderSize)
		return;
	u32 magic;
	u16 proto, seq, plen;
	u32 connId;
	std::memcpy(&magic, d, 4);
	std::memcpy(&proto, d + 4, 2);
	const u8 type = d[6];
	const u8 slot = d[7];
	std::memcpy(&connId, d + 8, 4);
	std::memcpy(&seq, d + 12, 2);
	std::memcpy(&plen, d + 14, 2);
	(void)seq;
	if (magic != kMagic || plen != len - kHeaderSize || type == 0 || type >= PKT_COUNT)
		return;
	Reader r(d + kHeaderSize, plen);
	const double now = nowMs();

	// Version-independent packets: discovery and the handshake answer with our version.
	if (type == PKT_DISCOVER) {
		if (!gIsHost || gState != PORT_MP_HOSTING)
			return;
		u32 token = r.u32_();
		if (!r.ok)
			return;
		Writer w;
		w.u32_(token);
		w.u16_(PORT_MP_PROTOCOL_VERSION);
		w.u16_((u16)gHostPort);
		w.u8_((u8)playerCount());
		w.u8_(gSettings.maxPlayers);
		w.u8_(gSettings.gameMode);
		w.str(gLobbyName, PORT_MP_LOBBY_NAME_MAX);
		w.str(gLocalName, PORT_MP_NAME_MAX);
		w.u8_(gPwHash != 0 ? 1 : 0);
		w.u8_((u8)gConnType);
		sendRaw(from, PKT_LOBBY_INFO, 0, 0, 0, w.buf, w.n);
		return;
	}
	if (type == PKT_LOBBY_INFO) {
		u32 token = r.u32_();
		u16 hp    = r.u16_();
		u16 port  = r.u16_();
		PortMpLobbyInfo li{};
		li.players    = r.u8_();
		li.maxPlayers = r.u8_();
		li.gameMode   = r.u8_();
		r.str(li.name, sizeof(li.name));
		r.str(li.hostName, sizeof(li.hostName));
		li.hasPassword = r.u8_();
		li.connType    = r.u8_();
		if (!r.ok || token != gDiscoverToken)
			return;
		sanitizeName(li.name, sizeof(li.name), "Lobby");
		sanitizeName(li.hostName, sizeof(li.hostName), "Host");
		inet_ntop(AF_INET, (void*)&from.sin_addr, li.address, sizeof(li.address));
		li.port       = port != 0 ? port : ntohs(from.sin_port);
		li.protocolOk = hp == PORT_MP_PROTOCOL_VERSION;
		li.pingMs     = (u16)(now - gDiscoverAt);
		li.seenMs     = now;
		// join by name: a LAN host with that name is a candidate (works without the server)
		if (gJoinByName && gState == PORT_MP_CONNECTING && li.protocolOk && nameEq(li.name, gJoinName)) {
			sockaddr_in a = from;
			a.sin_port     = htons(li.port);
			addCandidate(a, "LAN");
		}
		// one entry per host:port (it may answer on loopback and on the LAN address)
		for (auto& e : gLobbies)
			if (e.port == li.port && std::strcmp(e.name, li.name) == 0 && std::strcmp(e.hostName, li.hostName) == 0) {
				if (std::strcmp(li.address, "127.0.0.1") != 0 || std::strcmp(e.address, "127.0.0.1") == 0)
					e = li;
				return;
			}
		if (gLobbies.size() < 64)
			gLobbies.push_back(li);
		return;
	}
	if (type == PKT_CONNECT) {
		if (gIsHost && gState == PORT_MP_HOSTING)
			hostHandleConnect(from, proto, r);
		return;
	}
	if (type == PKT_REJECT) {
		if (gState != PORT_MP_CONNECTING)
			return;
		u32 nonce = r.u32_();
		u8 why    = r.u8_();
		u16 hp    = r.u16_();
		if (!r.ok || nonce != gConnectNonce)
			return;
		if (why == REJ_VERSION)
			failClient("The host runs a different version (protocol %u, ours %u).", hp, PORT_MP_PROTOCOL_VERSION);
		else if (why == REJ_FULL)
			failClient("The lobby is full.");
		else if (why == REJ_BADPASS)
			failClient("Wrong password for that lobby.");
		else
			failClient("The host refused the connection.");
		return;
	}
	if (proto != PORT_MP_PROTOCOL_VERSION)
		return;
	if (type == PKT_ACCEPT) {
		if (!gIsHost && gState == PORT_MP_CONNECTING
		    && (sameAddr(from, gHostAddr) || isCandidate(from) || from.sin_port == gHostAddr.sin_port))
			clientHandleAccept(from, r); // nonce-checked; any path the host answered on
		return;
	}

	// Everything else belongs to an established connection.
	Conn* c = nullptr;
	if (gIsHost) {
		if (slot < PORT_MP_MAX_PLAYERS && gConns[slot].used && gConns[slot].connId == connId) {
			if (!sameAddr(gConns[slot].addr, from)
			    && (!gConns[slot].established || now - gConns[slot].lastRecv > 2000.0)) {
				// the client settled on this path (several were tried), or its address changed after a
				// drop (NAT rebinding, new Wi-Fi): the random connection id identifies it
				if (gConns[slot].established)
					mplog("player %d (%s) is back from a new address", slot, gPlayers[slot].name);
				gConns[slot].addr = from;
			}
			if (sameAddr(gConns[slot].addr, from))
				c = &gConns[slot];
		}
	} else if (gState == PORT_MP_CONNECTED && gConns[0].used && gConns[0].connId == connId
	           && sameAddr(gConns[0].addr, from)) {
		c = &gConns[0];
	}
	if (c == nullptr)
		return;
	if (c->lagging) { // back after a short drop
		c->lagging = false;
		char t[96];
		if (gIsHost)
			std::snprintf(t, sizeof(t), "%s is back", gPlayers[c->slot].name);
		else
			std::snprintf(t, sizeof(t), "Reconnected to the lobby.");
		mplog("%s", t);
		port_mp_toast(t);
	}
	c->lastRecv    = now;
	c->established = true;

	switch (type) {
	case PKT_RELIABLE:
		receiveReliable(*c, r);
		break;
	case PKT_ACK:
		readAcks(*c, r);
		break;
	case PKT_SNAPSHOT: {
		int s = r.u8_();
		PortMpSnapshot snap;
		r.get(&snap, sizeof(snap));
		if (!r.ok || s >= PORT_MP_MAX_PLAYERS || s == gLocalSlot)
			return;
		if (gIsHost && s != c->slot)
			return; // clients only speak for themselves
		c->snapRx++;
		storeSnapshot(s, snap);
		if (gIsHost) {
			// relay: full rate to players in the same stage, 2 Hz to the rest
			for (auto& o : gConns) {
				if (!o.used || o.slot == s)
					continue;
				const Player& dst = gPlayers[o.slot];
				bool same = dst.stage == snap.stage && dst.scenario == snap.scenario;
				if (!same && now - o.relayAt[s] < 500.0)
					continue;
				o.relayAt[s] = now;
				Writer w;
				w.u8_((u8)s);
				w.put(&snap, sizeof(snap));
				sendConn(o, PKT_SNAPSHOT, w);
				o.snapTx++;
			}
		}
		break;
	}
	case PKT_PING: {
		double t = r.f64_();
		if (!r.ok)
			return;
		Writer w;
		w.f64_(t);
		sendConn(*c, PKT_PONG, w);
		break;
	}
	case PKT_PONG: {
		double t = r.f64_();
		if (!r.ok || t > now || now - t > 30000.0)
			return;
		const double sample = now - t;
		c->rttMs            = c->rttMs * 0.75 + sample * 0.25;
		if (gIsHost)
			gPlayers[c->slot].pingMs = (u16)(c->rttMs + 0.5);
		else if (gLocalSlot >= 0)
			gPlayers[gLocalSlot].pingMs = (u16)(c->rttMs + 0.5);
		break;
	}
	case PKT_APP:
		appReceive(*c, r, false);
		break;
	case PKT_ROSTER: { // host -> clients: pings and stages
		if (gIsHost)
			return;
		int n = r.u8_();
		for (int i = 0; i < n && r.ok; i++) {
			int s  = r.u8_();
			u16 pg = r.u16_();
			u8 st = r.u8_(), sc = r.u8_();
			if (r.ok && s < PORT_MP_MAX_PLAYERS && gPlayers[s].used) {
				if (s != gLocalSlot)
					gPlayers[s].pingMs = pg;
				if (s != gLocalSlot) {
					gPlayers[s].stage    = st;
					gPlayers[s].scenario = sc;
				}
			}
		}
		break;
	}
	case PKT_DISCONNECT: {
		u8 why = r.u8_();
		if (gIsHost) {
			hostDropClient(*c, LEAVE_QUIT);
		} else if (why == LEAVE_KICKED) {
			failClient("You were removed from the lobby.");
		} else {
			startMigration("The host left.");
		}
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------------------------
void sendLocalStage()
{
	if (gLocalSlot < 0)
		return;
	gPlayers[gLocalSlot].stage    = gLocalStage;
	gPlayers[gLocalSlot].scenario = gLocalScenario;
	Writer w;
	w.u8_((u8)gLocalSlot);
	w.u8_(gLocalStage);
	w.u8_(gLocalScenario);
	if (gIsHost) {
		broadcastMsg(MSG_STAGE, w, -1);
		// the host enters a stage: what the guests already collected there
		objCleanupVisits();
		auto it = gObjVisit.find((u32)gLocalStage << 8 | gLocalScenario);
		if (it != gObjVisit.end())
			for (u32 id : it->second)
				gObjEvents.push_back(ObjEvent{ id, 0xFF });
	} else if (gConns[0].used) {
		queueMsg(gConns[0], MSG_STAGE, w);
	}
}

void sendLocalInfo()
{
	if (gLocalSlot < 0)
		return;
	Writer w;
	w.u8_((u8)gLocalSlot);
	w.str(gLocalName, PORT_MP_NAME_MAX);
	stampGame(gLocalChar);
	w.put(&gLocalChar, sizeof(PortMpCharacter));
	if (gIsHost) {
		std::memcpy(gPlayers[gLocalSlot].name, gLocalName, sizeof(gLocalName));
		gPlayers[gLocalSlot].character = gLocalChar;
		broadcastMsg(MSG_PLAYER_INFO, w, -1);
	} else if (gConns[0].used) {
		queueMsg(gConns[0], MSG_PLAYER_INFO, w);
	}
}

// ---------------------------------------------------------------------------------
// Lobby server housekeeping (network thread, locked)
std::atomic<bool> gResolving{ false };
double gServerPingWanted = -1e9;

bool serverUnset()
{
	return gServerSpec[0] == '\0' || std::strstr(gServerSpec, "example.invalid") != nullptr;
}

void startResolve()
{
	gServerResolveAt = nowMs();
	if (serverUnset()) {
		gServerState    = PORT_MP_SERVER_UNSET;
		gServerResolved = false;
		return;
	}
	if (gResolving.exchange(true))
		return;
	if (gServerState != PORT_MP_SERVER_ONLINE)
		gServerState = PORT_MP_SERVER_CHECKING;
	std::string spec = gServerSpec;
	std::thread([spec]() {
		std::string host = spec;
		int port         = PORT_MP_DEFAULT_SERVER_PORT;
		size_t colon     = host.rfind(':');
		if (colon != std::string::npos) {
			port = std::atoi(host.c_str() + colon + 1);
			host.resize(colon);
		}
		sockaddr_in a{};
		addrinfo hints{}, *res = nullptr;
		hints.ai_family   = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		char ps[16];
		std::snprintf(ps, sizeof(ps), "%d", port > 0 && port < 65536 ? port : PORT_MP_DEFAULT_SERVER_PORT);
		const bool ok = getaddrinfo(host.c_str(), ps, &hints, &res) == 0 && res != nullptr;
		if (ok) {
			std::memcpy(&a, res->ai_addr, sizeof(a));
			freeaddrinfo(res);
		}
		std::lock_guard<std::mutex> lk(gMu);
		if (spec == gServerSpec) {
			gServerResolved = ok;
			gServerAddr     = a;
			if (!ok)
				gServerState = PORT_MP_SERVER_OFFLINE;
			gServerPingAt = -1e9; // ping at once
		}
		gResolving = false;
	}).detach();
}

void lanDiscover()
{
	Writer w;
	w.u32_(gDiscoverToken);
	for (int i = 0; i < kDiscoveryPorts; i++) {
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_port        = htons((u_short)(PORT_MP_DEFAULT_PORT + i));
		a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		sendRaw(a, PKT_DISCOVER, 0xFF, 0, 0, w.buf, w.n);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sendRaw(a, PKT_DISCOVER, 0xFF, 0, 0, w.buf, w.n);
	}
	if (gDefaultPort < PORT_MP_DEFAULT_PORT || gDefaultPort >= PORT_MP_DEFAULT_PORT + kDiscoveryPorts) {
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_port        = htons((u_short)gDefaultPort);
		a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		sendRaw(a, PKT_DISCOVER, 0xFF, 0, 0, w.buf, w.n);
	}
}

void serverTick(double now)
{
	const bool session = gState == PORT_MP_HOSTING || gState == PORT_MP_CONNECTING || gState == PORT_MP_CONNECTED;
	const bool wanted  = now - gServerPingWanted < 10000.0 || (session && gConnType == PORT_MP_CONN_SERVER);
	if (!wanted || gSock == INVALID_SOCKET)
		return;
	if (!gServerResolved) {
		if (now - gServerResolveAt > 10000.0)
			startResolve();
		return;
	}
	// status ping every 4 s while wanted; offline after 3 s without an answer
	if (now - gServerPingAt > 4000.0) {
		if (gServerPingAt > 0 && gServerPongAt < gServerPingAt && gServerState != PORT_MP_SERVER_UNSET)
			gServerState = PORT_MP_SERVER_OFFLINE;
		gServerPingAt    = now;
		gServerPingNonce = rnd32();
		Writer w;
		w.u32_(gServerPingNonce);
		srvSend(S_STATUS, w);
		if (gState == PORT_MP_OFF || gState == PORT_MP_FAILED) { // the Join page's lobby browser
			gServerListNonce = rnd32();
			Writer l;
			l.u32_(gServerListNonce);
			l.u16_(PORT_MP_PROTOCOL_VERSION);
			srvSend(S_LIST, l);
		}
	} else if (gServerPongAt < gServerPingAt && now - gServerPingAt > 3000.0) {
		gServerState = PORT_MP_SERVER_OFFLINE;
	}
	// host: keep the lobby registered
	if (gState == PORT_MP_HOSTING && gConnType == PORT_MP_CONN_SERVER) {
		if (!gRegistered && now - gRegSentAt > 1000.0) {
			gRegSentAt = now;
			Writer w;
			serverWriteRegister(w);
			srvSend(S_REGISTER, w);
			static double sWarned = -1e9;
			if (now - gRegStart > 5000.0 && now - sWarned > 30000.0) {
				sWarned = now;
				setStatus("Lobby server not reachable: \"%s\" can be joined on this network only (retrying).",
				          gLobbyName);
			}
		}
		if (gRegistered && now - gHeartbeatAt > 2000.0) {
			gHeartbeatAt = now;
			Writer w;
			w.u32_(gLobbyId);
			w.u32_(gLobbyToken);
			w.u8_((u8)playerCount());
			srvSend(S_HEARTBEAT, w);
		}
	}
}

// This machine's IPv4 addresses (GetAdaptersAddresses), for REGISTER and "join me at".
int collectLocal(PortMpLocalAddr* out, int max)
{
	ULONG size = 16 * 1024;
	std::vector<u8> buf(size);
	IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG rc    = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
	if (rc == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		aa = (IP_ADAPTER_ADDRESSES*)buf.data();
		rc = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
	}
	if (rc != NO_ERROR)
		return 0;
	int n = 0;
	for (IP_ADAPTER_ADDRESSES* a = aa; a != nullptr && n < max; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		char name[64] = {};
		WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name) - 1, nullptr, nullptr);
		char desc[128] = {};
		WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof(desc) - 1, nullptr, nullptr);
		auto has = [&](const char* s) { return std::strstr(name, s) != nullptr || std::strstr(desc, s) != nullptr; };
		// host-only virtual switches are no use to other players
		if (has("VirtualBox") || has("VMware") || has("vEthernet") || has("Hyper-V") || has("WSL"))
			continue;
		for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr && n < max; u = u->Next) {
			if (u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			const sockaddr_in* sin = (const sockaddr_in*)u->Address.lpSockaddr;
			const u32 ip          = ntohl(sin->sin_addr.s_addr);
			if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) // loopback, link-local
				continue;
			const bool vpn = has("Tailscale") || has("ZeroTier") || has("Radmin") || has("Hamachi")
			                 || (ip & 0xFFC00000u) == 0x64400000u /* 100.64/10 */ || (ip >> 24) == 25 || (ip >> 24) == 26;
			PortMpLocalAddr& o = out[n++];
			inet_ntop(AF_INET, (void*)&sin->sin_addr, o.ip, sizeof(o.ip));
			std::snprintf(o.adapter, sizeof(o.adapter), "%s", name);
			o.kind = vpn ? 1 : 0;
		}
	}
	return n;
}

int localEndpoints(u32* ipsBE, int max)
{
	PortMpLocalAddr a[8];
	const int n = collectLocal(a, 8);
	int k       = 0;
	for (int i = 0; i < n && k < max; i++) {
		in_addr ia;
		if (inet_pton(AF_INET, a[i].ip, &ia) == 1)
			ipsBE[k++] = ia.s_addr;
	}
	return k;
}
// ---------------------------------------------------------------------------------
// Host migration: when the host leaves or vanishes, the lowest remaining slot becomes the
// host on its own socket (taking the lobby name over on the lobby server), and everyone
// else re-joins it (by name through the server, or at the endpoint the old host shared),
// keeping their slot. Each game keeps running meanwhile. If it cannot work (nobody else, the
// new host unreachable), the session ends cleanly as before.
void hostSendPeers()
{
	Writer w;
	int n = 0;
	for (auto& c : gConns)
		n += c.used ? 1 : 0;
	w.u8_((u8)n);
	for (auto& c : gConns) {
		if (!c.used)
			continue;
		w.u8_((u8)c.slot);
		u32 ip = isRelayAddr(c.addr) ? 0 : c.addr.sin_addr.s_addr; // relayed: unknown (re-join by name)
		u16 port = c.addr.sin_port;
		w.put(&ip, 4);
		w.put(&port, 2);
	}
	broadcastMsg(MSG_PEERS, w, -1);
}

void hostSendLobbyKey(int toSlot)
{
	if (!gRegistered || gLobbyId == 0)
		return;
	Writer w;
	w.u32_(gLobbyId);
	w.u32_(gLobbyToken);
	if (toSlot >= 0 && gConns[toSlot].used)
		queueMsg(gConns[toSlot], MSG_LOBBYKEY, w);
	else
		broadcastMsg(MSG_LOBBYKEY, w, -1);
}

void becomeHost()
{
	sockaddr_in local{};
	int len = sizeof(local);
	getsockname(gSock, (sockaddr*)&local, &len);
	for (auto& c : gConns)
		c = Conn();
	gIsHost    = true;
	gMigrating = false;
	gState     = PORT_MP_HOSTING;
	gHostPort  = ntohs(local.sin_port);
	const double now = nowMs();
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		Player& p = gPlayers[i];
		if (!p.used)
			continue;
		p.isHost   = i == gLocalSlot;
		p.rejoinBy = i == gLocalSlot ? 0.0 : now + 20000.0;
		p.pingMs   = 0;
		p.histCount = 0;
	}
	// the old host's shared-objective / event lists are gone; ours start empty
	gObjPersistent.clear();
	gObjVisit.clear();
	gJoinByName = false;
	gCandidates.clear();
	gRegistered = false;
	gRegNonce   = rnd32();
	gRegSentAt  = -1e9;
	gRegStart   = now;
	if (gConnType == PORT_MP_CONN_SERVER) {
		if (gKeyLobbyId != 0) { // take the name over at once
			gLobbyId    = gKeyLobbyId;
			gLobbyToken = gKeyToken;
			Writer w;
			w.u32_(gRegNonce);
			w.u32_(gLobbyId);
			w.u32_(gLobbyToken);
			u32 ips[4];
			const int n = localEndpoints(ips, 4);
			w.u8_((u8)n);
			for (int i = 0; i < n; i++) {
				w.put(&ips[i], 4);
				u16 portBE = htons((u16)gHostPort);
				w.put(&portBE, 2);
			}
			srvSend(S_TAKEOVER, w);
			gRegSentAt = now; // REGISTER only if the takeover is refused
		}
		gRegTolerateUntil = now + 20000.0;
	}
	setStatus("You are the new host of \"%s\" (UDP port %d).", gLobbyName, gHostPort);
	port_mp_toast("You are the new host.");
}

void beginRejoin(int newHost)
{
	gConns[0]       = Conn();
	gIsHost         = false;
	gMigrating      = true;
	gState          = PORT_MP_CONNECTING;
	gRejoinSlot     = (u8)gLocalSlot;
	gRejoinSerial   = gPlayers[gLocalSlot].serial;
	gConnectNonce   = rnd32();
	gConnectStart   = nowMs();
	gLastConnect    = -1e9;
	gCandidates.clear();
	gRelayIds.clear();
	if (gConnType == PORT_MP_CONN_SERVER) {
		gJoinByName    = true;
		gJoinPhase     = 0;
		std::snprintf(gJoinName, sizeof(gJoinName), "%s", gLobbyName);
		gLookupNonce   = rnd32();
		gDiscoverToken = rnd32();
		gDiscoverAt    = nowMs();
		gLookupAt      = -1e9;
		gRelayAskAt    = -1e9;
		gFoundLobby    = 0;
		if (gPeers[newHost].known)
			addCandidate(gPeers[newHost].addr, "new host's endpoint");
	} else {
		if (!gPeers[newHost].known) {
			failClient("The host left, and the new host cannot be reached. The lobby has ended.");
			return;
		}
		gJoinByName = false;
		gHostAddr   = gPeers[newHost].addr;
	}
	setStatus("The host left: re-joining %s, the new host ...", gPlayers[newHost].name);
}

// Client: the host is gone (left or timed out).
void startMigration(const char* why)
{
	int oldHost = -1;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (gPlayers[i].used && gPlayers[i].isHost)
			oldHost = i;
	int newHost = -1, others = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (gPlayers[i].used && i != oldHost) {
			if (newHost < 0)
				newHost = i;
			if (i != gLocalSlot)
				others++;
		}
	if (newHost < 0 || others == 0 || gLocalSlot < 0) {
		failClient("%s The lobby has ended.", why);
		return;
	}
	char t[128];
	std::snprintf(t, sizeof(t), "%s %s is the new host.", why, gPlayers[newHost].name);
	mplog("%s", t);
	port_mp_toast(t);
	if (oldHost >= 0)
		gPlayers[oldHost] = Player();
	if (newHost == gLocalSlot)
		becomeHost();
	else
		beginRejoin(newHost);
}
void tick(double now)
{
	// LAN lobby list: forget hosts not heard from for 6 s (refresh re-asks)
	for (size_t i = 0; i < gLobbies.size();)
		if (now - gLobbies[i].seenMs > 6000.0)
			gLobbies.erase(gLobbies.begin() + i);
		else
			i++;

	serverTick(now);

	if (gState == PORT_MP_CONNECTING) {
		if (gJoinByName) {
			if (now - gConnectStart > (gMigrating ? 25000.0 : kJoinNameMs)) {
				if (gJoinPhase == 0 && (gFoundLobby == 0xFFFFFFFF || gServerState == PORT_MP_SERVER_ONLINE))
					failClient("No lobby called \"%s\" (on the server or this network).", gJoinName);
				else if (gJoinPhase == 0)
					failClient("No lobby called \"%s\" on this network, and the lobby server is not reachable.",
					           gJoinName);
				else
					failClient("Found \"%s\" but could not reach the host.", gJoinName);
				return;
			}
			if (gJoinPhase == 0 && now - gLookupAt > 500.0) {
				gLookupAt = now;
				Writer w; // LOOKUP: nonce, game protocol, password hash, name
				w.u32_(gLookupNonce);
				w.u16_(PORT_MP_PROTOCOL_VERSION);
				w.u32_((u32)gPwHash);
				w.u32_((u32)(gPwHash >> 32));
				w.str(gJoinName, 32);
				srvSend(S_LOOKUP, w);
				lanDiscover(); // the same name on this network
			}
			// punching did not connect in time (or forced): ask the server to relay
			if (gJoinPhase == 1 && gFoundLobby != 0 && gFoundLobby != 0xFFFFFFFF
			    && (gForceRelay || now - gFoundAt > kPunchMs) && now - gRelayAskAt > 500.0) {
				gRelayAskAt = now;
				Writer w;
				w.u32_(gConnectNonce);
				w.u32_(gFoundLobby);
				srvSend(S_RELAY_OPEN, w);
			}
			if (gJoinPhase == 0)
				return;
		} else if (now - gConnectStart > (gMigrating ? 20000.0 : kConnectMs)) {
			failClient("No answer from %s. Check the address, the port and the firewall.", addrStr(gHostAddr).c_str());
			return;
		}
		if (now - gLastConnect > 250.0) {
			gLastConnect = now;
			Writer w;
			w.u32_(kJoinTag);
			w.u32_(gConnectNonce);
			w.str(gLocalName, PORT_MP_NAME_MAX);
			stampGame(gLocalChar);
			w.put(&gLocalChar, sizeof(PortMpCharacter));
			w.u32_((u32)gPwHash);
			w.u32_((u32)(gPwHash >> 32));
			w.u8_(gMigrating ? gRejoinSlot : (u8)0xFF); // migration: the slot we had
			w.u32_(gMigrating ? gRejoinSerial : 0);
			if (gJoinByName) {
				if (gJoinPhase == 2)
					sendRaw(gHostAddr, PKT_CONNECT, 0xFF, 0, 0, w.buf, w.n);
				else if (!gForceRelay)
					for (auto& a : gCandidates)
						sendRaw(a, PKT_CONNECT, 0xFF, 0, 0, w.buf, w.n);
			} else {
				sendRaw(gHostAddr, PKT_CONNECT, 0xFF, 0, 0, w.buf, w.n);
			}
		}
		return;
	}
	if (gState != PORT_MP_HOSTING && gState != PORT_MP_CONNECTED)
		return;

	// timeouts, pings
	for (auto& c : gConns) {
		if (!c.used)
			continue;
		if (!c.lagging && now - c.lastRecv > kLagMs) {
			c.lagging = true;
			char t[112];
			if (gIsHost)
				std::snprintf(t, sizeof(t), "%s: connection problem (waiting up to %d s)", gPlayers[c.slot].name,
				              (int)(kTimeoutMs / 1000.0));
			else
				std::snprintf(t, sizeof(t), "Connection problem: reconnecting... (up to %d s)", (int)(kTimeoutMs / 1000.0));
			mplog("%s", t);
			port_mp_toast(t);
		}
		if (now - c.lastRecv > kTimeoutMs || c.outq.size() > 1024) {
			if (gIsHost) {
				hostDropClient(c, LEAVE_TIMEOUT);
				continue;
			}
			startMigration("Lost the connection to the host.");
			return;
		}
		if (now - c.lastPing > 1000.0) {
			c.lastPing = now;
			Writer w;
			w.f64_(now);
			sendConn(c, PKT_PING, w);
		}
	}

	// local snapshot: 30 Hz when it changed, 1 Hz repeat otherwise
	if (gHaveLocalSnap && gLocalSlot >= 0) {
		const bool changed = gLocalSnap.tick != gSentTick;
		if ((changed && now - gLastSnapSend >= 30.0) || now - gLastSnapSend >= 1000.0) {
			gLastSnapSend = now;
			gSentTick     = gLocalSnap.tick;
			Writer w;
			w.u8_((u8)gLocalSlot);
			w.put(&gLocalSnap, sizeof(gLocalSnap));
			for (auto& c : gConns) {
				if (!c.used)
					continue;
				if (gIsHost) {
					const Player& dst = gPlayers[c.slot];
					bool same = dst.stage == gLocalSnap.stage && dst.scenario == gLocalSnap.scenario;
					if (!same && now - c.relayAt[gLocalSlot] < 500.0)
						continue;
					c.relayAt[gLocalSlot] = now;
				}
				sendConn(c, PKT_SNAPSHOT, w);
				c.snapTx++;
			}
		}
	}

	// new host after a migration: players who did not come back are gone
	if (gIsHost)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			Player& p = gPlayers[i];
			if (p.used && p.rejoinBy > 0 && now > p.rejoinBy && !gConns[i].used) {
				mplog("player %d (%s) did not come back after the host change", i, p.name);
				char t2[96];
				std::snprintf(t2, sizeof(t2), "%s left", p.name);
				port_mp_toast(t2);
				p = Player();
				Writer w;
				w.u8_((u8)i);
				w.u8_(LEAVE_TIMEOUT);
				broadcastMsg(MSG_PLAYER_LEAVE, w, -1);
			}
		}
	// host: roster (pings, stages) every second
	if (gIsHost && now - gLastRoster > 1000.0) {
		gLastRoster = now;
		Writer w;
		w.u8_((u8)playerCount());
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			if (gPlayers[i].used) {
				w.u8_((u8)i);
				w.u16_(gPlayers[i].pingMs);
				w.u8_(gPlayers[i].stage);
				w.u8_(gPlayers[i].scenario);
			}
		for (auto& c : gConns)
			if (c.used)
				sendConn(c, PKT_ROSTER, w);
	}

	for (auto& c : gConns)
		flushReliable(c, now);

	if (sVerbose && now - gLastStats > 5000.0) {
		gLastStats = now;
		for (auto& c : gConns)
			if (c.used)
				mplog("stats %s slot %d: rtt %.0f ms, snapshots rx %u tx %u, resent %u, queued %u, dropped %u",
				      addrStr(c.addr).c_str(), gIsHost ? c.slot : -1, c.rttMs, c.snapRx, c.snapTx, c.resent,
				      (unsigned)c.outq.size(), c.dropped);
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			const Player& p = gPlayers[i];
			if (p.used && i != gLocalSlot && p.histCount > 0)
				mplog("remote %d (%s): tick %u stage %d/%d pos %.0f %.0f %.0f act %08x, %.0f ms old", i, p.name,
				      p.hist[0].snap.tick, p.hist[0].snap.stage, p.hist[0].snap.scenario, p.hist[0].snap.pos[0],
				      p.hist[0].snap.pos[1], p.hist[0].snap.pos[2], p.hist[0].snap.action, now - p.hist[0].rxMs);
		}
	}
}

void threadMain()
{
	u8 buf[2048];
	while (gThreadRunning.load()) {
		SOCKET s;
		int gen;
		{
			std::lock_guard<std::mutex> lk(gMu);
			s   = gSock;
			gen = gSockGen;
		}
		if (s == INVALID_SOCKET) {
			Sleep(10);
			std::lock_guard<std::mutex> lk(gMu);
			tick(nowMs());
			continue;
		}
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		timeval tv{ 0, 4000 };
		select(0, &rd, nullptr, nullptr, &tv);
		std::lock_guard<std::mutex> lk(gMu);
		if (gen != gSockGen || gSock == INVALID_SOCKET)
			continue;
		for (int i = 0; i < 256; i++) {
			sockaddr_in from{};
			int fl = sizeof(from);
			int n  = recvfrom(gSock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
			if (n <= 0)
				break;
			if (dropNow())
				continue;
			u32 magic = 0;
			if (n >= 6)
				std::memcpy(&magic, buf, 4);
			if (magic == kSrvMagic) {
				if (gServerResolved && sameAddr(from, gServerAddr))
					handleServerPacket(buf, n);
				continue;
			}
			handlePacket(buf, n, from);
			if (gen != gSockGen || gSock == INVALID_SOCKET)
				break;
		}
		tick(nowMs());
	}
}

// Locked init: settings, thread.
void ensureInit()
{
	if (gInit)
		return;
	gInit    = true;
	sVerbose = std::getenv("SMS_PORT_MP_LOG") != nullptr;
	int port = PORT_MP_DEFAULT_PORT, enabled = 1;
	port_mp_ini_load(gLocalName, sizeof(gLocalName), &port, &enabled);
	if (const char* n = std::getenv("SMS_PORT_MP_NAME"))
		std::snprintf(gLocalName, sizeof(gLocalName), "%s", n);
	sanitizeName(gLocalName, sizeof(gLocalName), "Player");
	gDefaultPort        = port > 0 && port < 65536 ? port : PORT_MP_DEFAULT_PORT;
	gEnabled            = enabled;
	gSettings.gameMode  = PORT_MP_MODE_NONE;
	gSettings.maxPlayers = 8;
	gSettings.startStage = 1;
	gSettings.startScenario = 2;
	port_mp_ini_server(gServerSpec, sizeof(gServerSpec));
	if (const char* s = std::getenv("SMS_PORT_MP_SERVER"))
		std::snprintf(gServerSpec, sizeof(gServerSpec), "%s", s);
	gForceRelay = std::getenv("SMS_PORT_MP_FORCE_RELAY") != nullptr;
}

// The network thread starts with the first host/join/LAN refresh (never in single player).
void ensureThread()
{
	ensureInit();
	if (gThreadRunning.load())
		return;
	gThreadRunning = true;
	gThread        = std::thread(threadMain);
	gThread.detach();
}

bool resolve(const char* host, int port, sockaddr_in* out)
{
	addrinfo hints{}, *res = nullptr;
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	char ps[16];
	std::snprintf(ps, sizeof(ps), "%d", port);
	if (getaddrinfo(host, ps, &hints, &res) != 0 || res == nullptr)
		return false;
	std::memcpy(out, res->ai_addr, sizeof(sockaddr_in));
	freeaddrinfo(res);
	return true;
}

} // namespace

// =================================================================================
extern "C" {

int port_mp_enabled(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	return gEnabled;
}

int port_mp_state(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gState;
}

int port_mp_active(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gState == PORT_MP_HOSTING || gState == PORT_MP_CONNECTED || gMigrating; // migrating: still in it
}

int port_mp_is_host(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gIsHost && gState == PORT_MP_HOSTING;
}

int port_mp_local_slot(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gLocalSlot;
}

const char* port_mp_status_text(void)
{
	static char sCopy[256];
	std::lock_guard<std::mutex> lk(gMu);
	std::memcpy(sCopy, gStatus, sizeof(sCopy));
	return sCopy;
}

double port_mp_time_ms(void) { return nowMs(); }

void port_mp_get_settings(PortMpSessionSettings* out)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	*out = gSettings;
}

int port_mp_get_player(int slot, PortMpPlayerInfo* out)
{
	std::memset(out, 0, sizeof(*out));
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return 0;
	std::lock_guard<std::mutex> lk(gMu);
	const Player& p = gPlayers[slot];
	if (!p.used)
		return 0;
	out->used        = 1;
	out->isLocal     = slot == gLocalSlot;
	out->isHost      = p.isHost;
	out->hasSnapshot = p.histCount > 0;
	out->serial      = p.serial;
	std::memcpy(out->name, p.name, sizeof(out->name));
	out->stage     = p.stage;
	out->scenario  = p.scenario;
	out->pingMs    = p.pingMs;
	if (gIsHost && slot != gLocalSlot)
		out->lagging = gConns[slot].used && gConns[slot].lagging;
	else if (!gIsHost && p.isHost)
		out->lagging = gConns[0].used && gConns[0].lagging;
	out->character = p.character;
	return 1;
}

int port_mp_get_snapshots(int slot, PortMpSnapshotRx* out, int max)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS || max <= 0)
		return 0;
	std::lock_guard<std::mutex> lk(gMu);
	const Player& p = gPlayers[slot];
	if (!p.used)
		return 0;
	int n = p.histCount < max ? p.histCount : max;
	std::memcpy(out, p.hist, sizeof(PortMpSnapshotRx) * n);
	return n;
}

int port_mp_same_stage(int slot)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return 0;
	std::lock_guard<std::mutex> lk(gMu);
	const Player& p = gPlayers[slot];
	if (!p.used || slot == gLocalSlot || gLocalStage == 0xFF)
		return 0;
	if (p.histCount > 0 && !(p.hist[0].snap.flags & PORT_MP_SNAP_VALID))
		return 0;
	return p.stage == gLocalStage && p.scenario == gLocalScenario;
}

static double sRealSnapAt = -1e9; // last port_mp_set_local_snapshot from the capture code (w15)

static void setLocalSnapshotLocked(const PortMpSnapshot* snap);

void port_mp_set_local_snapshot(const PortMpSnapshot* snap)
{
	if (snap == nullptr)
		return;
	std::lock_guard<std::mutex> lk(gMu);
	sRealSnapAt = nowMs();
	setLocalSnapshotLocked(snap);
}

// SMS_PORT_MP_TESTSNAP (port_mp_game.cpp): ignored while the real capture is sending.
void port_mp_net_test_snapshot(const PortMpSnapshot* snap)
{
	std::lock_guard<std::mutex> lk(gMu);
	if (nowMs() - sRealSnapAt < 2000.0)
		return;
	setLocalSnapshotLocked(snap);
}

static void setLocalSnapshotLocked(const PortMpSnapshot* snap)
{
	if (gState != PORT_MP_HOSTING && gState != PORT_MP_CONNECTED)
		return;
	gLocalSnap     = *snap;
	gHaveLocalSnap = true;
	if (gLocalSlot >= 0) {
		Player& p = gPlayers[gLocalSlot];
		p.hist[0].snap = *snap; // the local entry keeps only the latest
		p.hist[0].rxMs = nowMs();
		p.histCount    = 1;
	}
}

int port_mp_host(const char* lobbyName, int port, const PortMpSessionSettings* settings)
{
	return port_mp_host2(lobbyName, port, settings, PORT_MP_CONN_DIRECT, nullptr);
}

int port_mp_host2(const char* lobbyName, int port, const PortMpSessionSettings* settings, int connType,
                  const char* password)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureThread();
	if (gState == PORT_MP_HOSTING || gState == PORT_MP_CONNECTED || gState == PORT_MP_CONNECTING)
		endSession(nullptr);
	if (port <= 0 || port > 65535)
		port = gDefaultPort;
	bool opened = openSock(port);
	// server mode: the port is invisible to players, so take the next free one
	for (int k = 1; !opened && connType == PORT_MP_CONN_SERVER && k < kDiscoveryPorts; k++)
		opened = openSock(port = PORT_MP_DEFAULT_PORT + k);
	if (!opened) {
		gState = PORT_MP_FAILED;
		return 0;
	}
	if (settings != nullptr)
		gSettings = *settings;
	if (gSettings.maxPlayers < 2 || gSettings.maxPlayers > PORT_MP_MAX_PLAYERS)
		gSettings.maxPlayers = 8;
	if (gSettings.gameMode >= PORT_MP_MODE_COUNT)
		gSettings.gameMode = PORT_MP_MODE_NONE;
	if (gSettings.startStage == 0)
		gSettings.startStage = 1, gSettings.startScenario = 2;
	if (lobbyName != nullptr && lobbyName[0] != '\0')
		std::snprintf(gLobbyName, sizeof(gLobbyName), "%s", lobbyName);
	sanitizeName(gLobbyName, sizeof(gLobbyName), "Delfino Lobby");
	resetPlayers();
	for (auto& c : gConns)
		c = Conn();
	gHostPort  = port;
	gIsHost    = true;
	gLocalSlot = 0;
	Player& me = gPlayers[0];
	me.used    = true;
	me.isHost  = true;
	me.serial  = gSerialCounter++;
	std::memcpy(me.name, gLocalName, sizeof(me.name));
	stampGame(gLocalChar);
	me.character = gLocalChar;
	me.stage     = gLocalStage;
	me.scenario  = gLocalScenario;
	gState       = PORT_MP_HOSTING;
	gConnType    = connType == PORT_MP_CONN_SERVER ? PORT_MP_CONN_SERVER : PORT_MP_CONN_DIRECT;
	gPwHash      = pwHash(gLobbyName, password);
	gRegistered  = false;
	gRegNonce    = rnd32();
	gRegSentAt   = -1e9;
	gRegStart    = nowMs();
	if (gConnType == PORT_MP_CONN_SERVER) {
		if (!gServerResolved)
			startResolve();
		setStatus("Opening lobby \"%s\" on the lobby server ...", gLobbyName);
	} else {
		setStatus("Hosting \"%s\" on UDP port %d (up to %d players).", gLobbyName, port, gSettings.maxPlayers);
	}
	return 1;
}

int port_mp_join(const char* host, int port)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureThread();
	if (gState != PORT_MP_OFF && gState != PORT_MP_FAILED)
		endSession(nullptr);
	if (port <= 0 || port > 65535)
		port = gDefaultPort;
	if (!gWsaUp) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
			gWsaUp = true;
	}
	sockaddr_in a{};
	if (host == nullptr || !resolve(host, port, &a)) {
		gState = PORT_MP_FAILED;
		setStatus("Unknown host \"%s\".", host ? host : "");
		return 0;
	}
	if (!openSock(0)) {
		gState = PORT_MP_FAILED;
		return 0;
	}
	resetPlayers();
	for (auto& c : gConns)
		c = Conn();
	gHostAddr     = a;
	gIsHost       = false;
	gLocalSlot    = -1;
	gConnectNonce = rnd32();
	gConnectStart = nowMs();
	gLastConnect  = -1e9;
	gState        = PORT_MP_CONNECTING;
	gConnType     = PORT_MP_CONN_DIRECT;
	gPwHash       = 0;
	setStatus("Connecting to %s ...", addrStr(a).c_str());
	return 1;
}

int port_mp_join_name(const char* name, const char* password)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureThread();
	if (gState != PORT_MP_OFF && gState != PORT_MP_FAILED)
		endSession(nullptr);
	char n[PORT_MP_LOBBY_NAME_MAX] = {};
	std::snprintf(n, sizeof(n), "%s", name ? name : "");
	sanitizeName(n, sizeof(n), "");
	if (n[0] == '\0') {
		gState = PORT_MP_FAILED;
		setStatus("Type the name of the lobby to join.");
		return 0;
	}
	if (!openSock(0)) {
		gState = PORT_MP_FAILED;
		return 0;
	}
	resetPlayers();
	for (auto& c : gConns)
		c = Conn();
	resetServerSession();
	std::memcpy(gJoinName, n, sizeof(gJoinName));
	gJoinByName    = true;
	gJoinPhase     = 0;
	gConnType      = PORT_MP_CONN_SERVER;
	gPwHash        = pwHash(n, password);
	gHostAddr      = sockaddr_in{};
	gIsHost        = false;
	gLocalSlot     = -1;
	gConnectNonce  = rnd32();
	gLookupNonce   = rnd32();
	gDiscoverToken = rnd32();
	gDiscoverAt    = nowMs();
	gConnectStart  = nowMs();
	gLookupAt      = -1e9;
	gLastConnect   = -1e9;
	gRelayAskAt    = -1e9;
	gLobbies.clear();
	if (!gServerResolved)
		startResolve();
	gState = PORT_MP_CONNECTING;
	setStatus("Looking for \"%s\" ...", gJoinName);
	return 1;
}

int port_mp_conn_type(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gConnType;
}

int port_mp_relayed(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return !gIsHost && gConns[0].used && isRelayAddr(gConns[0].addr);
}

void port_mp_server_status(PortMpServerStatus* out)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	std::memset(out, 0, sizeof(*out));
	out->state   = serverUnset() ? PORT_MP_SERVER_UNSET : gServerState;
	out->lobbies = gServerLobbies;
	out->relay   = gServerRelay;
	std::snprintf(out->address, sizeof(out->address), "%s", gServerSpec);
	std::snprintf(out->publicAddr, sizeof(out->publicAddr), "%s", gPublicAddr);
}

void port_mp_server_refresh(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureThread();
	gServerPingWanted = nowMs();
	if (gSock == INVALID_SOCKET)
		openSock(0);
	if (!gServerResolved && !serverUnset() && nowMs() - gServerResolveAt > 10000.0)
		startResolve();
	if (serverUnset())
		gServerState = PORT_MP_SERVER_UNSET;
}

void port_mp_set_server(const char* hostPort)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	std::snprintf(gServerSpec, sizeof(gServerSpec), "%s", hostPort ? hostPort : "");
	gServerResolved  = false;
	gServerState     = PORT_MP_SERVER_UNKNOWN;
	gServerResolveAt = -1e9;
	gPublicAddr[0]   = '\0';
}

int port_mp_local_addresses(PortMpLocalAddr* out, int max) { return collectLocal(out, max); }

int port_mp_host_port(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gState == PORT_MP_HOSTING ? gHostPort : 0;
}

void port_mp_leave(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	if (gState == PORT_MP_FAILED) {
		gState = PORT_MP_OFF;
		return;
	}
	endSession("Left the lobby.");
}

void port_mp_set_name(const char* name)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	char n[PORT_MP_NAME_MAX] = {};
	std::snprintf(n, sizeof(n), "%s", name ? name : "");
	sanitizeName(n, sizeof(n), "Player");
	std::memcpy(gLocalName, n, sizeof(gLocalName));
	if (gState == PORT_MP_HOSTING) {
		uniqueName(gLocalName, gLocalSlot);
		sendLocalInfo();
	} else if (gState == PORT_MP_CONNECTED) {
		sendLocalInfo();
	}
}

const char* port_mp_get_name(void)
{
	static char sCopy[PORT_MP_NAME_MAX];
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	std::memcpy(sCopy, gLocalName, sizeof(sCopy));
	return sCopy;
}

void port_mp_set_character(const PortMpCharacter* c)
{
	if (c == nullptr)
		return;
	std::lock_guard<std::mutex> lk(gMu);
	gLocalChar = *c;
	if (gState == PORT_MP_HOSTING || gState == PORT_MP_CONNECTED)
		sendLocalInfo();
}

void port_mp_host_set_settings(const PortMpSessionSettings* s)
{
	if (s == nullptr)
		return;
	std::lock_guard<std::mutex> lk(gMu);
	if (gState != PORT_MP_HOSTING)
		return;
	PortMpSessionSettings n = *s;
	if (n.gameMode >= PORT_MP_MODE_COUNT)
		n.gameMode = PORT_MP_MODE_NONE;
	n.sharedObjectives = n.sharedObjectives ? 1 : 0;
	if (n.maxPlayers < 2 || n.maxPlayers > PORT_MP_MAX_PLAYERS)
		n.maxPlayers = gSettings.maxPlayers;
	gSettings = n;
	Writer w;
	w.put(&gSettings, sizeof(gSettings));
	broadcastMsg(MSG_SETTINGS, w, -1);
	mplog("settings: mode %d, shared objectives %d", n.gameMode, n.sharedObjectives);
}

void port_mp_lan_refresh(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureThread();
	if (gSock == INVALID_SOCKET && !openSock(0))
		return;
	gDiscoverToken = rnd32();
	gDiscoverAt    = nowMs();
	gLobbies.clear();
	Writer w;
	w.u32_(gDiscoverToken);
	for (int i = 0; i < kDiscoveryPorts; i++) {
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_port        = htons((u_short)(PORT_MP_DEFAULT_PORT + i));
		a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		sendRaw(a, PKT_DISCOVER, 0xFF, 0, 0, w.buf, w.n);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sendRaw(a, PKT_DISCOVER, 0xFF, 0, 0, w.buf, w.n);
	}
	if (gDefaultPort < PORT_MP_DEFAULT_PORT || gDefaultPort >= PORT_MP_DEFAULT_PORT + kDiscoveryPorts) {
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_port        = htons((u_short)gDefaultPort);
		a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		sendRaw(a, PKT_DISCOVER, 0xFF, 0, 0, w.buf, w.n);
	}
}

int port_mp_server_lobbies(PortMpLobbyInfo* out, int max)
{
	std::lock_guard<std::mutex> lk(gMu);
	if (gServerState != PORT_MP_SERVER_ONLINE)
		return 0;
	int n = 0;
	for (auto& l : gServerList)
		if (n < max)
			out[n++] = l;
	return n;
}

int port_mp_lan_lobbies(PortMpLobbyInfo* out, int max)
{
	std::lock_guard<std::mutex> lk(gMu);
	int n = 0;
	for (auto& l : gLobbies)
		if (n < max)
			out[n++] = l;
	return n;
}

// Game glue -> net: the local stage (0xFF when not in gameplay).
void port_mp_net_set_local_area(int stage, int scenario)
{
	std::lock_guard<std::mutex> lk(gMu);
	u8 st = (u8)stage, sc = (u8)scenario;
	if (st == gLocalStage && sc == gLocalScenario)
		return;
	gLocalStage    = st;
	gLocalScenario = sc;
	if (gState == PORT_MP_HOSTING || gState == PORT_MP_CONNECTED)
		sendLocalStage();
}

int port_mp_send(uint8_t channel, const void* data, int len, int reliable, int to)
{
	if (channel == 0 || len < 0 || len > PORT_MP_APP_MAX || (data == nullptr && len > 0))
		return 0;
	std::lock_guard<std::mutex> lk(gMu);
	if (gState != PORT_MP_HOSTING && gState != PORT_MP_CONNECTED)
		return 0;
	const u8 t = to < 0 || to >= PORT_MP_MAX_PLAYERS ? (u8)0xFF : (u8)to;
	if (t == gLocalSlot)
		return 0;
	if (gIsHost) {
		appForward(channel, t, (u8)gLocalSlot, (const u8*)data, len, reliable != 0, -1);
	} else if (gConns[0].used) {
		Writer w;
		appWrite(w, channel, t, (u8)gLocalSlot, (const u8*)data, len);
		if (!w.ok)
			return 0;
		if (reliable)
			queueMsg(gConns[0], MSG_APP, w);
		else
			sendConn(gConns[0], PKT_APP, w);
	}
	return 1;
}

int port_mp_recv(uint8_t channel, void* buf, int cap, int* fromSlot)
{
	std::lock_guard<std::mutex> lk(gMu);
	std::deque<AppMsg>& q = gAppQ[channel];
	if (q.empty())
		return 0;
	AppMsg& m = q.front();
	const int n = (int)m.data.size() < cap ? (int)m.data.size() : cap;
	if (n > 0)
		std::memcpy(buf, m.data.data(), n);
	if (fromSlot != nullptr)
		*fromSlot = m.from;
	q.pop_front();
	return n > 0 ? n : -1; // -1: an empty message
}

// Shared objectives: the local player collected `id` (game thread, port_mp_game.cpp).
void port_mp_objective_send(uint32_t id)
{
	std::lock_guard<std::mutex> lk(gMu);
	if (gState == PORT_MP_HOSTING) {
		hostObjCollect(id, gLocalSlot);
	} else if (gState == PORT_MP_CONNECTED && gConns[0].used) {
		std::vector<u32> one(1, id);
		sendObjList(gConns[0], OBJ_COLLECT, (u8)gLocalSlot, one);
	}
}

// Objectives collected by others (slot) or already collected on stage entry (slot 0xFF).
int port_mp_objective_poll(uint32_t* ids, uint8_t* slots, int max)
{
	std::lock_guard<std::mutex> lk(gMu);
	int n = 0;
	while (n < max && !gObjEvents.empty()) {
		ids[n]   = gObjEvents.front().id;
		slots[n] = gObjEvents.front().slot;
		gObjEvents.pop_front();
		n++;
	}
	return n;
}

// Port used for hosting by default ([multiplayer] port).
int port_mp_default_port(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	ensureInit();
	return gDefaultPort;
}

// Discord integration (port_discord.cpp): 1 when the lobby we are in is password-protected.
int port_mp_lobby_has_password(void)
{
	std::lock_guard<std::mutex> lk(gMu);
	return gPwHash != 0;
}

const char* port_mp_lobby_name(void)
{
	static char sCopy[PORT_MP_LOBBY_NAME_MAX];
	std::lock_guard<std::mutex> lk(gMu);
	std::memcpy(sCopy, gLobbyName, sizeof(sCopy));
	return sCopy;
}

} // extern "C"

#endif // SMS_PORT_AURORA
