
#include "playerbot/playerbot.h"
#include "EnemyPlayerValue.h"
#include "TargetValue.h"

using namespace ai;

std::list<ObjectGuid> EnemyPlayersValue::Calculate()
{
    std::list<ObjectGuid> result;
    if (ai->AllowActivity(ALL_ACTIVITY))
    {
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            // Check if we only need one attacker
            bool getOne = false;
            if (!qualifier.empty())
            {
                getOne = std::stoi(qualifier);
            }

            if (getOne)
            {
                // Try to get one enemy target
                result = AI_VALUE2(std::list<ObjectGuid>, "possible attack targets", 1);
                ApplyFilter(result, getOne);
            }

            // If the one enemy player failed, retry with multiple possible attack targets
            if (result.empty())
            {
                result = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
                ApplyFilter(result, getOne);
            }

            // "possible attack targets" is built exclusively from "attackers" -
            // units already in an active combat/threat relationship with the
            // bot (2026-07-27, confirmed live via debug logging: hasEnemy was
            // 0 for every bot standing right next to an untouched enemy). That
            // meant bots never proactively engaged anyone - only native combat
            // (a human player actually landing a hit) ever created a real
            // threat entry, so bots only fought back once directly attacked,
            // and other nearby bots never "assisted" either since this whole
            // value only ever looked at the bot's OWN attacker list. Falling
            // back to "possible targets" - a genuine proximity scan
            // (AnyUnfriendlyUnitInObjectRangeCheck via Cell::VisitAllObjects,
            // see PossibleTargetsValue.cpp) with no threat-table dependency -
            // gives bots a real "is anyone hostile nearby" signal. Scoped to
            // InBattleGround() only so normal world PvE bot behavior against
            // real players elsewhere doesn't change.
            if (result.empty() && bot->InBattleGround())
            {
                result = AI_VALUE(std::list<ObjectGuid>, "possible targets");
                ApplyFilter(result, getOne);
            }

            // World PvP (AiPlayerbot.WorldPvpSeek), ported from mod-playerbots'
            // NearestEnemyPlayersValue + PossibleTargetsValue: outside a battleground, a bot holding
            // the "world pvp" strategy goes after a nearby flagged enemy player instead of only
            // fighting back. Off by default.
            if (result.empty() && !bot->InBattleGround() && sPlayerbotAIConfig.worldPvpSeek &&
                ai->HasStrategy("world pvp", BotState::BOT_STATE_NON_COMBAT))
            {
                for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "possible targets"))
                {
                    Unit* target = ai->GetUnit(guid);
                    if (IsValid(target, bot) && IsWorldPvpTarget((Player*)target))
                    {
                        // One csv line per new engagement (not per tick): the bot is not on this target yet.
                        if (bot->GetSelectionGuid() != target->GetObjectGuid() && sPlayerbotAIConfig.hasLog("pvp_seek.csv"))
                            sPlayerbotAIConfig.log("pvp_seek.csv", (sPlayerbotAIConfig.GetTimestampStr() + "+00,seek," +
                                bot->GetName() + "," + target->GetName()).c_str());
                        result.push_back(guid);
                        if (getOne)
                            break;
                    }
                }
            }
        }
    }

    return result;
}

// mod-playerbots' open-world rules, in its order. IsValid() has already checked the target is
// a non-friendly player.
bool EnemyPlayersValue::IsWorldPvpTarget(Player* enemy)
{
    // 1 = only real players, 2 = bots too.
    if (sPlayerbotAIConfig.worldPvpSeek == 1 && !ai->IsRealPlayer(enemy))
        return false;

    // Only someone who can legally be attacked: an unflagged player is off limits.
    if (!ai->IsOpposing(enemy) || !enemy->IsPvP())
        return false;

    if (sPlayerbotAIConfig.IsInPvpProhibitedZone(sServerFacade.GetAreaId(enemy)))
        return false;

    // Aggro range: 20 yd, or the bot's sight range when it has more health than the target.
    float const aggro = bot->GetHealth() > enemy->GetHealth() ? sPlayerbotAIConfig.sightDistance : 20.0f;
    if (!bot->IsWithinDist(enemy, aggro) || fabs(bot->GetPositionZ() - enemy->GetPositionZ()) >= 30.0f)
        return false;

    // Level gap: 5+ above never; +/-4 (or 5+ below) 25 %, +/-3 50 %, +/-2 75 %, closer always.
    int32 const diff = int32(enemy->GetLevel()) - int32(bot->GetLevel());
    if (diff >= 5)
        return false;
    uint32 const chance = (std::abs(diff) >= 4) ? 25 : std::abs(diff) == 3 ? 50 : std::abs(diff) == 2 ? 75 : 100;
    if (chance == 100)
        return true;

    // Same roll for the same pair for two minutes (FNV-1a over both guids and the window), so the
    // bot does not re-decide every tick until it eventually says yes.
    uint64 hash = 14695981039346656037ULL;
    for (uint64 v : { bot->GetObjectGuid().GetRawValue(), enemy->GetObjectGuid().GetRawValue(), uint64(time(nullptr) / 120) })
    {
        hash ^= v;
        hash *= 1099511628211ULL;
    }
    return hash % 100 < chance;
}

