#include "Net.h"
#include "common.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "AnimBlendAssociation.h"
#include "AnimBlendHierarchy.h"
#include "AnimManager.h"
#include "Camera.h"
#include "CivilianPed.h"
#include "PlayerPed.h"
#include "Font.h"
#include "Frontend.h"
#include "ModelIndices.h"
#include "Pad.h"
#include "Particle.h"
#include "PlayerPed.h"
#include "PointLights.h"
#include "RpAnimBlend.h"
#include "RwHelper.h"
#include "Sprite.h"
#include "Sprite2d.h"
#include "Streaming.h"
#include "Text.h"
#include "Timer.h"
#include "ModelInfo.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "Weapon.h"
#include "WeaponInfo.h"
#include "World.h"
#include "main.h"

namespace CoopMenu
{

#if COOP_PLAINFONT
static void EnsureGlyphs();
#endif

enum {
	A_HOST = 1, A_BROWSE, A_FILEJOIN, A_CLOSE, A_DISCONNECT, A_BACK, A_FF, A_STARTHOST, A_HOSTBACK, A_NAME,
	A_JOIN = 100,
	T_TP = 200, T_SPAWN, T_CLOSE,
	T_TPP = 500, // + slot: teleport to that player
	T_CAT = 210,
	T_VEH = 1000
};
enum { M_NONE, M_COOP, M_TRAINER };

static const int MAXITEMS = 256;
static const int VISIBLE = 10;
static const int NUMCATS = 5;
static const char *catNames[NUMCATS] = {"Cars", "Bikes", "Boats", "Helicopters", "Planes"};

static int which = M_NONE;  // which menu is open
static bool hostSetup = false; // co-op menu: the page between "Host a game" and actually hosting
static int sel = 0, scroll = 0;
static int tpage = 0;       // trainer: 0 main, 1 categories, 2 vehicle list
static int tcat = 0;
static bool listDirty = true;
static char labels[MAXITEMS][40];
static int actions[MAXITEMS];
static int numItems = 0;
static char status[160];
static bool editingName = false; // typing the player name in the co-op menu
static char nameBuf[16];

// ---- chat ----
static const int CHAT_LINES = 8;
static bool chatOpen = false;
static char chatBuf[88];
static char chatText[CHAT_LINES][100];
static double chatTime[CHAT_LINES];
static int chatCount = 0;

static void
AddChatLine(const char *s)
{
	if(chatCount < CHAT_LINES) {
		chatCount++;
	} else {
		for(int i = 1; i < CHAT_LINES; i++) {
			strcpy(chatText[i - 1], chatText[i]);
			chatTime[i - 1] = chatTime[i];
		}
	}
	snprintf(chatText[chatCount - 1], sizeof chatText[0], "%s", s);
	chatTime[chatCount - 1] = CoopNet::Time();
}

static void
OpenMenu(int w)
{
	chatOpen = false;
	editingName = false;
	which = w;
	hostSetup = false;
	sel = 0;
	scroll = 0;
	tpage = 0;
	listDirty = true;
}

static void
BuildStatus()
{
	CoopNet::State st = CoopNet::GetState();
	if(st == CoopNet::OFF) {
		snprintf(status, sizeof status, "%s", CoopNet::Note());
	} else if(st == CoopNet::HOST) {
		if(CoopNet::HasPeer()) {
			float buf, pps;
			CoopNet::GetNetStats(buf, pps);
			snprintf(status, sizeof status, "HOSTING - %d/%d players (%s)  %.0f pkt/s  buffer %.0f ms", 1 + CoopNet::NumRemotePlayers(),
			         CoopNet::MaxPlayers(), CoopWorld::IsShared() ? "someone is close" : "all far apart", pps, buf);
		} else {
			snprintf(status, sizeof status, "HOSTING - waiting for players (1/%d)", CoopNet::MaxPlayers());
		}
	} else if(st == CoopNet::BROWSING) {
		snprintf(status, sizeof status, "Searching the LAN for games...");
	} else {
		if(CoopNet::PacketsReceived() == 0) {
			snprintf(status, sizeof status, "JOINING - no reply from host yet");
		} else {
			float buf, pps;
			CoopNet::GetNetStats(buf, pps);
			snprintf(status, sizeof status, "CONNECTED as player %d of %d (%s)  %.0f pkt/s  buffer %.0f ms", CoopNet::GetLocalSlot() + 1,
			         CoopNet::MaxPlayers(), CoopWorld::IsShared() ? "shared world" : "far apart", pps, buf);
		}
	}
}

static void
AddItem(const char *label, int action)
{
	if(numItems >= MAXITEMS) return;
	snprintf(labels[numItems], sizeof labels[0], "%s", label);
	actions[numItems++] = action;
}

static int
VehCategory(CBaseModelInfo *mi)
{
	if(mi == nil || mi->GetModelType() != MITYPE_VEHICLE) return -1;
	switch(((CVehicleModelInfo *)mi)->m_vehicleType) {
	case VEHICLE_TYPE_CAR: return 0;
	case VEHICLE_TYPE_BIKE: return 1;
	case VEHICLE_TYPE_BOAT: return 2;
	case VEHICLE_TYPE_HELI: return 3;
	case VEHICLE_TYPE_PLANE: return 4;
	}
	return -1;
}

static void
BuildVehicleList()
{
	numItems = 0;
	for(int id = 0; id < MODELINFOSIZE && numItems < MAXITEMS; id++) {
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(id);
		if(VehCategory(mi) != tcat) continue;
		CVehicleModelInfo *vi = (CVehicleModelInfo *)mi;
		char name[40];
		wchar *w = TheText.Get(vi->m_gameName);
		int k = 0;
		for(; w && w[k] && k < 30; k++) name[k] = w[k] < 128 ? (char)w[k] : '?';
		name[k] = 0;
		if(k == 0) snprintf(name, sizeof name, "%.8s", vi->m_gameName);
		char lab[40];
		snprintf(lab, sizeof lab, "%.26s (%d)", name, id);
		AddItem(lab, T_VEH + id);
	}
}

static void
BuildItems()
{
	if(which == M_TRAINER && tpage == 2) {
		if(listDirty) {
			BuildVehicleList();
			listDirty = false;
		}
		return;
	}

	numItems = 0;
	if(which == M_TRAINER) {
		if(tpage == 0) {
			int listed = 0;
			for(int sl = 0; sl < CoopNet::MaxPlayers(); sl++) {
				if(!CoopNet::IsSlotActive(sl)) continue;
				const char *nm = CoopNet::GetRemoteName(sl);
				char tb[48];
				if(nm[0]) snprintf(tb, sizeof tb, "Teleport to %s", nm);
				else snprintf(tb, sizeof tb, "Teleport to player %d", sl + 1);
				AddItem(tb, T_TPP + sl);
				listed++;
			}
			if(listed == 0) AddItem("Teleport to other player", T_TP);
			AddItem("Spawn vehicle >", T_SPAWN);
			AddItem("Close", T_CLOSE);
		} else {
			for(int i = 0; i < NUMCATS; i++) AddItem(catNames[i], T_CAT + i);
		}
		return;
	}

	CoopNet::State st = CoopNet::GetState();
	if(st == CoopNet::OFF) {
		char nb[40];
#if COOP_NAMES
		if(editingName) snprintf(nb, sizeof nb, "Name: %s_", nameBuf);
		else snprintf(nb, sizeof nb, "Name: %s", CoopNet::GetLocalName());
#endif
		if(hostSetup) {
			char b[48];
#if COOP_NAMES
			AddItem(nb, A_NAME);
#endif
			snprintf(b, sizeof b, "Friendly fire: %s", CoopWorld::GetFriendlyFire() ? "ON" : "OFF");
			AddItem(b, A_FF);
			AddItem("Host", A_STARTHOST);
			AddItem("Back", A_HOSTBACK);
		} else {
#if COOP_NAMES
			AddItem(nb, A_NAME);
#endif
			AddItem("Host a game", A_HOST);
			AddItem("Find LAN games", A_BROWSE);
			AddItem("Join IP from coop.txt", A_FILEJOIN);
			AddItem("Close", A_CLOSE);
		}
	} else if(st == CoopNet::BROWSING) {
		for(int i = 0; i < CoopNet::NumServers() && numItems < MAXITEMS - 2; i++) {
			const CoopNet::Server *sv = CoopNet::GetServer(i);
			char b[64];
			snprintf(b, sizeof b, "%s %d/%d (%s)", sv->name, (int)sv->players, (int)sv->maxPlayers, sv->ip);
			AddItem(b, A_JOIN + i);
		}
		AddItem("Back", A_BACK);
	} else {
		if(st == CoopNet::HOST) {
			char b[48];
			snprintf(b, sizeof b, "Friendly fire: %s", CoopWorld::GetFriendlyFire() ? "ON" : "OFF");
			AddItem(b, A_FF);
		}
		AddItem("Disconnect", A_DISCONNECT);
		AddItem("Close", A_CLOSE);
	}
}

static void
Activate(int a)
{
	if(a >= T_VEH) {
		CoopWorld::SpawnVehicle(a - T_VEH);
		return;
	}
	if(a >= T_TPP && a < T_TPP + 64) {
		CoopWorld::TeleportToPlayer(a - T_TPP);
		return;
	}
	if(a >= T_CAT) {
		tcat = a - T_CAT;
		tpage = 2;
		sel = scroll = 0;
		listDirty = true;
		return;
	}
	if(a >= T_TP) {
		switch(a) {
		case T_TP: CoopWorld::TeleportToOther(); break;
		case T_SPAWN:
			tpage = 1;
			sel = scroll = 0;
			break;
		case T_CLOSE: which = M_NONE; break;
		}
		return;
	}
	if(a >= A_JOIN) {
		CoopNet::JoinServer(a - A_JOIN);
		return;
	}
	switch(a) {
	case A_NAME:
		strncpy(nameBuf, CoopNet::GetLocalName(), sizeof nameBuf - 1);
		nameBuf[sizeof nameBuf - 1] = 0;
		editingName = true;
		CoopNet::TextInputReset();
		return;
	case A_HOST: hostSetup = true; break;
	case A_FF:
		CoopWorld::SetFriendlyFire(!CoopWorld::GetFriendlyFire());
		return; // keep the cursor where it is
	case A_STARTHOST:
		hostSetup = false;
		CoopNet::StartHost();
		break;
	case A_HOSTBACK: hostSetup = false; break;
	case A_BROWSE: CoopNet::StartBrowse(); break;
	case A_FILEJOIN: CoopNet::JoinFromFile(); break;
	case A_DISCONNECT:
	case A_BACK:
		hostSetup = false;
		CoopNet::Stop();
		break;
	case A_CLOSE: which = M_NONE; break;
	}
	sel = scroll = 0;
}

// ---- local player state, as sent to the other side ----
static CoopNet::PlayerState cachedLocal;
static bool pumpedThisFrame = false;
static uint8 shotCounter = 0;
static int lastWType = -1, lastAmmo = 0;

static void
GatherLocal(CoopNet::PlayerState &s)
{
	memset(&s, 0, sizeof s);
	s.vehId = -1;
	CPed *pl = FindPlayerPed();
	if(pl == nil) {
		s.flags |= 1;
		return;
	}

	CVector p = pl->GetPosition();
	s.x = p.x;
	s.y = p.y;
	s.z = p.z;
	s.heading = pl->m_fRotationCur;
	s.moveState = (uint8)pl->m_nMoveState;

	CWeapon *w = pl->GetWeapon();
	if(w) {
		s.weapon = (uint8)w->m_eWeaponType;
#if COOP_MUZZLE
		int ammo = (int)w->m_nAmmoInClip;
		if(lastWType == (int)w->m_eWeaponType && ammo < lastAmmo) shotCounter++;
		lastWType = (int)w->m_eWeaponType;
		lastAmmo = ammo;
#endif
	}
	s.shots = shotCounter;

#if COOP_AIM
	// exactly the two values the game itself feeds to PointGunInDirection (CPed::AimGun)
	s.aiming = pl->bIsAimingGun ? 1 : 0;
	s.aimYaw = pl->m_fLookDirection;
	s.aimPitch = ((CPlayerPed *)pl)->m_fFPSMoveHeading;
	s.looking = pl->bIsLooking ? 1 : 0;
#endif

	if(pl->bInVehicle) {
		s.flags |= 1;
		int32_t vid;
		uint8_t seat;
		if(CoopWorld::GetLocalRiding(vid, seat)) {
			s.flags |= 2;
			s.vehId = vid;
			s.seat = seat;
		}
#if !COOP_VEHLOOK
		return; // the seated pose is generated by the engine on the other side
#endif
		// With COOP_VEHLOOK every animation is sent, also the steering / bike lean / look / drive-by ones:
		// the engine on the other side derives those from the vehicle's own AI state, which is not the
		// real player's input, so they never matched.
	}
	RpClump *clump = (RpClump *)pl->m_rwObject;
	if(clump == nil) return;

	CoopNet::AnimEntry list[32];
	int cnt = 0;
	for(CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(clump); a != nil && cnt < 32; a = RpAnimBlendGetNextAssociation(a)) {
		if(a->blendAmount < 0.02f) continue;
		CoopNet::AnimEntry e;
		e.animId = (uint16)a->animId;
		e.group = (uint8)a->groupId;
		float bl = a->blendAmount > 1.0f ? 1.0f : a->blendAmount;
		e.blend = (uint8)(bl * 255.0f + 0.5f);
		e.time = a->currentTime;
		float sp = a->speed < 0.0f ? 0.0f : (a->speed > 3.98f ? 3.98f : a->speed);
		e.speed = (uint8)(sp * 64.0f + 0.5f);
		e.flags = (uint8)(a->flags & 0xFF);
		list[cnt++] = e;
	}

	// strongest animations first
	for(int i = 1; i < cnt; i++) {
		CoopNet::AnimEntry t = list[i];
		int j = i - 1;
		while(j >= 0 && list[j].blend < t.blend) {
			list[j + 1] = list[j];
			j--;
		}
		list[j + 1] = t;
	}

	s.numAnims = (uint8)(cnt > CoopNet::MAX_ANIMS ? CoopNet::MAX_ANIMS : cnt);
	for(int i = 0; i < s.numAnims; i++) s.anims[i] = list[i];
}

// call BEFORE CPad::UpdatePads()
#if COOP_MENUKEYS
// the keys the menu uses; the game must not see them while a menu is open (everything else keeps working)
static void
StripMenuKeys(CKeyboardState &k)
{
	k.UP = 0;
	k.DOWN = 0;
	k.LEFT = 0;
	k.RIGHT = 0;
	k.ENTER = 0;
	k.EXTENTER = 0;
	k.BACKSP = 0;
}
#endif

void
PreInput()
{
	CoopNet::MouseFrame(false, SCREEN_WIDTH, SCREEN_HEIGHT); // the menus are keyboard only
#if COOP_MENUKEYS
	if(which != M_NONE) {
		StripMenuKeys(CPad::TempKeyState);
		StripMenuKeys(CPad::NewKeyState);
		StripMenuKeys(CPad::OldKeyState);
	}
#endif
}

// call right AFTER CPad::UpdatePads()
void
Process(float x, float y, float z, float heading)
{
	(void)x;
	(void)y;
	(void)z;
	(void)heading;

	// poll every key every frame so the edge detection stays correct
#if COOP_PLAINFONT
	EnsureGlyphs();
#endif
	bool kF5 = CoopNet::KeyPressed(0);
	bool kG = CoopNet::KeyPressed(1);
	bool kF3 = CoopNet::KeyPressed(2);
	bool kUp = CoopNet::KeyPressed(3);
	bool kDown = CoopNet::KeyPressed(4);
	bool kEnter = CoopNet::KeyPressed(5);
	bool kBack = CoopNet::KeyPressed(6);
	bool kLeft = CoopNet::KeyPressed(7);
	bool kRight = CoopNet::KeyPressed(8);
	bool kChat = CoopNet::KeyPressed(10);

#if COOP_PADKEYS
	// ---- controller: R1+Square menu, R1+Cross trainer, R1+Triangle enter as passenger; in a menu: D-pad, Cross = select, Circle = back ----
	{
		static bool pR1, pSq, pCr, pTr, pCi, pU, pD, pL, pR;
		static double nextRep;
		const CControllerState &ps = CPad::GetPad(0)->NewState;
		bool r1 = ps.RightShoulder1 > 0, sq = ps.Square > 0, cr = ps.Cross > 0, tr = ps.Triangle > 0, ci = ps.Circle > 0;
		bool u = ps.DPadUp > 0, d = ps.DPadDown > 0, l = ps.DPadLeft > 0, r = ps.DPadRight > 0;
		double now = CoopNet::Time();
		if(r1 && sq && !(pR1 && pSq)) kF5 = true;
		if(r1 && cr && !(pR1 && pCr)) kF3 = true;
		if(r1 && tr && !(pR1 && pTr)) kG = true;
		if(which != M_NONE && !r1) {
			bool anyDir = u || d || l || r;
			bool newDir = (u && !pU) || (d && !pD) || (l && !pL) || (r && !pR);
			bool rep = anyDir && !newDir && now >= nextRep;
			if(newDir) nextRep = now + 0.4;
			else if(rep) nextRep = now + 0.12;
			if(u && (!pU || rep)) kUp = true;
			if(d && (!pD || rep)) kDown = true;
			if(l && !pL) kLeft = true;
			if(r && !pR) kRight = true;
			if(cr && !pCr) kEnter = true;
			if(ci && !pCi) kBack = true;
		}
		pR1 = r1; pSq = sq; pCr = cr; pTr = tr; pCi = ci; pU = u; pD = d; pL = l; pR = r;
	}
#endif

	if(kF5) OpenMenu(which == M_COOP ? M_NONE : M_COOP);
	if(kF3) OpenMenu(which == M_TRAINER ? M_NONE : M_TRAINER);
	if(FrontEndMenuManager.m_bMenuActive) which = M_NONE;
	if(CoopNet::GetState() != CoopNet::OFF) hostSetup = false;

	if(kG && which == M_NONE && !chatOpen && !FrontEndMenuManager.m_bMenuActive && !CTimer::GetIsPaused()) CoopWorld::TryEnterAsPassenger();

#if COOP_CHAT
	// lines from the other player (also while a menu is open)
	{
		char line[100];
		while(CoopNet::GetChat(line, sizeof line)) AddChatLine(line);
	}
	if(CoopNet::GetState() == CoopNet::OFF) chatOpen = false;
	if(kChat && !chatOpen && which == M_NONE && CoopNet::IsActive() && !FrontEndMenuManager.m_bMenuActive && !CTimer::GetIsPaused()) {
		chatOpen = true;
		chatBuf[0] = 0;
		CoopNet::TextInputReset();
	} else if(chatOpen) {
		if(FrontEndMenuManager.m_bMenuActive || which != M_NONE) {
			chatOpen = false;
		} else {
			CPad::GetPad(0)->Clear(true); // typing must not move the player
			char c = CoopNet::TextInput();
			int len = (int)strlen(chatBuf);
			if(c == 13) {
				chatOpen = false;
				if(chatBuf[0]) {
					char line[100];
					snprintf(line, sizeof line, "%s: %s", CoopNet::GetLocalName(), chatBuf);
					CoopNet::SendChat(line);
					AddChatLine(line);
				}
			} else if(c == 27) {
				chatOpen = false;
			} else if(c == 8) {
				if(len > 0) chatBuf[len - 1] = 0;
			} else if(c != 0 && len < 70) {
				chatBuf[len] = c;
				chatBuf[len + 1] = 0;
			}
		}
	}
#endif

	// the normal network update happens after the world update (UpdatePuppet).
	// If the game was paused last frame that never ran, so keep the connection alive here.
	if(!pumpedThisFrame && CoopNet::GetState() != CoopNet::OFF) CoopNet::Pump(cachedLocal);
	pumpedThisFrame = false;

	if(which == M_NONE) return;
#if COOP_PADKEYS
	CPad::GetPad(0)->Clear(true); // the game must not react to the buttons used in the menu
#endif

#if !COOP_MENUKEYS
	// block the game's own use of the arrows, Enter and Backspace (and everything else) while a menu is up
	CPad::GetPad(0)->Clear(true);
#endif
	// (with COOP_MENUKEYS the arrows, Enter and Backspace are removed in PreInput, before the pad is built)

	if(kF5 || kF3) return; // the menu was just opened or closed this frame

	if(editingName) {
		// typing: the game must see none of it, and none of the menu keys act meanwhile
		CPad::GetPad(0)->Clear(true);
		char c = CoopNet::TextInput();
		int len = (int)strlen(nameBuf);
		if(c == 13) {
			editingName = false;
			if(nameBuf[0]) CoopNet::SetLocalName(nameBuf);
		} else if(c == 27) {
			editingName = false;
		} else if(c == 8) {
			if(len > 0) nameBuf[len - 1] = 0;
		} else if(c != 0 && len < 15) {
			nameBuf[len] = c;
			nameBuf[len + 1] = 0;
		}
		return;
	}

	BuildItems();
	if(numItems == 0) return;
	if(sel >= numItems) sel = numItems - 1;

	if(kUp) sel = sel > 0 ? sel - 1 : numItems - 1;
	if(kDown) sel = sel < numItems - 1 ? sel + 1 : 0;
	if(kLeft) sel = sel - VISIBLE > 0 ? sel - VISIBLE : 0;
	if(kRight) sel = sel + VISIBLE < numItems - 1 ? sel + VISIBLE : numItems - 1;

	if(sel < scroll) scroll = sel;
	if(sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
	if(scroll < 0) scroll = 0;

	if(kEnter) Activate(actions[sel]);

	if(kBack) {
		if(hostSetup && which == M_COOP) {
			hostSetup = false;
			sel = scroll = 0;
		} else if(which == M_TRAINER && tpage > 0) {
			tpage--;
			sel = tpage == 1 ? tcat : 1;
			scroll = 0;
			listDirty = true;
		} else {
			which = M_NONE;
		}
	}
}

// ---- remote player puppets (one per other player, indexed by network slot) ----
struct PoseFilter {
	bool used;
	uint16 id;
	uint8 group;
	float t, tv, b, bv;
};

struct Puppet {
	CCivilianPed *ped;
	CVector lastPos;
	bool havePos;
	bool seated;
	int32 car;
	int curWeapon;
	uint8 lastShots;
	bool haveShots;
	bool aiming;
	bool looking;
	float hp;
	PoseFilter poseF[8]; // pose animations (bike lean, steering...) are smoothed per puppet

	Puppet() : havePos(false), seated(false), car(-1), curWeapon(-1), lastShots(0), haveShots(false), aiming(false), looking(false), hp(100.0f) {}
};
static Puppet pups[CoopNet::MAX_PLAYERS];

CPed *
GetPuppet(int slot)
{ return (slot >= 0 && slot < CoopNet::MAX_PLAYERS) ? pups[slot].ped : nil; }

CPed *
GetNearestPuppet(float x, float y, float z, int *slotOut)
{
	CPed *best = nil;
	float bd = 1.0e30f;
	if(slotOut) *slotOut = -1;
	for(int i = 0; i < CoopNet::MAX_PLAYERS; i++) {
		if(pups[i].ped == nil) continue;
		float d = (pups[i].ped->GetPosition() - CVector(x, y, z)).MagnitudeSqr();
		if(d < bd) {
			bd = d;
			best = pups[i].ped;
			if(slotOut) *slotOut = i;
		}
	}
	return best;
}


static void
RemovePuppet(Puppet &P)
{
	if(P.ped) {
		CCivilianPed *p = P.ped;
		COOP_CRUMB("Puppet:remove", (int)(&P - pups));
		P.ped = nil;
		CoopWorld::UnseatPed(p);
		CWorld::Remove(p);
		delete p;
	}
	P.havePos = false;
	P.seated = false;
	P.car = -1;
	P.curWeapon = -1;
	P.haveShots = false;
	P.aiming = false;
	P.looking = false;
	P.hp = 100.0f;
}

void
ReleasePuppetFrom(CVehicle *v)
{
	for(int i = 0; i < CoopNet::MAX_PLAYERS; i++) {
		Puppet &P = pups[i];
		if(P.ped && P.ped->bInVehicle && P.ped->m_pMyVehicle == v) RemovePuppet(P);
	}
}

// an animation the remote player is not playing: silence it and let the engine clean it up
// CAnimManager::AddAnimation crashes (null animation group data, see coop_crash.log) when the animation group
// the other player plays is not loaded on this machine. The call is guarded: on failure nothing is added, and
// the same (group, animation) is not tried again for a few seconds (it may load later).
#ifdef _MSC_VER
static CAnimBlendAssociation *
RawAddAnim(RpClump *clump, int group, int anim, bool *ok)
{
	CAnimBlendAssociation *a = nil;
	*ok = true;
	__try {
		a = CAnimManager::AddAnimation(clump, (AssocGroupId)group, (AnimationId)anim);
	} __except(1 /* EXCEPTION_EXECUTE_HANDLER */) {
		*ok = false;
	}
	return a;
}
#endif

static CAnimBlendAssociation *
SafeAddAnim(RpClump *clump, int group, int anim)
{
	static uint32 badKey[64];
	static double badUntil[64];
	static int badNext = 0;
	uint32 key = ((uint32)group << 16) | (uint32)anim;
	double now = CoopNet::Time();
	for(int i = 0; i < 64; i++)
		if(badKey[i] == key + 1 && now < badUntil[i]) return nil;
#ifdef _MSC_VER
	bool ok = true;
	CoopNet::GuardBegin();
	CAnimBlendAssociation *a = RawAddAnim(clump, group, anim, &ok);
	CoopNet::GuardEnd();
	if(!ok) {
		badKey[badNext] = key + 1;
		badUntil[badNext] = now + 5.0;
		badNext = (badNext + 1) % 64;
		COOP_CRUMB("anim group not loaded", group, anim);
		return nil;
	}
	return a;
#else
	return CAnimManager::AddAnimation(clump, (AssocGroupId)group, (AnimationId)anim);
#endif
}

static void
HideAssoc(CAnimBlendAssociation *a)
{
	a->blendAmount = 0.0f;
	a->blendDelta = -8.0f;
	a->flags |= ASSOC_DELETEFADEDOUT;
}

static void
SetAssoc(CAnimBlendAssociation *a, const CoopNet::AnimEntry &e, bool justAdded, float thresh = 0.08f)
{
	a->blendAmount = e.blend / 255.0f;
	a->blendDelta = 0.0f;
	a->speed = e.speed / 64.0f;
	a->flags = (uint16)((a->flags & 0xFF00) | (e.flags & ~(ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE)));

	float len = (a->hierarchy != nil) ? a->hierarchy->totalLength : 0.0f;
	bool repeat = (a->flags & ASSOC_REPEAT) != 0;

	float t = e.time;
	if(len > 0.01f) {
		if(repeat)
			t = fmodf(t, len);
		else if(t > len)
			t = len;
	}

	float diff = t - a->currentTime;
	if(repeat && len > 0.01f) {
		while(diff > len * 0.5f) diff -= len;
		while(diff < -len * 0.5f) diff += len;
	}
	// let the engine advance the animation itself and only correct real drift. Pose animations (speed 0:
	// bike lean, steering) are driven by their time, so they are set every frame.
	if(justAdded || e.speed < 4 || fabsf(diff) > thresh) a->SetCurrentTime(t);
}

// make the puppet play exactly what the remote player is playing
// critically damped follower: no kinks in the velocity, so no visible steps when a new snapshot arrives
static void
Damp(float &cur, float &vel, float target, float smoothTime, float dt)
{
	float omega = 2.0f / smoothTime;
	float x = omega * dt;
	float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
	float change = cur - target;
	float temp = (vel + omega * change) * dt;
	vel = (vel - omega * temp) * e;
	cur = target + (change + temp) * e;
}

// Pose animations (the bike lean the rider plays, the steering): their time and blend ARE the pose, and the
// network only gives us a new value ~30 times a second, so drawing them as they come looks choppy.
// They are followed with a critically damped spring instead.


static void
SmoothPoseAnims(Puppet &P, CoopNet::PlayerState &s)
{
	const float POSE_SMOOTH = 0.08f; // seconds: raise for smoother (but later), lower for snappier
	float dt = CTimer::GetTimeStep() / 50.0f;
	if(dt < 0.002f) dt = 0.002f;
	if(dt > 0.1f) dt = 0.1f;
	bool seen[8] = {false, false, false, false, false, false, false, false};
	for(int i = 0; i < s.numAnims; i++) {
		CoopNet::AnimEntry &e = s.anims[i];
		bool pose = e.speed < 4 || !(e.flags & ASSOC_RUNNING);
		if(!pose) continue;
		int k = -1;
		for(int j = 0; j < 8; j++)
			if(P.poseF[j].used && P.poseF[j].id == e.animId && P.poseF[j].group == e.group) k = j;
		if(k < 0) {
			for(int j = 0; j < 8; j++)
				if(!P.poseF[j].used) {
					k = j;
					P.poseF[j].used = true;
					P.poseF[j].id = e.animId;
					P.poseF[j].group = e.group;
					P.poseF[j].t = e.time;
					P.poseF[j].tv = 0.0f;
					P.poseF[j].b = (float)e.blend;
					P.poseF[j].bv = 0.0f;
					break;
				}
			if(k < 0) continue;
		} else {
			Damp(P.poseF[k].t, P.poseF[k].tv, e.time, POSE_SMOOTH, dt);
			Damp(P.poseF[k].b, P.poseF[k].bv, (float)e.blend, POSE_SMOOTH, dt);
		}
		seen[k] = true;
		e.time = P.poseF[k].t;
		float b = P.poseF[k].b;
		e.blend = (uint8)(b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b + 0.5f));
	}
	for(int j = 0; j < 8; j++)
		if(!seen[j]) P.poseF[j].used = false;
}

static void
ApplyAnims(Puppet &P, const CoopNet::PlayerState &rsIn, bool seated = false)
{
	CoopNet::PlayerState rs = rsIn;
	if(seated) SmoothPoseAnims(P, rs);
	RpClump *clump = (RpClump *)P.ped->m_rwObject;
	if(clump == nil) return;

	bool used[CoopNet::MAX_ANIMS];
	for(int i = 0; i < CoopNet::MAX_ANIMS; i++) used[i] = false;

	int localCount = 0;
	for(CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(clump); a != nil; a = RpAnimBlendGetNextAssociation(a)) {
		localCount++;
		int match = -1;
		for(int i = 0; i < rs.numAnims; i++) {
			if(!used[i] && rs.anims[i].animId == (uint16)a->animId && rs.anims[i].group == (uint8)a->groupId) {
				match = i;
				break;
			}
		}
		if(match >= 0) {
			used[match] = true;
			SetAssoc(a, rs.anims[match], false, seated ? 0.012f : 0.08f);
		} else if(!(seated && a->blendAmount < 0.02f && a->blendDelta <= 0.0f)) {
			// (a seated puppet keeps the engine's already silent associations; hiding them only makes
			// the engine delete and re-create them every frame)
			HideAssoc(a);
		}
	}

	// start animations the remote is playing that we don't have yet
	int adds = 0;
	for(int i = 0; i < rs.numAnims; i++) {
		if(used[i] || rs.anims[i].blend == 0) continue;
		if(adds >= 3 || localCount > 24) break; // safety valve: never flood the clump with associations
		CAnimBlendAssociation *a = SafeAddAnim(clump, rs.anims[i].group, rs.anims[i].animId);
		adds++;
		localCount++;
		if(a != nil) SetAssoc(a, rs.anims[i], true);
	}
}

// show the weapon the remote player is holding
static void
ApplyWeapon(Puppet &P, int w)
{
	if(w == P.curWeapon) return;
	if(w != WEAPONTYPE_UNARMED) {
		CWeaponInfo *wi = CWeaponInfo::GetWeaponInfo((eWeaponType)w);
		int mi = wi ? wi->m_nModelId : -1;
		if(mi >= 0 && !CStreaming::HasModelLoaded(mi)) {
			CStreaming::RequestModel(mi, STREAMFLAGS_DONT_REMOVE);
			return; // try again next frame
		}
		P.ped->GiveWeapon((eWeaponType)w, 999);
	}
	P.ped->SetCurrentWeapon((eWeaponType)w);
	P.curWeapon = w;
}

#if COOP_MUZZLE
static void
MuzzleFlash(Puppet &P)
{
	CVector pos = P.ped->GetPosition() + P.ped->GetForward() * 0.75f;
	pos.z += 0.45f;
	CParticle::AddParticle(PARTICLE_GUNFLASH, pos, CVector(0.0f, 0.0f, 0.0f));
	CPointLights::AddLight(CPointLights::LIGHT_POINT, pos, CVector(0.0f, 0.0f, 0.0f), 7.0f, 1.0f, 0.8f, 0.3f, CPointLights::FOG_NONE, false);
}
#endif

static bool
LoadTommy()
{
	const int model = MI_PLAYER;
	if(!CStreaming::HasModelLoaded(model)) {
		CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE);
		return false;
	}
	return true;
}

