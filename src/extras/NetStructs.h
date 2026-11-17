#pragma pack(push, 1)

#include "NetEndian.h"
struct VehNet {
	int32_t modelIndex;
	float posX, posY, posZ;
	float rotX, rotY, rotZ, rotW;
	float moveSpeedX, moveSpeedY, moveSpeedZ;
	float turnSpeedX, turnSpeedY, turnSpeedZ;
	float health;
	uint8_t doorStates[6];
	uint8_t turretRot;

	void Swap() { CoopSwap(this, "w 14f 6b b"); }
};

struct PedNet {
	int32_t modelIndex;
	float posX, posY, posZ;
	float heading;
	float moveSpeedX, moveSpeedY, moveSpeedZ;
	float health;
	uint8_t actionState;
	uint8_t weaponType;

	void Swap() { CoopSwap(this, "w 8f 2b"); }
};

#pragma pack(pop)

static_assert(sizeof(VehNet) == 67, "VehNet size mismatch");
static_assert(sizeof(PedNet) == 38, "PedNet size mismatch");