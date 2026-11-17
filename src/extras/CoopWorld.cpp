#include "Net.h"
#include "common.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Automobile.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Pools.h"
#include "Streaming.h"
#include "Timer.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "World.h"
#if COOP_ALLVEH
#include "Bike.h"
#include "Boat.h"
#include "Heli.h"
#include "Plane.h"
#endif
#include "AnimBlendAssociation.h"
#include "AnimBlendHierarchy.h"
#include "AnimManager.h"
#include "CivilianPed.h"
#include "Clock.h"
#include "Explosion.h"
#include "Fire.h"
#include "Population.h"
#include "RpAnimBlend.h"
#include "Weather.h"
#include "Weapon.h"

namespace CoopWorld
{

static const int MAX_SEND_VEH = 60;
static const int MAX_SEND_PED = 70;
static const int MAX_VPUP = 80;
static const int MAX_PPUP = 100;
static const int MAXC = 256;
static const float VEH_RANGE = 150.0f;
static const float PED_RANGE = 100.0f;
static const float SHARE_ENTER = 95.0f;  // closer than this: shared world (about where the host's own traffic ends)
static const float SHARE_LEAVE = 125.0f; // farther than this: independent worlds

static const int MAXP = CoopNet::MAX_PLAYERS; // network slots: 0 = host, 1.. = guests

static bool optFriendlyFire = false; // host option
static bool guestFriendlyFire = false; // what the host announced
static bool hostShared[MAXP];          // host: guest in that slot is in the shared world (decided per guest by distance)
static bool guestShared = false;       // guest: this machine is in the host's shared world
static double lastWorldSend = 0, lastEntSend = 0, lastFireSend = 0;
static int32 guestOwnedId[MAXP]; // host: the car the guest in that slot is driving (-1 = none)
static int32 adoptId[MAXP];      // host: copy of a car from that guest's own world, waiting for the guest to switch to it
static uint8 hostDmg[MAXP][13];  // scratch for ApplyExtras on the host copy of a guest's car
static CVehicle *enterCar = nil; // the vehicle the local player is walking to after G
static bool slotsInited = false;

static void
InitSlots()
{
	if(slotsInited) return;
	slotsInited = true;
	for(int i = 0; i < MAXP; i++) {
		hostShared[i] = false;
		guestOwnedId[i] = -1;
		adoptId[i] = -1;
	}
}

static bool
AnyHostShared()
{
	for(int i = 1; i < MAXP; i++)
		if(hostShared[i] && CoopNet::IsSlotActive(i)) return true;
	return false;
}

static int
NumGuestCars()
{
	int c = 0;
	for(int i = 1; i < MAXP; i++)
		if(guestOwnedId[i] >= 0) c++;
	return c;
}

// ---- puppets on the guest ----
static CVehicle *vcar[MAX_VPUP];
static int32 vid[MAX_VPUP];
static uint8 vflags[MAX_VPUP], vfree[MAX_VPUP], vtype[MAX_VPUP];
static uint8 vdmg[MAX_VPUP][13];
static bool vwreck[MAX_VPUP]; // the puppet was already a wreck when it appeared: never explode it
static double vlast[MAX_VPUP];
static CPed *pped[MAX_PPUP];
static int32 pid[MAX_PPUP];
static int32 pveh[MAX_PPUP];
static double plast[MAX_PPUP];
static float phealth[MAX_PPUP];

static int dbgCars = 0, dbgPeds = 0, dbgSeated = 0;
static char hintBuf[96] = "";
static double hintUntil = 0;
static char seatDbg[80] = "";

// ids from the other machine: never trust them as pool handles
static CVehicle *
SafeVeh(int32 id)
{
	CVehiclePool *vp = CPools::GetVehiclePool();
	if(id < 0 || (id >> 8) >= vp->GetSize()) return nil;
	return vp->GetAt(id);
}

static CPed *
SafePed(int32 id)
{
	CPedPool *pp = CPools::GetPedPool();
	if(id < 0 || (id >> 8) >= pp->GetSize()) return nil;
	return pp->GetAt(id);
}

static void
SetHint(const char *s)
{
	snprintf(hintBuf, sizeof hintBuf, "%s", s);
	hintUntil = CoopNet::Time() + 3.0;
}

const char *
GetHint()
{ return CoopNet::Time() < hintUntil ? hintBuf : nullptr; }

void
DebugLine(char *out, int n)
{
	if(CoopNet::GetState() == CoopNet::CLIENT)
		snprintf(out, n, "puppet vehicles %d  puppet peds %d (seated %d)  %s", dbgCars, dbgPeds, dbgSeated, seatDbg);
	else if(CoopNet::GetState() == CoopNet::HOST)
		snprintf(out, n, "%d guest(s) connected, %d driving  %s", CoopNet::NumRemotePlayers(), NumGuestCars(), seatDbg);
	else
		out[0] = 0;
}

bool
IsShared()
{
	CoopNet::State st = CoopNet::GetState();
	if(st == CoopNet::HOST) return AnyHostShared();
	if(st == CoopNet::CLIENT) return guestShared;
	return false;
}

// is that player drawn as a puppet in this machine's world?
bool
IsRemoteVisible(int slot)
{
	if(slot < 0 || slot >= MAXP || !CoopNet::IsSlotActive(slot)) return false;
	CoopNet::State st = CoopNet::GetState();
	if(st == CoopNet::HOST) return hostShared[slot];
	// guest: we show the host's world, so we also see whoever the host has in it
	if(st == CoopNet::CLIENT) return guestShared && CoopNet::IsRemoteShared(slot);
	return false;
}

void
SetFriendlyFire(bool on)
{ optFriendlyFire = on; }

bool
GetFriendlyFire()
{ return CoopNet::GetState() == CoopNet::CLIENT ? guestFriendlyFire : optFriendlyFire; }

static int
LocalWeaponType()
{
	CPed *pl = FindPlayerPed();
	if(pl) {
		CWeapon *wp = pl->GetWeapon();
		if(wp) return (int)wp->m_eWeaponType;
	}
	return (int)WEAPONTYPE_UNARMED;
}

// the local player (through the puppet it shot) hurt something on the other machine
void
ReportHit(int dstSlot, int32_t targetId, int model, float damage, int weapon)
{
#if COOP_HITS
	if(!CoopNet::IsActive() || damage <= 0.0f) return;
	CoopNet::HitNet h;
	memset(&h, 0, sizeof h);
	h.dstSlot = (uint8_t)(dstSlot >= 0 ? dstSlot : CoopNet::NO_SLOT);
	h.targetId = targetId;
	h.model = (int16_t)model;
	h.weapon = (uint8_t)(weapon >= 0 ? weapon : LocalWeaponType());
	h.damage = damage;
	CoopNet::SendHit(h);
#endif
}

// damage the other player caused here
static void
ApplyIncomingHits()
{
	CoopNet::HitNet h;
	CPed *pl = FindPlayerPed();
	while(CoopNet::GetHit(h)) {
#if COOP_HITS
		if(pl == nil || !(h.damage > 0.0f) || h.damage > 1000.0f || h.weapon >= WEAPONTYPE_TOTALWEAPONS) continue;
		CPed *culprit = CoopMenu::GetPuppet(h.srcSlot); // the player who did it, as seen in this world
		if(h.dstSlot != CoopNet::NO_SLOT) { // meant for the local player
			if(!GetFriendlyFire() || culprit == nil || pl->m_fHealth <= 0.0f) continue;
			pl->InflictDamage(culprit, (eWeaponType)h.weapon, h.damage, PEDPIECE_TORSO, 0);
			continue;
		}
		if(CoopNet::GetState() != CoopNet::HOST) continue; // only the host owns peds
		CPed *p = SafePed(h.targetId);
		if(p == nil || p == pl || p->GetModelIndex() == MI_PLAYER || p->GetModelIndex() != h.model || p->m_fHealth <= 0.0f) continue;
		p->InflictDamage(culprit ? culprit : pl, (eWeaponType)h.weapon, h.damage, PEDPIECE_TORSO, 0);
#endif
	}
}

// ---------------------------------------------------------------------
// Explosions the local player caused (grenades, rockets, molotovs).
// The sender's own copy of the other player is only a picture (proof against explosions), so the damage
// has to happen on the machine that owns the real thing: the explosion is replayed there.
//
// ONE-LINE HOOK, at the top of CExplosion::AddExplosion (src/weapons/Explosion.cpp), with #include "Net.h":
//
//     if(culprit != nil && culprit == FindPlayerPed() &&
//        (type == EXPLOSION_GRENADE || type == EXPLOSION_MOLOTOV || type == EXPLOSION_ROCKET))
//         CoopWorld::OnLocalExplosion((int)type, pos.x, pos.y, pos.z);
// ---------------------------------------------------------------------
static bool replayingBlast = false;
static double blastProtectUntil = 0;
static bool blastProtecting = false, blastSavedProof = false;

bool
IsReplayingBlast()
{ return replayingBlast; }

void
OnLocalExplosion(int type, float x, float y, float z)
{
#if COOP_BLASTS
	if(replayingBlast || !CoopNet::IsActive() || !IsShared()) return;
	CoopNet::BlastNet b;
	memset(&b, 0, sizeof b);
	b.type = (int16_t)type;
	b.x = x;
	b.y = y;
	b.z = z;
	CoopNet::SendBlast(b);
#endif
}

void
OnLocalObjectDamage(CObject *o)
{
	if(o == nil || !CoopNet::IsActive() || !IsShared() || IsReplayingBlast()) return;
	CVector p = o->GetPosition();
	static double lastT = 0.0;
	static float lx = 0.0f, ly = 0.0f, lz = 0.0f;
	double now = CoopNet::Time();
	if(now - lastT < 0.3 && fabsf(p.x - lx) < 0.5f && fabsf(p.y - ly) < 0.5f && fabsf(p.z - lz) < 0.5f) return;
	lastT = now;
	lx = p.x;
	ly = p.y;
	lz = p.z;
	CoopNet::BlastNet b;
	memset(&b, 0, sizeof b);
	b.type = (int16)(-1 - o->GetModelIndex()); // a negative type means "prop damaged", model index encoded
	b.x = p.x;
	b.y = p.y;
	b.z = p.z;
	CoopNet::SendBlast(b);
}

// the other machine damaged a prop: find ours and do the same
static void
ApplyPropDamage(const CoopNet::BlastNet &b)
{
	int model = -1 - (int)b.type;
	CObjectPool *op = CPools::GetObjectPool();
	CVector p(b.x, b.y, b.z);
	CObject *best = nil;
	float bd = 4.0f; // within 2 m
	for(int i = 0; i < op->GetSize(); i++) {
		CObject *o = op->GetSlot(i);
		if(o == nil || o->GetModelIndex() != model) continue;
		float d = (o->GetPosition() - p).MagnitudeSqr();
		if(d < bd) {
			bd = d;
			best = o;
		}
	}
	if(best) best->ObjectDamage(1000.0f);
}
static void
ApplyIncomingBlasts()
{
#if COOP_BLASTS
	CPed *pl = FindPlayerPed();
	double now = CoopNet::Time();
	CoopNet::BlastNet b;
	while(CoopNet::GetBlast(b)) {
		if(pl == nil) continue;
		if(b.type < 0) { // a prop the other machine damaged
			if(fabsf(b.x) < 4000.0f && fabsf(b.y) < 4000.0f && fabsf(b.z) < 1000.0f) {
				replayingBlast = true; // so ObjectDamage does not send it straight back
				ApplyPropDamage(b);
				replayingBlast = false;
			}
			continue;
		}
		// only the kinds we send, at a sane place: never feed raw network numbers to the explosion code
		if(!(b.type == EXPLOSION_GRENADE || b.type == EXPLOSION_MOLOTOV || b.type == EXPLOSION_ROCKET)) continue;
		if(!(fabsf(b.x) < 4000.0f && fabsf(b.y) < 4000.0f && fabsf(b.z) < 1000.0f)) continue;
		if(!GetFriendlyFire()) blastProtectUntil = now + 4.0;
		replayingBlast = true;
		CExplosion::AddExplosion(nil, CoopMenu::GetPuppet(b.srcSlot), (eExplosionType)b.type, CVector(b.x, b.y, b.z), 0);
		replayingBlast = false;
	}
	// without friendly fire the other player's explosions must not hurt the local player
	if(pl) {
		if(now < blastProtectUntil) {
			if(!blastProtecting) {
				blastSavedProof = pl->bExplosionProof;
				pl->bExplosionProof = true;
				blastProtecting = true;
			}
		} else if(blastProtecting) {
			pl->bExplosionProof = blastSavedProof;
			blastProtecting = false;
		}
	}
#endif
}

#if COOP_RUNOVER
// Running people over: the other side's peds are only pictures here (collision proof), so a car that
// drives into one has to tell the machine that owns the real ped.
static double runoverPed[MAX_PPUP];
static double runoverPlayer[MAXP];
static double pknock[MAX_PPUP]; // until when the local player's knockdown of this puppet plays out locally

static bool
VehicleTouches(CVehicle *v, const CVector &p, float margin = 0.5f)
{
	CColModel *cm = v->GetColModel();
	if(cm == nil) return false;
	CVector d = p - v->GetPosition();
	float lx = DotProduct(d, v->GetRight());
	float ly = DotProduct(d, v->GetForward());
	float lz = DotProduct(d, v->GetUp());
	return lx > cm->boundingBox.min.x - margin && lx < cm->boundingBox.max.x + margin &&
	       ly > cm->boundingBox.min.y - margin && ly < cm->boundingBox.max.y + margin &&
	       lz > cm->boundingBox.min.z - 1.0f && lz < cm->boundingBox.max.z + 0.5f;
}

static void
CheckRunOver()
{
	if(!IsShared()) return;
	CPed *pl = FindPlayerPed();
	CVehicle *v = (pl && pl->bInVehicle) ? pl->m_pMyVehicle : nil;
	bool driving = v != nil && v->pDriver == pl;
	float speed = driving ? v->m_vecMoveSpeed.Magnitude() : 0.0f; // units per frame; * 50 = metres per second

	// The other side's peds are solid pictures here: a car that hits one gets stopped dead and the ped stays
	// standing until the real ped reacts. While a car is on its way into one, let it pass through.
	for(int k = 0; k < MAX_PPUP; k++) {
		if(pped[k] == nil || pveh[k] >= 0) continue;
		if(CoopNet::Time() < pknock[k]) {
			// just knocked down: collision only off while the car body actually overlaps it, so it lands normally
			pped[k]->bUsesCollision = !VehicleTouches(v, pped[k]->GetPosition(), 0.3f);
			continue;
		}
		bool pass = driving && speed > 0.04f && VehicleTouches(v, pped[k]->GetPosition(), 2.5f);
		pped[k]->bUsesCollision = !pass;
		if(pass && VehicleTouches(v, pped[k]->GetPosition(), 0.6f)) v->m_vecMoveSpeed *= 0.97f;
	}

	if(!driving || speed < 0.1f) return;
	float dmg = 60.0f + speed * 50.0f * 8.0f; // fast enough to kill outright
	if(dmg > 250.0f) dmg = 250.0f;
	double now = CoopNet::Time();

	if(CoopNet::GetState() == CoopNet::CLIENT) { // the host's peds live on the host
		for(int k = 0; k < MAX_PPUP; k++) {
			CPed *p = pped[k];
			if(p == nil || pveh[k] >= 0 || now < runoverPed[k]) continue;
			if(!VehicleTouches(v, p->GetPosition(), 0.9f)) continue;
			runoverPed[k] = now + 0.7;
			ReportHit(CoopNet::NO_SLOT, pid[k], p->GetModelIndex(), dmg, (int)WEAPONTYPE_RUNOVERBYCAR);
			{ // react at once instead of waiting for the host's ped to die
				CVector fd = v->m_vecMoveSpeed;
				fd.z = 0.0f;
				int8 direction = p->GetLocalDirection(-fd);
				p->bIsStanding = false;
				p->ApplyMoveForce(fd * (p->m_fMass * 0.6f));
				p->ApplyMoveForce(0.0f, 0.0f, 2.0f);
				p->SetFall(2000, (AnimationId)(direction + ANIM_STD_HIGHIMPACT_FRONT), 0);
				pknock[k] = now + 1.5;
			}
		}
	}
	if(GetFriendlyFire()) {
		for(int slot = 0; slot < MAXP; slot++) {
			if(now < runoverPlayer[slot]) continue;
			CPed *other = CoopMenu::GetPuppet(slot);
			if(other && !other->bInVehicle && VehicleTouches(v, other->GetPosition())) {
				runoverPlayer[slot] = now + 0.7;
				ReportHit(slot, -1, 0, dmg, (int)WEAPONTYPE_RUNOVERBYCAR);
			}
		}
	}
}
#endif

bool
IsGuest()
{ return CoopNet::GetState() == CoopNet::CLIENT && CoopNet::PacketsReceived() > 0 && guestShared; }

// =====================================================================
// seating
//
// CPed::WarpPedIntoCar only seats a ped that already has an
// OBJECTIVE_ENTER_CAR_AS_DRIVER / _AS_PASSENGER objective; without one it
// returns early and leaves the ped half-seated and invisible. The game's own
// script command sets the objective first, so we do the same.
// =====================================================================
static int
PaxLimit(CVehicle *v)
{
	int m = (int)v->m_nNumMaxPassengers;
	return m > 4 ? 4 : m; // WarpPedIntoCar only fills the first 4 slots
}

int
FreePassengerSlots(CVehicle *v)
{
	if(v == nil) return 0;
	int n = 0, m = PaxLimit(v);
	for(int i = 0; i < m; i++)
		if(v->pPassengers[i] == nil) n++;
	return n;
}

static int
CountPassengers(CVehicle *v)
{
	int n = 0;
	for(int i = 0; i < 8; i++)
		if(v->pPassengers[i] != nil) n++;
	return n;
}

bool
SeatPed(CPed *p, CVehicle *car, bool asDriver)
{
	if(p == nil || car == nil) return false;

	COOP_CRUMB("SeatPed", (int)asDriver, (int)(p == FindPlayerPed()));
	bool ok = false;
	if(asDriver) {
		if(car->pDriver == nil || car->pDriver == p) {
			p->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, car);
			p->WarpPedIntoCar(car);
			ok = (car->pDriver == p);
			if(ok && p == FindPlayerPed()) car->SetStatus(STATUS_PLAYER);
		}
	} else if(FreePassengerSlots(car) > 0) {
		int before = CountPassengers(car);
		uint8 oldStatus = car->GetStatus();
		p->SetObjective(OBJECTIVE_ENTER_CAR_AS_PASSENGER, car);
		p->WarpPedIntoCar(car);
		car->SetStatus((eEntityStatus)oldStatus); // a passenger must not change who controls the car
		int added = CountPassengers(car) - before;
		if(added > 0) {
			car->m_nNumPassengers += (uint8)added; // WarpPedIntoCar does not count them
			ok = true;
		}
	}
	snprintf(seatDbg, sizeof seatDbg, "seat %s: %s", asDriver ? "driver" : "passenger", ok ? "ok" : "FAILED");
	return ok;
}