// the puppet is only a picture of the other player: nothing that happens on this machine may hurt it
static void
MakePuppetProof(CPed *p)
{
	p->bBulletProof = true;
	p->bFireProof = true;
	p->bCollisionProof = true;
	p->bMeleeProof = true;
	p->bExplosionProof = true;
}

// the remote player is sitting in a vehicle
static void
UpdateSeatedPuppet(Puppet &P, const CoopNet::PlayerState &rs)
{
	CVehicle *car = CoopWorld::ResolveVehicle(rs.vehId);
	if(car == nil) {
		RemovePuppet(P);
		return;
	}

	if(P.ped && (!P.seated || P.car != rs.vehId || !P.ped->bInVehicle)) RemovePuppet(P);

	if(P.ped == nil) {
		bool asDriver = rs.seat == 0;
		if(asDriver && car->pDriver != nil) return;                      // wait until the previous driver is gone
		if(!asDriver && CoopWorld::FreePassengerSlots(car) == 0) return; // no free seat yet
		if(!LoadTommy()) return;
		COOP_CRUMB("Puppet:newSeated", rs.seat, rs.vehId);
		CCivilianPed *p = new CCivilianPed(PEDTYPE_CIVMALE, MI_PLAYER);
		p->CharCreatedBy = MISSION_CHAR;
		p->SetPosition(car->GetPosition());
		CWorld::Add(p);
		if(!(asDriver ? CoopWorld::SeatPed(p, car, true) : CoopWorld::SeatPedAt(p, car, rs.seat))) {
			CWorld::Remove(p);
			delete p;
			return;
		}
		P.ped = p;
		P.ped->RegisterReference((CEntity **)&P.ped);
		P.seated = true;
		P.car = rs.vehId;
		P.curWeapon = -1;
		P.haveShots = false;
	}

	if(P.ped == nil || !P.seated || !P.ped->bInVehicle) return;

	// the engine sometimes tells a seated ped to get out (the local player took the driver's seat, ...).
	// The puppet only mirrors the remote player, so undo that before it is drawn.
	CoopWorld::KeepSeated(P.ped);
	if(rs.seat > 0) CoopWorld::MovePassengerSlot(P.ped, car, rs.seat); // the back seat stays the back seat

#if COOP_VEHLOOK
	ApplyAnims(P, rs, true);
#endif
#if COOP_VEHGUN
	ApplyWeapon(P, rs.weapon);
#if COOP_MUZZLE
	if(P.haveShots && rs.shots != P.lastShots) MuzzleFlash(P);
#endif
	P.lastShots = rs.shots;
	P.haveShots = true;
#endif
}

