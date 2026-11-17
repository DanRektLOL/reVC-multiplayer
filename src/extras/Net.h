#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#if defined(_XBOX) || (defined(_MSC_VER) && _MSC_VER < 1900)
inline int
snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = _vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	if(n) buf[n - 1] = '\0';
	return r;
}
#endif

class CVehicle;
class CPed;
class CObject;
class CCoopNetwork
{
public:
	static void Init();
	static void Shutdown();
	static void Update();
	static void SendVehicleSync(const void *data, size_t size);
	static void SendPedSync(const void *data, size_t size);
	static bool IsConnected();
};

// Number of players in one session: slot 0 is the host, slots 1..COOP_MAX_PLAYERS-1 are guests.
// Topology: star. Every guest talks only to the host; the host relays player state, hits, blasts and chat.
// Raise this to scale up (max 255). Bandwidth grows with the square of the player count on the host, see
// the interest-management notes at the top of Net.cpp before going far above 8.
#define COOP_MAX_PLAYERS 4

// Feature switches. If the build fails inside one feature, set that switch to 0 and rebuild.
#define COOP_ALLVEH 1 // bikes, boats, helicopters, planes (0 = cars only)
#define COOP_DOORS 1  // door open/close angles
#define COOP_DAMAGE 1 // panel / door / bumper damage
#define COOP_DMGEXTRA 1 // tyre bursts and repair (Fix) on top of COOP_DAMAGE; set 0 if the build fails in Fix / BurstTyre / GetWheelStatus
#define COOP_FIRE 1   // world fires
#define COOP_MUZZLE 1 // muzzle flash and light on the puppet
#define COOP_AIM 1    // aim yaw / pitch on the puppet
#define COOP_BIKELEAN 1 // bike lean angle (needs COOP_ALLVEH)
#define COOP_BIKESMOOTH 1 // low-pass filter on network-placed bikes (position, orientation, lean)
#define COOP_VEHLOOK 1  // sync look-left/right/behind animations of people sitting in vehicles
#define COOP_SEATFIX 1  // re-pose seated peds after a network-placed vehicle moved
#define COOP_STOPAI 1   // clear the traffic AI of a car the other player took over (a car that still "drives itself" refuses G / enter-car)
#define COOP_PADKEYS 0  // controller: R1+Square menu, R1+Cross trainer, R1+Triangle enter as passenger, D-pad/Cross/Circle inside menus
#define COOP_MENUKEYS 1 // while a menu is open only block the arrow keys, Enter and Backspace (0 = block all pad input like before)
#define COOP_EXPLODE 1  // a vehicle that blew up on one machine blows up on the other too
#define COOP_CHAT 1     // T opens the chat line (needs COOP_NAMES for the sender's name)
#define COOP_NAMES 1    // player names, entered in the menu, shown above the other player
#define COOP_BLASTS 1   // grenades / rockets / molotovs the local player caused explode on the other machine too (needs the one-line hook in CExplosion::AddExplosion, see CoopWorld.cpp)
#define COOP_RUNOVER 1  // running over the other side's peds / the other player
#define COOP_HITS 1     // damage to the other player and to the host's peds travels over the network (friendly fire option)
#define COOP_PLAINFONT 1 // menu, chat and name tag use a normal system font (Arial, rendered with GDI) instead of the game's font; 0 = game font
#define COOP_WEATHERFIX 1 // guest: hold the host's weather every frame and recompute rain / clouds from it (no flicker)
#define COOP_NOSKID 1   // network-placed cars never register tyre skids (no skidmarks, no tyre squeal); set 0 if the build fails in m_aWheelState
#define COOP_VEHGUN 1   // equip the puppet's weapon and show muzzle flashes while it sits in a vehicle (drive-by aiming with Q/E); set 0 if the gun shows while the puppet only sits

#define COOP_LATEFIX 1  // puppet vehicles: brake / tail lights, pedals and bike lean are re-applied right before rendering (needs the BeforePreRender hook, see CoopWorld.cpp)
#define COOP_WEATHERHOLD 1 // guest: the clock and the weather transition run smoothly between host packets, and the engine's own CWeather::Update is blocked (needs the one-line hook in CWeather::Update, see CoopWorld.cpp)