// keep an already seated passenger in the seat the real player is in (the engine fills the first free one)
void
MovePassengerSlot(CPed *p, CVehicle *car, int seat)
{
	int want = seat - 1;
	if(p == nil || car == nil || want < 0 || want >= PaxLimit(car)) return;
	if(car->pPassengers[want] != nil) return; // taken (by p itself or by somebody else)
	for(int i = 0; i < 8; i++) {
		if(car->pPassengers[i] == p) {
			car->pPassengers[want] = p;
			car->pPassengers[i] = nil;
			return;
		}
	}
}

bool
SeatPedAt(CPed *p, CVehicle *car, int seat)
{
	if(seat <= 0) return SeatPed(p, car, true);
	bool ok = SeatPed(p, car, false);
	if(ok) MovePassengerSlot(p, car, seat);
	return ok;
}

void
UnseatPed(CPed *p)
{
	if(p == nil) return;
	CVehicle *car = p->m_pMyVehicle;
	if(p->bInVehicle && car != nil) {
		if(car->pDriver == p)
			car->RemoveDriver();
		else
			car->RemovePassenger(p);
	}
	p->bInVehicle = false;
	p->m_pMyVehicle = nil;
}

// A seated puppet only mirrors a real person on the other machine. The engine may still order it out
// of the vehicle (the local player took the driver's seat, an AI reaction...), which looked like a short
// exit animation followed by the puppet being rebuilt inside the car. Cancel that before it is drawn.
void
KeepSeated(CPed *p)
{
	if(p == nil || !p->bInVehicle || p->m_pMyVehicle == nil) return;
	int ps = p->m_nPedState;
	bool leaving = (ps == PED_EXIT_CAR || ps == PED_DRAG_FROM_CAR || ps == PED_CARJACK);
	if(p->m_objective == OBJECTIVE_LEAVE_CAR) {
		p->ClearObjective();
		leaving = true;
	}
	if(leaving) p->m_nPedState = PED_DRIVING;
}

// ---- vehicle types ----
static int
VehType(CVehicle *v)
{
	if(v == nil) return -1;
#if COOP_ALLVEH
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(v->GetModelIndex());
	if(mi == nil || mi->GetModelType() != MITYPE_VEHICLE) return -1;
	switch(((CVehicleModelInfo *)mi)->m_vehicleType) {
	case VEHICLE_TYPE_CAR: return 0;
	case VEHICLE_TYPE_BIKE: return 1;
	case VEHICLE_TYPE_BOAT: return 2;
	case VEHICLE_TYPE_HELI: return 3;
	case VEHICLE_TYPE_PLANE: return 4;
	}
	return -1;
#else
	return v->IsCar() ? 0 : -1;
#endif
}

static CVehicle *
MakeVehicle(int type, int model)
{
	switch(type) {
	case 0: return new CAutomobile(model, MISSION_VEHICLE);
#if COOP_ALLVEH
	case 1: return new CBike(model, MISSION_VEHICLE);
	case 2: return new CBoat(model, MISSION_VEHICLE);
	case 3: return new CHeli(model, MISSION_VEHICLE);
	case 4: return new CPlane(model, MISSION_VEHICLE);
#endif
	}
	return nil;
}

static CVehicle *
PuppetCarById(int32 id)
{
	for(int k = 0; k < MAX_VPUP; k++)
		if(vcar[k] && vid[k] == id) return vcar[k];
	return nil;
}

CVehicle *
ResolveVehicle(int32_t id)
{
	if(id < 0) return nil;
	CoopNet::State st = CoopNet::GetState();
	if(st == CoopNet::HOST) return SafeVeh(id);
	if(st == CoopNet::CLIENT) return PuppetCarById(id);
	return nil;
}

int32_t
GetVehicleId(CVehicle *v)
{
	if(v == nil) return -1;
	CoopNet::State st = CoopNet::GetState();
	if(st == CoopNet::HOST) {
		if(VehType(v) < 0) return -1;
		return CPools::GetVehiclePool()->GetIndex(v);
	}
	if(st == CoopNet::CLIENT) {
		for(int k = 0; k < MAX_VPUP; k++)
			if(vcar[k] == v) return vid[k];
	}
	return -1;
}

static int
SeatOf(CVehicle *v, CPed *p)
{
	if(v->pDriver == p) return 0;
	for(int i = 0; i < 8; i++)
		if(v->pPassengers[i] == p) return i + 1;
	return 1;
}

bool
GetLocalRiding(int32_t &vehId, uint8_t &seat)
{
	CPed *pl = FindPlayerPed();
	if(pl == nil || !pl->bInVehicle || pl->m_pMyVehicle == nil) return false;
	CVehicle *v = pl->m_pMyVehicle;
	int32_t id = GetVehicleId(v);
	if(id < 0) return false;
	vehId = id;
	seat = (uint8_t)SeatOf(v, pl);
	return true;
}

// =====================================================================
// vehicle state -> network
// =====================================================================
static int16
PackS(float f, float scale)
{
	float v = f * scale;
	if(v > 32767.0f) v = 32767.0f;
	if(v < -32767.0f) v = -32767.0f;
	return (int16)v;
}

// Puppet vehicles are only pictures of the real vehicle on the other machine. Nothing that happens
// locally (collisions, bullets, fire) may damage them: the damage arrives over the network instead.
// Whoever drives a vehicle for real (local player, or the owner on that machine) is never proof.
static void
SetNetProof(CVehicle *v, bool on)
{
#if COOP_DAMAGE
	v->bBulletProof = on;
	v->bFireProof = on;
	v->bCollisionProof = on;
	v->bMeleeProof = on;
	v->bExplosionProof = on;
#endif
}

// A car that came out of the traffic still has its AI mission. While a puppet sits in it the AI would
// drive it between two network updates, so it is never standing still when the player tries to get in.
static void
StopAI(CVehicle *v)
{
#if COOP_STOPAI
	v->AutoPilot.m_nCarMission = MISSION_NONE;
	v->AutoPilot.m_nTempAction = TEMPACT_NONE;
	v->AutoPilot.m_nCruiseSpeed = 0;
#endif
}