#if COOP_HITS
// The puppet is a picture of the other player, so it normally cannot be hurt. With friendly fire it can:
// whatever damage the local weapons do to it is reported to the other machine, where the real player
// takes it, and the puppet's own health is put back (it never dies here, the real player does).
static void
UpdatePuppetHits(Puppet &P, int slot)
{
	if(P.ped == nil) return;
	bool ff = CoopWorld::GetFriendlyFire();
	P.ped->bBulletProof = !ff;
	P.ped->bMeleeProof = !ff;
	P.ped->bFireProof = true;
	P.ped->bExplosionProof = true;
	P.ped->bCollisionProof = true;

	float hp = P.ped->m_fHealth;
	if(hp < P.hp - 0.5f) {
		CVehicle *pv = P.ped->bInVehicle ? P.ped->m_pMyVehicle : nil;
		bool blown = pv != nil && pv->GetStatus() == STATUS_WRECKED; // killed by its vehicle exploding: that is synced separately
		if(ff && !blown) CoopWorld::ReportHit(slot, -1, 0, P.hp - hp);
		int ps = P.ped->m_nPedState;
		P.ped->m_fHealth = 100.0f;
		if(ps == PED_DIE || ps == PED_DEAD || ps == PED_FALL || ps == PED_GETUP) {
			RemovePuppet(P); // knocked over by the hit: build a fresh one
			return;
		}
	}
	P.hp = P.ped->m_fHealth;
}
#endif