// Crash log: writes coop_crash.log (+ a .dmp minidump) next to the game exe when the game crashes, and keeps
// coop_crumbs.log (what the mod was doing during the last second) up to date. See the end of Net.cpp.
#define COOP_CRASHLOG 1

namespace CoopNet
{

enum State { OFF, HOST, CLIENT, BROWSING };

struct Server {
	char name[32];
	char ip[16];
	uint8_t players, maxPlayers; // from the host's discovery reply
};

static const int MAX_ANIMS = 10;
static const int PED_ANIMS = 3;
static const int MAX_FIRES = 24;
static const int MAX_PLAYERS = COOP_MAX_PLAYERS;
static const int NO_SLOT = 255; // "nobody" in HitNet / BlastNet

#pragma pack(push, 1)
struct AnimEntry {
	uint16_t animId;
	uint8_t group;
	uint8_t blend; // 0..255 maps to 0..1
	float time;    // current playback time in seconds
	uint8_t speed; // playback speed * 64
	uint8_t flags; // low byte of the animation association flags
};

struct WorldNet {
	uint8_t hours, minutes, seconds, interp; // interp: weather blend 0..255
	int16_t oldWeather, newWeather;
	uint8_t shared, friendlyFire, pad[2]; // shared: players are close, host world is shown to the guest; friendlyFire: players can hurt each other
};

struct VehNet {
	int32_t id;
	int16_t model;
	uint8_t flags; // 1 lights, 2 engine, 4 brake, 8 reverse, 16 has driver, 32 host player is inside, 128 wrecked / exploded
	uint8_t col1, col2;
	int8_t steer;
	uint8_t type; // 0 car, 1 bike, 2 boat, 3 heli, 4 plane
	uint8_t freeSeats;
	uint8_t health; // health / 4
	uint8_t doorOpen[6];
	uint8_t doorState[6];
	uint8_t panelState[7];
	uint8_t wheelState[4]; // damage manager wheel status (0 = fine)
	float x, y, z;
	int16_t fwd[3];
	int16_t up[3];
	int16_t vel[3]; // move speed * 8192
	int16_t lean;   // bike lean angle in radians * 10000
	int16_t steerHi; // steering angle in radians * 10000 (the driver's wheel / handlebar animation follows it)
	int8_t gas, brake; // pedals * 127
	int16_t pedLR, pedUD; // bike: rider lean left/right and up/down * 10000
	int8_t leanIn;        // bike: forward / back lean input * 127
	uint8_t burn;         // 1 = the vehicle itself is on fire
};

struct PedNet {
	int32_t id;
	int16_t model;
	uint8_t moveState, flags;
	float x, y, z, heading;
	uint8_t numAnims, seat, pad[2];
	int32_t vehId; // -1 when on foot
	AnimEntry anims[PED_ANIMS];
};

struct FireNet {
	float x, y, z;
	uint8_t size, pad[3];
};

// damage the sender's player caused on the receiver's side
struct HitNet {
	int32_t targetId; // host ped id (pool handle); only used when dstSlot == NO_SLOT
	int16_t model;    // model of that ped, to catch a recycled pool slot
	uint8_t weapon;
	uint8_t srcSlot;  // who caused it (filled in by the network layer, ignore what you put here)
	uint8_t dstSlot;  // player that gets hurt (0 = host, 1.. = guests); NO_SLOT = a host ped (targetId)
	uint8_t pad[3];
	float damage;
};

// an explosion the sender's player caused (grenade, rocket, molotov): replayed on the receiver's side
struct BlastNet {
	int16_t type;
	uint8_t srcSlot; // who caused it (filled in by the network layer)
	uint8_t pad;
	float x, y, z;
};
#pragma pack(pop)

struct RemoteVeh {
	int32_t id;
	int16_t model;
	uint8_t flags, col1, col2, type, freeSeats, health;
	uint8_t doorOpen[6], doorState[6], panelState[7], wheelState[4];
	float x, y, z;
	float fx, fy, fz;
	float ux, uy, uz;
	float vx, vy, vz;
	float steer;
	float lean;
	float gas, brake;
	float pedLR, pedUD, leanIn;
	uint8_t burn;
};

struct PlayerState {
	float x, y, z, heading;
	uint8_t moveState;
	uint8_t flags; // bit 0 = hidden (in a vehicle), bit 1 = riding a synced vehicle (vehId/seat valid)
	uint8_t numAnims;
	uint8_t seat;
	int32_t vehId;
	uint8_t weapon, shots, aiming, looking; // looking: the head is turned towards aimYaw (not aiming)
	float aimYaw, aimPitch; // the game's own m_fLookDirection and m_fFPSMoveHeading
	AnimEntry anims[MAX_ANIMS];
};

void
Pump(const PlayerState &local);

// ---- players ----
int
GetLocalSlot(); // 0 on the host, the assigned slot on a guest, -1 while a guest has not been accepted yet
int
MaxPlayers();
bool
IsSlotActive(int slot); // another player is connected in this slot (never true for the local slot)
int
NumRemotePlayers();
bool
IsRemoteShared(int slot); // guest: the host says that player is in the host's shared world (slot 0 = always)
void
SetPeerShared(int slot, bool shared); // host: remembered and announced to all guests in the roster
bool
KeyPressed(int which); // 0 F5, 1 G, 2 F3, 3 Up, 4 Down, 5 Enter, 6 Backspace, 7 Left, 8 Right, 9 key 3, 10 T
double
Time();

void
MouseFrame(bool menuOpen, float scrW, float scrH);
void
GetMouse(float &x, float &y, bool &leftClick, bool &rightClick);

void
StartHost();
void
StartBrowse();
bool
JoinServer(int index);
void
JoinFromFile();
void
Stop();

State
GetState();
int
NumServers();
const Server *
GetServer(int i);
bool
HasPeer();
uint32_t
PacketsReceived();
bool
GetRemote(int slot, float &x, float &y, float &z); // newest raw position of that player, false when unknown
bool
GetRemoteInterp(int slot, PlayerState &out); // smoothed state for rendering that player's puppet
void
GetNetStats(float &bufferMs, float &packetsPerSec); // guest: the link to the host. host: worst buffer, total packets
void
GetNetStats(int slot, float &bufferMs, float &packetsPerSec);
const char *
Note();

// ---- world sync ----
void
SendWorld(int slot, const WorldNet &w); // host -> that guest
void
SendVehicles(int slot, const VehNet *v, int n); // host -> that guest
void
SendPeds(int slot, const PedNet *p, int n); // host -> that guest
void
SendFires(int slot, const FireNet *f, int n); // host -> that guest
void
SendOwnedVehicle(const VehNet &v); // guest: the car it is driving
bool
GetWorld(WorldNet &w); // guest: true once per new packet
int
GetRemoteVehicles(RemoteVeh *out, int max); // guest: interpolated
int
GetRemotePeds(PedNet *out, int max); // guest: interpolated
bool
GetFires(FireNet *out, int &n); // guest: true once per new packet
bool
GetGuestVehicle(int slot, RemoteVeh &out); // host: the car that guest is driving, interpolated
void
SendHit(const HitNet &h); // route by h.dstSlot (a host ped: NO_SLOT goes to the host); sent a few times, receivers drop duplicates
bool
GetHit(HitNet &out);      // next hit addressed to this machine, false when none (out.srcSlot = who caused it)
void
SendBlast(const BlastNet &b); // goes to every other player; sent a few times, receivers drop duplicates
bool
GetBlast(BlastNet &out);

// ---- fonts ----
// Renders one printable ASCII character with a normal Windows font (24 px) into a 32x32 RGBA buffer
// (white, alpha = coverage, rows top to bottom) and returns its advance in pixels. false if GDI failed.
bool
RenderGlyph(char c, uint8_t *rgba, int &advance);

// ---- names / text entry ----
void
SetLocalName(const char *name); // up to 15 characters
const char *
GetLocalName();
const char *
GetRemoteName(int slot); // "" until that player told us
char
TextInput(); // one typed character per call (edge detected): 8 backspace, 13 enter, 27 escape, 0 nothing
void
TextInputReset();

// ---- chat ----
void
SendChat(const char *line); // a finished line ("Name: text"), at most 95 characters
bool
GetChat(char *out, int n);  // next line received (from anybody), false when none

// ---- crash log ----
void
InstallCrashLog(); // safe to call every frame, only does something the first time
void
CrashTick();       // call once per frame: refreshes coop_crumbs.log about once a second
void
Crumb(const char *what, int a = 0, int b = 0); // remember "what" (a string literal!) and two numbers; very cheap
void
SetCrashInfo(void (*cb)(char *out, int n)); // game side adds its own state to the crash report
void
GuardBegin(); // an exception that is caught on purpose (__try / __except) follows: the crash log must not report it
void
GuardEnd();

// true while hosting or connected as a guest
inline bool
IsActive()
{ return GetState() == HOST || GetState() == CLIENT; }

} // namespace CoopNet