#if COOP_FIRE
// a fire that is attached to this vehicle (a burning car), not a fire on the ground
static CFire *
FindVehicleFire(CVehicle *v)
{
	for(int i = 0; i < NUM_FIRES; i++) {
		CFire &f = gFireManager.m_aFires[i];
		if(f.m_bIsOngoing && f.m_pEntity == v) return &f;
	}
	return nil;
}

static void
ApplyVehicleFire(CVehicle *v, bool burn)
{
	CFire *f = FindVehicleFire(v);
	if(burn && f == nil) {
		bool proof = v->bFireProof;
		v->bFireProof = false; // the puppet is proof, the flames must still start
		gFireManager.StartFire(v, nil, 0.8f, true);
		v->bFireProof = proof;
	} else if(!burn && f != nil) {
		f->Extinguish();
	}
}
#else
static CFire *FindVehicleFire(CVehicle *) { return nil; }
static void ApplyVehicleFire(CVehicle *, bool) {}
#endif

static void
FillVehNet(CoopNet::VehNet &n, CVehicle *v, int32 id, int type)
{
	memset(&n, 0, sizeof n);
	CVector pos = v->GetPosition();
	CVector f = v->GetForward(), u = v->GetUp();
	CVector ms = v->m_vecMoveSpeed;
	n.id = id;
	n.type = (uint8)type;
	n.model = (int16)v->GetModelIndex();
	n.col1 = (uint8)v->m_currentColour1;
	n.col2 = (uint8)v->m_currentColour2;
	if(v->bLightsOn) n.flags |= 1;
	if(v->bEngineOn) n.flags |= 2;
	if(v->m_fBrakePedal > 0.3f || v->bIsHandbrakeOn) n.flags |= 4;
	if(v->bIsHandbrakeOn) n.flags |= 64; // handbrake: lights the brake lamps, but the receiver never sets the real handbrake (skidmarks)
	if(v->m_fGasPedal < -0.1f) n.flags |= 8;
	if(v->pDriver) n.flags |= 16;
#if COOP_EXPLODE
	if(v->GetStatus() == STATUS_WRECKED) n.flags |= 128;
#endif
	v->m_nDoorLock = CARLOCK_UNLOCKED; // the game's own random locks are not wanted here

	n.freeSeats = (uint8)FreePassengerSlots(v);
	n.burn = FindVehicleFire(v) != nil ? 1 : 0;
	float hp = v->m_fHealth * 0.25f;
	n.health = (uint8)(hp < 0.0f ? 0.0f : (hp > 250.0f ? 250.0f : hp));

	float st = v->m_fSteerAngle;
	if(st > 0.7f) st = 0.7f;
	if(st < -0.7f) st = -0.7f;
	n.steer = (int8)(st / 0.7f * 127.0f);
	n.steerHi = PackS(st, 10000.0f);
	n.gas = (int8)(PackS(v->m_fGasPedal, 127.0f));
	n.brake = (int8)(PackS(v->m_fBrakePedal, 127.0f));

	n.x = pos.x;
	n.y = pos.y;
	n.z = pos.z;
	n.fwd[0] = PackS(f.x, 32767.0f);
	n.fwd[1] = PackS(f.y, 32767.0f);
	n.fwd[2] = PackS(f.z, 32767.0f);
	n.up[0] = PackS(u.x, 32767.0f);
	n.up[1] = PackS(u.y, 32767.0f);
	n.up[2] = PackS(u.z, 32767.0f);
	n.vel[0] = PackS(ms.x, 8192.0f);
	n.vel[1] = PackS(ms.y, 8192.0f);
	n.vel[2] = PackS(ms.z, 8192.0f);

#if COOP_ALLVEH && COOP_BIKELEAN
	if(type == 1) {
		CBike *b = (CBike *)v;
		n.lean = PackS(b->m_fLeanLRAngle, 10000.0f);
		n.pedLR = PackS(b->m_fPedLeanAmountLR, 10000.0f);
		n.pedUD = PackS(b->m_fPedLeanAmountUD, 10000.0f);
		n.leanIn = (int8)PackS(b->m_fLeanInput, 127.0f);
	}
#endif

#if COOP_DOORS || COOP_DAMAGE
	if(type == 0) {
		CAutomobile *a = (CAutomobile *)v;
#if COOP_DOORS
		for(int i = 0; i < 6; i++) {
			float open = a->Doors[i].RetAngleWhenOpen();
			float r = open != 0.0f ? a->Doors[i].m_fAngle / open : 0.0f;
			n.doorOpen[i] = (uint8)((r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r)) * 255.0f + 0.5f);
		}
#endif
#if COOP_DAMAGE
		for(int i = 0; i < 6; i++) n.doorState[i] = (uint8)a->Damage.GetDoorStatus(i);
		for(int i = 0; i < 7; i++) n.panelState[i] = (uint8)a->Damage.GetPanelStatus(i);
#if COOP_DMGEXTRA
		for(int i = 0; i < 4; i++) n.wheelState[i] = (uint8)a->Damage.GetWheelStatus(i);
#endif
#endif
	}
#endif
}

// =====================================================================
// vehicle state <- network
// =====================================================================
static void
ApplyExtras(CVehicle *v, int type, const CoopNet::RemoteVeh &r, uint8 *cache)
{
#if COOP_DOORS || COOP_DAMAGE
	if(type != 0) return;
	CAutomobile *a = (CAutomobile *)v;
#if COOP_DOORS
	// while the local player walks to / gets into / out of this vehicle the engine owns the doors
	CPed *lp = FindPlayerPed();
	bool localUse = (v == enterCar);
	if(lp && lp->m_pMyVehicle == v) {
		int lps = lp->m_nPedState;
		if(lps == PED_ENTER_CAR || lps == PED_SEEK_CAR || lps == PED_EXIT_CAR || lps == PED_CARJACK || lps == PED_DRAG_FROM_CAR) localUse = true;
	}
	for(int i = 0; i < 6 && !localUse; i++) {
		float open = a->Doors[i].RetAngleWhenOpen();
		float target = open * (r.doorOpen[i] / 255.0f);
		float cur = a->Doors[i].m_fAngle;
		float diff = target - cur;
		float ang = target;
		// ease towards the target so a door swings instead of jumping between snapshots
		if(fabsf(diff) > 0.003f && fabsf(diff) < fabsf(open) * 0.9f) {
			float k = 1.0f - powf(0.72f, CTimer::GetTimeStep());
			ang = cur + diff * k;
		}
		a->Doors[i].m_fAngle = ang;
		a->Doors[i].m_fPrevAngle = ang;
	}
#endif
#if COOP_DAMAGE
	// The local damage manager is compared with the remote one every frame (not with what was applied
	// last time), so damage that came from anywhere else still gets corrected.
	// Damage can only go away through Fix() (Pay 'n' Spray, trainer repair).
#if COOP_DMGEXTRA
	bool repaired = false;
	for(int i = 0; i < 6; i++)
		if((int)r.doorState[i] < (int)a->Damage.GetDoorStatus(i)) repaired = true;
	for(int i = 0; i < 7; i++)
		if((int)r.panelState[i] < (int)a->Damage.GetPanelStatus(i)) repaired = true;
	for(int i = 0; i < 4; i++)
		if(r.wheelState[i] == 0 && (int)a->Damage.GetWheelStatus(i) != 0) repaired = true;
	if(repaired) a->Fix();
#endif

	static const int doorNode[6] = {CAR_BONNET, CAR_BOOT, CAR_DOOR_LF, CAR_DOOR_RF, CAR_DOOR_LR, CAR_DOOR_RR};
	static const int panelNode[7] = {CAR_WING_LF, CAR_WING_RF, CAR_WING_LR, CAR_WING_RR, CAR_WINDSCREEN, CAR_BUMP_FRONT, CAR_BUMP_REAR};
	for(int i = 0; i < 6; i++) {
		if((int)a->Damage.GetDoorStatus(i) != (int)r.doorState[i]) {
			a->Damage.SetDoorStatus(i, r.doorState[i]);
			a->SetDoorDamage(doorNode[i], (eDoors)i, true);
			a->Damage.SetDoorStatus(i, r.doorState[i]); // the visual update must not leave a different status behind
		}
		cache[i] = r.doorState[i];
	}
	for(int i = 0; i < 7; i++) {
		if((int)a->Damage.GetPanelStatus(i) != (int)r.panelState[i]) {
			a->Damage.SetPanelStatus(i, r.panelState[i]);
			if(i >= 5)
				a->SetBumperDamage(panelNode[i], (ePanels)i, true);
			else
				a->SetPanelDamage(panelNode[i], (ePanels)i, true);
			a->Damage.SetPanelStatus(i, r.panelState[i]);
		}
		cache[6 + i] = r.panelState[i];
	}
#if COOP_DMGEXTRA
	for(int i = 0; i < 4; i++)
		if(r.wheelState[i] != 0 && (int)a->Damage.GetWheelStatus(i) == 0) a->BurstTyre((uint8)i, false);
#endif
#endif
#endif
}

// The engine poses seated peds from the vehicle matrix during the world update. A vehicle
// that is moved by the network afterwards leaves its occupants one frame behind, which looks
// like riders sunk backwards into their seats. Pose them again from the new matrix.
static void
RefreshOccupants(CVehicle *car)
{
#if COOP_SEATFIX
	CPed *d = car->pDriver;
	if(d && d->bInVehicle && d->m_pMyVehicle == car) d->SetPedPositionInCar();
	for(int i = 0; i < 8; i++) {
		CPed *p = car->pPassengers[i];
		if(p && p->bInVehicle && p->m_pMyVehicle == car) p->SetPedPositionInCar();
	}
#endif
}

#if COOP_ALLVEH && COOP_BIKESMOOTH
// Bikes are small and light, so tiny differences between network snapshots show up as shaking.
// Keep a filtered copy of each bike's target and move it a fraction of the way each frame.
// Collision, physics and seating are not touched.
struct BikeSmooth {
	CVehicle *car;
	float p[3], v[3]; // followed position and its velocity (units per second)
	float f[3], u[3], lean, leanV, pLR, pLRv, pUD, pUDv, lIn, lInv; // the lean values are followed by springs too
};
static BikeSmooth bikeSm[16];
static int bikeSmNext = 0;