// one other player's puppet
static void
UpdateOnePuppet(int slot)
{
	Puppet &P = pups[slot];
	COOP_CRUMB("Puppet:update", slot);
	// the vehicle the puppet sits in was removed by the engine
	if(P.ped && P.ped->bInVehicle && P.ped->m_pMyVehicle == nil) RemovePuppet(P);

#if COOP_HITS
	UpdatePuppetHits(P, slot);
#endif

	// the local world can still knock the puppet down or kill it (car bump, headshot). A downed or
	// dead ped stops following the network position (the model stays, only the shadow moves),
	// so throw it away and build a fresh one.
	if(P.ped && !P.seated) {
		int ps = P.ped->m_nPedState;
		if(P.ped->m_fHealth < 1.0f || ps == PED_DIE || ps == PED_DEAD || ps == PED_FALL || ps == PED_GETUP) RemovePuppet(P);
	}

	CoopNet::State st = CoopNet::GetState();
	bool connected = (st == CoopNet::CLIENT) || (st == CoopNet::HOST && CoopNet::HasPeer());

	CoopNet::PlayerState rs;
	if(!connected || FindPlayerPed() == nil || !CoopWorld::IsRemoteVisible(slot) || !CoopNet::GetRemoteInterp(slot, rs)) {
		RemovePuppet(P);
		return;
	}

	if(rs.flags & 2) {
		UpdateSeatedPuppet(P, rs);
		return;
	}
	if(rs.flags & 1) {
		RemovePuppet(P); // in a vehicle that is not synced
		return;
	}

	if(P.ped && P.seated) RemovePuppet(P); // got out of the vehicle

	CVector target(rs.x, rs.y, rs.z);

	if(P.ped == nil) {
		if(!LoadTommy()) return;
		COOP_CRUMB("Puppet:newOnFoot", slot);
		P.ped = new CCivilianPed(PEDTYPE_CIVMALE, MI_PLAYER);
		P.ped->CharCreatedBy = MISSION_CHAR;
		MakePuppetProof(P.ped);
		P.ped->RegisterReference((CEntity **)&P.ped);
		P.ped->SetPosition(target);
		CWorld::Add(P.ped);
		P.lastPos = target;
		P.havePos = true;
		P.seated = false;
		P.curWeapon = -1;
		P.haveShots = false;
		P.aiming = false;
	} else {
		float moved = (target - P.lastPos).Magnitude();
		if(!P.havePos || moved > 15.0f) {
			P.ped->Teleport(target);
		} else if(moved > 0.0005f) {
			CWorld::Remove(P.ped);
			P.ped->SetPosition(target);
			CWorld::Add(P.ped);
		}
		P.lastPos = target;
		P.havePos = true;
	}

	// the network owns this ped: no drifting, no pushing
	P.ped->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
	P.ped->m_fRotationCur = rs.heading;
	P.ped->m_fRotationDest = rs.heading;
	P.ped->SetHeading(rs.heading);
	P.ped->m_nMoveState = (decltype(P.ped->m_nMoveState))rs.moveState;

	ApplyAnims(P, rs);
	ApplyWeapon(P, rs.weapon);

#if COOP_MUZZLE
	if(P.haveShots && rs.shots != P.lastShots) MuzzleFlash(P);
#endif
	P.lastShots = rs.shots;
	P.haveShots = true;

#if COOP_AIM
	if(rs.aiming) {
		// same call the game makes for a ped aiming a gun: world-space yaw, pitch as-is
		CWeaponInfo *wi = CWeaponInfo::GetWeaponInfo((eWeaponType)rs.weapon);
		if(wi && wi->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM))
			P.ped->m_pedIK.m_flags |= CPedIK::AIMS_WITH_ARM;
		else
			P.ped->m_pedIK.m_flags &= ~CPedIK::AIMS_WITH_ARM;
		P.ped->m_pedIK.PointGunInDirection(rs.aimYaw, rs.aimPitch);
		P.aiming = true;
	} else if(P.aiming) {
		if(P.ped->m_pedIK.RestoreGunPosn()) P.aiming = false;
	}

	// head movement: the game's own look direction, relative to the way the ped faces
	if(!rs.aiming && !P.aiming) {
		if(rs.looking) {
			float rel = rs.aimYaw - P.ped->m_fRotationCur;
			while(rel > 3.14159265f) rel -= 6.2831853f;
			while(rel < -3.14159265f) rel += 6.2831853f;
			P.ped->m_pedIK.LookInDirection(rel, 0.0f);
			P.looking = true;
		} else {
			P.looking = false; // stop steering the head; the engine relaxes it by itself
		}
	}
