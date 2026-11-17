#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include "Net.h"
#include "NetStructs.h"
#include <cstdio>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _XBOX
#include <xtl.h> // pulls in winsockx / xnet / xinput
#pragma comment(lib, "xnet.lib")
// link: xnet.lib (or xnetd.lib in debug); xonline.lib if using Live
#elif defined(__ANDROID__)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
// ---- Android: BSD sockets behind the Winsock names the rest of this file uses ----
// (Android is little-endian like the PC, so the wire format needs no swapping.)
typedef int SOCKET;
typedef uint32_t DWORD;
typedef int BOOL;
typedef long LONG;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef FIONBIO
#define FIONBIO 0
#endif
#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EWOULDBLOCK
#define MAKEWORD(a, b) ((a) | ((b) << 8))
#define InterlockedIncrement(p) __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p) __sync_sub_and_fetch((p), 1)
struct WSADATA {
	int unused;
};
static inline int
WSAStartup(int, WSADATA *)
{ return 0; }
static inline int
WSACleanup()
{ return 0; }
static inline int
ioctlsocket(SOCKET s, int, u_long *mode)
{
	int fl = fcntl(s, F_GETFL, 0);
	if(fl < 0) return -1;
	fl = *mode ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
	return fcntl(s, F_SETFL, fl);
}
static inline short
GetAsyncKeyState(int)
{ return 0; }
static inline BOOL
GetComputerNameA(char *buf, DWORD *size)
{
	strcpy(buf, "Android");
	*size = 7;
	return TRUE;
}
// bionic wants socklen_t* where Winsock takes int*
static inline int
coop_recvfrom(SOCKET s, char *b, int n, int flags, sockaddr *a, int *len)
{
	socklen_t sl = (socklen_t)*len;
	int r = (int)recvfrom(s, b, (size_t)n, flags, a, &sl);
	*len = (int)sl;
	return r;
}
#define recvfrom coop_recvfrom
#define _vsnprintf vsnprintf
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef _MSC_VER
#define COOP_ALIGN(n) __declspec(align(n))
#else
#define COOP_ALIGN(n) __attribute__((aligned(n)))
#endif

#ifdef _XBOX
// snprintf for the 360 comes from Net.h

// no keyboard on 360 (pad input comes from CoopMenu / CPad, see COOP_PADKEYS)
inline short
GetAsyncKeyState(int)
{ return 0; }

// no computer name on 360
inline BOOL
GetComputerNameA(char *buf, DWORD *size)
{
	strcpy(buf, "Xbox360");
	*size = 7;
	return TRUE;
}

// inet_pton / inet_ntop don't exist on 360, IPv4 versions:
inline int
inet_pton(int, const char *src, void *dst)
{
	unsigned int a = inet_addr(src);
	if(a == INADDR_NONE && strcmp(src, "255.255.255.255") != 0) return 0;
	*(unsigned int *)dst = a;
	return 1;
}
inline const char *
inet_ntop(int, const void *src, char *dst, int size)
{
	const unsigned char *b = (const unsigned char *)src;
	_snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	dst[size - 1] = 0;
	return dst;
}
#endif

// ---------------------------------------------------------------------------
// Shared platform network start/stop (XNet on 360, plain Winsock on PC).
// Safe to call more than once.
// ---------------------------------------------------------------------------
#ifdef __ANDROID__
// Call once at start-up with the app's writable files directory (e.g. from the port's own data path).
// Used for coop.txt, coopname.txt and coopnet.log. Without it, files go in the working directory.
static char g_dataDir[256] = "";
void
CoopSetDataDir(const char *dir)
{
	snprintf(g_dataDir, sizeof g_dataDir, "%s", dir ? dir : "");
	size_t n = strlen(g_dataDir);
	if(n && g_dataDir[n - 1] != '/' && n + 1 < sizeof g_dataDir) {
		g_dataDir[n] = '/';
		g_dataDir[n + 1] = 0;
	}
}
#endif

// Plain text log next to the xex (360: game:\coopnet.log, PC: working directory). Open it after a test run.
static void
CoopLog(const char *fmt, ...)
{
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	b[sizeof b - 1] = 0;
#ifdef _XBOX
	FILE *f = fopen("game:\\coopnet.log", "a");
#elif defined(__ANDROID__)
	char lp[300];
	snprintf(lp, sizeof lp, "%scoopnet.log", g_dataDir);
	FILE *f = fopen(lp, "a");
#else
	FILE *f = fopen("coopnet.log", "a");
#endif
	if(f) {
		fputs(b, f);
		fputc('\n', f);
		fclose(f);
	}
}

#ifdef _XBOX
// Retail consoles (RGH included) ignore XNET_STARTUP_BYPASS_SECURITY. Per vendor/webdav/xbox/net_xbox.cpp,
// a socket only talks to plain LAN peers when these two undocumented options are set BEFORE bind().
enum { SO_INSECURE_A = 0x5801, SO_INSECURE_B = 0x5802 };
static void
MarkSocketInsecure(SOCKET sk)
{
	BOOL one = TRUE;
	int ra = setsockopt(sk, SOL_SOCKET, SO_INSECURE_A, (const char *)&one, sizeof(one));
	int ea = ra ? WSAGetLastError() : 0;
	int rb = setsockopt(sk, SOL_SOCKET, SO_INSECURE_B, (const char *)&one, sizeof(one));
	int eb = rb ? WSAGetLastError() : 0;
	CoopLog("setsockopt 0x5801 -> %d (WSA %d), 0x5802 -> %d (WSA %d)", ra, ea, rb, eb);
}
#else
static inline void
MarkSocketInsecure(SOCKET)
{}
#endif

static bool g_platformNetUp = false;

static bool
CoopPlatformNetStart()
{
	if(g_platformNetUp) return true;

#ifdef _XBOX
	XNetStartupParams xnp;
	memset(&xnp, 0, sizeof(xnp));
	xnp.cfgSizeOfStruct = sizeof(xnp);
	xnp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY; // needed to talk to PCs / non-Live peers
	xnp.cfgSockMaxDgramSockets = 8;
	xnp.cfgSockMaxStreamSockets = 32;
	xnp.cfgSockDefaultRecvBufsizeInK = 16;
	xnp.cfgSockDefaultSendBufsizeInK = 16;

	// The webdav code may already have started XNet; a second call failing is not fatal, so carry on.
	INT xr = XNetStartup(&xnp);
	if(xr != 0) std::printf("XNetStartup returned 0x%X (already started?), continuing\n", xr);
	CoopLog("XNetStartup = 0x%X", (unsigned)xr);

	// Sockets are useless until the console has an IP (DHCP). Wait up to 8 s for it.
	{
		XNADDR xna;
		DWORD t0 = GetTickCount(), r;
		do {
			r = XNetGetTitleXnAddr(&xna);
			if(r & XNET_GET_XNADDR_PENDING) Sleep(100);
		} while((r & XNET_GET_XNADDR_PENDING) && GetTickCount() - t0 < 8000);
		if(r & XNET_GET_XNADDR_NONE)
			{
			std::printf("XNet: no IP address (no link / DHCP failed)\n");
			CoopLog("XNet: NO IP (no link / DHCP failed) r=0x%X", (unsigned)r);
		}
		else
			CoopLog("XNet: IP %u.%u.%u.%u", xna.ina.S_un.S_un_b.s_b1, xna.ina.S_un.S_un_b.s_b2, xna.ina.S_un.S_un_b.s_b3, xna.ina.S_un.S_un_b.s_b4);
	}
#endif

	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		std::printf("WSAStartup failed.\n");
		CoopLog("WSAStartup failed");
#ifdef _XBOX
		XNetCleanup();
#endif
		return false;
	}

	g_platformNetUp = true;
	CoopLog("network layer up");
	return true;
}

static void
CoopPlatformNetStop()
{
	if(!g_platformNetUp) return;
	WSACleanup();
#ifdef _XBOX
	XNetCleanup();
#endif
	g_platformNetUp = false;
}

static SOCKET g_Socket = INVALID_SOCKET;
static bool g_bInitialized = false;

void
CCoopNetwork::Init()
{
	if(g_bInitialized) return;

	if(!CoopPlatformNetStart()) return;

	g_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(g_Socket != INVALID_SOCKET) {
		MarkSocketInsecure(g_Socket);
		unsigned long mode = 1;
		ioctlsocket(g_Socket, FIONBIO, &mode);
	}

	g_bInitialized = true;
	std::printf("CCoopNetwork Initialized for Xbox 360 / Platform.\n");
}

void
CCoopNetwork::Shutdown()
{
	if(!g_bInitialized) return;

	if(g_Socket != INVALID_SOCKET) {
		closesocket(g_Socket);
		g_Socket = INVALID_SOCKET;
	}

	CoopPlatformNetStop();

	g_bInitialized = false;
}

void
CCoopNetwork::Update()
{
	if(!g_bInitialized || g_Socket == INVALID_SOCKET) return;

	COOP_ALIGN(16) char recvBuffer[2048];
	sockaddr_in fromAddr;
	int fromLen = sizeof(fromAddr);

	int bytesRead = recvfrom(g_Socket, recvBuffer, sizeof(recvBuffer), 0, (sockaddr *)&fromAddr, &fromLen);
	if(bytesRead > 0) {
		uint8_t packetType = recvBuffer[0];
		if(packetType == 1 && bytesRead >= (int)sizeof(VehNet)) {
			VehNet *veh = (VehNet *)(recvBuffer + 1);
			veh->Swap();
		} else if(packetType == 2 && bytesRead >= (int)sizeof(PedNet)) {
			PedNet *ped = (PedNet *)(recvBuffer + 1);
			ped->Swap();
		}
	}
}

void
CCoopNetwork::SendVehicleSync(const void *data, size_t size)
{
	if(!g_bInitialized || g_Socket == INVALID_SOCKET || !data) return;

	COOP_ALIGN(4) char sendBuffer[sizeof(VehNet) + 1];
	sendBuffer[0] = 1;

	VehNet *vehPacket = (VehNet *)(sendBuffer + 1);
	*vehPacket = *(const VehNet *)data;
	vehPacket->Swap();
	// NOTE: this never calls sendto() (no destination address is known here).
}

void
CCoopNetwork::SendPedSync(const void *data, size_t size)
{
	if(!g_bInitialized || g_Socket == INVALID_SOCKET || !data) return;

	COOP_ALIGN(4) char sendBuffer[sizeof(PedNet) + 1];
	sendBuffer[0] = 2;

	PedNet *pedPacket = (PedNet *)(sendBuffer + 1);
	*pedPacket = *(const PedNet *)data;
	pedPacket->Swap();
	// NOTE: this never calls sendto() (no destination address is known here).
}

bool
CCoopNetwork::IsConnected()
{ return g_bInitialized && g_Socket != INVALID_SOCKET; }