static void
SmoothNorm(float *v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if(l > 1e-6f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void
Damp1(float &cur, float &vel, float target, float smoothTime, float dt)
{
	float omega = 2.0f / smoothTime;
	float x = omega * dt;
	float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
	float change = cur - target;
	float temp = (vel + omega * change) * dt;
	vel = (vel - omega * temp) * e;
	cur = target + (change + temp) * e;
}

// A critically damped follower (like a spring that never overshoots) driven by the network position.
// It does not depend on the frame rate or on how the snapshots are spaced, and the velocity used for the
// lag compensation comes from the follower itself, so snapshot noise is not amplified.
static void
SmoothBike(CVehicle *car, CoopNet::RemoteVeh &r)
{
	const float smoothTime = 0.05f;
	float dt = CTimer::GetTimeStep() / 50.0f;
	if(dt < 0.002f) dt = 0.002f;
	if(dt > 0.1f) dt = 0.1f;

	BikeSmooth *s = nil;
	for(int i = 0; i < 16; i++)
		if(bikeSm[i].car == car) {
			s = &bikeSm[i];
			break;
		}
	bool fresh = false;
	if(s == nil) {
		s = &bikeSm[bikeSmNext];
		bikeSmNext = (bikeSmNext + 1) % 16;
		s->car = car;
		fresh = true;
	}
	float tx = r.x, ty = r.y, tz = r.z;
	float dx = tx - s->p[0], dy = ty - s->p[1], dz = tz - s->p[2];
	if(fresh || dx * dx + dy * dy + dz * dz > 25.0f) { // first sight or a teleport: snap
		s->p[0] = tx; s->p[1] = ty; s->p[2] = tz;
		s->v[0] = s->v[1] = s->v[2] = 0.0f;
		s->f[0] = r.fx; s->f[1] = r.fy; s->f[2] = r.fz;
		s->u[0] = r.ux; s->u[1] = r.uy; s->u[2] = r.uz;
		s->lean = r.lean;
		s->pLR = r.pedLR;
		s->pUD = r.pedUD;
		s->lIn = r.leanIn;
		s->leanV = s->pLRv = s->pUDv = s->lInv = 0.0f;
		return;
	}

	float omega = 2.0f / smoothTime;
	float x = omega * dt;
	float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
	float target[3] = {tx, ty, tz};
	for(int i = 0; i < 3; i++) {
		float change = s->p[i] - target[i];
		float temp = (s->v[i] + omega * change) * dt;
		s->v[i] = (s->v[i] - omega * temp) * e;
		s->p[i] = target[i] + (change + temp) * e;
	}

	float a = 1.0f - expf(-dt / 0.04f);
	s->f[0] += (r.fx - s->f[0]) * a; s->f[1] += (r.fy - s->f[1]) * a; s->f[2] += (r.fz - s->f[2]) * a;
	s->u[0] += (r.ux - s->u[0]) * a; s->u[1] += (r.uy - s->u[1]) * a; s->u[2] += (r.uz - s->u[2]) * a;
	// The lean angle and the rider's lean values changed in steps (a new snapshot ~30 times a second) and the
	// first-order filter that was used here keeps a kink at every step. Springs have none.
	// LEAN_SMOOTH: seconds; raise for smoother (but later) lean, lower for snappier.
	const float LEAN_SMOOTH = 0.09f;
	Damp1(s->lean, s->leanV, r.lean, LEAN_SMOOTH, dt);
	Damp1(s->pLR, s->pLRv, r.pedLR, LEAN_SMOOTH, dt);
	Damp1(s->pUD, s->pUDv, r.pedUD, LEAN_SMOOTH, dt);
	Damp1(s->lIn, s->lInv, r.leanIn, LEAN_SMOOTH, dt);
	SmoothNorm(s->f);
	SmoothNorm(s->u);

	// the follower trails a moving target by speed * smoothTime: add that back
	r.x = s->p[0] + s->v[0] * smoothTime;
	r.y = s->p[1] + s->v[1] * smoothTime;
	r.z = s->p[2] + s->v[2] * smoothTime;
	r.fx = s->f[0]; r.fy = s->f[1]; r.fz = s->f[2];
	r.ux = s->u[0]; r.uy = s->u[1]; r.uz = s->u[2];
	r.lean = s->lean;
	r.pedLR = s->pLR;
	r.pedUD = s->pUD;
	r.leanIn = s->lIn;
}
#endif


#if COOP_LATEFIX
// ---- late fix-up of network-driven vehicles ----
// The engine's own code runs on a puppet vehicle between our update and the moment it is drawn: its AI
// (mission NONE after StopAI) sets brake / handbrake / lights again, and the bike physics reacts to the lean
// values and shifts the bike a little. What the other player sees is then not what the owner sent: brake
// lights half on or off, tail lights switching at night, bikes that lean or pitch on their own.
// So the final (smoothed) network values are cached here and put back by BeforePreRender(), the last moment
// before CRenderer::PreRender() reads them.
struct NetVisual {
	CVehicle *car;
	CoopNet::RemoteVeh r;
	int type;
	double t;
};
static const int MAX_NETVIS = MAX_VPUP + MAXP + 4;
static NetVisual netVis[MAX_NETVIS];

static void
RememberNetVisual(CVehicle *car, const CoopNet::RemoteVeh &r, int type)
{
	int freeI = -1;
	double now = CoopNet::Time();
	for(int i = 0; i < MAX_NETVIS; i++) {
		if(netVis[i].car == car) {
			freeI = i;
			break;
		}
		if(freeI < 0 && (netVis[i].car == nil || now - netVis[i].t > 0.5)) freeI = i;
	}
	if(freeI < 0) return;
	netVis[freeI].car = car;
	netVis[freeI].r = r;
	netVis[freeI].type = type;
	netVis[freeI].t = now;
}

static bool
IsLiveVehicle(CVehicle *v)
{
	CVehiclePool *vp = CPools::GetVehiclePool();
	for(int i = 0; i < vp->GetSize(); i++)
		if(vp->GetSlot(i) == v) return true;
	return false;
}

void
BeforePreRender()
{
	if(!CoopNet::IsActive()) return;
	double now = CoopNet::Time();
	for(int i = 0; i < MAX_NETVIS; i++) {
		NetVisual &nv = netVis[i];
		if(nv.car == nil) continue;
		if(now - nv.t > 0.3 || !IsLiveVehicle(nv.car)) { // stale: the vehicle is no longer network driven (or is gone)
			nv.car = nil;
			continue;
		}
		CVehicle *car = nv.car;
		const CoopNet::RemoteVeh &r = nv.r;
		if(car->GetStatus() == STATUS_WRECKED) continue;

		// where the owner says it is: physics may have nudged it since
		CVector f(r.fx, r.fy, r.fz), u(r.ux, r.uy, r.uz);
		CMatrix &m = car->GetMatrix();
		m.GetRight() = CrossProduct(f, u);
		m.GetForward() = f;
		m.GetUp() = u;
		m.GetPosition() = CVector(r.x, r.y, r.z);
		m.UpdateRW();
		car->UpdateRwFrame();
		car->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);

		// lamps: exactly what the owner's car shows
		car->bLightsOn = (r.flags & 1) != 0;
		car->bEngineOn = (r.flags & 2) != 0;
		car->m_fBrakePedal = (r.flags & 64) && r.brake < 1.0f ? 1.0f : r.brake;
		car->m_fGasPedal = r.gas;
		car->m_fSteerAngle = r.steer;
		car->bIsHandbrakeOn = false; // lamps come from the pedal above; the real handbrake would make skidmarks

#if COOP_ALLVEH && COOP_BIKELEAN
		if(nv.type == 1) { // bike lean is shown only: the owner's values, never anything local
			CBike *b = (CBike *)car;
			b->m_fLeanLRAngle = r.lean;
			b->m_fLeanLRAngle2 = r.lean;
			b->m_fPedLeanAmountLR = r.pedLR;
			b->m_fPedLeanAmountUD = r.pedUD;
			b->m_fLeanInput = r.leanIn;
			b->bLeanMatrixClean = false;
		}
#endif
		RefreshOccupants(car);
	}
}
#else
static void RememberNetVisual(CVehicle *, const CoopNet::RemoteVeh &, int) {}
void BeforePreRender() {}
#endif

// drive the vehicle from network data; the owner's machine does the real physics
static void
PlaceVehicle(CVehicle *car, const CoopNet::RemoteVeh &rIn, int type, bool blowOk = true)
{
	CoopNet::RemoteVeh r = rIn;
#if COOP_ALLVEH && COOP_BIKESMOOTH
	if(type == 1) SmoothBike(car, r);
#endif
	CVector f(r.fx, r.fy, r.fz), u(r.ux, r.uy, r.uz);
	CVector right = CrossProduct(f, u);
	CMatrix &m = car->GetMatrix();
	CVector oldPos = m.GetPosition();
	m.GetRight() = right;
	m.GetForward() = f;
	m.GetUp() = u;
	m.GetPosition() = CVector(r.x, r.y, r.z);
	m.UpdateRW();
	car->UpdateRwFrame();

	car->m_vecMoveSpeed = CVector(r.vx, r.vy, r.vz);
	car->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
	car->m_fSteerAngle = r.steer;
	car->m_fBrakePedal = (r.flags & 64) && r.brake < 1.0f ? 1.0f : r.brake;
	car->m_fGasPedal = r.gas;
	car->bLightsOn = (r.flags & 1) != 0;
	car->bEngineOn = (r.flags & 2) != 0;
	bool wrecked = false;
#if COOP_EXPLODE
	wrecked = (r.flags & 128) != 0;
#endif
	if(wrecked) {
		// it blew up on the other machine: blow it up here too (once). A wreck that was already burnt out
		// when we first saw it is just marked wrecked, no new explosion.
		if(car->GetStatus() != STATUS_WRECKED) {
			if(blowOk) {
				car->BlowUpCar(nil);
			} else {
				car->m_fHealth = 0.0f;
				car->SetStatus(STATUS_WRECKED);
			}
		}
	} else {
		float hp = r.health * 4.0f;
		car->m_fHealth = hp < 260.0f ? 260.0f : hp;
		car->SetStatus((car->pDriver != nil || (r.flags & 16)) ? STATUS_PHYSICS : STATUS_ABANDONED);
	}
	car->m_nDoorLock = CARLOCK_UNLOCKED;
	SetNetProof(car, true);
	StopAI(car);
	car->bUsesCollision = true;
	if(type == 3 || type == 4) car->bAffectedByGravity = false;

#if COOP_NOSKID
	// We move the car by force and the engine's AI pedals (brake / handbrake of a driver without a mission) act
	// on its wheels meanwhile, so every wheel looked like it was skidding: skidmarks and tyre squeal all the
	// time. The brake lights and pedals shown come from the network values set above; the wheels are told
	// they are rolling normally.
	car->bIsHandbrakeOn = false;
	if(type == 0) {
		CAutomobile *au = (CAutomobile *)car;
		for(int i = 0; i < 4; i++) au->m_aWheelState[i] = WHEEL_STATE_NORMAL;
	}
#endif

#if COOP_ALLVEH && COOP_BIKELEAN
	if(type == 1) {
		CBike *b = (CBike *)car;
		b->m_fLeanLRAngle = r.lean;
		b->m_fLeanLRAngle2 = r.lean;
		b->m_fPedLeanAmountLR = r.pedLR;
		b->m_fPedLeanAmountUD = r.pedUD;
		b->m_fLeanInput = r.leanIn;
		// the lean matrix used for drawing is cached; without this it keeps the angle the bike's own
		// (puppet) physics computed, which is what made the tilt wrong and jumpy
		b->bLeanMatrixClean = false;
	}
#endif

	if((m.GetPosition() - oldPos).Magnitude() > 15.0f) {
		CWorld::Remove(car);
		CWorld::Add(car);
	}
	RememberNetVisual(car, r, type);
	RefreshOccupants(car);
}

// =====================================================================
// host side
// =====================================================================
struct Cand {
	float d;
	void *p;
};

static void
SortCands(Cand *c, int n)
{
	for(int i = 1; i < n; i++) {
		Cand t = c[i];
		int j = i - 1;
		while(j >= 0 && c[j].d > t.d) {
			c[j + 1] = c[j];
			j--;
		}
		c[j + 1] = t;
	}
}

static int
GatherAnims(CPed *p, CoopNet::AnimEntry *out, int maxn)
{
	RpClump *clump = (RpClump *)p->m_rwObject;
	if(clump == nil) return 0;

	CoopNet::AnimEntry list[24];
	int cnt = 0;
	for(CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(clump); a != nil && cnt < 24; a = RpAnimBlendGetNextAssociation(a)) {
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
	for(int i = 1; i < cnt; i++) {
		CoopNet::AnimEntry t = list[i];
		int j = i - 1;
		while(j >= 0 && list[j].blend < t.blend) {
			list[j + 1] = list[j];
			j--;
		}
		list[j + 1] = t;
	}
	int n = cnt > maxn ? maxn : cnt;
	for(int i = 0; i < n; i++) out[i] = list[i];
	return n;
}

// The guest in `slot` is driving a car from its own (independent) world. Make a copy of it in the host
// world so everybody sees it; the guest then swaps its car for the copy (see AdoptSwap).
static void
HostAdopt(int slot)
{
	CVehiclePool *vp = CPools::GetVehiclePool();
	int32 &adopt = adoptId[slot];
	if(adopt >= 0 && (SafeVeh(adopt) == nil || guestOwnedId[slot] == adopt)) adopt = -1; // gone, or already taken over

	CoopNet::RemoteVeh g;
	bool have = CoopNet::GetGuestVehicle(slot, g);
	if(!have || g.id >= 0) {
		if(!have && adopt >= 0) { // the guest got out before swapping: the copy stays in the world, parked
			CVehicle *c = SafeVeh(adopt);
			if(c) {
				c->SetStatus(STATUS_ABANDONED);
				SetNetProof(c, false);
			}
			adopt = -1;
		}
		return;
	}

	CVehicle *car = adopt >= 0 ? SafeVeh(adopt) : nil;
	if(car == nil) {
		int model = g.model;
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(model);
		if(mi == nil || mi->GetModelType() != MITYPE_VEHICLE) return;
		if(!CStreaming::HasModelLoaded(model)) {
			CStreaming::RequestModel(model, 0);
			return;
		}
		if(vp->GetNoOfFreeSpaces() < 4) return;
		COOP_CRUMB("H:adoptNewCar", slot, model);
		car = MakeVehicle(g.type, model);
		if(car == nil) return;
		car->m_currentColour1 = g.col1;
		car->m_currentColour2 = g.col2;
		car->SetPosition(CVector(g.x, g.y, g.z));
		CWorld::Add(car);
		adopt = vp->GetIndex(car);
	}
	PlaceVehicle(car, g, g.type);
	ApplyExtras(car, g.type, g, hostDmg[slot]);
	ApplyVehicleFire(car, g.burn != 0);
}

// the guest in `slot` is driving one of our cars: take our AI out of it and mirror the guest's version
static void
HostOwnership(int slot)
{
	HostAdopt(slot);
	CoopNet::PlayerState rs;
	int32 want = -1;
	if(CoopNet::GetRemoteInterp(slot, rs) && (rs.flags & 2) && rs.seat == 0) want = rs.vehId;
	if(want >= 0) {
		CVehicle *v = SafeVeh(want);
		// only skip the car when the HOST is the one driving it. FindPlayerVehicle() is also
		// the car the host sits in as a passenger, which used to drop the guest's ownership
		// and leave the car abandoned and frozen.
		if(v == nil || v->pDriver == FindPlayerPed() || VehType(v) < 0) want = -1;
		// another guest owns it right now: that one is told to get out by its own driver-conflict check,
		// then the car changes hands here
		for(int o = 1; o < MAXP && want >= 0; o++)
			if(o != slot && guestOwnedId[o] == want) want = -1;
	}

	if(want != guestOwnedId[slot]) {
		if(guestOwnedId[slot] >= 0) {
			CVehicle *old = SafeVeh(guestOwnedId[slot]);
			if(old) {
				old->SetStatus(STATUS_ABANDONED);
				SetNetProof(old, false);
			}
		}
		guestOwnedId[slot] = want;
		if(want >= 0) {
			CVehicle *v = SafeVeh(want);
			if(v && v->pDriver && v->pDriver != FindPlayerPed()) {
				CPopulation::RemovePed(v->pDriver);
				v->pDriver = nil;
			}
		}
	}

	if(guestOwnedId[slot] >= 0) {
		CVehicle *v = SafeVeh(guestOwnedId[slot]);
		CoopNet::RemoteVeh r;
		if(v && CoopNet::GetGuestVehicle(slot, r) && r.id == guestOwnedId[slot]) {
			// the guest's state arrives (and is interpolated) a little late: push it forward by part of
			// that delay along its own velocity so the car and everyone in it don't trail behind
			float bufMs, pps;
			CoopNet::GetNetStats(slot, bufMs, pps);
			float lead = bufMs * 0.001f * 50.0f * 0.6f;
			if(VehType(v) == 1) lead = 0.0f; // bikes: the smoothing follower does that
			if(lead > 4.0f) lead = 4.0f;
			if(lead < 0.0f) lead = 0.0f;
			r.x += r.vx * lead;
			r.y += r.vy * lead;
			r.z += r.vz * lead;
			PlaceVehicle(v, r, VehType(v));
			ApplyExtras(v, VehType(v), r, hostDmg[slot]); // doors, panels and tyres of the guest's car
			ApplyVehicleFire(v, r.burn != 0);
		}
	}
}

// nobody is connected in that slot (any more): let go of everything that belonged to it
static void
HostForgetSlot(int slot)
{
	hostShared[slot] = false;
	if(guestOwnedId[slot] >= 0) {
		CVehicle *old = SafeVeh(guestOwnedId[slot]);
		if(old) {
			old->SetStatus(STATUS_ABANDONED);
			SetNetProof(old, false);
		}
		guestOwnedId[slot] = -1;
	}
	adoptId[slot] = -1;
}

#if COOP_FIRE
static void
HostSendFires()
{
	static CoopNet::FireNet fs[CoopNet::MAX_FIRES];
	int n = 0;
	for(int i = 0; i < NUM_FIRES && n < CoopNet::MAX_FIRES; i++) {
		CFire &f = gFireManager.m_aFires[i];
		if(!f.m_bIsOngoing) continue;
		if(f.m_pEntity != nil && f.m_pEntity->IsVehicle()) continue; // burning vehicles travel with the vehicle (VehNet.burn)
		fs[n].x = f.m_vecPos.x;
		fs[n].y = f.m_vecPos.y;
		fs[n].z = f.m_vecPos.z;
		fs[n].size = 1;
		n++;
	}
	for(int slot = 1; slot < MAXP; slot++)
		if(hostShared[slot] && CoopNet::IsSlotActive(slot)) CoopNet::SendFires(slot, fs, n);
}
#endif

static void
HostSend()
{
	CPed *player = FindPlayerPed();
	if(player == nil) return;
	double now = CoopNet::Time();

	bool doWorld = now - lastWorldSend >= 0.1;
	bool doEnt = now - lastEntSend >= 0.033;
	if(doWorld) lastWorldSend = now;
	if(doEnt) lastEntSend = now;

	for(int slot = 1; slot < MAXP; slot++) {
		if(!CoopNet::IsSlotActive(slot)) {
			if(hostShared[slot] || guestOwnedId[slot] >= 0 || adoptId[slot] >= 0) COOP_CRUMB("H:forgetSlot", slot);
			HostForgetSlot(slot);
			continue;
		}

		COOP_CRUMB("H:ownership", slot);
		HostOwnership(slot);

		// shared / independent decision for this guest (with hysteresis)
		float gx, gy, gz;
		bool known = CoopNet::GetRemote(slot, gx, gy, gz);
		CVector gpos(gx, gy, gz);
		if(!known) {
			hostShared[slot] = false;
		} else {
			float dist = (player->GetPosition() - gpos).Magnitude();
			if(hostShared[slot]) {
				if(dist > SHARE_LEAVE) hostShared[slot] = false;
			} else if(dist < SHARE_ENTER) {
				hostShared[slot] = true;
			}
		}
		CoopNet::SetPeerShared(slot, hostShared[slot]);

		// the guest drove its car out of the shared world: the car goes with it, so remove our copy
		if(!hostShared[slot] && guestOwnedId[slot] >= 0) {
			CVehicle *c = SafeVeh(guestOwnedId[slot]);
			if(c && !(player->bInVehicle && player->m_pMyVehicle == c)) {
				COOP_CRUMB("H:deleteGuestCar", slot, (int)guestOwnedId[slot]);
				CoopMenu::ReleasePuppetFrom(c);
				CWorld::Remove(c);
				delete c;
			}
			guestOwnedId[slot] = -1;
		}

		if(doWorld) {
			CoopNet::WorldNet w;
			memset(&w, 0, sizeof w);
			w.hours = (uint8)CClock::ms_nGameClockHours;
			w.minutes = (uint8)CClock::ms_nGameClockMinutes;
			w.seconds = (uint8)CClock::ms_nGameClockSeconds;
			w.oldWeather = (int16)CWeather::OldWeatherType;
			w.newWeather = (int16)CWeather::NewWeatherType;
			float iv = CWeather::InterpolationValue;
			w.interp = (uint8)((iv < 0.0f ? 0.0f : (iv > 1.0f ? 1.0f : iv)) * 255.0f);
			w.shared = hostShared[slot] ? 1 : 0;
			w.friendlyFire = optFriendlyFire ? 1 : 0;
			CoopNet::SendWorld(slot, w);
		}

		if(!hostShared[slot] || !doEnt) continue;

		COOP_CRUMB("H:buildLists", slot);
		static Cand cand[MAXC];
		int nc = 0;

		// ---- vehicles (nearest to this guest first) ----
		CVehiclePool *vp = CPools::GetVehiclePool();
		CVehicle *playerVeh = FindPlayerVehicle();
		for(int i = 0; i < vp->GetSize() && nc < MAXC; i++) {
			CVehicle *v = vp->GetSlot(i);
			if(v == nil || VehType(v) < 0) continue;
			if(guestOwnedId[slot] >= 0 && vp->GetIndex(v) == guestOwnedId[slot]) continue; // this guest owns it right now
			float d = (v->GetPosition() - gpos).Magnitude();
			if(v == playerVeh)
				d = -1.0f; // always send the car the host is in
			else if(d > VEH_RANGE)
				continue;
			cand[nc].d = d;
			cand[nc].p = v;
			nc++;
		}
		SortCands(cand, nc);

		static CoopNet::VehNet vs[MAX_SEND_VEH];
		int nv = 0;
		for(int i = 0; i < nc && nv < MAX_SEND_VEH; i++) {
			CVehicle *v = (CVehicle *)cand[i].p;
			FillVehNet(vs[nv], v, vp->GetIndex(v), VehType(v));
			nv++;
		}
		COOP_CRUMB("H:sendVehicles", slot, nv);
		CoopNet::SendVehicles(slot, vs, nv);

		// ---- pedestrians (including drivers and passengers) ----
		nc = 0;
		CPedPool *pp = CPools::GetPedPool();
		for(int i = 0; i < pp->GetSize() && nc < MAXC; i++) {
			CPed *p = pp->GetSlot(i);
			if(p == nil || p == player) continue;
			if(p->GetModelIndex() == MI_PLAYER) continue; // those are the co-op puppets of the other players
			if(p->bInVehicle) {
				CVehicle *pv = p->m_pMyVehicle;
				if(pv == nil || VehType(pv) < 0) continue;
				if(guestOwnedId[slot] >= 0 && vp->GetIndex(pv) == guestOwnedId[slot]) continue;
			}
			float d = (p->GetPosition() - gpos).Magnitude();
			if(d > PED_RANGE) continue;
			cand[nc].d = d;
			cand[nc].p = p;
			nc++;
		}
		SortCands(cand, nc);

		static CoopNet::PedNet ps[MAX_SEND_PED];
		int np = 0;
		for(int i = 0; i < nc && np < MAX_SEND_PED; i++) {
			CPed *p = (CPed *)cand[i].p;
			CVector pos = p->GetPosition();
			CoopNet::PedNet &n = ps[np++];
			memset(&n, 0, sizeof n);
			n.id = pp->GetIndex(p);
			n.model = (int16)p->GetModelIndex();
			n.moveState = (uint8)p->m_nMoveState;
			n.x = pos.x;
			n.y = pos.y;
			n.z = pos.z;
			n.heading = p->m_fRotationCur;
			n.vehId = -1;
			if(p->bInVehicle) {
				n.vehId = vp->GetIndex(p->m_pMyVehicle);
				n.seat = (uint8)SeatOf(p->m_pMyVehicle, p);
				n.numAnims = 0;
			} else {
				n.numAnims = (uint8)GatherAnims(p, n.anims, CoopNet::PED_ANIMS);
			}
		}
		COOP_CRUMB("H:sendPeds", slot, np);
		CoopNet::SendPeds(slot, ps, np);
	}

#if COOP_FIRE
	if(now - lastFireSend >= 0.2) {
		HostSendFires();
		lastFireSend = now;
	}
#endif
}

// =====================================================================
// guest side
// =====================================================================
static void
DeletePuppetPed(int k)
{
	if(pped[k] == nil) return;
	CPed *p = pped[k];
	COOP_CRUMB("DeletePuppetPed", k);
	pped[k] = nil;
	pveh[k] = -1;
	UnseatPed(p);
	CWorld::Remove(p);
	delete p;
}

static void
DeletePuppetCar(int k)
{
	if(vcar[k] == nil) return;
	CVehicle *car = vcar[k];
	COOP_CRUMB("DeletePuppetCar", k, (int)vid[k]);
	CPed *pl = FindPlayerPed();
	if(car == FindPlayerVehicle() || (pl && car->pDriver == pl)) return; // never delete the car the local player is sitting in

	CoopMenu::ReleasePuppetFrom(car);
	for(int p = 0; p < MAX_PPUP; p++)
		if(pped[p] && pveh[p] == vid[k]) DeletePuppetPed(p);

	vcar[k] = nil;
	CWorld::Remove(car);
	delete car;
}

static void
RemoveAllPuppets()
{
	// the vehicle the local player sits in goes with them into their own world: it stops being a puppet
	CPed *me = FindPlayerPed();
	for(int k = 0; k < MAX_VPUP; k++) {
		if(vcar[k] != nil && (vcar[k] == FindPlayerVehicle() || (me && vcar[k]->pDriver == me))) {
			vcar[k]->CleanUpOldReference((CEntity **)&vcar[k]);
			vcar[k] = nil;
			vid[k] = -1;
		}
	}
	for(int i = 0; i < MAX_PPUP; i++) DeletePuppetPed(i);
	for(int i = 0; i < MAX_VPUP; i++) DeletePuppetCar(i);
	dbgCars = dbgPeds = dbgSeated = 0;
}

static CoopNet::WorldNet lastWorld; // the newest world packet; its weather is held every frame
static bool haveWorld = false;

#if COOP_WEATHERHOLD
// Between two host packets (10 per second) the clock and the weather blend are carried on locally, at the speed
// the host showed, and corrected softly when the next packet arrives. Before, both were snapped back to the
// last packet every frame: the blend value (and so rain, clouds, fog and light) stepped 10 times a second, and
// the clock was thrown back across an hour boundary again and again, which made the engine pick weather over
// and over. That was the flicker.
static const double GAME_SEC_PER_SEC = 60.0; // 1 game minute = 1 real second (the game's default clock speed)
static double wxStamp = 0;       // local time of the newest world packet
static double wxHostSec = 0;     // host clock (seconds of the day) in that packet
static float wxIvPkt = 0.0f;     // blend value in that packet
static float wxRate = 0.0f;      // blend change per second, measured over a couple of seconds
static double wxRefT = 0;        // rate measurement start
static float wxRefIv = 0.0f;
static float wxShown = 0.0f;     // blend value we show
static bool wxShownValid = false;
static double wxClockShown = 0;  // clock we show (seconds of the day)
static bool wxClockValid = false;
static double wxLastApply = 0;
static bool weatherApplying = false;

static void
WeatherHoldReset()
{
	wxShownValid = false;
	wxClockValid = false;
	wxRate = 0.0f;
	wxRefT = 0;
}
#endif

static void
ApplyWorld()
{
	CoopNet::WorldNet w;
	if(CoopNet::GetWorld(w)) {
#if COOP_WEATHERHOLD
		double pnow = CoopNet::Time();
		float ivPkt = w.interp / 255.0f;
		if(!haveWorld || lastWorld.oldWeather != w.oldWeather || lastWorld.newWeather != w.newWeather) {
			wxRate = 0.0f; // new transition: nothing known about its speed yet
			wxRefT = pnow;
			wxRefIv = ivPkt;
			wxShownValid = false;
		} else if(pnow - wxRefT >= 2.0) { // measure over >= 2 s: one 1/255 step of rounding is then negligible
			float r = (ivPkt - wxRefIv) / (float)(pnow - wxRefT);
			wxRate = (r < 0.0f || r > 0.5f) ? 0.0f : r;
			wxRefT = pnow;
			wxRefIv = ivPkt;
		}
		wxStamp = pnow;
		wxHostSec = w.hours * 3600.0 + w.minutes * 60.0 + w.seconds;
		wxIvPkt = ivPkt;
#endif
		lastWorld = w;
		haveWorld = true;
#if !COOP_WEATHERHOLD
		CClock::ms_nGameClockHours = w.hours;
		CClock::ms_nGameClockMinutes = w.minutes;
		CClock::ms_nGameClockSeconds = w.seconds;
#endif
		guestShared = (w.shared != 0);
		guestFriendlyFire = (w.friendlyFire != 0);
	}
	if(!haveWorld) return;

#if COOP_WEATHERHOLD
	{
		double now = CoopNet::Time();
		double dtw = now - wxLastApply;
		wxLastApply = now;
		if(dtw < 0.001) dtw = 0.001;
		if(dtw > 0.1) dtw = 0.1;
		double since = now - wxStamp;
		if(since > 0.5) since = 0.5; // packets stopped: don't run on forever

		// clock: host time + local progress since the packet; never thrown back by a small correction
		double want = wxHostSec + since * GAME_SEC_PER_SEC;
		if(wxClockValid) {
			double d = want - wxClockShown;
			if(d < -43200.0) d += 86400.0;
			if(d > 43200.0) d -= 86400.0;
			if(d < 0.0 && d > -120.0) want = wxClockShown; // behind by a little: hold, the host catches up
		}
		while(want >= 86400.0) want -= 86400.0;
		while(want < 0.0) want += 86400.0;
		wxClockShown = want;
		wxClockValid = true;
		CClock::ms_nGameClockHours = (int)(want / 3600.0);
		CClock::ms_nGameClockMinutes = (int)(fmod(want, 3600.0) / 60.0);
		CClock::ms_nGameClockSeconds = (int)fmod(want, 60.0);

		// weather blend: carried on at the host's speed, followed softly
		int16 oldW = lastWorld.oldWeather, newW = lastWorld.newWeather;
		float tgt = wxIvPkt + wxRate * (float)since;
		if(oldW == newW) tgt = 0.0f; // steady weather: nothing to blend, nothing that could end
		if(tgt < 0.0f) tgt = 0.0f;
		if(tgt > 0.995f) tgt = 0.995f; // never let the engine see the end of a transition (it would pick its own next weather)
		if(!wxShownValid) {
			wxShown = tgt;
			wxShownValid = true;
		} else {
			wxShown += (tgt - wxShown) * (1.0f - expf(-(float)dtw / 0.25f));
		}

		CWeather::ForcedWeatherType = -1; // a script on this machine must not override the host's weather
		CWeather::OldWeatherType = oldW;
		CWeather::NewWeatherType = newW;
		CWeather::InterpolationValue = wxShown;
		weatherApplying = true;
		CWeather::Update(); // rain, clouds, fog and wet roads are derived in here
		weatherApplying = false;
		CWeather::OldWeatherType = oldW; // Update advanced the transition a little: back to the host's values
		CWeather::NewWeatherType = newW;
		CWeather::InterpolationValue = wxShown;
	}
#elif COOP_WEATHERFIX
	// The host sends its weather 10 times a second. In between, this machine's own CWeather::Update kept running:
	// it advanced the transition, and at an hour change or when a transition ended it picked ITS OWN next weather.
	// The values were put back at the next packet, but rain, clouds and fog are derived inside Update, so they
	// flickered (and rain showed up on the ground). Now the host's weather is set every frame, and Update is run
	// once more on it so everything derived (rain, clouds, fog, wet roads) matches the host's weather.
	int16 oldW = lastWorld.oldWeather, newW = lastWorld.newWeather;
	float iv = lastWorld.interp / 255.0f;
	if(oldW == newW) iv = 0.0f;   // steady weather: nothing to blend, nothing that could end
	if(iv > 0.995f) iv = 0.995f;  // never let the engine see the end of a transition (it would pick its own next weather)
	CWeather::OldWeatherType = oldW;
	CWeather::NewWeatherType = newW;
	CWeather::InterpolationValue = iv;
	CWeather::Update();
	CWeather::OldWeatherType = oldW; // Update advanced the transition a little: start the next frame from the host's values
	CWeather::NewWeatherType = newW;
	CWeather::InterpolationValue = iv;
#else
	CWeather::OldWeatherType = lastWorld.oldWeather;
	CWeather::NewWeatherType = lastWorld.newWeather;
	CWeather::InterpolationValue = lastWorld.interp / 255.0f;
#endif
}

bool
WeatherLocked()
{
#if COOP_WEATHERHOLD
	return haveWorld && !weatherApplying && CoopNet::GetState() == CoopNet::CLIENT;
#else
	return false;
#endif
}

// in shared mode the guest takes its traffic and pedestrians from the host, so drop the local ones
static void
ClearLocalAmbient()
{
	CVehicle *playerCar = FindPlayerVehicle();
	CPed *player = FindPlayerPed();

	CVehiclePool *vp = CPools::GetVehiclePool();
	for(int i = 0; i < vp->GetSize(); i++) {
		CVehicle *v = vp->GetSlot(i);
		if(v == nil || v == playerCar) continue;
		if(v->VehicleCreatedBy != RANDOM_VEHICLE && v->VehicleCreatedBy != PARKED_VEHICLE) continue;
		if(v->pDriver) {
			CPopulation::RemovePed(v->pDriver);
			v->pDriver = nil;
		}
		for(int k = 0; k < 8; k++) {
			if(v->pPassengers[k]) {
				CPopulation::RemovePed(v->pPassengers[k]);
				v->pPassengers[k] = nil;
			}
		}
		COOP_CRUMB("ClearAmbient:deleteVehicle", i);
		CWorld::Remove(v);
		delete v;
	}

	CPedPool *pp = CPools::GetPedPool();
	for(int i = 0; i < pp->GetSize(); i++) {
		CPed *p = pp->GetSlot(i);
		if(p == nil || p == player || p->bInVehicle) continue;
		if(p->CharCreatedBy != RANDOM_CHAR) continue;
		COOP_CRUMB("ClearAmbient:removePed", i);
		CPopulation::RemovePed(p);
	}
}

static bool
ModelIs(int model, int type)
{
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(model);
	return mi != nil && mi->GetModelType() == type;
}

// the car the local player drives that belongs to the guest's own world (not a host puppet)
static CVehicle *
NativeDrivenVehicle()
{
	CPed *pl = FindPlayerPed();
	if(pl == nil || !pl->bInVehicle || pl->m_pMyVehicle == nil) return nil;
	CVehicle *v = pl->m_pMyVehicle;
	if(v->pDriver != pl || VehType(v) < 0) return nil;
	for(int k = 0; k < MAX_VPUP; k++)
		if(vcar[k] == v) return nil;
	return v;
}

// the host made a copy of our car: move the player, speed and orientation over and delete the original
static bool
AdoptSwap(CVehicle *nat, CVehicle *car)
{
	CPed *pl = FindPlayerPed();
	if(pl == nil) return false;

	CMatrix &s = nat->GetMatrix();
	CMatrix &d = car->GetMatrix();
	d.GetRight() = s.GetRight();
	d.GetForward() = s.GetForward();
	d.GetUp() = s.GetUp();
	d.GetPosition() = s.GetPosition();
	d.UpdateRW();
	car->UpdateRwFrame();
	car->m_vecMoveSpeed = nat->m_vecMoveSpeed;
	car->m_vecTurnSpeed = nat->m_vecTurnSpeed;
	car->m_fSteerAngle = nat->m_fSteerAngle;
	car->m_fHealth = nat->m_fHealth;
	car->bLightsOn = nat->bLightsOn;
	car->bEngineOn = nat->bEngineOn;
	CWorld::Remove(car);
	CWorld::Add(car);

	UnseatPed(pl);
	if(!SeatPed(pl, car, true)) {
		SeatPed(pl, nat, true); // could not switch: stay in the original
		return false;
	}

	for(int k = 0; k < 8; k++) {
		if(nat->pPassengers[k]) {
			CPopulation::RemovePed(nat->pPassengers[k]);
			nat->pPassengers[k] = nil;
		}
	}
	SetNetProof(car, false);
	CWorld::Remove(nat);
	delete nat;
	return true;
}

static void
SyncVehicles()
{
	static CoopNet::RemoteVeh rv[MAX_VPUP];
	int n = CoopNet::GetRemoteVehicles(rv, MAX_VPUP);
	double now = CoopNet::Time();
	CPed *pl = FindPlayerPed();

	for(int i = 0; i < n; i++) {
		int slot = -1, freeSlot = -1;
		for(int k = 0; k < MAX_VPUP; k++) {
			if(vcar[k] && vid[k] == rv[i].id) {
				slot = k;
				break;
			}
			if(!vcar[k] && freeSlot < 0) freeSlot = k;
		}
		if(slot < 0) {
			if(freeSlot < 0) continue;
			int model = rv[i].model;
			if(!ModelIs(model, MITYPE_VEHICLE)) continue;
			if(!CStreaming::HasModelLoaded(model)) {
				CStreaming::RequestModel(model, 0);
				continue;
			}
			COOP_CRUMB("Sync:newVehicle", (int)rv[i].id, model);
			CVehicle *car = MakeVehicle(rv[i].type, model);
			if(car == nil) continue;
			car->m_currentColour1 = rv[i].col1;
			car->m_currentColour2 = rv[i].col2;
			vcar[freeSlot] = car;
			vid[freeSlot] = rv[i].id;
			vtype[freeSlot] = rv[i].type;
			vwreck[freeSlot] = (rv[i].flags & 128) != 0;
			memset(vdmg[freeSlot], 0, sizeof vdmg[freeSlot]);
			car->RegisterReference((CEntity **)&vcar[freeSlot]);
			CWorld::Add(car);
			slot = freeSlot;

			// this is the host's copy of the car we are driving in our own world: switch to it
			CVehicle *nat = NativeDrivenVehicle();
			if(nat && nat->GetModelIndex() == model &&
			   (nat->GetPosition() - CVector(rv[i].x, rv[i].y, rv[i].z)).Magnitude() < 12.0f)
				AdoptSwap(nat, car);
		}
		vlast[slot] = now;
		vflags[slot] = rv[i].flags;
		vfree[slot] = rv[i].freeSeats;

		CVehicle *car = vcar[slot];

		bool isLocalDriver = pl && pl->bInVehicle && car->pDriver == pl;

		if(!isLocalDriver) {
			PlaceVehicle(car, rv[i], vtype[slot], !vwreck[slot]);
			ApplyExtras(car, vtype[slot], rv[i], vdmg[slot]);
			ApplyVehicleFire(car, rv[i].burn != 0);
		} else {
			SetNetProof(car, false); // the local player drives it for real now: its damage is real, and is sent to the host
		}
	}

	// Remove old vehicle puppets
	for(int k = 0; k < MAX_VPUP; k++) {
		if(vcar[k] && now - vlast[k] > 1.0) DeletePuppetCar(k);
	}
}

// ---- pedestrian animation: play exactly what the host ped plays ----
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
SetAssoc(CAnimBlendAssociation *a, const CoopNet::AnimEntry &e, bool justAdded)
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

	if(justAdded || fabsf(diff) > 0.08f) a->SetCurrentTime(t);
}

static void
ApplyPedAnims(CPed *ped, const CoopNet::PedNet &rs)
{
	RpClump *clump = (RpClump *)ped->m_rwObject;
	if(clump == nil) return;

	bool used[CoopNet::PED_ANIMS] = {false, false, false};
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
			SetAssoc(a, rs.anims[match], false);
		} else {
			HideAssoc(a);
		}
	}

	int adds = 0;
	for(int i = 0; i < rs.numAnims; i++) {
		if(used[i] || rs.anims[i].blend == 0) continue;
		if(adds >= 2 || localCount > 24) break;

		CAnimBlendAssociation *a = SafeAddAnim(clump, rs.anims[i].group, rs.anims[i].animId);
		adds++;
		localCount++;

		if(a != nil) SetAssoc(a, rs.anims[i], true);
	}
}