#endif
}

// call once per frame while the game is running, AFTER the world update
void
UpdatePuppet()
{
	CoopNet::State st = CoopNet::GetState();

	// send the state of the frame we just finished, and read whatever has arrived
	if(st != CoopNet::OFF) {
		GatherLocal(cachedLocal);
		COOP_CRUMB("Pump");
		CoopNet::Pump(cachedLocal);
		pumpedThisFrame = true;
	}

	for(int slot = 0; slot < CoopNet::MAX_PLAYERS; slot++) UpdateOnePuppet(slot);
}

#if COOP_PLAINFONT
// A normal font instead of the game's one. CoopNet::RenderGlyph draws each printable ASCII character with GDI
// (Arial); here every glyph becomes a small RW texture that is drawn with CSprite2d. If anything fails the game
// font is used, so this can never leave the menu without text.
static const int GLYPH_PX = 32;     // texture size of one glyph
static const float GLYPH_EM = 24.0f; // the size they were rendered at
// Allocated once and never freed on purpose: a static CSprite2d array was destroyed by the C runtime at exit, after
// the engine had already shut down, and RwTextureDestroy crashed (coop_crash.log, 'atexit destructor for glyphSpr').
static CSprite2d *glyphSpr = nil; // ' ' .. '~'
static float glyphAdv[95];
static bool glyphReady = false, glyphTried = false;