#if COOP_CRASHLOG
#define COOP_CRUMB(...) CoopNet::Crumb(__VA_ARGS__)
#else
#define COOP_CRUMB(...) ((void)0)
#endif

namespace CoopMenu
{
void
PreInput();
void
Process(float x, float y, float z, float heading);
void
UpdatePuppet();
void
Draw();
void
ReleasePuppetFrom(CVehicle *v);
CPed *
GetPuppet(int slot); // that player's puppet in this world (may be nil)
CPed *
GetNearestPuppet(float x, float y, float z, int *slotOut = nullptr); // closest puppet that exists, or nil
} // namespace CoopMenu

namespace CoopWorld
{
void
Update();
bool
IsGuest(); // guest AND in shared mode (host world is shown)
bool
IsShared(); // this machine's world is the host's shared world (host: at least one guest is close)
bool
IsRemoteVisible(int slot); // that player is shown as a puppet in the local world
CVehicle *
ResolveVehicle(int32_t id);
int32_t
GetVehicleId(CVehicle *v);
bool
GetLocalRiding(int32_t &vehId, uint8_t &seat);
void
TryEnterAsPassenger();
const char *
GetHint();
void
SetFriendlyFire(bool on); // host option
bool
GetFriendlyFire();        // host: the option; guest: what the host announced
void
ReportHit(int dstSlot, int32_t targetId, int model, float damage, int weapon = -1); // tell the machine that owns the target (dstSlot = a player, NO_SLOT = a host ped) the local player hurt it (weapon -1 = the local weapon)
void
OnLocalExplosion(int type, float x, float y, float z); // called from CExplosion::AddExplosion for every explosion
bool
IsReplayingBlast();
void
BeforePreRender(); // call once per frame right before CRenderer::PreRender() (see CoopWorld.cpp)
bool
WeatherLocked();   // CWeather::Update must return immediately while this is true (guest: the host drives the weather)
void
DebugLine(char *out, int n);
void
OnLocalObjectDamage(CObject *o);

// seating (the engine only seats a ped when it has an enter-car objective)
bool
SeatPed(CPed *p, CVehicle *car, bool asDriver);
bool
SeatPedAt(CPed *p, CVehicle *car, int seat); // seat 0 = driver, 1.. = that passenger seat (falls back to any free one)
void
MovePassengerSlot(CPed *p, CVehicle *car, int seat); // keep an already seated passenger in the right seat
void
UnseatPed(CPed *p);
void
KeepSeated(CPed *p); // undo any attempt of the engine to make a seated puppet leave its vehicle
int
FreePassengerSlots(CVehicle *v);

// trainer
bool
SpawnVehicle(int model); // spawns any vehicle model in front of the player
bool
TeleportToOther(); // teleports the local player (or the car they drive) to the nearest other player
bool
TeleportToPlayer(int slot); // ... or to a chosen one
} // namespace CoopWorld