static void
SyncPeds()
{
	static CoopNet::PedNet rp[MAX_PPUP];
	int n = CoopNet::GetRemotePeds(rp, MAX_PPUP);
	double now = CoopNet::Time();

	for(int i = 0; i < n; i++) {
		int slot = -1, freeSlot = -1;

		for(int k = 0; k < MAX_PPUP; k++) {
			if(pped[k] && pid[k] == rp[i].id) {
				slot = k;
				break;
			}
			if(!pped[k] && freeSlot < 0) freeSlot = k;
		}

		// the ped got into or out of a car: rebuild it
		if(slot >= 0 && pveh[slot] != rp[i].vehId) {
			DeletePuppetPed(slot);
			freeSlot = slot;
			slot = -1;
		}

#if COOP_HITS
		// the local player hurt this puppet: the real ped lives on the host, so tell the host and throw
		// the puppet away (a fresh one is built from the host's data next frame, so it never dies here)
		if(slot >= 0 && pveh[slot] < 0 && pped[slot] != nil) {
			float hp = pped[slot]->m_fHealth;
			if(hp < phealth[slot] - 0.5f) {
				CoopWorld::ReportHit(CoopNet::NO_SLOT, rp[i].id, rp[i].model, phealth[slot] - hp);
				DeletePuppetPed(slot);
				continue;
			}
		}
#endif

		CVector target(rp[i].x, rp[i].y, rp[i].z);
		if(slot < 0) {
			if(freeSlot < 0) continue;
			int model = rp[i].model;
			if(!ModelIs(model, MITYPE_PED)) continue;
			if(!CStreaming::HasModelLoaded(model)) {
				CStreaming::RequestModel(model, 0);
				continue;
			}
			CVehicle *car = nil;
			bool asDriver = rp[i].seat == 0;
			if(rp[i].vehId >= 0) {
				car = PuppetCarById(rp[i].vehId);
				if(car == nil) continue;                                // its car has not arrived yet
				if(asDriver && car->pDriver != nil) continue;           // somebody is already driving it
				if(!asDriver && FreePassengerSlots(car) == 0) continue; // no free seat
			}
			COOP_CRUMB("Sync:newPed", (int)rp[i].id, model);
			CPed *p = new CCivilianPed(PEDTYPE_CIVMALE, model);
			p->CharCreatedBy = MISSION_CHAR;
#if COOP_HITS
			// a puppet standing on the ground can be shot / punched: that is reported to the host (see below).
			// Fire, explosions and collisions are the host's business. Seated ones are not hurt at all.
			p->bFireProof = true;
			p->bExplosionProof = true;
			p->bCollisionProof = true;
			p->bBulletProof = (car != nil);
			p->bMeleeProof = (car != nil);
#endif
			p->SetPosition(car ? car->GetPosition() : target);
			CWorld::Add(p);
			if(car && !(asDriver ? SeatPed(p, car, true) : SeatPedAt(p, car, rp[i].seat))) {
				CWorld::Remove(p);
				delete p;
				continue;
			}
			pped[freeSlot] = p;
#if COOP_RUNOVER
			pknock[freeSlot] = 0.0;
#endif
			phealth[freeSlot] = p->m_fHealth;
			pid[freeSlot] = rp[i].id;
			pveh[freeSlot] = rp[i].vehId;
			p->RegisterReference((CEntity **)&pped[freeSlot]);
			slot = freeSlot;
		}
		plast[slot] = now;

		if(pveh[slot] >= 0) { // seated: the engine poses it from the car
#if COOP_RUNOVER
			if(now < pknock[slot]) continue; // the local knockdown is playing: do not overwrite it with the host's pose
#endif
			KeepSeated(pped[slot]);
			continue;
		}

		CPed *p = pped[slot];
		CWorld::Remove(p);
		p->SetPosition(target);
		CWorld::Add(p);

		p->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
		p->m_fRotationCur = rp[i].heading;
		p->m_fRotationDest = rp[i].heading;
		p->SetHeading(rp[i].heading);
		p->m_nMoveState = (decltype(p->m_nMoveState))rp[i].moveState;
		ApplyPedAnims(p, rp[i]);
	}

	for(int k = 0; k < MAX_PPUP; k++)
		if(pped[k] && now - plast[k] > 0.5) DeletePuppetPed(k);
}