static void
EnsureGlyphs()
{
	if(glyphTried) return;
	glyphTried = true;
	glyphSpr = new CSprite2d[95];
	static uint8 rgba[GLYPH_PX * GLYPH_PX * 4];
	for(int i = 0; i < 95; i++) {
		int adv = 0;
		if(!CoopNet::RenderGlyph((char)(32 + i), rgba, adv)) return;
		glyphAdv[i] = (float)adv;
		if(i == 0) continue; // the space has no picture
		RwRaster *ras = RwRasterCreate(GLYPH_PX, GLYPH_PX, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
		if(ras == nil) return;
		RwUInt8 *dst = RwRasterLock(ras, 0, rwRASTERLOCKWRITE);
		if(dst == nil) {
			RwRasterDestroy(ras);
			return;
		}
		int stride = ras->stride; // (librw declares RwRasterGetStride but does not define it; the member is set by the lock)
		if(stride < GLYPH_PX * 4) stride = GLYPH_PX * 4;
		// (the glyphs are white, so the colour order of the backend does not matter: alpha is the 4th byte)
		for(int y = 0; y < GLYPH_PX; y++)
			memcpy(dst + y * stride, rgba + y * GLYPH_PX * 4, GLYPH_PX * 4);
		RwRasterUnlock(ras);
		RwTexture *tex = RwTextureCreate(ras);
		if(tex == nil) {
			RwRasterDestroy(ras);
			return;
		}
		RwTextureSetFilterMode(tex, rwFILTERLINEAR);
		RwTextureSetAddressing(tex, rwTEXTUREADDRESSCLAMP);
		glyphSpr[i].m_pTexture = tex;
	}
	glyphReady = true;
}

static float
PlainWidth(const char *s, float em)
{
	float w = 0.0f;
	for(; *s; s++) {
		int c = (unsigned char)*s;
		if(c < 32 || c > 126) c = '?';
		w += glyphAdv[c - 32];
	}
	return w * (em / GLYPH_EM);
}

// x, y = top left of the text; em = height of the font in screen units
static void
PlainText(float x, float y, const char *s, const CRGBA &col, float em)
{
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void *)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void *)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void *)rwBLENDINVSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void *)FALSE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void *)FALSE);
	RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void *)rwFILTERLINEAR);
	float k = em / GLYPH_EM;
	float shadow = SCREEN_SCALE_Y(1.0f);
	for(int pass = 0; pass < 2; pass++) {
		CRGBA c = pass == 0 ? CRGBA(0, 0, 0, col.a) : col;
		float off = pass == 0 ? shadow : 0.0f;
		float pen = x;
		for(const char *p = s; *p; p++) {
			int ch = (unsigned char)*p;
			if(ch < 32 || ch > 126) ch = '?';
			int i = ch - 32;
			if(i != 0) {
				float l = pen - 2.0f * k + off, t = y + off;
				glyphSpr[i].Draw(CRect(l, t, l + GLYPH_PX * k, t + GLYPH_PX * k), c);
			}
			pen += glyphAdv[i] * k;
		}
	}
}

