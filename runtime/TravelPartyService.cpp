#include "playerbot/playerbot.h"
#include "TravelPartyService.h"
#include "BotActivityLease.h"
#include "BotManager.h"
#include "PlayerbotAIStorage.h"
#include "../ai/playerbot/PlayerbotAI.h"
#include "../ai/playerbot/PlayerbotAIConfig.h"
#include "../ai/playerbot/RandomBotFacade.h"
#include "../ai/playerbot/strategy/actions/MovementActions.h"
#include "Group/Group.h"
#include "Maps/PathFinder.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"
#include "Log.h"
#include "TravelDestinations.h"
#include "TravelTowns.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace TortoiseBots
{

namespace
{

// MovementAction::MoveTo does the module's own pathing and movement bookkeeping. A bare
// MotionMaster::MovePoint on a bot was measured cancelled on the next tick, so the leader walks
// through MoveTo; it is protected, hence this shim.
class TravelMover : public ai::MovementAction
{
public:
    explicit TravelMover(PlayerbotAI* ai) : MovementAction(ai, "travel party") {}
    using ai::MovementAction::MoveTo;
};

// MoveTo returning true does not mean the bot moved: a collapsed path returns true having issued
// nothing. Check that movement started and fall back to MovePoint when it did not. Codes for the
// log: T = MoveTo moving, S = MoveTo claimed success but nothing moves, F = MoveTo declined,
// P = MoveTo demoted after dead orders.
char IssueMove(Player* leader, PlayerbotAI* lAI, uint32 mapId, float x, float y, float z, bool forceRaw)
{
    // ignoreEnemyTargets: otherwise the path is clipped just short of any hostile in aggro range,
    // the leader stops a few yards away, never pulls, and re-paths there forever.
    if (!forceRaw && TravelMover(lAI).MoveTo(mapId, x, y, z, false, false, false, true))
    {
        if (leader->IsMoving())
            return 'T';
        leader->GetMotionMaster()->MovePoint(990001, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return 'S';
    }

    leader->GetMotionMaster()->MovePoint(990001, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return forceRaw ? 'P' : 'F';
}

// Plain file in the server's working directory. Every line a party writes carries its leader's
// name, because several marches interleave.
void TravelLog(std::string const& line)
{
    std::ofstream f("F2_travel.log", std::ios::app);
    if (f)
        f << "[" << (uint32)time(nullptr) << "] " << line << "\n";
}

// This core's level cap. Registry rows above it are unreachable (Grim Batol, 61).
uint32 const MAX_TRAVEL_DEST_LEVEL = 60;

// Capital musters, one per (faction, continent), from Turtle's game_tele, all outdoors.
// Alliance/Kalimdor is Auberdine, not Darnassus: Teldrassil is reachable only by boat.
// team follows AreaTable: 2 = Alliance, 4 = Horde.
struct TravelMuster
{
    uint32 team;
    uint32 map;
    float x, y, z;
    char const* name;
};

TravelMuster const kMusters[] = {
    { 4, 1,  1493.35f, -4414.17f,  23.00f, "Orgrimmar gates" },      // Durotar
    { 4, 0,  1830.93f,   236.19f,  60.54f, "Ruins of Lordaeron" },   // Tirisfal, above Undercity
    { 2, 1,  6501.40f,   481.61f,   6.27f, "Auberdine" },            // Darkshore
    { 2, 0, -9448.55f,    68.24f,  56.32f, "Goldshire" },            // Elwynn
};

// Closest friendly town to a destination: same map, same team. Team 0 rows are mostly
// wilderness graveyards, so they are never musters.
// ponytail: nearest wins outright, no minimum route; 6 of 70 pairings are under 500 yd.
TravelTown const* ClosestFriendlyTown(uint32 mapId, uint32 team, float x, float y)
{
    if (!team)
        return nullptr;
    TravelTown const* best = nullptr;
    float bestDist = 0.f;
    for (TravelTown const& t : kTravelTowns)
    {
        if (t.map != mapId || t.team != team)
            continue;
        float const dx = t.x - x, dy = t.y - y;
        float const dist = dx * dx + dy * dy;
        if (!best || dist < bestDist)
        {
            best = &t;
            bestDist = dist;
        }
    }
    return best;
}

// Does the straight muster->door line cross a zone a real player is in? Sampled every 400 yd,
// which cannot step over a vanilla zone. A bad interpolated z returns zone 0 and is skipped.
bool RouteCrossesZone(uint32 mapId, float ax, float ay, float az, float bx, float by, float bz,
                      std::set<uint32> const& zones)
{
    float const dx = bx - ax, dy = by - ay, dz = bz - az;
    uint32 const steps = std::max<uint32>(1, (uint32)(std::sqrt(dx * dx + dy * dy) / 400.0f));
    for (uint32 i = 0; i <= steps; ++i)
    {
        float const t = (float)i / (float)steps;
        uint32 const zone = sTerrainMgr.GetZoneId(mapId, ax + dx * t, ay + dy * t, az + dz * t);
        if (zone && zones.count(zone))
            return true;
    }
    return false;
}

bool IsEnemyCapital(TravelDest const& d, uint32 team)
{
    AreaEntry const* a = GetAreaEntryByAreaID(sTerrainMgr.GetZoneId(d.map, d.x, d.y, d.z));
    return a && (a->Flags & AREA_FLAG_CAPITAL) && a->Team && a->Team != team;
}

// Recruitable for a march, level and faction aside.
bool IsTravelEligible(Player* bot)
{
    return bot && bot->IsInWorld() && bot->IsAlive() && !bot->GetGroup() && !bot->IsInCombat() &&
           !bot->InBattleGround() && !bot->InBattleGroundQueue() && !bot->IsBeingTeleported() &&
           !bot->IsTaxiFlying() && sRandomBotFacade.IsRandomBot(bot) &&
           !sRandomBotFacade.IsPinnedBot(bot->GetGUIDLow()) &&
           BotActivityLeaseManager::Instance().IsAvailableForBackground(bot->GetGUIDLow()) &&
           PlayerbotAIStorage::Instance().GetAI(bot);
}

bool TeamMatches(Player* bot, uint32 team)
{
    return (team == 2 && bot->GetTeam() == ALLIANCE) || (team == 4 && bot->GetTeam() == HORDE);
}

} // namespace

#define PartyLog(p, line) TravelLog("[" + (p).leaderName + "] " + (line))

TravelPartyService& TravelPartyService::Instance()
{
    static TravelPartyService instance;
    return instance;
}

void TravelPartyService::Spawn()
{
    if (m_parties.size() >= sPlayerbotAIConfig.travelPartyMaxConcurrent)
    {
        TravelLog("CAP full, no party this round: active=" + std::to_string(m_parties.size()) + "/" +
                  std::to_string(sPlayerbotAIConfig.travelPartyMaxConcurrent));
        return;
    }

    // Zones with a real player in them, for the optional "someone might see it" gate.
    std::map<uint32, std::set<uint32>> playerZones;
    if (sPlayerbotAIConfig.travelPartyRequirePlayerZone)
    {
        for (auto const& itr : sWorld.GetAllSessions())
        {
            WorldSession* session = itr.second;
            if (!session || !session->HasNetworkTransport())
                continue;
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
                playerZones[player->GetMapId()].insert(player->GetZoneId());
        }
        if (playerZones.empty())
        {
            TravelLog("REQUIRE PLAYER ZONE is on and no real player is in world - no party will form, by design");
            return;
        }
    }

    // Walk the capitals in random order so one continent does not take every slot.
    size_t const musterCount = sizeof(kMusters) / sizeof(kMusters[0]);
    std::vector<size_t> order(musterCount);
    for (size_t i = 0; i < musterCount; ++i)
        order[i] = i;
    for (size_t i = musterCount; i > 1; --i)
        std::swap(order[i - 1], order[urand(0, (uint32)i - 1)]);

    // Both rolls are per attempt: the ratios are ratios of parties. A raid attempt always musters at
    // the capital; a dungeon attempt musters at the closest friendly town ClosestTownPct of the time.
    bool const townRun = sPlayerbotAIConfig.travelPartyClosestTownPct > 0 &&
                         urand(1, 100) <= sPlayerbotAIConfig.travelPartyClosestTownPct;
    bool const raidRun = sPlayerbotAIConfig.travelPartyRaidPct > 0 &&
                         urand(1, 100) <= sPlayerbotAIConfig.travelPartyRaidPct;

    struct Candidate { TravelMuster m; TravelDest const* d; char const* branch; };
    std::vector<Candidate> viable;
    uint32 unseenRoutes = 0;
    std::vector<Player*> const bots = BotManager::Instance().GetAllBots();

    for (size_t mi : order)
    {
        TravelMuster const& m = kMusters[mi];

        uint32 perLevel[81] = {0};
        for (Player* bot : bots)
            if (IsTravelEligible(bot) && TeamMatches(bot, m.team) && bot->GetLevel() <= 80)
                ++perLevel[bot->GetLevel()];

        for (TravelDest const& d : kTravelDests)
        {
            if (d.map != m.map || d.reqLevel > MAX_TRAVEL_DEST_LEVEL)
                continue;
            // At RaidPct 0 raids stay in the pool as ordinary destinations.
            if (sPlayerbotAIConfig.travelPartyRaidPct > 0 && d.isRaid != raidRun)
                continue;

            // Low-level content always musters at the closest town: a level-20 crew walking
            // Silverpine to the Deadmines lost its leader 5 times out of 5.
            bool const lowBand = sPlayerbotAIConfig.travelPartyLowLevelBand > 0 &&
                                 d.maxLevel <= sPlayerbotAIConfig.travelPartyLowLevelBand;
            TravelMuster use = m;
            char const* branch = raidRun ? "city(raid)" : "city";
            if (!raidRun && sPlayerbotAIConfig.travelPartyClosestTownPct > 0 && (townRun || lowBand))
                if (TravelTown const* t = ClosestFriendlyTown(m.map, m.team, d.x, d.y))
                {
                    use = TravelMuster{ t->team, t->map, t->x, t->y, t->z, t->name };
                    branch = townRun ? "town" : "town(low band)";
                }

            // Never march into the enemy's capital (Stockades, Ragefire Chasm).
            if (IsEnemyCapital(d, use.team))
                continue;

            // Long routes kill low-level parties: 6 of 9 lost the leader over 6000 yd, 0 of 8 under.
            if (lowBand && sPlayerbotAIConfig.travelPartyLowLevelMaxRoute > 0.f &&
                std::hypot(d.x - use.x, d.y - use.y) > sPlayerbotAIConfig.travelPartyLowLevelMaxRoute)
                continue;

            if (sPlayerbotAIConfig.travelPartyRequirePlayerZone)
            {
                auto const zit = playerZones.find(use.map);
                if (zit == playerZones.end() ||
                    !RouteCrossesZone(use.map, use.x, use.y, use.z, d.x, d.y, d.z, zit->second))
                {
                    ++unseenRoutes;
                    continue;
                }
            }

            uint32 have = 0;
            for (uint32 l = d.minLevel; l <= d.maxLevel && l <= 80; ++l)
                have += perLevel[l];
            if (have >= 2)
                viable.push_back({ use, &d, branch });
        }

        if (!viable.empty())
            break;
    }

    if (viable.empty())
    {
        TravelLog(unseenRoutes
            ? "no party: " + std::to_string(unseenRoutes) + " route(s) had bots but crossed no zone a real player is in"
            : std::string("no party: no destination has 2+ eligible bots in its band"));
        return;
    }

    Candidate const& pick = viable[urand(0, viable.size() - 1)];
    TravelDest const& dest = *pick.d;
    TravelLog("SELECTED " + std::string(dest.name) + " (trigger " + std::to_string(dest.trigger) +
              ", req " + std::to_string((uint32)dest.reqLevel) + ") from " + pick.m.name + " band " +
              std::to_string((uint32)dest.minLevel) + "-" + std::to_string((uint32)dest.maxLevel) +
              " via " + pick.branch + " route " +
              std::to_string((uint32)std::hypot(dest.x - pick.m.x, dest.y - pick.m.y)) + " yd");

    std::vector<Player*> picked;
    for (Player* bot : bots)
    {
        if (picked.size() >= 5)
            break;
        if (IsTravelEligible(bot) && TeamMatches(bot, pick.m.team) &&
            bot->GetLevel() >= dest.minLevel && bot->GetLevel() <= dest.maxLevel)
            picked.push_back(bot);
    }
    if (picked.size() < 2)
        return;

    // The highest-level member leads: it is the one most likely to survive the road.
    std::sort(picked.begin(), picked.end(), [](Player* a, Player* b) { return a->GetLevel() > b->GetLevel(); });

    // Lease first: it is what keeps every other service and the leave-group rule off the crew.
    std::vector<Player*> crew;
    for (Player* bot : picked)
        if (BotActivityLeaseManager::Instance().TryAcquire(bot->GetGUIDLow(), BotActivity::Dungeon))
            crew.push_back(bot);
    auto releaseAll = [&crew]()
    {
        for (Player* bot : crew)
            BotActivityLeaseManager::Instance().Release(bot->GetGUIDLow(), BotActivity::Dungeon);
    };
    if (crew.size() < 2)
    {
        releaseAll();
        return;
    }

    Player* leader = crew.front();
    PlayerbotAI* leaderAI = PlayerbotAIStorage::Instance().GetAI(leader);

    // Bots do not consent: create the group and add members directly, as the core's LFT does.
    Group* group = new Group;
    if (!group->Create(leader->GetObjectGuid(), leader->GetName()))
    {
        delete group;
        releaseAll();
        return;
    }
    sObjectMgr.AddGroup(group);

    Party party;
    party.leaderGuid = leader->GetObjectGuid();
    party.leaderName = leader->GetName();
    party.memberGuids.push_back(leader->GetObjectGuid());
    party.mapId = dest.map;
    party.destX = dest.x;
    party.destY = dest.y;
    party.destZ = dest.z;
    party.destName = dest.name;
    // Raids are marched to but never entered: insideMap 0 is the whole gate.
    if (!dest.isRaid)
    {
        party.insideMap = dest.inMap;
        party.insideX = dest.inX;
        party.insideY = dest.inY;
        party.insideZ = dest.inZ;
        party.insideO = dest.inO;
    }

    // All AI changes before any teleport: a teleport to another map takes the bot off its map until it
    // lands, and ResetStrategies asserts GetMap() (crashed the first tb5 boot).
    std::vector<Player*> members;
    for (size_t i = 1; i < crew.size(); ++i)
    {
        Player* member = crew[i];
        PlayerbotAI* memberAI = PlayerbotAIStorage::Instance().GetAI(member);
        if (!memberAI || !group->AddMember(member->GetObjectGuid(), member->GetName()))
        {
            BotActivityLeaseManager::Instance().Release(member->GetGUIDLow(), BotActivity::Dungeon);
            continue;
        }

        memberAI->SetMaster(leader);
        memberAI->ResetStrategies();
        memberAI->ChangeStrategy("+follow,-grind,-rpg,-travel", BotState::BOT_STATE_NON_COMBAT);
        party.memberGuids.push_back(member->GetObjectGuid());
        members.push_back(member);
    }

    // The leader is driven by the tick below; its own autonomous strategies would pick another target.
    leaderAI->ChangeStrategy(sPlayerbotAIConfig.travelPartyLeaderStrip, BotState::BOT_STATE_NON_COMBAT);

    leader->TeleportTo(pick.m.map, pick.m.x, pick.m.y, pick.m.z, 0.f);
    for (Player* member : members)
        member->TeleportTo(pick.m.map, pick.m.x + frand(-4.f, 4.f), pick.m.y + frand(-4.f, 4.f), pick.m.z, 0.f);

    // 5 min of slack plus 2 s per yard; the stall detector is the real guard against a wedged march.
    uint32 const now = (uint32)time(nullptr);
    party.deadline = now + 300 + (uint32)(std::hypot(dest.x - pick.m.x, dest.y - pick.m.y) * 2.0f);
    party.bestProgressTime = now;
    m_parties.push_back(std::move(party));

    std::ostringstream o;
    o << "FORMED " << m_parties.back().memberGuids.size() << " bots; leader " << leader->GetName()
      << " muster (" << (int)pick.m.x << "," << (int)pick.m.y << "," << (int)pick.m.z << ") -> dest ("
      << (int)dest.x << "," << (int)dest.y << "," << (int)dest.z << ") territory " << pick.m.team
      << " active=" << m_parties.size() << "/" << sPlayerbotAIConfig.travelPartyMaxConcurrent;
    TravelLog(o.str());
}

void TravelPartyService::Update()
{
    if (!sPlayerbotAIConfig.travelParties)
        return;

    uint32 const now = (uint32)time(nullptr);

    // Disbanded while teleporting: reset once back on a map, drop if logged out.
    for (auto it = m_pendingReset.begin(); it != m_pendingReset.end();)
    {
        Player* bot = ObjectAccessor::FindPlayerNotInWorld(*it);
        PlayerbotAI* botAI = bot ? PlayerbotAIStorage::Instance().GetAI(bot) : nullptr;
        if (bot && botAI && !bot->IsInWorld())
        {
            ++it;
            continue;
        }
        if (botAI)
            botAI->ResetStrategies();
        it = m_pendingReset.erase(it);
    }

    if (sPlayerbotAIConfig.travelPartySpawnInterval && now - m_lastSpawn >= sPlayerbotAIConfig.travelPartySpawnInterval)
    {
        m_lastSpawn = now;
        Spawn();
    }

    for (auto it = m_parties.begin(); it != m_parties.end();)
    {
        Party& p = *it;
        Player* leader = ObjectAccessor::FindPlayerNotInWorld(p.leaderGuid);
        bool done = false;

        // Entering: far teleports are asynchronous. Wait for everyone to land before disbanding, or
        // each bot walks in alone and binds to its own copy. Bounded so a lost bot cannot hold the slot.
        if (p.enteringUntil)
        {
            uint32 inside = 0;
            for (ObjectGuid const& guid : p.memberGuids)
            {
                Player* member = sObjectMgr.GetPlayer(guid);
                if (member && member->IsInWorld() && !member->IsBeingTeleported() && member->GetMapId() == p.insideMap)
                    ++inside;
            }
            if (inside == (uint32)p.memberGuids.size() || now >= p.enteringUntil)
            {
                PartyLog(p, "ENTERED " + p.destName + " map=" + std::to_string(p.insideMap) + " (" +
                         std::to_string(inside) + "/" + std::to_string(p.memberGuids.size()) + " bots inside" +
                         (inside == (uint32)p.memberGuids.size() ? "" : ", gave up waiting") + ") -> disband");
                Disband(p);
                it = m_parties.erase(it);
            }
            else
                ++it;
            continue;
        }

        // Still travelling to the muster (a teleport to another map lands a tick or more later): wait,
        // or the checks below read "not in world" as a lost leader. The deadline still applies.
        if (leader && leader->IsBeingTeleported() && now < p.deadline)
        {
            ++it;
            continue;
        }

        // A leader death is a setback: revive in place, up to TravelPartyMaxDeaths. A party being
        // farmed by something it cannot beat has to stop with a verdict instead.
        if (leader && leader->IsInWorld() && !leader->IsAlive() && leader->GetMapId() == p.mapId &&
            !leader->IsBeingTeleported() && p.leaderDeaths < sPlayerbotAIConfig.travelPartyMaxDeaths)
        {
            ++p.leaderDeaths;
            leader->ResurrectPlayer(1.0f);
            leader->SpawnCorpseBones();
            PartyLog(p, "LEADER DIED at " + std::to_string((int)leader->GetDistance3dToCenter(p.destX, p.destY, p.destZ)) +
                     " yd out -> revived (" + std::to_string((int)p.leaderDeaths) + "/" +
                     std::to_string(sPlayerbotAIConfig.travelPartyMaxDeaths) + ")");
            p.bestProgressTime = now;
            p.curTgtSet = false;
            p.lastOrderSet = false;
            p.deadOrders = 0;
            p.forceRawMove = false;
            p.recoverUntil = now + 45;
            p.deadline += 60;
        }
        else if (!leader || !leader->IsInWorld() || !leader->IsAlive() || leader->GetMapId() != p.mapId)
        {
            std::ostringstream o;
            o << "LEADER LOST (";
            if (!leader)                   o << "no player object";
            else if (!leader->IsInWorld()) o << "not in world";
            else if (!leader->IsAlive())   o << "dead at " << (int)leader->GetDistance3dToCenter(p.destX, p.destY, p.destZ)
                                             << " yd out after " << (int)p.leaderDeaths << " revives";
            else                           o << "wrong map " << leader->GetMapId() << " != " << p.mapId;
            o << ") -> disband";
            PartyLog(p, o.str());
            done = true;
        }
        else if (now >= p.deadline)
        {
            PartyLog(p, "TIMEOUT -> disband");
            done = true;
        }
        // Arrival is 2D with a generous vertical tolerance: several doors sit below cliffs that are
        // off the navmesh (Wailing Caverns ended 80 yd straight up, Blackfathom 115).
        else if (leader->GetDistance2dToCenter(p.destX, p.destY) <= 20.f &&
                 std::fabs(leader->GetPositionZ() - p.destZ) <= sPlayerbotAIConfig.travelPartyArriveZ)
        {
            if (sPlayerbotAIConfig.travelPartyEnterInstance && p.insideMap)
            {
                // Do what the trigger does for a player. The leader goes first so the instance
                // binding is the leader's.
                PartyLog(p, "ARRIVED -> entering " + p.destName);
                leader->TeleportTo(p.insideMap, p.insideX, p.insideY, p.insideZ, p.insideO);
                for (ObjectGuid const& guid : p.memberGuids)
                {
                    if (guid == p.leaderGuid)
                        continue;
                    Player* member = sObjectMgr.GetPlayer(guid);
                    if (!member || !member->IsInWorld())
                        continue;
                    if (!member->IsAlive())
                    {
                        member->ResurrectPlayer(1.0f);
                        member->SpawnCorpseBones();
                    }
                    member->TeleportTo(p.insideMap, p.insideX + frand(-3.f, 3.f), p.insideY + frand(-3.f, 3.f),
                                       p.insideZ, p.insideO);
                }
                p.enteringUntil = now + 60;  // ponytail: fixed wait, not a per-bot state machine
            }
            else
            {
                PartyLog(p, "ARRIVED -> disband");
                done = true;
            }
        }
        else if (PlayerbotAI* lAI = PlayerbotAIStorage::Instance().GetAI(leader))
        {
            if (!leader->IsBeingTeleported())
            {
                // Something may hand the leader its autonomous strategies back; re-strip when any
                // stripped one is live again.
                std::string const& strip = sPlayerbotAIConfig.travelPartyLeaderStrip;
                std::stringstream ss(strip);
                for (std::string tok; std::getline(ss, tok, ',');)
                {
                    tok.erase(0, tok.find_first_not_of(" +-"));
                    tok.erase(tok.find_last_not_of(' ') + 1);
                    if (!tok.empty() && lAI->HasStrategy(tok, BotState::BOT_STATE_NON_COMBAT))
                    {
                        lAI->ChangeStrategy(strip, BotState::BOT_STATE_NON_COMBAT);
                        break;
                    }
                }

                float const lx = leader->GetPositionX();
                float const ly = leader->GetPositionY();
                float const lz = leader->GetPositionZ();
                float const destDist = leader->GetDistance3dToCenter(p.destX, p.destY, p.destZ);

                // Fighting is part of travelling. While anyone fights, the march yields completely and
                // the stall clock and deadline stop.
                bool fighting = false;
                for (ObjectGuid const& guid : p.memberGuids)
                {
                    Player* member = sObjectMgr.GetPlayer(guid);
                    if (member && member->IsInWorld() && member->IsInCombat())
                    {
                        fighting = true;
                        break;
                    }
                }

                if (fighting && !p.inCombat)
                {
                    p.inCombat = true;
                    p.combatStart = now;
                    PartyLog(p, "COMBAT start at " + std::to_string((int)destDist) + " yd out");
                }
                else if (!fighting && p.inCombat)
                {
                    uint32 const spent = now - p.combatStart;
                    p.inCombat = false;
                    p.curTgtSet = false;
                    p.deadOrders = 0;
                    p.forceRawMove = false;
                    p.lastOrderSet = false;
                    p.bestProgressTime = now;
                    // The food strategy feeds the crew, but only while nothing orders it to move.
                    p.recoverUntil = now + 45;
                    p.deadline += spent + 45;
                    PartyLog(p, "COMBAT end after " + std::to_string(spent) + "s, resuming march");
                }

                bool holding = p.inCombat;
                if (!holding && now < p.recoverUntil)
                {
                    // ponytail: health only; a healer out of mana still walks.
                    for (ObjectGuid const& guid : p.memberGuids)
                    {
                        Player* member = sObjectMgr.GetPlayer(guid);
                        if (member && member->IsInWorld() && member->IsAlive() && lAI->GetHealthPercent(*member) < 80)
                        {
                            holding = true;
                            break;
                        }
                    }
                    if (!holding)
                        p.recoverUntil = 0;
                }

                if (holding)
                {
                    p.bestProgressTime = now;
                    // A hold must never be silent, or the log looks like a hung server.
                    if (now - p.lastLogTime >= 15)
                    {
                        p.lastLogTime = now;
                        PartyLog(p, "HOLD (" + std::string(p.inCombat ? "combat" : "recovery") + ") at " +
                                 std::to_string((int)destDist) + " yd out, " + std::to_string(now - p.combatStart) + "s");
                    }
                    ++it;
                    continue;
                }

                if (destDist < p.bestDist - 2.0f)
                {
                    p.bestDist = destDist;
                    p.bestProgressTime = now;
                }

                // No closer for 45 s near the door (the navmesh ends short of it) or 90 s on the road
                // (wedged): stop with a verdict instead of burning the deadline.
                bool const nearDest = leader->GetDistance2dToCenter(p.destX, p.destY) <= 120.f;
                if (now - p.bestProgressTime >= (nearDest ? 45u : 90u))
                {
                    std::ostringstream o;
                    o << "STALLED " << (nearDest ? "near destination" : "en route") << " (best "
                      << (int)p.bestDist << " yd, now " << (int)destDist << " yd) -> disband";
                    PartyLog(p, o.str());
                    done = true;
                }

                // Keep the crew together: follow alone leaves members stuck on geometry, so walk
                // stragglers in and snap back anyone truly lost. Dead followers are revived in place.
                float maxMemberDist = 0.f;
                for (ObjectGuid const& mg : p.memberGuids)
                {
                    if (mg == p.leaderGuid)
                        continue;
                    Player* member = sObjectMgr.GetPlayer(mg);
                    if (!member || !member->IsInWorld())
                        continue;

                    if (!member->IsAlive() && !member->IsBeingTeleported() && sPlayerbotAIConfig.travelPartyMaxDeaths > 0)
                    {
                        member->ResurrectPlayer(1.0f);
                        member->SpawnCorpseBones();
                        PartyLog(p, "MEMBER DIED " + std::string(member->GetName()) + " -> revived");
                    }

                    if (PlayerbotAI* mAI = PlayerbotAIStorage::Instance().GetAI(member))
                    {
                        if (mAI->HasStrategy("grind", BotState::BOT_STATE_NON_COMBAT) ||
                            mAI->HasStrategy("rpg", BotState::BOT_STATE_NON_COMBAT) ||
                            !mAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT))
                        {
                            mAI->SetMaster(leader);
                            mAI->ChangeStrategy("+follow,-grind,-rpg,-travel", BotState::BOT_STATE_NON_COMBAT);
                        }
                    }

                    float md = member->GetMapId() == leader->GetMapId() ? member->GetDistance3dToCenter(lx, ly, lz) : 100000.f;
                    if (md > 55.f)
                    {
                        // The leader never waits (stopping dismounts it), so snapping holds the group.
                        member->NearTeleportTo(lx + frand(-4.f, 4.f), ly + frand(-4.f, 4.f), lz, member->GetOrientation());
                        md = 4.f;
                    }
                    else if (md > 12.f && !member->IsMoving() && !member->IsBeingTeleported())
                        member->GetMotionMaster()->MovePoint(990002, lx + frand(-5.f, 5.f), ly + frand(-5.f, 5.f), lz,
                                                             MOVE_PATHFINDING | MOVE_RUN_MODE);
                    maxMemberDist = std::max(maxMemberDist, md);
                }
                p.maxMemberDist = maxMemberDist;

                // Leader: one navmesh hop at a time, re-pathing only when it has stopped or reached
                // the current waypoint.
                float const toTgt = p.curTgtSet ? leader->GetDistance2dToCenter(p.curTgtX, p.curTgtY) : 1e9f;
                if (!done && now >= p.retryPathAfter && (!leader->IsMoving() || !p.curTgtSet || toTgt <= 12.f))
                {
                    // Three orders in a row that covered under 5 yd and MoveTo loses the wheel.
                    if (p.lastOrderSet)
                    {
                        if (leader->GetDistance2dToCenter(p.lastOrderX, p.lastOrderY) < 5.f)
                        {
                            if (p.deadOrders < 250)
                                ++p.deadOrders;
                            if (p.deadOrders >= 3)
                                p.forceRawMove = true;
                        }
                        else
                        {
                            p.deadOrders = 0;
                            p.forceRawMove = false;
                        }
                    }
                    p.lastOrderX = lx;
                    p.lastOrderY = ly;
                    p.lastOrderSet = true;

                    PathType ptype = PATHFIND_BLANK;
                    float endX = 0.f, endY = 0.f, endZ = 0.f;

                    // A real navmesh path (NORMAL or INCOMPLETE, not a straight line through terrain)
                    // whose furthest node within 200 yd is a real step toward the door.
                    auto tryTarget = [&](float tgx, float tgy, float tgz) -> bool
                    {
                        PathFinder g(leader);
                        g.calculate(tgx, tgy, tgz);
                        ptype = g.getPathType();
                        if ((ptype & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) == 0 ||
                            (ptype & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) != 0 ||
                            g.getPath().size() <= 1)
                            return false;

                        // ponytail: fixed 200 yd hop, not tuned per terrain.
                        Vector3 end = g.getPath().back();
                        for (auto const& node : g.getPath())
                            if (leader->GetDistance3dToCenter(node.x, node.y, node.z) <= 200.f)
                                end = node;

                        float const stepLen = leader->GetDistance3dToCenter(end.x, end.y, end.z);
                        float const endToDest = std::sqrt((end.x - p.destX) * (end.x - p.destX) +
                                                          (end.y - p.destY) * (end.y - p.destY) +
                                                          (end.z - p.destZ) * (end.z - p.destZ));
                        if (stepLen < 25.f || endToDest > destDist - 15.f)
                            return false;

                        endX = end.x;
                        endY = end.y;
                        endZ = end.z;
                        return true;
                    };

                    bool usable = tryTarget(p.destX, p.destY, p.destZ);
                    if (!usable)
                    {
                        // Door unreachable from here: aim at points fanned out toward it instead.
                        float const baseAng = atan2(p.destY - ly, p.destX - lx);
                        float const dists[] = {250.f, 150.f, 90.f};
                        float const offs[] = {0.f, 0.4f, -0.4f, 0.8f, -0.8f};
                        for (float d : dists)
                        {
                            for (float a : offs)
                                if ((usable = tryTarget(lx + cos(baseAng + a) * d, ly + sin(baseAng + a) * d, lz)))
                                    break;
                            if (usable)
                                break;
                        }
                    }

                    // Keep the committed waypoint unless the new one is a real improvement; beside an
                    // obstacle INCOMPLETE paths flip between endpoints on either side of it.
                    if (usable && p.curTgtSet && !p.forceRawMove &&
                        std::hypot(endX - p.destX, endY - p.destY) > std::hypot(p.curTgtX - p.destX, p.curTgtY - p.destY) - 10.f &&
                        leader->GetDistance2dToCenter(p.curTgtX, p.curTgtY) > 12.f)
                    {
                        endX = p.curTgtX;
                        endY = p.curTgtY;
                        endZ = p.curTgtZ;
                    }

                    char moveCode;
                    if (usable)
                    {
                        p.curTgtX = endX;
                        p.curTgtY = endY;
                        p.curTgtZ = endZ;
                        p.curTgtSet = true;
                        p.retryPathAfter = 0;
                        moveCode = IssueMove(leader, lAI, p.mapId, endX, endY, endZ, p.forceRawMove);
                    }
                    else
                    {
                        // Transient off-mesh spot: nudge 10 yd toward the door and retry in 2 s.
                        p.curTgtSet = false;
                        p.retryPathAfter = now + 2;
                        float const dx = p.destX - lx, dy = p.destY - ly;
                        float const flat = std::sqrt(dx * dx + dy * dy);
                        float const step = std::min(10.0f, flat);
                        moveCode = IssueMove(leader, lAI, p.mapId, flat > 1.f ? lx + dx / flat * step : p.destX,
                                             flat > 1.f ? ly + dy / flat * step : p.destY, lz, p.forceRawMove);
                    }

                    // Only when the mover reports trouble; a healthy march is 'T' on every order.
                    if (moveCode != 'T')
                        PartyLog(p, "PATH type=" + std::to_string((uint32)ptype) + " usable=" + std::to_string(usable) +
                                 " mv=" + moveCode + " remaining=" +
                                 std::to_string((int)leader->GetDistance2dToCenter(p.destX, p.destY)));
                }
            }

            if (now - p.lastLogTime >= 15)
            {
                p.lastLogTime = now;
                std::ostringstream o;
                o << "leader " << leader->GetName() << " zone " << leader->GetZoneId() << " pos ("
                  << (int)leader->GetPositionX() << "," << (int)leader->GetPositionY() << "," << (int)leader->GetPositionZ()
                  << ") dist " << (int)leader->GetDistance3dToCenter(p.destX, p.destY, p.destZ)
                  << " best " << (int)p.bestDist << " moving=" << leader->IsMoving()
                  << " active=" << lAI->AllowActivity(ALL_ACTIVITY, true)
                  << " groupSpread=" << (int)p.maxMemberDist << " lvl=" << leader->GetLevel()
                  // The leader's own last action found the "move to loot" wedge (G1); keep it.
                  << " lastAct=" << (lAI->GetCurrentEngine() ? lAI->GetCurrentEngine()->GetLastAction().substr(0, 160) : "<no engine>")
                  << " strat=";
                for (auto const& sv : lAI->GetStrategies(BotState::BOT_STATE_NON_COMBAT))
                    o << sv << "|";
                PartyLog(p, o.str());
            }
        }

        if (done)
        {
            Disband(p);
            it = m_parties.erase(it);
        }
        else
            ++it;
    }
}

void TravelPartyService::Disband(Party& p)
{
    if (Player* leader = ObjectAccessor::FindPlayerNotInWorld(p.leaderGuid))
        if (Group* group = leader->GetGroup())
            group->Disband(true);

    // Hand every bot back to normal random-bot life.
    for (ObjectGuid const& guid : p.memberGuids)
    {
        BotActivityLeaseManager::Instance().Release(guid.GetCounter(), BotActivity::Dungeon);
        Player* member = ObjectAccessor::FindPlayerNotInWorld(guid);
        if (!member)
            continue;
        if (PlayerbotAI* memberAI = PlayerbotAIStorage::Instance().GetAI(member))
        {
            memberAI->SetMaster(nullptr);
            // Mid-teleport (off its map) ResetStrategies would assert; Update does it once it lands.
            if (member->IsInWorld())
                memberAI->ResetStrategies();
            else
                m_pendingReset.push_back(guid);
        }
    }
}

} // namespace TortoiseBots