#if COOP_FIRE
static CFire *gfire[CoopNet::MAX_FIRES];

static void
SyncFires()
{
	static CoopNet::FireNet fr[CoopNet::MAX_FIRES];
	int n = 0;
	if(!CoopNet::GetFires(fr, n)) return;
	bool keep[CoopNet::MAX_FIRES];
	for(int k = 0; k < CoopNet::MAX_FIRES; k++) keep[k] = false;

	for(int j = 0; j < n; j++) {
		CVector p(fr[j].x, fr[j].y, fr[j].z);
		int found = -1;
		for(int k = 0; k < CoopNet::MAX_FIRES; k++) {
			if(gfire[k] && gfire[k]->m_bIsOngoing && (gfire[k]->m_vecPos - p).Magnitude() < 2.0f) {
				found = k;
				break;
			}
		}
		if(found >= 0) {
			keep[found] = true;
			continue;
		}
		int freeSlot = -1;
		for(int k = 0; k < CoopNet::MAX_FIRES; k++) {
			if(!gfire[k] || !gfire[k]->m_bIsOngoing) {
				freeSlot = k;
				break;
			}
		}
		if(freeSlot < 0) continue;
		gFireManager.StartFire(p, 1.0f, false);

		// StartFire returns void, so find the newly created fire.
		CFire *f = nil;

		for(int i = 0; i < NUM_FIRES; i++) {
			CFire &candidate = gFireManager.m_aFires[i];

			if(candidate.m_bIsOngoing && (candidate.m_vecPos - p).Magnitude() < 2.0f) {
				f = &candidate;
				break;
			}
		}

		gfire[freeSlot] = f;

		if(f) keep[freeSlot] = true;
	}

	for(int k = 0; k < CoopNet::MAX_FIRES; k++) {
		if(gfire[k] && !keep[k]) {
			if(gfire[k]->m_bIsOngoing) gfire[k]->Extinguish();
			gfire[k] = nil;
		}
	}
}
#endif

