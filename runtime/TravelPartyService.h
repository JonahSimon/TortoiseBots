#pragma once

#include "ObjectGuid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TortoiseBots
{

// Overland travel parties (AiPlayerbot.TravelParties, default off): a few random bots of one
// faction muster at a town and walk to a dungeon or raid door, so the roads between them look
// travelled. Destinations come from the TravelDestinations.h registry, musters from TravelTowns.h
// or a faction capital. Ported from JonahSimon/tortoise-wow's RandomPlayerbotMgr (F2).
//
// Members hold the Dungeon activity lease while marching. That keeps the random-bot services off
// them (no timed logout, revive, strategy roulette or relocation), stops the bot-only-group leave
// rule, and keeps them always active, since a party of bots far from any player would otherwise be
// throttled and stop walking.
class TravelPartyService
{
public:
    static TravelPartyService& Instance();

    // Called from RandomBotService::Update at its ~1 s cadence.
    void Update();

private:
    struct Party
    {
        ObjectGuid leaderGuid;
        std::string leaderName;          // log lines still name the party after the leader is gone
        std::vector<ObjectGuid> memberGuids;  // includes the leader
        uint32_t mapId = 0;
        float destX = 0.f, destY = 0.f, destZ = 0.f;
        std::string destName;
        uint32_t deadline = 0;           // unix time, hard anti-stuck timeout
        uint32_t lastLogTime = 0;
        float maxMemberDist = 0.f;
        // The waypoint the leader is walking to. Re-issuing a move every tick restarts the spline.
        float curTgtX = 0.f, curTgtY = 0.f, curTgtZ = 0.f;
        bool curTgtSet = false;
        float bestDist = 1e9f;           // closest approach to the door so far
        uint32_t bestProgressTime = 0;   // last time bestDist improved (stall detector)
        // MoveTo can claim success and deliver no travel. Judge it on ground covered between
        // orders and demote to a plain MovePoint after three dead orders.
        float lastOrderX = 0.f, lastOrderY = 0.f;
        bool lastOrderSet = false;
        uint8_t deadOrders = 0;
        bool forceRawMove = false;
        bool inCombat = false;
        uint32_t combatStart = 0;
        uint32_t recoverUntil = 0;       // hold so the crew can eat and drink after a fight
        uint8_t leaderDeaths = 0;
        // Where crossing the door puts a player (areatrigger_teleport target); 0 = raid, never entered.
        uint32_t insideMap = 0;
        float insideX = 0.f, insideY = 0.f, insideZ = 0.f, insideO = 0.f;
        uint32_t enteringUntil = 0;      // non-zero: teleports issued, waiting for everyone to land
        uint32_t retryPathAfter = 0;     // a failed pathfind cannot succeed again from the same spot
    };

    void Spawn();
    void Disband(Party& p);

    std::vector<Party> m_parties;
    std::vector<ObjectGuid> m_pendingReset;  // disbanded mid-teleport, strategies reset on landing
    uint32_t m_lastSpawn = 0;
};

} // namespace TortoiseBots