static float
TextEm()
{ return SCREEN_SCALE_Y(14.0f); }
#endif

static void
Text(float x, float y, const char *s, const CRGBA &col)
{
#if COOP_PLAINFONT
	if(glyphReady) {
		PlainText(x, y, s, col, TextEm());
		return;
	}
#endif
	wchar w[128];
	AsciiToUnicode(s, w);
	CFont::SetColor(col);
	CFont::PrintString(x, y, w);
}

#if COOP_NAMES
// every other player's name above their head
static void
DrawNameTag(int slot)
{
	CCivilianPed *ped = pups[slot].ped;
	if(ped == nil) return;
	const char *name = CoopNet::GetRemoteName(slot);
	if(!name[0]) return;

	CVector pos = ped->GetPosition();
	pos.z += ped->bInVehicle ? 1.35f : 1.2f;
	float dist = (pos - TheCamera.GetPosition()).Magnitude();
	if(dist > 80.0f) return;

	RwV3d in, out;
	in.x = pos.x;
	in.y = pos.y;
	in.z = pos.z;
	float w, h;
	if(!CSprite::CalcScreenCoors(in, &out, &w, &h, true)) return;

	float k = 1.0f - dist / 120.0f;
	if(k < 0.55f) k = 0.55f;
#if COOP_PLAINFONT
	if(glyphReady) {
		float em = TextEm() * k;
		PlainText(out.x - PlainWidth(name, em) * 0.5f, out.y, name, CRGBA(255, 255, 255, 255), em);
		return;
	}
#endif
	CFont::SetFontStyle(FONT_BANK);
	CFont::SetBackgroundOff();
	CFont::SetScale(SCREEN_SCALE_X(0.34f * k), SCREEN_SCALE_Y(0.6f * k));
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetPropOn();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));

	wchar wbuf[32];
	AsciiToUnicode(name, wbuf);
	float tw = CFont::GetStringWidth(wbuf, true);
	CFont::SetColor(CRGBA(255, 255, 255, 255));
	CFont::PrintString(out.x - tw * 0.5f, out.y, wbuf);
}
#endif