// the guest is the driver of a host car: send its real state to the host
static void
GuestSendOwned()
{
	CPed *pl = FindPlayerPed();
	if(pl == nil || !pl->bInVehicle || pl->m_pMyVehicle == nil) return;
	CVehicle *v = pl->m_pMyVehicle;
	if(v->pDriver != pl) return;
	for(int k = 0; k < MAX_VPUP; k++) {
		if(vcar[k] == v) {
			CoopNet::VehNet n;
			FillVehNet(n, v, vid[k], vtype[k]);
			CoopNet::SendOwnedVehicle(n);
			return;
		}
	}

	// a car from our own world: offer it to the host (id -1 = "make a copy of this")
	int t = VehType(v);
	if(t >= 0 && NativeDrivenVehicle() == v) {
		CoopNet::VehNet n;
		FillVehNet(n, v, -1, t);
		CoopNet::SendOwnedVehicle(n);
	}
}

// G: get into the nearest vehicle that belongs to the other player's world
// ---- walking into a vehicle with G ----
static bool enterDriver = false;
static bool enterRetried = false;
static bool enterSawBusy = false;    // the engine really started walking / entering
static double enterIdleSince = -1.0; // when it stopped being busy again
static double enterStart = 0;

static void
ClearEnter()
{
	if(enterCar) enterCar->CleanUpOldReference((CEntity **)&enterCar);
	enterCar = nil;
}

// let the engine walk the player to the door and play the normal get-in animation
static void
StartEnter(CPed *pl, CVehicle *car, bool asDriver)
{
	ClearEnter();
	enterCar = car;
	car->RegisterReference((CEntity **)&enterCar);
	enterDriver = asDriver;
	enterStart = CoopNet::Time();
	enterRetried = false;
	enterSawBusy = false;
	enterIdleSince = -1.0;
	StopAI(car);
	pl->SetObjective(asDriver ? OBJECTIVE_ENTER_CAR_AS_DRIVER : OBJECTIVE_ENTER_CAR_AS_PASSENGER, car);
}

// Two players can end up as driver of the same vehicle (one took it from the other, "carjacking").
// The one who has been driving longer gets out; the newcomer keeps the wheel.
static void
ResolveDriverConflict()
{
	static int32 lastId = -1;
	static double since = 0, lastEject = -10;
	CPed *pl = FindPlayerPed();
	int32_t vehId;
	uint8_t seat;
	if(pl == nil || !GetLocalRiding(vehId, seat) || seat != 0) {
		lastId = -1;
		return;
	}
	double now = CoopNet::Time();
	if(vehId != lastId) {
		lastId = vehId;
		since = now;
	}
	bool conflict = false;
	for(int slot = 0; slot < MAXP && !conflict; slot++) {
		CoopNet::PlayerState rs;
		if(!CoopNet::GetRemoteInterp(slot, rs)) continue;
		if((rs.flags & 2) && rs.seat == 0 && rs.vehId == vehId) conflict = true;
	}
	if(!conflict) return;
	if(now - since < 2.5 || now - lastEject < 3.0) return;
	lastEject = now;
	pl->SetObjective(OBJECTIVE_LEAVE_CAR, pl->m_pMyVehicle);
	SetHint("The other player took the driver's seat");
}

// call every frame: finish the entering, or warp the player in if the engine did not start it
static void
UpdateEnter()
{
	if(enterCar == nil) return;
	CPed *pl = FindPlayerPed();
	if(pl == nil) {
		ClearEnter();
		return;
	}
	if(pl->bInVehicle) {
		ClearEnter();
		return;
	}
	double now = CoopNet::Time();
	double el = now - enterStart;
	int ps = pl->m_nPedState;
	bool engineBusy = (ps == PED_ENTER_CAR || ps == PED_SEEK_CAR);

	// The engine started the walk and then stopped without the ped being in the car: it was interrupted,
	// the same as cancelling with F. Let it go instead of retrying or warping in. The short grace period
	// keeps a one-frame state change at the end of a successful entry from counting as a cancel.
	if(engineBusy) {
		enterSawBusy = true;
		enterIdleSince = -1.0;
	} else if(enterSawBusy) {
		if(enterIdleSince < 0.0) enterIdleSince = now;
		if(now - enterIdleSince > 0.3) {
			ClearEnter();
			pl->ClearObjective();
			SetHint("Cancelled");
			return;
		}
		return; // still inside the grace period: do nothing, do not retry
	}

	// (the rest of UpdateEnter stays as it is, starting with "the engine did not start walking")
	// the engine did not start walking: give it one more try (stand the car still first) before warping
	if(!engineBusy && el > 0.5 && !enterRetried) {
		enterRetried = true;
		StopAI(enterCar);
		if(enterCar->m_vecMoveSpeed.Magnitude() < 0.3f) enterCar->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
		pl->ClearObjective();
		pl->SetObjective(enterDriver ? OBJECTIVE_ENTER_CAR_AS_DRIVER : OBJECTIVE_ENTER_CAR_AS_PASSENGER, enterCar);
		return;
	}
	if((!engineBusy && el > 1.8) || el > 8.0) {
		CVehicle *car = enterCar;
		bool ok = enterDriver ? car->pDriver == nil : FreePassengerSlots(car) > 0;
		char why[80];
		snprintf(why, sizeof why, "enter fallback: ped state %d, car speed %.2f", ps, car->m_vecMoveSpeed.Magnitude());
		ClearEnter();
		pl->ClearObjective();
		if(ok) SeatPed(pl, car, enterDriver);
		snprintf(seatDbg, sizeof seatDbg, "%s", why); // shown in the debug line; tells why the engine did not walk
	}
}