bool EnemyPlayersValue::IsValid(Unit* target, Player* player)
{
    if (target)
    {
        // If the target is a player
        Player* enemyPlayer = dynamic_cast<Player*>(target);
        if (enemyPlayer)
        {
            // If the target is friendly to the player
            if (sServerFacade.IsFriendlyTo(target, player))
            {
                return false;
            }

            // Check that the target is not a mind controlled ally
            if (target->HasAuraType(SPELL_AURA_MOD_CHARM) || target->HasAuraType(SPELL_AURA_MOD_POSSESS))
            {
                Player* targetPlayer = dynamic_cast<Player*>(target);
                if (player && targetPlayer && IsInGroup_Helper(player, targetPlayer))
                {
                    return false;
                }
            }

            // A masterless random bot that died to this named player recently does
            // not INITIATE a fight with it; the trade-kill loop in Ashenvale ran
            // every ~60 s because both sides kept re-selecting each other. Owned
            // bots (real master) still obey their player. The gate is the bot's
            // own combat state: while the bot is out of combat and has no
            // attackers, the avoided killer is refused; as soon as the bot is in
            // combat (IsInCombat or any attacker — melee, ranged or caster, the
            // core sets UNIT_FLAG_IN_COMBAT for spells via SetInCombatWith),
            // normal target selection applies and retaliation works with no new
            // heuristics. Selection/target GUIDs deliberately never count: a
            // player merely clicking the bot must not provoke it.
            if (PlayerbotAI* playerAi = PlayerbotAIStorage::Instance().GetAI(player))
            {
                if (!playerAi->HasRealPlayerMaster() && playerAi->ShouldAvoidPlayerKiller(enemyPlayer->GetName()))
                {
                    if (!player->IsInCombat() && player->GetAttackers().empty())
                        return false;
                }
            }

            return true;
        }
    }

    return false;
}

void EnemyPlayersValue::ApplyFilter(std::list<ObjectGuid>& targets, bool getOne)
{
    std::list<ObjectGuid> filteredTargets;
    for (const ObjectGuid& targetGuid : targets)
    {
        Unit* target = ai->GetUnit(targetGuid);
        if (IsValid(target, bot))
        {
            filteredTargets.push_back(target->getObjectGuid());

            if (getOne)
            {
                break;
            }
        }
    }

    targets = filteredTargets;
}

bool HasEnemyPlayersValue::Calculate()
{
    return !context->GetValue<std::list<ObjectGuid>>("enemy player targets", 1)->Get().empty();
}

Unit* EnemyPlayerValue::Calculate()
{
    // Prioritize the duel opponent
    if (bot->m_duel && !bot->m_duel->opponent.IsEmpty()) {
        if (Unit* opp = ObjectAccessor::GetUnit(*bot, bot->m_duel->opponent)) {
            if (!sServerFacade.IsFriendlyTo(opp, bot))
                return opp;
        }
    }

    Unit* bestEnemyPlayer = nullptr;
    std::list<ObjectGuid> enemyPlayers = AI_VALUE(std::list<ObjectGuid>, "enemy player targets");
    if (!enemyPlayers.empty())
    {
        const bool isMelee = !ai->IsRanged(bot);
        uint32 bestEnemyPlayerHealth = std::numeric_limits<uint32>::max();
        float bestEnemyPlayerDistance = std::numeric_limits<float>::max();

        // Use the first enemy player as a base
        Unit* firstTarget = ai->GetUnit(enemyPlayers.front());
        if (firstTarget)
        {
            bestEnemyPlayerDistance = firstTarget->GetDistance(bot);
            bestEnemyPlayerHealth = firstTarget->GetHealth();
            bestEnemyPlayer = firstTarget;
        }

        for (const ObjectGuid& targetGuid : enemyPlayers)
        {
            Unit* target = ai->GetUnit(targetGuid);
            if (target)
            {
                // Prioritize an enemy player if it has a battleground flag
                if ((bot->GetTeam() == HORDE && target->HasAura(23333)) ||
                    (bot->GetTeam() == ALLIANCE && target->HasAura(23335)))
                {
                    bestEnemyPlayer = target;
                    break;
                }

                if (isMelee)
                {
                    // Score best enemy player based on lowest distance
                    const float distanceToEnemyPlayer = target->GetDistance(bot);
                    if (distanceToEnemyPlayer < bestEnemyPlayerDistance)
                    {
                        bestEnemyPlayerDistance = distanceToEnemyPlayer;
                        bestEnemyPlayer = target;
                    }
                }
                else
                {
                    // Score best enemy player based on lowest health
                    const uint32 enemyPlayerHealth = target->GetHealth();
                    if (enemyPlayerHealth < bestEnemyPlayerHealth)
                    {
                        bestEnemyPlayerHealth = enemyPlayerHealth;
                        bestEnemyPlayer = target;
                    }
                }
            }
        }
    }

    return bestEnemyPlayer;
}


float EnemyPlayerValue::GetMaxAttackDistance(Player* bot)
{
    if (!bot->GetBattleGround())
        return 60.0f;

    if (bot->InBattleGround())
    {
        BattleGround* bg = bot->GetBattleGround();
        if (!bg)
            return 40.0f;

        BattleGroundTypeId bgType = bg->GetTypeID();

        if (bgType == BATTLEGROUND_AV)
        {
            bool strifeTime = bg->GetStartTime() < (uint32)(20 * MINUTE * IN_MILLISECONDS);
            return strifeTime ? 40.0f : 10.0f;
        }
    }

    return 40.0f;
}