// call from the 2D render pass
void
Draw()
{
	CoopNet::State st = CoopNet::GetState();
	if(which == M_NONE && st == CoopNet::OFF) return;

	BuildStatus();

	CFont::SetFontStyle(FONT_BANK);
	CFont::SetBackgroundOff();
	CFont::SetScale(SCREEN_SCALE_X(0.34f), SCREEN_SCALE_Y(0.6f));
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetPropOn();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));

#if COOP_NAMES
	for(int slot = 0; slot < CoopNet::MAX_PLAYERS; slot++) DrawNameTag(slot);
	// (the tag changed the font settings; the menu below sets them again)
	CFont::SetScale(SCREEN_SCALE_X(0.34f), SCREEN_SCALE_Y(0.6f));
	CFont::SetCentreOff();
#endif

#if COOP_CHAT
	{
		double nowT = CoopNet::Time();
		float lh = SCREEN_SCALE_Y(15.0f);
		float base = SCREEN_HEIGHT - SCREEN_SCALE_Y(125.0f);
		int shownLines = 0;
		for(int i = chatCount - 1; i >= 0; i--) {
			if(!chatOpen && nowT - chatTime[i] > 12.0) continue;
			Text(SCREEN_SCALE_X(20.0f), base - shownLines * lh - lh, chatText[i], CRGBA(235, 235, 235, 255));
			shownLines++;
		}
		if(chatOpen) {
			char in[100];
			snprintf(in, sizeof in, "> %s_", chatBuf);
			Text(SCREEN_SCALE_X(20.0f), base, in, CRGBA(255, 220, 60, 255));
		}
	}
#endif


		if(which == M_NONE) {
		CFont::DrawFonts();
		return;
	}

	BuildItems();
	if(sel >= numItems) sel = numItems > 0 ? numItems - 1 : 0;

	const char *title = "CO-OP";
	if(which == M_COOP && hostSetup) title = "CO-OP - Host options";
	char tbuf[48];
	if(which == M_TRAINER) {
		if(tpage == 0) title = "TRAINER";
		else if(tpage == 1) title = "TRAINER - Spawn";
		else {
			snprintf(tbuf, sizeof tbuf, "TRAINER - %s", catNames[tcat]);
			title = tbuf;
		}
	}

	float px = SCREEN_SCALE_X(20.0f), py = SCREEN_SCALE_Y(50.0f);
	float pw = SCREEN_SCALE_X(230.0f), rh = SCREEN_SCALE_Y(17.0f);
	float headerH = rh * 1.3f;
	int shown = numItems - scroll;
	if(shown > VISIBLE) shown = VISIBLE;
	if(shown < 0) shown = 0;
	float bottom = py + headerH + shown * rh + rh * 1.3f;

	CSprite2d::DrawRect(CRect(px, py, px + pw, bottom), CRGBA(0, 0, 0, 190));
	for(int i = 0; i < shown; i++) {
		if(scroll + i != sel) continue;
		float ry = py + headerH + i * rh;
		CSprite2d::DrawRect(CRect(px + 2, ry, px + pw - 2, ry + rh), CRGBA(200, 150, 20, 230));
	}

	Text(px + SCREEN_SCALE_X(6.0f), py + SCREEN_SCALE_Y(3.0f), title, CRGBA(255, 220, 60, 255));
	for(int i = 0; i < shown; i++) {
		float ry = py + headerH + i * rh;
		Text(px + SCREEN_SCALE_X(8.0f), ry + SCREEN_SCALE_Y(2.0f), labels[scroll + i], CRGBA(255, 255, 255, 255));
	}

	char foot[64];
	snprintf(foot, sizeof foot, "%d/%d   Enter: select   Bksp: back", numItems > 0 ? sel + 1 : 0, numItems);
	Text(px + SCREEN_SCALE_X(6.0f), bottom - rh * 1.15f, foot, CRGBA(170, 170, 170, 255));

	if(which == M_COOP) {
		Text(px, bottom + SCREEN_SCALE_Y(4.0f), status, CRGBA(255, 255, 255, 255));
		if(st == CoopNet::BROWSING && CoopNet::NumServers() == 0)
			Text(px, bottom + SCREEN_SCALE_Y(20.0f), "(no games found yet)", CRGBA(180, 180, 180, 255));
	}
	CFont::DrawFonts();
}

} // namespace CoopMenu