void
TryEnterAsPassenger()
{
	if(enterCar != nil) { // G again while walking to the vehicle: cancel, like F
		CPed *p = FindPlayerPed();
		ClearEnter();
		if(p != nil) {
			p->ClearObjective();
			if(p->m_nPedState == PED_SEEK_CAR) p->SetIdle();
		}
		SetHint("Cancelled");
		return;
	}
	CoopNet::State st = CoopNet::GetState();
	CPed *pl = FindPlayerPed();
	if(pl == nil) return;

	if(st == CoopNet::HOST) {
		if(!CoopNet::HasPeer()) {
			SetHint("G: no guest connected");
			return;
		}
		if(pl->bInVehicle) {
			SetHint("G: you are already in a vehicle");
			return;
		}
		CVehicle *car = nil;
		float bestD = 1.0e30f;
		for(int slot = 1; slot < MAXP; slot++) { // the nearest vehicle that any guest is driving
			if(guestOwnedId[slot] < 0) continue;
			CVehicle *c = SafeVeh(guestOwnedId[slot]);
			if(c == nil) continue;
			float d = (c->GetPosition() - pl->GetPosition()).Magnitude();
			if(d < bestD) {
				bestD = d;
				car = c;
			}
		}
		if(car == nil) {
			SetHint("G: no guest is driving a vehicle");
			return;
		}
		if((car->GetPosition() - pl->GetPosition()).Magnitude() > 8.0f) {
			SetHint("G: the guest's vehicle is too far away");
			return;
		}
		if(FreePassengerSlots(car) == 0) {
			SetHint("G: that vehicle is full");
			return;
		}
		StartEnter(pl, car, false);
		SetHint("Getting in as passenger");
		return;
	}

	if(st != CoopNet::CLIENT) return;
	if(CoopNet::PacketsReceived() == 0) {
		SetHint("G: not connected yet");
		return;
	}
	if(!guestShared) {
		SetHint("G: too far from the host, no shared world");
		return;
	}
	if(pl->bInVehicle) {
		SetHint("G: you are already in a vehicle");
		return;
	}

	CVector pp = pl->GetPosition();
	int bk = -1;
	float bd = 6.0f;
	for(int k = 0; k < MAX_VPUP; k++) {
		if(vcar[k] == nil) continue;
		float d = (vcar[k]->GetPosition() - pp).Magnitude();
		if(d < bd) {
			bd = d;
			bk = k;
		}
	}
	if(bk < 0) {
		char b[96];
		snprintf(b, sizeof b, "G: no host vehicle within 6 m (%d known)", dbgCars);
		SetHint(b);
		return;
	}

	CVehicle *car = vcar[bk];
	if(FreePassengerSlots(car) == 0) {
		SetHint("G: no free passenger seat");
		return;
	}
	StartEnter(pl, car, false); // G is strictly the passenger seat; take the driver's seat the normal way
	SetHint("Getting in as passenger");
}

// =====================================================================
// trainer helpers
// =====================================================================
static int
ModelVehType(int model)
{
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(model);
	if(mi == nil || mi->GetModelType() != MITYPE_VEHICLE) return -1;
	switch(((CVehicleModelInfo *)mi)->m_vehicleType) {
	case VEHICLE_TYPE_CAR: return 0;
#if COOP_ALLVEH
	case VEHICLE_TYPE_BIKE: return 1;
	case VEHICLE_TYPE_BOAT: return 2;
	case VEHICLE_TYPE_HELI: return 3;
	case VEHICLE_TYPE_PLANE: return 4;
#endif
	}
	return -1;
}

bool
SpawnVehicle(int model)
{
	CPed *pl = FindPlayerPed();
	if(pl == nil) return false;
	int type = ModelVehType(model);
	if(type < 0) {
		SetHint("Spawn: that model cannot be spawned");
		return false;
	}
	if(CPools::GetVehiclePool()->GetNoOfFreeSpaces() < 2) {
		SetHint("Spawn: vehicle pool is full");
		return false;
	}
	if(!CStreaming::HasModelLoaded(model)) {
		CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE);
		CStreaming::LoadAllRequestedModels(false);
	}
	if(!CStreaming::HasModelLoaded(model)) {
		SetHint("Spawn: model failed to load");
		return false;
	}

	CVector pos = pl->GetPosition() + pl->GetForward() * 5.0f;
	if(type == 0 || type == 1) {
		float gz = CWorld::FindGroundZForCoord(pos.x, pos.y);
		pos.z = (gz > -50.0f && fabsf(gz - pl->GetPosition().z) < 20.0f) ? gz + 1.5f : pl->GetPosition().z + 1.0f;
	} else if(type == 2) {
		pos.z = pl->GetPosition().z + 1.0f;
	} else {
		pos.z = pl->GetPosition().z + 5.0f;
	}

	CVehicle *car = MakeVehicle(type, model);
	if(car == nil) {
		SetHint("Spawn: could not create the vehicle");
		return false;
	}
	car->SetHeading(pl->m_fRotationCur);
	car->SetPosition(pos);
	car->SetStatus(STATUS_ABANDONED);
	car->m_nDoorLock = CARLOCK_UNLOCKED;
	CWorld::Add(car);

	// some classes (the police helicopter) come with a pilot and fly by themselves: strip that
	if(car->pDriver) {
		CPopulation::RemovePed(car->pDriver);
		car->pDriver = nil;
	}
	for(int i = 0; i < 8; i++) {
		if(car->pPassengers[i]) {
			CPopulation::RemovePed(car->pPassengers[i]);
			car->pPassengers[i] = nil;
		}
	}
	car->m_nNumPassengers = 0;
	car->SetStatus(STATUS_ABANDONED);
	car->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
	car->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
	SetHint("Vehicle spawned");
	return true;
}

static bool
TeleportTo(CPed *pl, float x, float y, float z)
{
	CVector t(x + 1.5f, y, z + 0.5f);

	if(!pl->bInVehicle) {
		pl->Teleport(t);
		SetHint("Teleported to the other player");
		return true;
	}
	CVehicle *car = pl->m_pMyVehicle;
	if(car != nil && car->pDriver == pl) {
		CWorld::Remove(car);
		car->SetPosition(CVector(t.x + 2.0f, t.y, t.z + 1.0f));
		car->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
		car->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
		CWorld::Add(car);
		SetHint("Teleported your vehicle to the other player");
		return true;
	}
	SetHint("Teleport: leave the vehicle first");
	return false;
}

bool
TeleportToPlayer(int slot)
{
	CPed *pl = FindPlayerPed();
	if(pl == nil) return false;
	float x, y, z;
	if(!CoopNet::IsActive() || !CoopNet::IsSlotActive(slot) || !CoopNet::GetRemote(slot, x, y, z)) {
		SetHint("Teleport: that player is not connected");
		return false;
	}
	return TeleportTo(pl, x, y, z);
}

// nearest connected player
bool
TeleportToOther()
{
	CPed *pl = FindPlayerPed();
	if(pl == nil) return false;
	int best = -1;
	float bd = 1.0e30f;
	CVector me = pl->GetPosition();
	for(int slot = 0; slot < MAXP; slot++) {
		float x, y, z;
		if(!CoopNet::IsActive() || !CoopNet::IsSlotActive(slot) || !CoopNet::GetRemote(slot, x, y, z)) continue;
		float d = (CVector(x, y, z) - me).Magnitude();
		if(d < bd) {
			bd = d;
			best = slot;
		}
	}
	if(best < 0) {
		SetHint("Teleport: no other player is connected");
		return false;
	}
	return TeleportToPlayer(best);
}

// call once per frame after the world update
#if COOP_CRASHLOG
// what the crash report says about the game side (runs inside the crash handler, so keep it simple)
static void
CrashInfo(char *out, int n)
{
	int k = 0;
	CPed *pl = FindPlayerPed();
	if(pl) {
		CVector p = pl->GetPosition();
		k += snprintf(out + k, n - k, "  local player at %.1f %.1f %.1f  inVehicle %d  health %.0f\r\n", p.x, p.y, p.z, (int)pl->bInVehicle, pl->m_fHealth);
	} else {
		k += snprintf(out + k, n - k, "  no local player\r\n");
	}
	k += snprintf(out + k, n - k, "  guestShared %d  anyHostShared %d  friendlyFire %d  puppet cars %d  puppet peds %d (seated %d)\r\n", (int)guestShared,
	              (int)AnyHostShared(), (int)GetFriendlyFire(), dbgCars, dbgPeds, dbgSeated);
	for(int s = 0; s < MAXP && k < n - 200; s++) {
		CPed *pp = CoopMenu::GetPuppet(s);
		k += snprintf(out + k, n - k, "  slot %d: active %d visible %d hostShared %d guestOwnedCar %d adoptCar %d puppet %s%s\r\n", s,
		              (int)CoopNet::IsSlotActive(s), (int)IsRemoteVisible(s), (int)hostShared[s], (int)guestOwnedId[s], (int)adoptId[s],
		              pp ? "yes" : "no", pp && pp->bInVehicle ? " (in vehicle)" : "");
	}
	int cars = 0, peds = 0;
	for(int i = 0; i < MAX_VPUP; i++)
		if(vcar[i]) cars++;
	for(int i = 0; i < MAX_PPUP; i++)
		if(pped[i]) peds++;
	if(k < n - 100) snprintf(out + k, n - k, "  tracked puppet vehicles %d/%d, puppet peds %d/%d\r\n", cars, MAX_VPUP, peds, MAX_PPUP);
}
#endif

void
Update()
{
	InitSlots();
#if COOP_CRASHLOG
	CoopNet::InstallCrashLog();
	CoopNet::SetCrashInfo(CrashInfo);
	CoopNet::CrashTick();
#endif
	COOP_CRUMB("World::Update", (int)CoopNet::GetState());
	CoopNet::State st = CoopNet::GetState();
	if(st != CoopNet::CLIENT) { // forget the old host's weather when not a guest
		haveWorld = false;
#if COOP_WEATHERHOLD
		WeatherHoldReset();
#endif
	}
	UpdateEnter();
	if(CoopNet::IsActive() && CoopNet::PacketsReceived() > 0) {
		COOP_CRUMB("U:driverConflict");
		ResolveDriverConflict();
		COOP_CRUMB("U:hits");
		ApplyIncomingHits();
		COOP_CRUMB("U:blasts");
		ApplyIncomingBlasts();
#if COOP_RUNOVER
		COOP_CRUMB("U:runover");
		CheckRunOver();
#endif
	}

	if(st == CoopNet::HOST) {
		COOP_CRUMB("H:send");
		HostSend(); // also lets go of the slots of players that left
		return;
	}
	for(int i = 0; i < MAXP; i++) {
		hostShared[i] = false;
		guestOwnedId[i] = -1;
		adoptId[i] = -1;
	}

	if(st == CoopNet::CLIENT && CoopNet::PacketsReceived() > 0) {
		if(FindPlayerPed() == nil) return;
		COOP_CRUMB("G:applyWorld");
		ApplyWorld();
		if(guestShared) {
			COOP_CRUMB("G:clearAmbient");
			ClearLocalAmbient();
			COOP_CRUMB("G:syncVehicles");
			SyncVehicles();
			COOP_CRUMB("G:syncPeds");
			SyncPeds();
#if COOP_FIRE
			COOP_CRUMB("G:syncFires");
			SyncFires();
#endif
			COOP_CRUMB("G:sendOwned");
			GuestSendOwned();
		} else {
			RemoveAllPuppets();
		}

		dbgCars = dbgPeds = dbgSeated = 0;
		for(int k = 0; k < MAX_VPUP; k++)
			if(vcar[k]) dbgCars++;
		for(int k = 0; k < MAX_PPUP; k++) {
			if(pped[k]) {
				dbgPeds++;
				if(pveh[k] >= 0) dbgSeated++;
			}
		}
		return;
	}

	guestShared = false;
	RemoveAllPuppets();
}

} // namespace CoopWorld