#if !defined(_XBOX) && !defined(__ANDROID__)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace CoopNet
{

static const uint32_t MAGIC = 0x56434F57;
static const unsigned short PORT = 7777;
enum {
	PKT_POS = 1,
	PKT_DISCOVER = 2,
	PKT_INFO = 3,
	PKT_WORLD = 4,
	PKT_VEHS = 5,
	PKT_PEDS = 6,
	PKT_FIRES = 7,
	PKT_GVEH = 8,
	PKT_HIT = 9,
	PKT_NAME = 10,
	PKT_BLAST = 11,
	PKT_CHAT = 12,
	PKT_JOIN = 13,
	PKT_WELCOME = 14,
	PKT_FULL = 15,
	PKT_LEAVE = 16,
	PKT_ROSTER = 17
};
static const int MAX_SERVERS = 8;
static const int MAXSNAP = 64;
static const double MIN_DELAY = 0.020;
static const double MAX_DELAY = 0.200;
static const double MAX_EXTRAP = 0.150;
static const double SEND_MIN_INTERVAL = 0.008;
static const double RELAY_MIN_INTERVAL = 0.025;
static const double TIMEOUT = 3.0;
static const double JOIN_INTERVAL = 0.5;
static const float PI_F = 3.14159265f;

static const int MAX_VENTS = 128;
static const int MAX_PENTS = 160;
static const int ESNAP = 5;
static const double ENT_DELAY = 0.08;
static const double ENT_TIMEOUT = 0.6;
static const int VEHS_PER_PKT = 16;
static const int PEDS_PER_PKT = 18;

#pragma pack(push, 1)
struct Header {
	uint32_t magic, type;
};
struct PosPacket {
	Header h;
	uint32_t seq;
	uint32_t timeMs;
	uint8_t slot, pad[3];
	float x, y, z, heading;
	uint8_t moveState, flags, numAnims, seat;
	int32_t vehId;
	uint8_t weapon, shots, aiming, looking;
	float aimYaw, aimPitch;
	AnimEntry anims[MAX_ANIMS];
};
struct InfoPacket {
	Header h;
	char name[32];
	uint8_t players, maxPlayers, pad[2];
};
struct JoinPacket {
	Header h;
	char name[16];
};
struct WelcomePacket {
	Header h;
	uint8_t slot, maxPlayers, pad[2];
};
struct RosterEntry {
	uint8_t active, shared, pad[2];
	char name[16];
};
struct RosterPacket {
	Header h;
	RosterEntry e[MAX_PLAYERS];
};
struct WorldPacket {
	Header h;
	uint32_t seq;
	WorldNet w;
};
struct EntHeader {
	Header h;
	uint32_t seq;
	uint32_t timeMs;
	uint32_t count;
};
struct HitPacket {
	Header h;
	uint32_t seq;
	HitNet hit;
};
struct NamePacket {
	Header h;
	char name[16];
};
struct ChatPacket {
	Header h;
	uint32_t seq;
	char text[96];
};
struct BlastPacket {
	Header h;
	uint32_t seq;
	BlastNet blast;
};
#pragma pack(pop)

static const int POS_HEADER_SIZE = (int)offsetof(PosPacket, anims);

// ---------------------------------------------------------------------------
// Wire format: LITTLE-ENDIAN (the PC's native order). The 360 is big-endian, so every packet is
// byte-swapped on the way out and on the way in. A swap is its own inverse, so the same function
// serves both directions. On PC all of this compiles to nothing.
// Rules: swap a packet right after building it (before sendto) and right after copying it out of the
// receive buffer. Single-byte fields never need it. The host relays position packets as raw bytes.
// ---------------------------------------------------------------------------
#ifdef _XBOX
static inline void
Sw16(void *p)
{
	unsigned char *b = (unsigned char *)p, t = b[0];
	b[0] = b[1];
	b[1] = t;
}
static inline void
Sw32(void *p)
{
	unsigned char *b = (unsigned char *)p, t = b[0];
	b[0] = b[3];
	b[3] = t;
	t = b[1];
	b[1] = b[2];
	b[2] = t;
}
#else
static inline void
Sw16(void *)
{}
static inline void
Sw32(void *)
{}
#endif
#define SW16(s, f) Sw16(&(s).f)
#define SW32(s, f) Sw32(&(s).f)

static void
SwapHeader(Header &h)
{
	SW32(h, magic);
	SW32(h, type);
}
static void
SwapAnims(AnimEntry *a, int n)
{
	for(int i = 0; i < n && i < MAX_ANIMS; i++) {
		SW16(a[i], animId);
		SW32(a[i], time);
	}
}
static void
SwapPosFixed(PosPacket &p)
{
	SwapHeader(p.h);
	SW32(p, seq);
	SW32(p, timeMs);
	SW32(p, x);
	SW32(p, y);
	SW32(p, z);
	SW32(p, heading);
	SW32(p, vehId);
	SW32(p, aimYaw);
	SW32(p, aimPitch);
}
static void
SwapPos(PosPacket &p)
{
	int n = p.numAnims; // a byte, read before anything is swapped
	SwapPosFixed(p);
	SwapAnims(p.anims, n);
}
static void
SwapEntHeader(EntHeader &e)
{
	SwapHeader(e.h);
	SW32(e, seq);
	SW32(e, timeMs);
	SW32(e, count);
}
static void
SwapVeh(VehNet &v)
{
	SW32(v, id);
	SW16(v, model);
	SW32(v, x);
	SW32(v, y);
	SW32(v, z);
	for(int i = 0; i < 3; i++) {
		SW16(v, fwd[i]);
		SW16(v, up[i]);
		SW16(v, vel[i]);
	}
	SW16(v, lean);
	SW16(v, steerHi);
	SW16(v, pedLR);
	SW16(v, pedUD);
}
static void
SwapPed(PedNet &p)
{
	SW32(p, id);
	SW16(p, model);
	SW32(p, x);
	SW32(p, y);
	SW32(p, z);
	SW32(p, heading);
	SW32(p, vehId);
	SwapAnims(p.anims, PED_ANIMS);
}
static void
SwapFire(FireNet &f)
{
	SW32(f, x);
	SW32(f, y);
	SW32(f, z);
}
static void
SwapWorldPacket(WorldPacket &p)
{
	SwapHeader(p.h);
	SW32(p, seq);
	SW16(p.w, oldWeather);
	SW16(p.w, newWeather);
}
static void
SwapHitPacket(HitPacket &p)
{
	SwapHeader(p.h);
	SW32(p, seq);
	SW32(p.hit, targetId);
	SW16(p.hit, model);
	SW32(p.hit, damage);
}
static void
SwapBlastPacket(BlastPacket &p)
{
	SwapHeader(p.h);
	SW32(p, seq);
	SW16(p.blast, type);
	SW32(p.blast, x);
	SW32(p.blast, y);
	SW32(p.blast, z);
}
static void
SwapChatPacket(ChatPacket &p)
{
	SwapHeader(p.h);
	SW32(p, seq);
}
// swap an item that sits at an arbitrary address inside a send buffer
static void
SwapVehAt(char *at)
{
	VehNet v;
	memcpy(&v, at, sizeof v);
	SwapVeh(v);
	memcpy(at, &v, sizeof v);
}
static void
SwapPedAt(char *at)
{
	PedNet v;
	memcpy(&v, at, sizeof v);
	SwapPed(v);
	memcpy(at, &v, sizeof v);
}
static void
SwapFireAt(char *at)
{
	FireNet v;
	memcpy(&v, at, sizeof v);
	SwapFire(v);
	memcpy(at, &v, sizeof v);
}
static Header
MkHeader(uint32_t type)
{
	Header h;
	h.magic = MAGIC;
	h.type = type;
	SwapHeader(h);
	return h;
}

struct Snap {
	double t;
	PlayerState s;
};

struct VEnt {
	bool used;
	int32_t id;
	double lastRx;
	int n;
	double t[ESNAP];
	RemoteVeh s[ESNAP];
};

struct PEnt {
	bool used;
	int32_t id;
	double lastRx;
	int n;
	double t[ESNAP];
	PedNet s[ESNAP];
};

struct Peer {
	bool used;
	sockaddr_in addr;
	double lastRx, lastRelay;
	uint32_t entTx, hitTx, blastTx, chatTx;
	uint32_t hitRx, blastRx, chatRx, gvRx;
	bool haveHit, haveBlast, haveChat, haveGv;
};

struct RemotePlayer {
	bool active;
	bool shared;
	char name[16];
	Snap snaps[MAXSNAP];
	int numSnaps;
	uint32_t lastSeq;
	bool haveSeq;
	double clockOffset;
	bool haveOffset;
	double curDelay, avgInterval, jitter;
	double lastSendT, lastArriveT;
	bool haveLastT;
	float pps;
	uint32_t statCount;
	double statStart;
	double lastRx;
	float x, y, z;
	bool havePos;
};

static State state = OFF;
static SOCKET sock = INVALID_SOCKET;
static int localSlot = -1;
static bool welcomed = false;
static Peer peers[MAX_PLAYERS];
static RemotePlayer rp[MAX_PLAYERS];
static uint32_t sendSeq = 0, recvCount = 0;
static double lastSendSec = 0, lastDiscoverSec = -10.0, lastJoinSec = -10.0, lastRosterSec = -10.0, sessionStart = 0;
static bool rosterDirty = false;
static char note[160] = "Co-op is off";
static Server servers[MAX_SERVERS];
static double seen[MAX_SERVERS];
static int numServers = 0;

static VEnt vents[MAX_VENTS];
static PEnt pents[MAX_PENTS];
static VEnt gv[MAX_PLAYERS];
static uint32_t gvSeq = 0;
static uint32_t lastVehSeq = 0, lastPedSeq = 0, lastFireSeq = 0;
static bool haveVehSeq = false, havePedSeq = false, haveFireSeq = false;
static WorldNet latestWorld;
static bool worldFresh = false;
static FireNet fires[MAX_FIRES];
static int numFires = 0;
static bool firesFresh = false;

static const int HITQ = 32;
static HitNet hitQ[HITQ];
static int hitHead = 0, hitTail = 0;

static const int BLASTQ = 16;
static BlastNet blastQ[BLASTQ];
static int blastHead = 0, blastTail = 0;

static const int CHATQ = 8;
static char chatQ[CHATQ][96];
static int chatHead = 0, chatTail = 0;

static char localName[16] = "";
static double lastNameSend = -10.0;

static double
NowSec()
{
#ifdef __ANDROID__
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
	static LARGE_INTEGER freq = {0};
	if(freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)freq.QuadPart;
#endif
}

double
Time()
{ return NowSec(); }

// ---------------------------------------------------------------------------
// Send helpers (were missing from the file)
// ---------------------------------------------------------------------------
static void
SendToPeer(const Peer &p, const void *data, int n)
{
	if(sock == INVALID_SOCKET || !p.used || n <= 0) return;
	int r = sendto(sock, (const char *)data, n, 0, (const sockaddr *)&p.addr, sizeof p.addr);
	static int logged = 0;
	if((r < 0 || logged < 25) && logged < 60) {
		logged++;
		const unsigned char *ab = (const unsigned char *)&p.addr.sin_addr;
		CoopLog("sendto %u.%u.%u.%u:%u len %d -> %d (WSA %d)", ab[0], ab[1], ab[2], ab[3], (unsigned)ntohs(p.addr.sin_port), n, r, r < 0 ? WSAGetLastError() : 0);
	}
}

// Host only: send to every connected guest except 'exceptSlot' (-1 = nobody excluded).
static void
SendToAll(const void *data, int n, int exceptSlot = -1)
{
	if(sock == INVALID_SOCKET || n <= 0) return;
	for(int i = 1; i < MAX_PLAYERS; i++)
		if(i != exceptSlot && peers[i].used) SendToPeer(peers[i], data, n);
}

static void
ResetRemotePlayer(int slot)
{
	RemotePlayer &R = rp[slot];
	memset(&R, 0, sizeof R);
	R.curDelay = 0.06;
	R.avgInterval = 0.033;
	R.jitter = 0.005;
	R.statStart = NowSec();
}

static void
ResetWorldState()
{
	memset(vents, 0, sizeof vents);
	memset(pents, 0, sizeof pents);
	memset(gv, 0, sizeof gv);
	haveVehSeq = havePedSeq = haveFireSeq = false;
	worldFresh = false;
	numFires = 0;
	firesFresh = false;
	hitHead = hitTail = 0;
	blastHead = blastTail = 0;
	chatHead = chatTail = 0;
}

static void
ResetRemote()
{
	for(int i = 0; i < MAX_PLAYERS; i++) ResetRemotePlayer(i);
	ResetWorldState();
}

static bool
SameAddr(const sockaddr_in &a, const sockaddr_in &b)
{ return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

static int
FindPeerSlot(const sockaddr_in &a)
{
	for(int i = 1; i < MAX_PLAYERS; i++)
		if(peers[i].used && SameAddr(peers[i].addr, a)) return i;
	return -1;
}

static int
NumPeers()
{
	int c = 0;
	for(int i = 1; i < MAX_PLAYERS; i++)
		if(peers[i].used) c++;
	return c;
}

static void
MakePath(char *out, size_t n, const char *name)
{
#ifdef _XBOX
	// files live next to the xex on the 360
	snprintf(out, n, "game:\\%s", name);
#elif defined(__ANDROID__)
	snprintf(out, n, "%s%s", g_dataDir, name);
#else
	static char exeDir[MAX_PATH] = "";
	if(!exeDir[0]) {
		GetModuleFileNameA(NULL, exeDir, MAX_PATH);
		char *s = strrchr(exeDir, '\\');
		if(s) *(s + 1) = 0;
	}
	snprintf(out, n, "%s%s", exeDir, name);
#endif
}

static bool
GameHasFocus()
{
#if defined(_XBOX) || defined(__ANDROID__)
	return true;
#else
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
#endif
}

bool
KeyPressed(int which)
{
	static bool prev[11];
	static double nextRepeat[11];
	if(which < 0 || which > 10) return false;
#if defined(_XBOX) || defined(__ANDROID__)
	bool down = false; // 360: pad hotkeys are handled by CoopMenu through the game's CPad (COOP_PADKEYS)
#else
	static const int vks[11] = {VK_F5, 'G', VK_F3, VK_UP, VK_DOWN, VK_RETURN, VK_BACK, VK_LEFT, VK_RIGHT, '3', 'T'};
	bool down = GameHasFocus() && (GetAsyncKeyState(vks[which]) & 0x8000) != 0;
#endif
	double now = NowSec();
	bool pressed = down && !prev[which];
	if(pressed) {
		nextRepeat[which] = now + 0.4;
	} else if(down && (which == 3 || which == 4 || which == 7 || which == 8) && now >= nextRepeat[which]) {
		pressed = true;
		nextRepeat[which] = now + 0.07;
	}
	prev[which] = down;
	return pressed;
}

static void
SendLeave(const Peer &p)
{
	Header h = MkHeader(PKT_LEAVE);
	for(int i = 0; i < 2; i++) SendToPeer(p, &h, sizeof h);
}

void
Stop()
{
	if(sock != INVALID_SOCKET) {
		if(state == CLIENT && welcomed && peers[0].used)
			SendLeave(peers[0]);
		else if(state == HOST)
			for(int i = 1; i < MAX_PLAYERS; i++)
				if(peers[i].used) SendLeave(peers[i]);
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	state = OFF;
	memset(peers, 0, sizeof peers);
	localSlot = -1;
	welcomed = false;
	rosterDirty = false;
	recvCount = 0;
	numServers = 0;
	ResetRemote();
	snprintf(note, sizeof note, "Co-op is off");
}

static bool
OpenSocket(unsigned short bindPort)
{
	if(!CoopPlatformNetStart()) return false;
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(sock == INVALID_SOCKET) {
		CoopLog("socket() failed, WSA %d", WSAGetLastError());
		return false;
	}
	MarkSocketInsecure(sock); // must come before bind()

	u_long nonBlocking = 1;
	ioctlsocket(sock, FIONBIO, &nonBlocking);

	BOOL yes = TRUE;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);

	int bufSize = 1 << 20;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufSize, sizeof bufSize);

#if !defined(_XBOX) && !defined(__ANDROID__)
	// Windows quirk: stop ICMP "port unreachable" from killing UDP recv. Not needed on 360.
	BOOL off = FALSE;
	DWORD ret = 0;
	WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
#endif

	sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(bindPort);
	if(bind(sock, (sockaddr *)&a, sizeof a) != 0) {
		CoopLog("bind port %u failed, WSA %d", (unsigned)bindPort, WSAGetLastError());
		closesocket(sock);
		sock = INVALID_SOCKET;
		return false;
	}
	sessionStart = NowSec();
	CoopLog("socket open, bound to port %u", (unsigned)bindPort);
	return true;
}

void
StartHost()
{
	CoopLog("StartHost called");
	Stop();
	if(!OpenSocket(PORT)) {
		snprintf(note, sizeof note, "Host failed (error %d). Is another game already hosting?", WSAGetLastError());
		return;
	}
	state = HOST;
	localSlot = 0;
	welcomed = true;
}

void
StartBrowse()
{
	CoopLog("StartBrowse called");
	Stop();
	if(!OpenSocket(0)) {
		snprintf(note, sizeof note, "Search failed (error %d)", WSAGetLastError());
		return;
	}
	state = BROWSING;
	lastDiscoverSec = -10.0;
}

static void
BeginClient(const sockaddr_in &host)
{
	memset(peers, 0, sizeof peers);
	peers[0].used = true;
	peers[0].addr = host;
	peers[0].lastRx = NowSec();
	localSlot = -1;
	welcomed = false;
	lastJoinSec = -10.0;
	recvCount = 0;
	ResetRemote();
	state = CLIENT;
}

bool
JoinServer(int index)
{
	if(state != BROWSING || index < 0 || index >= numServers) return false;
	sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(PORT);
	if(inet_pton(AF_INET, servers[index].ip, &a.sin_addr) != 1) return false;
	BeginClient(a);
	snprintf(note, sizeof note, "Joining %s", servers[index].name);
	return true;
}

void
JoinFromFile()
{
	Stop();
	char path[MAX_PATH], ip[64] = "";
	MakePath(path, sizeof path, "coop.txt");
	FILE *f = fopen(path, "r");
	if(f) {
		if(fgets(ip, sizeof ip, f)) ip[strcspn(ip, "\r\n ")] = 0;
		fclose(f);
	}
	CoopLog("JoinFromFile: file '%s' ip '%s'", path, ip);
	if(!ip[0]) {
		snprintf(note, sizeof note, "No IP found in %s", path);
		return;
	}
	if(!OpenSocket(0)) {
		snprintf(note, sizeof note, "Join failed (error %d)", WSAGetLastError());
		return;
	}
	sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(PORT);
	if(inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
		closesocket(sock);
		sock = INVALID_SOCKET;
		snprintf(note, sizeof note, "Invalid IP in coop.txt");
		return;
	}
	BeginClient(a);
}

static void
AddServer(const sockaddr_in &from, const char *name, int players, int maxPlayers)
{
	char ip[16];
	inet_ntop(AF_INET, (void *)&from.sin_addr, ip, sizeof ip);
	int slot = -1;
	for(int i = 0; i < numServers; i++)
		if(strcmp(servers[i].ip, ip) == 0) {
			slot = i;
			break;
		}
	if(slot < 0) {
		if(numServers >= MAX_SERVERS) return;
		slot = numServers++;
	}
	strncpy(servers[slot].ip, ip, sizeof servers[slot].ip - 1);
	servers[slot].ip[sizeof servers[slot].ip - 1] = 0;
	strncpy(servers[slot].name, name, sizeof servers[slot].name - 1);
	servers[slot].name[sizeof servers[slot].name - 1] = 0;
	servers[slot].players = (uint8_t)players;
	servers[slot].maxPlayers = (uint8_t)maxPlayers;
	seen[slot] = NowSec();
}

static void
PushSnap(RemotePlayer &R, const PosPacket &p, double now)
{
	if(R.haveSeq && (int32_t)(p.seq - R.lastSeq) <= 0) return;
	R.haveSeq = true;
	R.lastSeq = p.seq;

	double t = p.timeMs / 1000.0;

	if(R.haveLastT) {
		double sdt = t - R.lastSendT, adt = now - R.lastArriveT;
		if(sdt > 0.0 && sdt < 0.5) {
			R.avgInterval += (sdt - R.avgInterval) * 0.05;
			R.jitter += (fabs(adt - sdt) - R.jitter) * 0.05;
			double target = R.avgInterval * 1.25 + R.jitter * 2.0;
			if(target < MIN_DELAY) target = MIN_DELAY;
			if(target > MAX_DELAY) target = MAX_DELAY;
			R.curDelay += (target - R.curDelay) * 0.02;
		}
	}
	R.haveLastT = true;
	R.lastSendT = t;
	R.lastArriveT = now;

	double off = now - t;
	if(!R.haveOffset || off < R.clockOffset) {
		R.clockOffset = off;
		R.haveOffset = true;
	} else {
		R.clockOffset += (off - R.clockOffset) * 0.002;
	}

	if(R.numSnaps == MAXSNAP) {
		memmove(&R.snaps[0], &R.snaps[1], sizeof(Snap) * (MAXSNAP - 1));
		R.numSnaps--;
	}
	Snap &sn = R.snaps[R.numSnaps++];
	sn.t = t;
	memset(&sn.s, 0, sizeof sn.s);
	sn.s.x = p.x;
	sn.s.y = p.y;
	sn.s.z = p.z;
	sn.s.heading = p.heading;
	sn.s.moveState = p.moveState;
	sn.s.flags = p.flags;
	sn.s.seat = p.seat;
	sn.s.vehId = p.vehId;
	sn.s.weapon = p.weapon;
	sn.s.shots = p.shots;
	sn.s.aiming = p.aiming;
	sn.s.looking = p.looking;
	sn.s.aimYaw = p.aimYaw;
	sn.s.aimPitch = p.aimPitch;
	sn.s.numAnims = p.numAnims;
	memcpy(sn.s.anims, p.anims, p.numAnims * sizeof(AnimEntry));
}

static void
DecodeVeh(const VehNet &v, RemoteVeh &r)
{
	memset(&r, 0, sizeof r);
	r.id = v.id;
	r.model = v.model;
	r.flags = v.flags;
	r.col1 = v.col1;
	r.col2 = v.col2;
	r.type = v.type;
	r.freeSeats = v.freeSeats;
	r.health = v.health;
	memcpy(r.doorOpen, v.doorOpen, 6);
	memcpy(r.doorState, v.doorState, 6);
	memcpy(r.panelState, v.panelState, 7);
	memcpy(r.wheelState, v.wheelState, 4);
	r.x = v.x;
	r.y = v.y;
	r.z = v.z;
	r.fx = v.fwd[0] / 32767.0f;
	r.fy = v.fwd[1] / 32767.0f;
	r.fz = v.fwd[2] / 32767.0f;
	r.ux = v.up[0] / 32767.0f;
	r.uy = v.up[1] / 32767.0f;
	r.uz = v.up[2] / 32767.0f;
	r.vx = v.vel[0] / 8192.0f;
	r.vy = v.vel[1] / 8192.0f;
	r.vz = v.vel[2] / 8192.0f;
	r.steer = v.steerHi / 10000.0f;
	r.lean = v.lean / 10000.0f;
	r.gas = v.gas / 127.0f;
	r.brake = v.brake / 127.0f;
	r.pedLR = v.pedLR / 10000.0f;
	r.pedUD = v.pedUD / 10000.0f;
	r.leanIn = v.leanIn / 127.0f;
	r.burn = v.burn;
}

static VEnt *
FindVEnt(int32_t id)
{
	VEnt *free = nullptr, *oldest = &vents[0];
	for(int i = 0; i < MAX_VENTS; i++) {
		if(vents[i].used && vents[i].id == id) return &vents[i];
		if(!vents[i].used && !free) free = &vents[i];
		if(vents[i].lastRx < oldest->lastRx) oldest = &vents[i];
	}
	VEnt *e = free ? free : oldest;
	memset(e, 0, sizeof *e);
	e->used = true;
	e->id = id;
	return e;
}

static PEnt *
FindPEnt(int32_t id)
{
	PEnt *free = nullptr, *oldest = &pents[0];
	for(int i = 0; i < MAX_PENTS; i++) {
		if(pents[i].used && pents[i].id == id) return &pents[i];
		if(!pents[i].used && !free) free = &pents[i];
		if(pents[i].lastRx < oldest->lastRx) oldest = &pents[i];
	}
	PEnt *e = free ? free : oldest;
	memset(e, 0, sizeof *e);
	e->used = true;
	e->id = id;
	return e;
}

template <class T>
static void
PushEnt(int &n, double *t, T *s, const T &v, double tt)
{
	if(n > 0 && tt <= t[n - 1]) return;
	if(n == ESNAP) {
		for(int i = 1; i < ESNAP; i++) {
			t[i - 1] = t[i];
			s[i - 1] = s[i];
		}
		n--;
	}
	t[n] = tt;
	s[n] = v;
	n++;
}

static void
HandleVehs(const char *buf, int n, double now)
{
	if(n < (int)sizeof(EntHeader)) return;
	EntHeader eh;
	memcpy(&eh, buf, sizeof eh);
	SwapEntHeader(eh);
	if(haveVehSeq && (int32_t)(eh.seq - lastVehSeq) <= 0) return;
	haveVehSeq = true;
	lastVehSeq = eh.seq;
	if(eh.count > (uint32_t)VEHS_PER_PKT || n < (int)(sizeof eh + eh.count * sizeof(VehNet))) return;
	double t = eh.timeMs / 1000.0;
	for(uint32_t i = 0; i < eh.count; i++) {
		VehNet v;
		memcpy(&v, buf + sizeof eh + i * sizeof(VehNet), sizeof v);
		SwapVeh(v);
		RemoteVeh r;
		DecodeVeh(v, r);
		VEnt *e = FindVEnt(v.id);
		PushEnt(e->n, e->t, e->s, r, t);
		e->lastRx = now;
	}
}

static void
HandlePeds(const char *buf, int n, double now)
{
	if(n < (int)sizeof(EntHeader)) return;
	EntHeader eh;
	memcpy(&eh, buf, sizeof eh);
	SwapEntHeader(eh);
	if(havePedSeq && (int32_t)(eh.seq - lastPedSeq) <= 0) return;
	havePedSeq = true;
	lastPedSeq = eh.seq;
	if(eh.count > (uint32_t)PEDS_PER_PKT || n < (int)(sizeof eh + eh.count * sizeof(PedNet))) return;
	double t = eh.timeMs / 1000.0;
	for(uint32_t i = 0; i < eh.count; i++) {
		PedNet p;
		memcpy(&p, buf + sizeof eh + i * sizeof(PedNet), sizeof p);
		SwapPed(p);
		if(p.numAnims > PED_ANIMS) p.numAnims = PED_ANIMS;
		PEnt *e = FindPEnt(p.id);
		PushEnt(e->n, e->t, e->s, p, t);
		e->lastRx = now;
	}
}

static void
HandleFires(const char *buf, int n)
{
	if(n < (int)sizeof(EntHeader)) return;
	EntHeader eh;
	memcpy(&eh, buf, sizeof eh);
	SwapEntHeader(eh);
	if(haveFireSeq && (int32_t)(eh.seq - lastFireSeq) <= 0) return;
	if(eh.count > (uint32_t)MAX_FIRES || n < (int)(sizeof eh + eh.count * sizeof(FireNet))) return;
	haveFireSeq = true;
	lastFireSeq = eh.seq;
	numFires = (int)eh.count;
	if(numFires > 0) {
		memcpy(fires, buf + sizeof eh, numFires * sizeof(FireNet));
		for(int i = 0; i < numFires; i++) SwapFire(fires[i]);
	}
	firesFresh = true;
}

static void
HandleGVeh(int slot, const char *buf, int n, double now)
{
	if(n < (int)(sizeof(EntHeader) + sizeof(VehNet))) return;
	Peer &P = peers[slot];
	EntHeader eh;
	memcpy(&eh, buf, sizeof eh);
	SwapEntHeader(eh);
	if(P.haveGv && (int32_t)(eh.seq - P.gvRx) <= 0) return;
	P.haveGv = true;
	P.gvRx = eh.seq;
	VehNet v;
	memcpy(&v, buf + sizeof eh, sizeof v);
	SwapVeh(v);
	RemoteVeh r;
	DecodeVeh(v, r);
	VEnt &g = gv[slot];
	if(!g.used || g.id != v.id) {
		memset(&g, 0, sizeof g);
		g.used = true;
		g.id = v.id;
	}
	PushEnt(g.n, g.t, g.s, r, eh.timeMs / 1000.0);
	g.lastRx = now;
}

static void
SendHitTo(Peer &P, const HitNet &h)
{
	HitPacket p;
	memset(&p, 0, sizeof p);
	p.h.magic = MAGIC;
	p.h.type = PKT_HIT;
	p.seq = ++P.hitTx;
	p.hit = h;
	SwapHitPacket(p);
	for(int i = 0; i < 3; i++) SendToPeer(P, &p, sizeof p);
}

static void
SendBlastTo(Peer &P, const BlastNet &b)
{
	BlastPacket p;
	memset(&p, 0, sizeof p);
	p.h.magic = MAGIC;
	p.h.type = PKT_BLAST;
	p.seq = ++P.blastTx;
	p.blast = b;
	SwapBlastPacket(p);
	for(int i = 0; i < 3; i++) SendToPeer(P, &p, sizeof p);
}

static void
SendChatTo(Peer &P, const char *line)
{
	ChatPacket p;
	memset(&p, 0, sizeof p);
	p.h.magic = MAGIC;
	p.h.type = PKT_CHAT;
	p.seq = ++P.chatTx;
	strncpy(p.text, line, sizeof p.text - 1);
	SwapChatPacket(p);
	for(int i = 0; i < 2; i++) SendToPeer(P, &p, sizeof p);
}

static void
HandleHit(int src, const char *buf, int n)
{
	if(n < (int)sizeof(HitPacket)) return;
	HitPacket p;
	memcpy(&p, buf, sizeof p);
	SwapHitPacket(p);
	Peer &P = peers[src];
	if(P.haveHit && (int32_t)(p.seq - P.hitRx) <= 0) return;
	P.hitRx = p.seq;
	P.haveHit = true;

	HitNet h = p.hit;
	if(state == HOST) h.srcSlot = (uint8_t)src;
	if(h.srcSlot >= MAX_PLAYERS) return;

	if(state == HOST && h.dstSlot != NO_SLOT && h.dstSlot != 0) {
		if(h.dstSlot < MAX_PLAYERS && h.dstSlot != src && peers[h.dstSlot].used) SendHitTo(peers[h.dstSlot], h);
		return;
	}
	int next = (hitHead + 1) % HITQ;
	if(next == hitTail) return;
	hitQ[hitHead] = h;
	hitHead = next;
}

static void
HandleBlast(int src, const char *buf, int n)
{
	if(n < (int)sizeof(BlastPacket)) return;
	BlastPacket p;
	memcpy(&p, buf, sizeof p);
	SwapBlastPacket(p);
	Peer &P = peers[src];
	if(P.haveBlast && (int32_t)(p.seq - P.blastRx) <= 0) return;
	P.blastRx = p.seq;
	P.haveBlast = true;

	BlastNet b = p.blast;
	if(state == HOST) {
		b.srcSlot = (uint8_t)src;
		for(int i = 1; i < MAX_PLAYERS; i++)
			if(i != src && peers[i].used) SendBlastTo(peers[i], b);
	}
	if(b.srcSlot >= MAX_PLAYERS) return;
	int next = (blastHead + 1) % BLASTQ;
	if(next == blastTail) return;
	blastQ[blastHead] = b;
	blastHead = next;
}

static void
HandleChat(int src, const char *buf, int n)
{
	if(n < (int)sizeof(ChatPacket)) return;
	ChatPacket p;
	memcpy(&p, buf, sizeof p);
	SwapChatPacket(p);
	Peer &P = peers[src];
	if(P.haveChat && (int32_t)(p.seq - P.chatRx) <= 0) return;
	P.chatRx = p.seq;
	P.haveChat = true;
	p.text[sizeof p.text - 1] = 0;

	char clean[96];
	int k = 0;
	for(int i = 0; p.text[i] && k < 95; i++)
		if((unsigned char)p.text[i] >= 32 && (unsigned char)p.text[i] < 127) clean[k++] = p.text[i];
	clean[k] = 0;

	if(state == HOST) {
		for(int i = 1; i < MAX_PLAYERS; i++)
			if(i != src && peers[i].used) SendChatTo(peers[i], clean);
	}
	int next = (chatHead + 1) % CHATQ;
	if(next == chatTail) return;
	memcpy(chatQ[chatHead], clean, sizeof clean);
	chatHead = next;
}

static void
CleanName(const char *in, char *out)
{
	int k = 0;
	for(int i = 0; i < 15 && in[i]; i++)
		if((unsigned char)in[i] >= 32 && (unsigned char)in[i] < 127) out[k++] = in[i];
	out[k] = 0;
}

static void
HandleName(int slot, const char *buf, int n)
{
	if(n < (int)sizeof(NamePacket)) return;
	NamePacket p;
	memcpy(&p, buf, sizeof p);
	p.name[sizeof p.name - 1] = 0;
	char nm[16];
	CleanName(p.name, nm);
	if(strcmp(nm, rp[slot].name) != 0) {
		memcpy(rp[slot].name, nm, sizeof nm);
		rosterDirty = true;
	}
}

static void
DropPeer(int slot)
{
	peers[slot].used = false;
	ResetRemotePlayer(slot);
	memset(&gv[slot], 0, sizeof gv[slot]);
	rosterDirty = true;
	if(NumPeers() == 0) recvCount = 0;
}

static void
HandleJoin(const sockaddr_in &from, const char *buf, int n, double now)
{
	int slot = FindPeerSlot(from);
	if(slot < 0) {
		for(int i = 1; i < MAX_PLAYERS; i++)
			if(!peers[i].used) {
				slot = i;
				break;
			}
		if(slot < 0) {
			Header h = MkHeader(PKT_FULL);
			sendto(sock, (const char *)&h, sizeof h, 0, (const sockaddr *)&from, sizeof from);
			return;
		}
		memset(&peers[slot], 0, sizeof peers[slot]);
		peers[slot].used = true;
		peers[slot].addr = from;
		ResetRemotePlayer(slot);
		memset(&gv[slot], 0, sizeof gv[slot]);
		rp[slot].active = true;
		if(n >= (int)sizeof(JoinPacket)) {
			JoinPacket jp;
			memcpy(&jp, buf, sizeof jp);
			jp.name[sizeof jp.name - 1] = 0;
			CleanName(jp.name, rp[slot].name);
		}
		rosterDirty = true;
	}
	peers[slot].lastRx = now;

	WelcomePacket w;
	memset(&w, 0, sizeof w);
	w.h.magic = MAGIC;
	w.h.type = PKT_WELCOME;
	w.slot = (uint8_t)slot;
	w.maxPlayers = (uint8_t)MAX_PLAYERS;
	SwapHeader(w.h);
	SendToPeer(peers[slot], &w, sizeof w);
}

static void
HandleRoster(const char *buf, int n)
{
	if(n < (int)sizeof(RosterPacket)) return;
	RosterPacket p;
	memcpy(&p, buf, sizeof p);
	for(int i = 0; i < MAX_PLAYERS; i++) {
		if(i == localSlot) continue;
		RemotePlayer &R = rp[i];
		bool was = R.active;
		R.active = p.e[i].active != 0;
		R.shared = p.e[i].shared != 0;
		p.e[i].name[sizeof p.e[i].name - 1] = 0;
		CleanName(p.e[i].name, R.name);
		if(was && !R.active) ResetRemotePlayer(i);
	}
}

static void
AcceptPos(int slot, const PosPacket &pk, double now)
{
	RemotePlayer &R = rp[slot];
	R.lastRx = now;
	R.x = pk.x;
	R.y = pk.y;
	R.z = pk.z;
	R.havePos = true;
	R.statCount++;
	PushSnap(R, pk, now);
}

static void
RelayPos(int fromSlot, const char *buf, int n, double now)
{
	Peer &src = peers[fromSlot];
	if(now - src.lastRelay < RELAY_MIN_INTERVAL) return;
	src.lastRelay = now;
	PosPacket tmp;
	memset(&tmp, 0, sizeof tmp);
	memcpy(&tmp, buf, n < (int)sizeof tmp ? n : (int)sizeof tmp);
	tmp.slot = (uint8_t)fromSlot;
	SendToAll(&tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp, fromSlot);
}

static void
Receive()
{
	for(;;) {
		char buf[2048];
		sockaddr_in from;
		int fromLen = sizeof from;
		int n = recvfrom(sock, buf, sizeof buf, 0, (sockaddr *)&from, &fromLen);
		if(n <= 0) {
			int e = n < 0 ? WSAGetLastError() : 0;
			static int errLogged = 0;
			if(e != 0 && e != WSAEWOULDBLOCK && errLogged < 20) {
				errLogged++;
				CoopLog("recvfrom error %d", e);
			}
			break;
		}
		{
			static int rxLogged = 0;
			if(rxLogged < 30) {
				rxLogged++;
				const unsigned char *fb = (const unsigned char *)&from.sin_addr;
				CoopLog("recvfrom %d bytes from %u.%u.%u.%u:%u", n, fb[0], fb[1], fb[2], fb[3], (unsigned)ntohs(from.sin_port));
			}
		}
		if(n < (int)sizeof(Header)) continue;
		Header hdr;
		memcpy(&hdr, buf, sizeof hdr);
		SwapHeader(hdr);
		Header *h = &hdr;
		if(h->magic != MAGIC) continue;
		double now = NowSec();

		int src = -1;
		if(state == HOST)
			src = FindPeerSlot(from);
		else if(state == CLIENT && peers[0].used && SameAddr(from, peers[0].addr))
			src = 0;
		if(src > 0)
			peers[src].lastRx = now;
		else if(src == 0)
			peers[0].lastRx = now;
		if(h->type != PKT_POS) COOP_CRUMB("Rx", (int)h->type, src);

		if(h->type == PKT_DISCOVER && state == HOST) {
			InfoPacket p;
			memset(&p, 0, sizeof p);
			p.h.magic = MAGIC;
			p.h.type = PKT_INFO;
			char name[32] = "Vice City host";
			DWORD sz = sizeof name;
			GetComputerNameA(name, &sz);
			strncpy(p.name, GetLocalName()[0] ? GetLocalName() : name, sizeof p.name - 1);
			p.players = (uint8_t)(1 + NumPeers());
			p.maxPlayers = (uint8_t)MAX_PLAYERS;
			SwapHeader(p.h);
			sendto(sock, (const char *)&p, sizeof p, 0, (sockaddr *)&from, fromLen);
		} else if(h->type == PKT_INFO && state == BROWSING && n >= (int)sizeof(InfoPacket)) {
			InfoPacket *p = (InfoPacket *)buf;
			p->name[sizeof p->name - 1] = 0;
			AddServer(from, p->name, p->players, p->maxPlayers);
		} else if(h->type == PKT_JOIN && state == HOST) {
			HandleJoin(from, buf, n, now);
		} else if(h->type == PKT_WELCOME && state == CLIENT && src == 0 && n >= (int)sizeof(WelcomePacket)) {
			WelcomePacket *w = (WelcomePacket *)buf;
			if(w->maxPlayers != MAX_PLAYERS || w->slot == 0 || w->slot >= MAX_PLAYERS) {
				int theirs = w->maxPlayers;
				Stop();
				snprintf(note, sizeof note, "Host runs %d players, this build supports %d", theirs, (int)MAX_PLAYERS);
				return;
			}
			if(!welcomed) {
				welcomed = true;
				localSlot = w->slot;
				snprintf(note, sizeof note, "Connected as player %d", localSlot + 1);
			}
		} else if(h->type == PKT_FULL && state == CLIENT && src == 0) {
			Stop();
			snprintf(note, sizeof note, "The host is full (%d players)", (int)MAX_PLAYERS);
			return;
		} else if(h->type == PKT_LEAVE) {
			if(state == HOST && src > 0) {
				DropPeer(src);
			} else if(state == CLIENT && src == 0) {
				Stop();
				snprintf(note, sizeof note, "The host closed the connection");
				return;
			}
		} else if(h->type == PKT_POS && n >= POS_HEADER_SIZE) {
			PosPacket pk;
			memset(&pk, 0, sizeof pk);
			memcpy(&pk, buf, n < (int)sizeof pk ? n : (int)sizeof pk);
			SwapPosFixed(pk);
			if(pk.numAnims > MAX_ANIMS || n < POS_HEADER_SIZE + (int)pk.numAnims * (int)sizeof(AnimEntry)) continue;
			SwapAnims(pk.anims, pk.numAnims);

			if(state == HOST) {
				if(src < 0) {
					Header lv = MkHeader(PKT_LEAVE);
					sendto(sock, (const char *)&lv, sizeof lv, 0, (sockaddr *)&from, fromLen);
					continue;
				}
				AcceptPos(src, pk, now);
				recvCount++;
				RelayPos(src, buf, n, now);
			} else if(state == CLIENT && src == 0 && welcomed) {
				int slot = pk.slot;
				if(slot >= MAX_PLAYERS || slot == localSlot) continue;
				AcceptPos(slot, pk, now);
				if(slot == 0) recvCount++;
			}
		} else if(state == HOST && src < 0) {
			if(h->type == PKT_GVEH || h->type == PKT_HIT || h->type == PKT_BLAST || h->type == PKT_CHAT || h->type == PKT_NAME) {
				Header lv = MkHeader(PKT_LEAVE);
				sendto(sock, (const char *)&lv, sizeof lv, 0, (sockaddr *)&from, fromLen);
			}
		} else if(state == HOST && h->type == PKT_GVEH) {
			HandleGVeh(src, buf, n, now);
		} else if(h->type == PKT_HIT && src >= 0 && (state == HOST || state == CLIENT)) {
			HandleHit(src, buf, n);
		} else if(h->type == PKT_BLAST && src >= 0 && (state == HOST || state == CLIENT)) {
			HandleBlast(src, buf, n);
		} else if(h->type == PKT_CHAT && src >= 0 && (state == HOST || state == CLIENT)) {
			HandleChat(src, buf, n);
		} else if(h->type == PKT_NAME && state == HOST) {
			HandleName(src, buf, n);
		} else if(state == CLIENT && src == 0) {
			if(h->type == PKT_ROSTER) {
				HandleRoster(buf, n);
			} else if(h->type == PKT_WORLD && n >= (int)sizeof(WorldPacket)) {
				WorldPacket wp;
				memcpy(&wp, buf, sizeof wp);
				SwapWorldPacket(wp);
				latestWorld = wp.w;
				worldFresh = true;
			} else if(h->type == PKT_VEHS) {
				HandleVehs(buf, n, now);
			} else if(h->type == PKT_PEDS) {
				HandlePeds(buf, n, now);
			} else if(h->type == PKT_FIRES) {
				HandleFires(buf, n);
			}
		}
	}
}

static void
UpdateStats(double now)
{
	for(int i = 0; i < MAX_PLAYERS; i++) {
		RemotePlayer &R = rp[i];
		if(now - R.statStart >= 1.0) {
			R.pps = (float)(R.statCount / (now - R.statStart));
			R.statCount = 0;
			R.statStart = now;
		}
	}
}

static void
BuildPos(PosPacket &p, const PlayerState &local, double now, int slot)
{
	memset(&p, 0, sizeof p);
	p.h.magic = MAGIC;
	p.h.type = PKT_POS;
	p.seq = sendSeq++;
	p.timeMs = (uint32_t)((now - sessionStart) * 1000.0);
	p.slot = (uint8_t)slot;
	p.x = local.x;
	p.y = local.y;
	p.z = local.z;
	p.heading = local.heading;
	p.moveState = local.moveState;
	p.flags = local.flags;
	p.seat = local.seat;
	p.vehId = local.vehId;
	p.weapon = local.weapon;
	p.shots = local.shots;
	p.aiming = local.aiming;
	p.looking = local.looking;
	p.aimYaw = local.aimYaw;
	p.aimPitch = local.aimPitch;
	int na = local.numAnims > MAX_ANIMS ? MAX_ANIMS : local.numAnims;
	p.numAnims = (uint8_t)na;
	memcpy(p.anims, local.anims, na * sizeof(AnimEntry));
}

static void
SendRoster()
{
	RosterPacket r;
	memset(&r, 0, sizeof r);
	r.h.magic = MAGIC;
	r.h.type = PKT_ROSTER;
	for(int i = 0; i < MAX_PLAYERS; i++) {
		if(i == 0) {
			r.e[0].active = 1;
			r.e[0].shared = 1;
			CleanName(GetLocalName(), r.e[0].name);
		} else {
			r.e[i].active = peers[i].used ? 1 : 0;
			r.e[i].shared = rp[i].shared ? 1 : 0;
			memcpy(r.e[i].name, rp[i].name, sizeof r.e[i].name);
		}
	}
	SwapHeader(r.h);
	SendToAll(&r, sizeof r);
}

void
Pump(const PlayerState &local)
{
	static int lastLogState = -1;
	if((int)state != lastLogState) {
		lastLogState = (int)state;
		CoopLog("Pump: state now %d (0 off, 1 host, 2 client, 3 browsing)", (int)state);
	}
	if(state == OFF) return;
	double now = NowSec();
	COOP_CRUMB("Net::Pump");
	Receive();
	if(state == OFF) return;
	UpdateStats(now);

	if(state == HOST) {
		for(int i = 1; i < MAX_PLAYERS; i++)
			if(peers[i].used && now - peers[i].lastRx > TIMEOUT) DropPeer(i);
	}

	if(state == BROWSING) {
		if(now - lastDiscoverSec >= 2.0) {
			Header h = MkHeader(PKT_DISCOVER);
			sockaddr_in b;
			memset(&b, 0, sizeof b);
			b.sin_family = AF_INET;
			b.sin_port = htons(PORT);
			b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
			sendto(sock, (const char *)&h, sizeof h, 0, (sockaddr *)&b, sizeof b);
			lastDiscoverSec = now;
		}
		for(int i = 0; i < numServers;) {
			if(now - seen[i] > 7.0) {
				servers[i] = servers[numServers - 1];
				seen[i] = seen[numServers - 1];
				numServers--;
			} else {
				i++;
			}
		}
		return;
	}

	if(state == CLIENT && !welcomed) {
		if(now - lastJoinSec >= JOIN_INTERVAL) {
			JoinPacket jp;
			memset(&jp, 0, sizeof jp);
			jp.h.magic = MAGIC;
			jp.h.type = PKT_JOIN;
			CleanName(GetLocalName(), jp.name);
			SwapHeader(jp.h);
			SendToPeer(peers[0], &jp, sizeof jp);
			lastJoinSec = now;
		}
		return;
	}

	bool connected = state == HOST ? NumPeers() > 0 : peers[0].used;
	if(connected && now - lastSendSec >= SEND_MIN_INTERVAL) {
		PosPacket p;
		BuildPos(p, local, now, localSlot);
		int size = POS_HEADER_SIZE + p.numAnims * (int)sizeof(AnimEntry);
		SwapPos(p);
		if(state == HOST)
			SendToAll(&p, size);
		else
			SendToPeer(peers[0], &p, size);
		lastSendSec = now;
	}

	if(state == CLIENT && now - lastNameSend >= 1.0) {
		NamePacket np;
		memset(&np, 0, sizeof np);
		np.h.magic = MAGIC;
		np.h.type = PKT_NAME;
		strncpy(np.name, GetLocalName(), sizeof np.name - 1);
		SwapHeader(np.h);
		SendToPeer(peers[0], &np, sizeof np);
		lastNameSend = now;
	}

	if(state == HOST && NumPeers() > 0 && (rosterDirty || now - lastRosterSec >= 1.0)) {
		SendRoster();
		lastRosterSec = now;
		rosterDirty = false;
	}
}

void
SendWorld(int slot, const WorldNet &w)
{
	if(state != HOST || slot < 1 || slot >= MAX_PLAYERS || !peers[slot].used) return;
	WorldPacket p;
	memset(&p, 0, sizeof p);
	p.h.magic = MAGIC;
	p.h.type = PKT_WORLD;
	p.seq = peers[slot].entTx++;
	p.w = w;
	SwapWorldPacket(p);
	SendToPeer(peers[slot], &p, sizeof p);
}

static void
SendBatch(int slot, uint32_t type, const void *items, int n, int itemSize, int perPkt)
{
	if(state != HOST || slot < 1 || slot >= MAX_PLAYERS || !peers[slot].used || n <= 0) return;
	uint32_t timeMs = (uint32_t)((NowSec() - sessionStart) * 1000.0);
	const char *src = (const char *)items;
	for(int i = 0; i < n; i += perPkt) {
		int c = (n - i) < perPkt ? (n - i) : perPkt;
		char buf[1500];
		EntHeader eh;
		eh.h.magic = MAGIC;
		eh.h.type = type;
		eh.seq = peers[slot].entTx++;
		eh.timeMs = timeMs;
		eh.count = (uint32_t)c;
		SwapEntHeader(eh);
		memcpy(buf, &eh, sizeof eh);
		memcpy(buf + sizeof eh, src + (size_t)i * itemSize, (size_t)c * itemSize);
		for(int k = 0; k < c; k++) {
			char *it = buf + sizeof eh + (size_t)k * itemSize;
			if(type == PKT_VEHS)
				SwapVehAt(it);
			else
				SwapPedAt(it);
		}
		SendToPeer(peers[slot], buf, (int)(sizeof eh + (size_t)c * itemSize));
	}
}

void
SendVehicles(int slot, const VehNet *v, int n)
{ SendBatch(slot, PKT_VEHS, v, n, sizeof(VehNet), VEHS_PER_PKT); }
void
SendPeds(int slot, const PedNet *p, int n)
{ SendBatch(slot, PKT_PEDS, p, n, sizeof(PedNet), PEDS_PER_PKT); }

void
SendFires(int slot, const FireNet *f, int n)
{
	if(state != HOST || slot < 1 || slot >= MAX_PLAYERS || !peers[slot].used) return;
	if(n > MAX_FIRES) n = MAX_FIRES;
	if(n < 0) n = 0;
	char buf[sizeof(EntHeader) + MAX_FIRES * sizeof(FireNet)];
	EntHeader eh;
	eh.h.magic = MAGIC;
	eh.h.type = PKT_FIRES;
	eh.seq = peers[slot].entTx++;
	eh.timeMs = (uint32_t)((NowSec() - sessionStart) * 1000.0);
	eh.count = (uint32_t)n;
	SwapEntHeader(eh);
	memcpy(buf, &eh, sizeof eh);
	if(n > 0) memcpy(buf + sizeof eh, f, (size_t)n * sizeof(FireNet));
	for(int k = 0; k < n; k++) SwapFireAt(buf + sizeof eh + (size_t)k * sizeof(FireNet));
	SendToPeer(peers[slot], buf, (int)(sizeof eh + (size_t)n * sizeof(FireNet)));
}

void
SendHit(const HitNet &hn)
{
	HitNet h = hn;
	if(state == HOST) {
		h.srcSlot = 0;
		if(h.dstSlot >= 1 && h.dstSlot < MAX_PLAYERS && peers[h.dstSlot].used) SendHitTo(peers[h.dstSlot], h);
	} else if(state == CLIENT && welcomed) {
		h.srcSlot = (uint8_t)localSlot;
		SendHitTo(peers[0], h);
	}
}

void
SendBlast(const BlastNet &bn)
{
	BlastNet b = bn;
	if(state == HOST) {
		b.srcSlot = 0;
		for(int i = 1; i < MAX_PLAYERS; i++)
			if(peers[i].used) SendBlastTo(peers[i], b);
	} else if(state == CLIENT && welcomed) {
		b.srcSlot = (uint8_t)localSlot;
		SendBlastTo(peers[0], b);
	}
}

bool
GetBlast(BlastNet &out)
{
	if(blastTail == blastHead) return false;
	out = blastQ[blastTail];
	blastTail = (blastTail + 1) % BLASTQ;
	return true;
}

void
SendChat(const char *line)
{
	if(line == nullptr) return;
	if(state == HOST) {
		for(int i = 1; i < MAX_PLAYERS; i++)
			if(peers[i].used) SendChatTo(peers[i], line);
	} else if(state == CLIENT && welcomed) {
		SendChatTo(peers[0], line);
	}
}

bool
GetChat(char *out, int n)
{
	if(chatTail == chatHead || n <= 0) return false;
	strncpy(out, chatQ[chatTail], n - 1);
	out[n - 1] = 0;
	chatTail = (chatTail + 1) % CHATQ;
	return true;
}

static void
NameFilePath(char *out, size_t n)
{ MakePath(out, n, "coopname.txt"); }

void
SetLocalName(const char *name)
{
	int k = 0;
	for(int i = 0; name && name[i] && k < 15; i++)
		if((unsigned char)name[i] >= 32 && (unsigned char)name[i] < 127) localName[k++] = name[i];
	localName[k] = 0;
	char path[MAX_PATH];
	NameFilePath(path, sizeof path);
	FILE *f = fopen(path, "w");
	if(f) {
		fputs(localName, f);
		fclose(f);
	}
	lastNameSend = -10.0;
}

const char *
GetLocalName()
{
	static bool loaded = false;
	if(!loaded) {
		loaded = true;
		char path[MAX_PATH];
		NameFilePath(path, sizeof path);
		FILE *f = fopen(path, "r");
		if(f) {
			char b[64] = "";
			if(fgets(b, sizeof b, f)) {
				int k = 0;
				for(int i = 0; b[i] && k < 15; i++)
					if((unsigned char)b[i] >= 32 && (unsigned char)b[i] < 127) localName[k++] = b[i];
				localName[k] = 0;
			}
			fclose(f);
		}
		if(!localName[0]) {
			char cn[32] = "Player";
			DWORD sz = sizeof cn;
			GetComputerNameA(cn, &sz);
			strncpy(localName, cn, 15);
			localName[15] = 0;
		}
	}
	return localName;
}

#ifdef _XBOX
// ---- 360: no GDI, no keyboard, no mouse -------------------------------------
bool
RenderGlyph(char, uint8_t *, int &advance)
{
	advance = 0;
	return false; // use the game's own font renderer on 360
}

// ---- 360 text entry: the console's own on-screen keyboard -------------------------------------
// The game polls TextInput() for one character at a time while a text field (name / chat) is being
// edited, and calls TextInputReset() when it starts waiting for text. So: TextInputReset() arms the
// keyboard, the next TextInput() opens the pop-up, and when the user presses Done the result is fed
// back one character per call (backspaces first so an existing name is replaced, then the text,
// then Enter). Cancel feeds Escape.
static XOVERLAPPED kbOv;
static WCHAR kbResult[96];
static bool kbActive = false, kbArmed = false;
static char kbQueue[256];
static int kbQHead = 0, kbQTail = 0;

static void
KbPush(char c)
{
	int n = (kbQTail + 1) % (int)sizeof kbQueue;
	if(n == kbQHead) return;
	kbQueue[kbQTail] = c;
	kbQTail = n;
}

static bool
KbOpen()
{
	for(DWORD u = 0; u < 4; u++) {
		memset(&kbOv, 0, sizeof kbOv);
		memset(kbResult, 0, sizeof kbResult);
		DWORD r = XShowKeyboardUI(u, VKBD_DEFAULT, L"", L"Co-op", L"Type your text and press Done", kbResult, 96, &kbOv);
		if(r == ERROR_IO_PENDING) {
			CoopLog("keyboard UI opened for user %u", (unsigned)u);
			return true;
		}
		CoopLog("XShowKeyboardUI user %u -> %u", (unsigned)u, (unsigned)r);
	}
	return false;
}

void
TextInputReset()
{
	if(!kbActive) kbArmed = true;
}

char
TextInput()
{
	if(kbActive && XHasOverlappedIoCompleted(&kbOv)) {
		kbActive = false;
		DWORD ex = XGetOverlappedExtendedError(&kbOv);
		CoopLog("keyboard UI finished, result %u", (unsigned)ex);
		if(ex == ERROR_SUCCESS) {
			for(int i = 0; i < 40; i++) KbPush(8); // clear whatever the field already holds
			for(int i = 0; i < 95 && kbResult[i]; i++)
				if(kbResult[i] >= 32 && kbResult[i] < 127) KbPush((char)kbResult[i]);
			KbPush(13);
		} else {
			KbPush(27);
		}
	}
	if(!kbActive && kbQHead == kbQTail && kbArmed) {
		kbArmed = false;
		if(KbOpen())
			kbActive = true;
		else
			KbPush(27); // no keyboard available (no signed-in profile?): leave the field
	}
	if(kbQHead != kbQTail) {
		char c = kbQueue[kbQHead];
		kbQHead = (kbQHead + 1) % (int)sizeof kbQueue;
		return c;
	}
	return 0;
}

static float mvx = 0, mvy = 0;

void
MouseFrame(bool, float, float)
{}

void
GetMouse(float &x, float &y, bool &leftClick, bool &rightClick)
{
	x = mvx;
	y = mvy;
	leftClick = false;
	rightClick = false;
}
#elif defined(__ANDROID__)
// ---- Android: no GDI font, no keyboard, no mouse (touch is the game's own) ----
bool
RenderGlyph(char, uint8_t *, int &advance)
{
	advance = 0;
	return false; // set COOP_PLAINFONT to 0 in Net.h so the game font is used
}

void
TextInputReset()
{}

char
TextInput()
{ return 0; } // TODO: call the Android soft keyboard through JNI

static float mvx = 0, mvy = 0;

void
MouseFrame(bool, float, float)
{}

void
GetMouse(float &x, float &y, bool &leftClick, bool &rightClick)
{
	x = mvx;
	y = mvy;
	leftClick = false;
	rightClick = false;
}
#else
bool
RenderGlyph(char c, uint8_t *rgba, int &advance)
{
	static HDC dc = NULL;
	static HBITMAP bmp = NULL;
	static HFONT font = NULL;
	static void *bits = NULL;
	static bool failed = false;
	if(failed) return false;
	if(dc == NULL) {
		dc = CreateCompatibleDC(NULL);
		if(dc) {
			BITMAPINFO bi;
			memset(&bi, 0, sizeof bi);
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = 32;
			bi.bmiHeader.biHeight = -32;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
		}
		font = CreateFontA(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
		                   DEFAULT_PITCH | FF_SWISS, "Arial");
		if(dc == NULL || bmp == NULL || bits == NULL || font == NULL) {
			failed = true;
			return false;
		}
		SelectObject(dc, bmp);
		SelectObject(dc, font);
		SetBkMode(dc, OPAQUE);
		SetBkColor(dc, RGB(0, 0, 0));
		SetTextColor(dc, RGB(255, 255, 255));
	}
	memset(bits, 0, 32 * 32 * 4);
	RECT r = {0, 0, 32, 32};
	HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
	FillRect(dc, &r, black);
	TextOutA(dc, 2, 1, &c, 1);
	int w = 0;
	if(!GetCharWidth32A(dc, (UINT)(unsigned char)c, (UINT)(unsigned char)c, &w)) w = 12;
	advance = w;
	GdiFlush();
	const uint32_t *src = (const uint32_t *)bits;
	for(int i = 0; i < 32 * 32; i++) {
		uint32_t px = src[i];
		uint32_t b = px & 0xFF, g = (px >> 8) & 0xFF, rr = (px >> 16) & 0xFF;
		uint32_t a = b > g ? b : g;
		if(rr > a) a = rr;
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = (uint8_t)a;
	}
	return true;
}

static bool textPrev[256];

void
TextInputReset()
{
	for(int i = 0; i < 256; i++) textPrev[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
}

char
TextInput()
{
	if(!GameHasFocus()) return 0;
	bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	char result = 0;
	for(int vk = 8; vk < 256; vk++) {
		bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool pressed = down && !textPrev[vk];
		textPrev[vk] = down;
		if(!pressed || result) continue;
		if(vk == VK_BACK)
			result = 8;
		else if(vk == VK_RETURN)
			result = 13;
		else if(vk == VK_ESCAPE)
			result = 27;
		else if(vk >= 'A' && vk <= 'Z')
			result = shift ? (char)vk : (char)(vk + 32);
		else if(vk >= '0' && vk <= '9')
			result = shift ? ")!@#$%^&*("[vk - '0'] : (char)vk;
		else if(vk == VK_SPACE)
			result = ' ';
		else if(vk == VK_OEM_MINUS)
			result = shift ? '_' : '-';
		else if(vk == VK_OEM_PERIOD)
			result = shift ? '>' : '.';
		else if(vk == VK_OEM_COMMA)
			result = shift ? '<' : ',';
		else if(vk == VK_OEM_2)
			result = shift ? '?' : '/';
		else if(vk == VK_OEM_1)
			result = shift ? ':' : ';';
		else if(vk == VK_OEM_7)
			result = shift ? '"' : '\'';
		else if(vk == VK_OEM_PLUS)
			result = shift ? '+' : '=';
	}
	return result;
}
#endif // _XBOX

bool
GetHit(HitNet &out)
{
	if(hitTail == hitHead) return false;
	out = hitQ[hitTail];
	hitTail = (hitTail + 1) % HITQ;
	return true;
}

void
SendOwnedVehicle(const VehNet &v)
{
	if(state != CLIENT || !welcomed) return;
	char buf[sizeof(EntHeader) + sizeof(VehNet)];
	EntHeader eh;
	eh.h.magic = MAGIC;
	eh.h.type = PKT_GVEH;
	eh.seq = gvSeq++;
	eh.timeMs = (uint32_t)((NowSec() - sessionStart) * 1000.0);
	eh.count = 1;
	SwapEntHeader(eh);
	memcpy(buf, &eh, sizeof eh);
	memcpy(buf + sizeof eh, &v, sizeof v);
	SwapVehAt(buf + sizeof eh);
	SendToPeer(peers[0], buf, (int)sizeof buf);
}

bool
GetWorld(WorldNet &w)
{
	if(!worldFresh) return false;
	w = latestWorld;
	worldFresh = false;
	return true;
}

bool
GetFires(FireNet *out, int &n)
{
	if(!firesFresh) return false;
	n = numFires;
	if(numFires > 0) memcpy(out, fires, (size_t)numFires * sizeof(FireNet));
	firesFresh = false;
	return true;
}

static float
LerpAngle(float a, float b, float f)
{
	float d = b - a;
	while(d > PI_F) d -= 2.0f * PI_F;
	while(d < -PI_F) d += 2.0f * PI_F;
	return a + d * f;
}

static bool
Same(const AnimEntry &a, const AnimEntry &b)
{ return a.animId == b.animId && a.group == b.group; }

static void
AdvanceTimes(PlayerState &o, double dt)
{
	for(int i = 0; i < o.numAnims; i++) {
		float t = o.anims[i].time + (float)(dt * (o.anims[i].speed / 64.0f));
		o.anims[i].time = t < 0.0f ? 0.0f : t;
	}
}

bool
GetRemoteInterp(int slot, PlayerState &out)
{
	if(state != HOST && state != CLIENT) return false;
	if(slot < 0 || slot >= MAX_PLAYERS || slot == localSlot) return false;
	RemotePlayer &R = rp[slot];
	if(R.numSnaps == 0) return false;
	double now = NowSec();
	if(now - R.lastRx > TIMEOUT) return false;

	double rt = now - R.clockOffset - R.curDelay;
	const Snap &oldest = R.snaps[0];
	const Snap &newest = R.snaps[R.numSnaps - 1];

	if(R.numSnaps == 1 || rt <= oldest.t) {
		out = oldest.s;
		return true;
	}

	if(rt >= newest.t) {
		out = newest.s;
		double ex = rt - newest.t;
		if(ex > MAX_EXTRAP) ex = MAX_EXTRAP;
		const Snap &prev = R.snaps[R.numSnaps - 2];
		double dt = newest.t - prev.t;
		float dx = newest.s.x - prev.s.x, dy = newest.s.y - prev.s.y, dz = newest.s.z - prev.s.z;
		if(dt > 0.001 && (dx * dx + dy * dy + dz * dz) < 400.0f) {
			out.x += (float)(dx / dt * ex);
			out.y += (float)(dy / dt * ex);
			out.z += (float)(dz / dt * ex);
		}
		AdvanceTimes(out, ex);
		return true;
	}

	int b = 1;
	while(b < R.numSnaps - 1 && R.snaps[b].t < rt) b++;
	const Snap &sa = R.snaps[b - 1];
	const Snap &sb = R.snaps[b];
	double span = sb.t - sa.t;
	float f = span > 1e-6 ? (float)((rt - sa.t) / span) : 1.0f;
	if(f < 0.0f) f = 0.0f;
	if(f > 1.0f) f = 1.0f;

	float dx = sb.s.x - sa.s.x, dy = sb.s.y - sa.s.y, dz = sb.s.z - sa.s.z;
	if(dx * dx + dy * dy + dz * dz > 400.0f) {
		out = sb.s;
		return true;
	}

	memset(&out, 0, sizeof out);
	out.x = sa.s.x + dx * f;
	out.y = sa.s.y + dy * f;
	out.z = sa.s.z + dz * f;
	out.heading = LerpAngle(sa.s.heading, sb.s.heading, f);
	out.moveState = sb.s.moveState;
	out.flags = sb.s.flags;
	out.seat = sb.s.seat;
	out.vehId = sb.s.vehId;
	out.weapon = sb.s.weapon;
	out.shots = sb.s.shots;
	out.aiming = sb.s.aiming;
	out.looking = sb.s.looking;
	out.aimYaw = LerpAngle(sa.s.aimYaw, sb.s.aimYaw, f);
	out.aimPitch = sa.s.aimPitch + (sb.s.aimPitch - sa.s.aimPitch) * f;

	int n = 0;
	double dtA = rt - sa.t;
	for(int i = 0; i < sa.s.numAnims && n < MAX_ANIMS; i++) {
		AnimEntry e = sa.s.anims[i];
		float bA = (float)e.blend, bB = 0.0f;
		float tA = e.time, tB = e.time;
		bool both = false;
		for(int j = 0; j < sb.s.numAnims; j++) {
			if(Same(e, sb.s.anims[j])) {
				bB = (float)sb.s.anims[j].blend;
				tB = sb.s.anims[j].time;
				both = true;
				break;
			}
		}
		float bl = bA + (bB - bA) * f;
		e.blend = (uint8_t)(bl < 0.0f ? 0.0f : (bl > 255.0f ? 255.0f : bl + 0.5f));
		e.time += (float)(dtA * (e.speed / 64.0f));
		if(both) {
			float adv = (float)(span * (e.speed / 64.0f));
			float dT = fabsf(tB - tA), dev = fabsf(tB - (tA + adv));
			if((e.speed < 4 && dT < 0.5f) || (dev > 0.02f && dT < 0.3f)) e.time = tA + (tB - tA) * f;
		}
		out.anims[n++] = e;
	}
	double dtB = sb.t - rt;
	for(int j = 0; j < sb.s.numAnims && n < MAX_ANIMS; j++) {
		AnimEntry e = sb.s.anims[j];
		bool inA = false;
		for(int i = 0; i < sa.s.numAnims; i++) {
			if(Same(e, sa.s.anims[i])) {
				inA = true;
				break;
			}
		}
		if(inA) continue;
		e.blend = (uint8_t)(e.blend * f + 0.5f);
		float t = e.time - (float)(dtB * (e.speed / 64.0f));
		e.time = t < 0.0f ? 0.0f : t;
		out.anims[n++] = e;
	}
	out.numAnims = (uint8_t)n;
	return true;
}

static void
Norm3(float &x, float &y, float &z)
{
	float l = sqrtf(x * x + y * y + z * z);
	if(l > 1e-6f) {
		x /= l;
		y /= l;
		z /= l;
	}
}

// Interpolation delay per entity: at least ENT_DELAY, but never less than 1.5 snapshot intervals, so a
// slower sender (or a busy frame) does not leave the renderer waiting for a snapshot that is not there yet.
// That "ran out of snapshots" case is what made the lean and steering values step instead of glide.
template <class T>
static double
EntDelay(const T &e)
{
	double d = ENT_DELAY;
	if(e.n >= 2) {
		double avg = (e.t[e.n - 1] - e.t[0]) / (double)(e.n - 1);
		d = avg * 1.5;
		if(d < ENT_DELAY) d = ENT_DELAY;
		if(d > 0.25) d = 0.25;
	}
	return d;
}

static RemoteVeh
InterpVeh(const VEnt &e, double rt)
{
	int n = e.n;
	RemoteVeh o = e.s[n - 1];
	if(n < 2) return o;
	if(rt <= e.t[0]) return e.s[0];

	if(rt >= e.t[n - 1]) {
		const RemoteVeh &a = e.s[n - 2], &b = e.s[n - 1];
		double dt = e.t[n - 1] - e.t[n - 2];
		double ex = rt - e.t[n - 1];
		if(ex > MAX_EXTRAP) ex = MAX_EXTRAP;
		float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
		if(dt > 1e-4 && dx * dx + dy * dy + dz * dz < 900.0f) {
			o.x = b.x + (float)(dx / dt * ex);
			o.y = b.y + (float)(dy / dt * ex);
			o.z = b.z + (float)(dz / dt * ex);
		}
		if(dt > 1e-4) {
			// keep the lean / steering channels moving instead of freezing until the next packet
			float k = (float)(ex / dt);
			if(k > 1.0f) k = 1.0f;
			o.lean = b.lean + (b.lean - a.lean) * k;
			o.pedLR = b.pedLR + (b.pedLR - a.pedLR) * k;
			o.pedUD = b.pedUD + (b.pedUD - a.pedUD) * k;
			o.leanIn = b.leanIn + (b.leanIn - a.leanIn) * k;
			o.steer = b.steer + (b.steer - a.steer) * k;
		}
		return o;
	}

	int i = 0;
	while(i < n - 2 && e.t[i + 1] < rt) i++;
	const RemoteVeh &a = e.s[i], &b = e.s[i + 1];
	double span = e.t[i + 1] - e.t[i];
	float f = span > 1e-6 ? (float)((rt - e.t[i]) / span) : 1.0f;
	if(f < 0.0f) f = 0.0f;
	if(f > 1.0f) f = 1.0f;

	float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	if(dx * dx + dy * dy + dz * dz > 900.0f) return b;

	o = b;
	o.x = a.x + dx * f;
	o.y = a.y + dy * f;
	o.z = a.z + dz * f;
	o.fx = a.fx + (b.fx - a.fx) * f;
	o.fy = a.fy + (b.fy - a.fy) * f;
	o.fz = a.fz + (b.fz - a.fz) * f;
	o.ux = a.ux + (b.ux - a.ux) * f;
	o.uy = a.uy + (b.uy - a.uy) * f;
	o.uz = a.uz + (b.uz - a.uz) * f;
	o.vx = a.vx + (b.vx - a.vx) * f;
	o.vy = a.vy + (b.vy - a.vy) * f;
	o.vz = a.vz + (b.vz - a.vz) * f;
	o.steer = a.steer + (b.steer - a.steer) * f;
	o.lean = a.lean + (b.lean - a.lean) * f;
	o.gas = a.gas + (b.gas - a.gas) * f;
	o.brake = a.brake + (b.brake - a.brake) * f;
	o.pedLR = a.pedLR + (b.pedLR - a.pedLR) * f;
	o.pedUD = a.pedUD + (b.pedUD - a.pedUD) * f;
	o.leanIn = a.leanIn + (b.leanIn - a.leanIn) * f;
	Norm3(o.fx, o.fy, o.fz);
	Norm3(o.ux, o.uy, o.uz);
	return o;
}

static PedNet
InterpPed(const PEnt &e, double rt)
{
	int n = e.n;
	PedNet o = e.s[n - 1];
	if(n < 2) return o;
	if(rt <= e.t[0]) return e.s[0];

	if(rt >= e.t[n - 1]) {
		const PedNet &a = e.s[n - 2], &b = e.s[n - 1];
		double dt = e.t[n - 1] - e.t[n - 2];
		double ex = rt - e.t[n - 1];
		if(ex > MAX_EXTRAP) ex = MAX_EXTRAP;
		float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
		if(dt > 1e-4 && dx * dx + dy * dy + dz * dz < 900.0f) {
			o.x = b.x + (float)(dx / dt * ex);
			o.y = b.y + (float)(dy / dt * ex);
			o.z = b.z + (float)(dz / dt * ex);
		}
		return o;
	}

	int i = 0;
	while(i < n - 2 && e.t[i + 1] < rt) i++;
	const PedNet &a = e.s[i], &b = e.s[i + 1];
	double span = e.t[i + 1] - e.t[i];
	float f = span > 1e-6 ? (float)((rt - e.t[i]) / span) : 1.0f;
	if(f < 0.0f) f = 0.0f;
	if(f > 1.0f) f = 1.0f;

	float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	if(dx * dx + dy * dy + dz * dz > 900.0f) return b;

	o = b;
	o.x = a.x + dx * f;
	o.y = a.y + dy * f;
	o.z = a.z + dz * f;
	o.heading = LerpAngle(a.heading, b.heading, f);
	return o;
}

int
GetRemoteVehicles(RemoteVeh *out, int max)
{
	if(state != CLIENT) return 0;
	double now = NowSec();
	double rt = now - rp[0].clockOffset - ENT_DELAY;
	int c = 0;
	for(int i = 0; i < MAX_VENTS; i++) {
		VEnt &e = vents[i];
		if(!e.used || e.n == 0) continue;
		if(now - e.lastRx > ENT_TIMEOUT) {
			e.used = false;
			continue;
		}
		if(c >= max) break;
		out[c++] = rp[0].haveOffset ? InterpVeh(e, now - rp[0].clockOffset - EntDelay(e)) : e.s[e.n - 1];
	}
	return c;
}

bool
GetGuestVehicle(int slot, RemoteVeh &out)
{
	if(state != HOST || slot < 1 || slot >= MAX_PLAYERS) return false;
	VEnt &g = gv[slot];
	if(!g.used || g.n == 0) return false;
	double now = NowSec();
	if(now - g.lastRx > ENT_TIMEOUT) return false;
	out = rp[slot].haveOffset ? InterpVeh(g, now - rp[slot].clockOffset - EntDelay(g)) : g.s[g.n - 1];
	return true;
}

int
GetRemotePeds(PedNet *out, int max)
{
	if(state != CLIENT) return 0;
	double now = NowSec();
	double rt = now - rp[0].clockOffset - ENT_DELAY;
	int c = 0;
	for(int i = 0; i < MAX_PENTS; i++) {
		PEnt &e = pents[i];
		if(!e.used || e.n == 0) continue;
		if(now - e.lastRx > ENT_TIMEOUT) {
			e.used = false;
			continue;
		}
		if(c >= max) break;
		out[c++] = rp[0].haveOffset ? InterpPed(e, now - rp[0].clockOffset - EntDelay(e)) : e.s[e.n - 1];
	}
	return c;
}

void
GetNetStats(int slot, float &bufferMs, float &packetsPerSec)
{
	if(slot < 0 || slot >= MAX_PLAYERS) slot = 0;
	bufferMs = (float)(rp[slot].curDelay * 1000.0);
	packetsPerSec = rp[slot].pps;
}

void
GetNetStats(float &bufferMs, float &packetsPerSec)
{
	if(state != HOST) {
		GetNetStats(0, bufferMs, packetsPerSec);
		return;
	}
	bufferMs = 0.0f;
	packetsPerSec = 0.0f;
	for(int i = 1; i < MAX_PLAYERS; i++) {
		if(!peers[i].used) continue;
		float b = (float)(rp[i].curDelay * 1000.0);
		if(b > bufferMs) bufferMs = b;
		packetsPerSec += rp[i].pps;
	}
}

#if !defined(_XBOX) && !defined(__ANDROID__)
static float mvx = 0, mvy = 0;
static bool mPinned = false, mPrevL = false, mPrevR = false;
static bool mClickL = false, mClickR = false;

void
MouseFrame(bool menuOpen, float scrW, float scrH)
{
	mClickL = mClickR = false;
	if(!menuOpen || !GameHasFocus()) {
		mPinned = false;
		return;
	}

	HWND hw = GetForegroundWindow();
	RECT rc;
	if(!GetClientRect(hw, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
	POINT c = {rc.right / 2, rc.bottom / 2};
	ClientToScreen(hw, &c);
	POINT p;
	GetCursorPos(&p);

	bool l = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	bool r = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

	if(!mPinned) {
		mvx = scrW * 0.5f;
		mvy = scrH * 0.5f;
		mPinned = true;
	} else {
		mvx += (p.x - c.x) * scrW / (float)rc.right;
		mvy += (p.y - c.y) * scrH / (float)rc.bottom;
		if(mvx < 0) mvx = 0;
		if(mvy < 0) mvy = 0;
		if(mvx > scrW) mvx = scrW;
		if(mvy > scrH) mvy = scrH;
		mClickL = l && !mPrevL;
		mClickR = r && !mPrevR;
	}
	mPrevL = l;
	mPrevR = r;

	SetCursorPos(c.x, c.y);
}

void
GetMouse(float &x, float &y, bool &leftClick, bool &rightClick)
{
	x = mvx;
	y = mvy;
	leftClick = mClickL;
	rightClick = mClickR;
}
#endif

State
GetState()
{ return state; }
int
NumServers()
{ return numServers; }
const Server *
GetServer(int i)
{ return (i >= 0 && i < numServers) ? &servers[i] : nullptr; }
bool
HasPeer()
{ return state == HOST ? NumPeers() > 0 : (state == CLIENT && peers[0].used); }
uint32_t
PacketsReceived()
{ return recvCount; }
const char *
Note()
{ return note; }

int
GetLocalSlot()
{
	if(state == HOST) return 0;
	if(state == CLIENT && welcomed) return localSlot;
	return -1;
}

int
MaxPlayers()
{ return MAX_PLAYERS; }

bool
IsSlotActive(int slot)
{
	if(slot < 0 || slot >= MAX_PLAYERS) return false;
	if(state == HOST) return slot > 0 && peers[slot].used;
	if(state == CLIENT && welcomed) return slot != localSlot && (slot == 0 || rp[slot].active);
	return false;
}

int
NumRemotePlayers()
{
	int c = 0;
	for(int i = 0; i < MAX_PLAYERS; i++)
		if(IsSlotActive(i)) c++;
	return c;
}

bool
IsRemoteShared(int slot)
{
	if(slot < 0 || slot >= MAX_PLAYERS || !IsSlotActive(slot)) return false;
	if(state == CLIENT && slot == 0) return true;
	return rp[slot].shared;
}

void
SetPeerShared(int slot, bool shared)
{
	if(state != HOST || slot < 1 || slot >= MAX_PLAYERS) return;
	if(rp[slot].shared != shared) {
		rp[slot].shared = shared;
		rosterDirty = true;
	}
}

bool
GetRemote(int slot, float &x, float &y, float &z)
{
	if(slot < 0 || slot >= MAX_PLAYERS || !rp[slot].havePos) return false;
	x = rp[slot].x;
	y = rp[slot].y;
	z = rp[slot].z;
	return true;
}

const char *
GetRemoteName(int slot)
{ return (slot >= 0 && slot < MAX_PLAYERS) ? rp[slot].name : ""; }

static volatile LONG guardDepth = 0;

void
GuardBegin()
{ InterlockedIncrement(&guardDepth); }

void
GuardEnd()
{ InterlockedDecrement(&guardDepth); }

#if COOP_CRASHLOG && (defined(_XBOX) || defined(__ANDROID__))
// The Win32 crash logger is x86/PC only. On 360 these are no-op stubs so callers still link.
void
Crumb(const char *, int, int)
{}
void
SetCrashInfo(void (*)(char *, int))
{}
void
InstallCrashLog()
{}
void
CrashTick()
{}
#endif

#if COOP_CRASHLOG && !defined(_XBOX) && !defined(__ANDROID__)
#include <stdarg.h>

struct CrumbEntry {
	const char *what;
	int a, b;
	double t;
};
static const int CRUMBS = 512;
static CrumbEntry crumbs[CRUMBS];
static volatile unsigned crumbHead = 0;
static void (*crashInfoCb)(char *, int) = nullptr;
static char crashPath[MAX_PATH] = "";
static char crumbPath[MAX_PATH] = "";
static bool crashInstalled = false;
static volatile LONG crashHandling = 0;
static int crashCount = 0;
static DWORD crashMainThread = 0;
static double lastCrumbFlush = 0;
static LPTOP_LEVEL_EXCEPTION_FILTER prevFilter = nullptr;
static HANDLE crashFile = INVALID_HANDLE_VALUE;

void
Crumb(const char *what, int a, int b)
{
	unsigned i = crumbHead % CRUMBS;
	crumbs[i].what = what;
	crumbs[i].a = a;
	crumbs[i].b = b;
	crumbs[i].t = NowSec();
	crumbHead++;
}

void
SetCrashInfo(void (*cb)(char *, int))
{ crashInfoCb = cb; }

static void
CW(const char *s)
{
	DWORD w;
	WriteFile(crashFile, s, (DWORD)strlen(s), &w, nullptr);
}

static void
CWF(const char *fmt, ...)
{
	char b[600];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	b[sizeof b - 1] = 0;
	CW(b);
}

static bool
Readable(const void *p, size_t n)
{
	const char *c = (const char *)p, *end = c + n;
	while(c < end) {
		MEMORY_BASIC_INFORMATION mbi;
		if(!VirtualQuery(c, &mbi, sizeof mbi)) return false;
		if(mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
		c = (const char *)mbi.BaseAddress + mbi.RegionSize;
	}
	return true;
}

static bool
IsCodeAddr(uintptr_t a)
{
	MEMORY_BASIC_INFORMATION mbi;
	if(!VirtualQuery((const void *)a, &mbi, sizeof mbi)) return false;
	if(mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
	return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool
LooksLikeReturn(uintptr_t a)
{
	if(!IsCodeAddr(a) || !Readable((const void *)(a - 6), 6)) return false;
	const unsigned char *p = (const unsigned char *)a;
	return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] == 0x15 || p[-5] == 0x25)) || p[-2] == 0xFF || p[-3] == 0xFF;
}

static void
AddrStr(uintptr_t a, char *out, int n)
{
	MEMORY_BASIC_INFORMATION mbi;
	out[0] = 0;
	if(VirtualQuery((const void *)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.AllocationBase && mbi.Type == MEM_IMAGE) {
		char path[MAX_PATH];
		path[0] = 0;
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof path);
		const char *base = strrchr(path, '\\');
		base = base ? base + 1 : path;
		_snprintf(out, n - 1, "%s+0x%lX", base, (unsigned long)(a - (uintptr_t)mbi.AllocationBase));
	} else {
		_snprintf(out, n - 1, "0x%lX", (unsigned long)a);
	}
	out[n - 1] = 0;
}

static const char *
ExceptionName(DWORD c)
{
	switch(c) {
	case 0xC0000005: return "ACCESS_VIOLATION";
	case 0xC0000006: return "IN_PAGE_ERROR";
	case 0xC000001D: return "ILLEGAL_INSTRUCTION";
	case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
	case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
	case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
	case 0xC0000095: return "INT_OVERFLOW";
	case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
	case 0xC00000FD: return "STACK_OVERFLOW";
	case 0xC0000374: return "HEAP_CORRUPTION";
	case 0xC0000409: return "STACK_BUFFER_OVERRUN / fail-fast";
	}
	return "other";
}

static bool
IsFatalCode(DWORD c)
{
	switch(c) {
	case 0xC0000005:
	case 0xC0000006:
	case 0xC000001D:
	case 0xC0000025:
	case 0xC000008C:
	case 0xC0000094:
	case 0xC0000095:
	case 0xC0000096:
	case 0xC00000FD:
	case 0xC0000374:
	case 0xC0000409: return true;
	}
	return false;
}

static void
DumpCrumbs(int maxCount)
{
	double now = NowSec();
	unsigned total = crumbHead;
	unsigned n = total < (unsigned)CRUMBS ? total : (unsigned)CRUMBS;
	if((int)n > maxCount) n = (unsigned)maxCount;
	CWF("last %u steps (oldest first, time = ms before now):\r\n", n);
	for(unsigned k = 0; k < n; k++) {
		const CrumbEntry &e = crumbs[(total - n + k) % CRUMBS];
		if(e.what == nullptr) continue;
		CWF("  -%7.1f  %s  %d %d\r\n", (now - e.t) * 1000.0, e.what, e.a, e.b);
	}
}

static void
DumpCoopState()
{
	CWF("co-op state: %s  localSlot %d  welcomed %d  peers %d  packets %u  max players %d\r\n",
	    state == HOST       ? "HOST"
	    : state == CLIENT   ? "CLIENT"
	    : state == BROWSING ? "BROWSING"
	                        : "OFF",
	    localSlot, (int)welcomed, NumPeers(), (unsigned)recvCount, (int)MAX_PLAYERS);
	CWF("note: %s\r\n", note);
	double now = NowSec();
	for(int i = 0; i < MAX_PLAYERS; i++) {
		CWF("  slot %d: peer %d  active %d  shared %d  snaps %d  lastRx %.2fs ago  name '%s'\r\n", i, (int)peers[i].used, (int)rp[i].active,
		    (int)rp[i].shared, rp[i].numSnaps, rp[i].lastRx > 0 ? now - rp[i].lastRx : -1.0, rp[i].name);
	}
}

typedef BOOL(WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
#pragma pack(push, 4)
struct MiniDumpExc {
	DWORD threadId;
	EXCEPTION_POINTERS *ep;
	BOOL clientPointers;
};
#pragma pack(pop)

static void
WriteMiniDump(EXCEPTION_POINTERS *ep, const char *stamp)
{
	char dumpPath[MAX_PATH];
	strncpy(dumpPath, crashPath, sizeof dumpPath - 1);
	dumpPath[sizeof dumpPath - 1] = 0;
	char *slash = strrchr(dumpPath, '\\');
	if(slash)
		slash[1] = 0;
	else
		dumpPath[0] = 0;
	size_t len = strlen(dumpPath);
	_snprintf(dumpPath + len, sizeof dumpPath - len - 1, "coop_crash_%s.dmp", stamp);
	dumpPath[sizeof dumpPath - 1] = 0;

	HMODULE dbg = LoadLibraryA("dbghelp.dll");
	if(!dbg) {
		CW("minidump: dbghelp.dll not found\r\n");
		return;
	}
	MiniDumpWriteDump_t fn = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
	HANDLE f = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(fn && f != INVALID_HANDLE_VALUE) {
		MiniDumpExc mei;
		mei.threadId = GetCurrentThreadId();
		mei.ep = ep;
		mei.clientPointers = FALSE;
		BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40, ep ? &mei : nullptr, nullptr, nullptr);
		CWF("minidump: %s  (%s)\r\n", dumpPath, ok ? "written" : "FAILED");
	} else {
		CW("minidump: could not be created\r\n");
	}
	if(f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

static void
WriteCrash(EXCEPTION_POINTERS *ep, const char *via)
{
	if(InterlockedCompareExchange(&crashHandling, 1, 0) != 0) return;
	if(++crashCount > 4) {
		InterlockedExchange(&crashHandling, 0);
		return;
	}
	crashFile = CreateFileA(crashPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(crashFile == INVALID_HANDLE_VALUE) {
		InterlockedExchange(&crashHandling, 0);
		return;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);
	char stamp[32];
	_snprintf(stamp, sizeof stamp - 1, "%02d%02d%02d", st.wHour, st.wMinute, st.wSecond);
	stamp[sizeof stamp - 1] = 0;

	CW("\r\n==============================================================================\r\n");
	CWF("CRASH %04d-%02d-%02d %02d:%02d:%02d  (%s)\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, via);
	if(ep && ep->ExceptionRecord) {
		EXCEPTION_RECORD *er = ep->ExceptionRecord;
		char where[300];
		AddrStr((uintptr_t)er->ExceptionAddress, where, sizeof where);
		CWF("exception 0x%08lX %s at %s\r\n", (unsigned long)er->ExceptionCode, ExceptionName(er->ExceptionCode), where);
		if(er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
			const char *kind = er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing (DEP)";
			CWF("access violation %s address 0x%lX\r\n", kind, (unsigned long)er->ExceptionInformation[1]);
			if(er->ExceptionInformation[1] < 0x10000)
				CWF("  tiny address: a nil pointer used with a field offset of 0x%lX\r\n", (unsigned long)er->ExceptionInformation[1]);
		}
	}
	CWF("thread %lu (%s thread of the mod)\r\n", (unsigned long)GetCurrentThreadId(), GetCurrentThreadId() == crashMainThread ? "the same" : "a DIFFERENT");

	uintptr_t sp = 0;
	if(ep && ep->ContextRecord) {
		CONTEXT *cx = ep->ContextRecord;
#ifdef _WIN64
		sp = (uintptr_t)cx->Rsp;
		CWF("RIP %016llX RSP %016llX RBP %016llX\r\nRAX %016llX RBX %016llX RCX %016llX RDX %016llX\r\nRSI %016llX RDI %016llX\r\n",
		    (unsigned long long)cx->Rip, (unsigned long long)cx->Rsp, (unsigned long long)cx->Rbp, (unsigned long long)cx->Rax,
		    (unsigned long long)cx->Rbx, (unsigned long long)cx->Rcx, (unsigned long long)cx->Rdx, (unsigned long long)cx->Rsi,
		    (unsigned long long)cx->Rdi);
#else
		sp = (uintptr_t)cx->Esp;
		CWF("EIP %08lX ESP %08lX EBP %08lX\r\nEAX %08lX EBX %08lX ECX %08lX EDX %08lX\r\nESI %08lX EDI %08lX\r\n", (unsigned long)cx->Eip,
		    (unsigned long)cx->Esp, (unsigned long)cx->Ebp, (unsigned long)cx->Eax, (unsigned long)cx->Ebx, (unsigned long)cx->Ecx,
		    (unsigned long)cx->Edx, (unsigned long)cx->Esi, (unsigned long)cx->Edi);
#endif
	}

	if(sp) {
		CW("call stack (stack scan, innermost first; the .dmp has the exact one):\r\n");
		NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
		uintptr_t top = (uintptr_t)tib->StackBase;
		if(top - sp > 32768) top = sp + 32768;
		int printed = 0;
		for(uintptr_t p = sp & ~(uintptr_t)(sizeof(void *) - 1); p + sizeof(void *) <= top && printed < 48; p += sizeof(void *)) {
			if(!Readable((const void *)p, sizeof(void *))) break;
			uintptr_t v = *(const uintptr_t *)p;
			if(!LooksLikeReturn(v)) continue;
			char s[300];
			AddrStr(v, s, sizeof s);
			CWF("  %s\r\n", s);
			printed++;
		}
	}

	DumpCoopState();

	if(crashInfoCb) {
		static char info[3000];
		info[0] = 0;
		CW("game state:\r\n");
#ifdef _MSC_VER
		__try {
			crashInfoCb(info, sizeof info);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			CW("  (reading the game state faulted too)\r\n");
		}
#else
		crashInfoCb(info, sizeof info);
#endif
		info[sizeof info - 1] = 0;
		CW(info);
	}

	DumpCrumbs(200);
	WriteMiniDump(ep, stamp);
	CW("==============================================================================\r\n");
	CloseHandle(crashFile);
	crashFile = INVALID_HANDLE_VALUE;
	InterlockedExchange(&crashHandling, 0);
}

static LONG WINAPI
CrashVeh(EXCEPTION_POINTERS *ep)
{
	if(guardDepth > 0) return EXCEPTION_CONTINUE_SEARCH;
	if(ep && ep->ExceptionRecord && IsFatalCode(ep->ExceptionRecord->ExceptionCode)) WriteCrash(ep, "exception seen first");
	return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI
CrashFilter(EXCEPTION_POINTERS *ep)
{
	WriteCrash(ep, "unhandled exception");
	return prevFilter ? prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void
InstallCrashLog()
{
	if(crashInstalled) return;
	crashInstalled = true;
	MakePath(crashPath, sizeof crashPath, "coop_crash.log");
	MakePath(crumbPath, sizeof crumbPath, "coop_crumbs.log");
	crashMainThread = GetCurrentThreadId();

	WIN32_FILE_ATTRIBUTE_DATA fad;
	if(GetFileAttributesExA(crashPath, GetFileExInfoStandard, &fad) && fad.nFileSizeHigh == 0 && fad.nFileSizeLow > (1u << 20)) DeleteFileA(crashPath);

	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	if(k32) {
		typedef BOOL(WINAPI * Guar_t)(PULONG);
		Guar_t g = (Guar_t)GetProcAddress(k32, "SetThreadStackGuarantee");
		if(g) {
			ULONG sz = 96 * 1024;
			g(&sz);
		}
		typedef PVOID(WINAPI * Veh_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
		Veh_t av = (Veh_t)GetProcAddress(k32, "AddVectoredExceptionHandler");
		if(av) av(1, CrashVeh);
	}
	prevFilter = SetUnhandledExceptionFilter(CrashFilter);
	Crumb("crash log installed");
}

void
CrashTick()
{
	if(!crashInstalled) return;
	double now = NowSec();
	if(now - lastCrumbFlush < 1.0 || crashHandling) return;
	lastCrumbFlush = now;
	HANDLE prev = crashFile;
	crashFile = CreateFileA(crumbPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(crashFile == INVALID_HANDLE_VALUE) {
		crashFile = prev;
		return;
	}
	CW("Newest steps of the co-op mod, refreshed every second. If the game died, the last lines say what it was doing.\r\n");
	DumpCoopState();
	DumpCrumbs(120);
	CloseHandle(crashFile);
	crashFile = prev;
}
#endif

} // namespace CoopNet
