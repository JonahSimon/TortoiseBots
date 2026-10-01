#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    class AttackEnemyPlayersStrategy : public Strategy
    {
    public:
        AttackEnemyPlayersStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "pvp"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "pvp"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy detect nearby enemy players and makes the bot attack them.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    private:
        void InitCombatTriggers(std::list<TriggerNode*> &triggers) override;
        // Also registered for non-combat (2026-07-27): an IDLE bot never
        // transitions into BOT_STATE_COMBAT on its own just because an enemy
        // player is standing nearby - only the reactive "was attacked" path
        // (or an explicit attack action) does that. Since this trigger was
        // previously only wired into the combat engine, two idle enemy bots
        // standing next to each other in a BG never fought until a human
        // player attacked one of them first (chicken-and-egg: needed combat
        // state to check the trigger that starts combat). The action itself
        // (AttackEnemyPlayerAction -> AttackAction) already transitions the
        // bot into combat state once it actually attacks, so it's safe to
        // fire from the non-combat engine too.
        void InitNonCombatTriggers(std::list<TriggerNode*> &triggers) override;
    };

    // Open-world PvP seeking (AiPlayerbot.WorldPvpSeek). EnemyPlayersValue only scans the open world
    // for bots holding this strategy. AiFactory puts it in the NON-COMBAT engine, where "pvp" never
    // is, so the inherited non-combat trigger is what lets an idle bot start the fight; without it
    // only a bot already fighting a mob would ever turn on a player.
    class WorldPvpStrategy : public AttackEnemyPlayersStrategy
    {
    public:
        WorldPvpStrategy(PlayerbotAI* ai) : AttackEnemyPlayersStrategy(ai) {}
        std::string getName() override { return "world pvp"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "world pvp"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This bot will attack nearby PvP-flagged enemy players in the open world instead of only fighting back. Needs AiPlayerbot.WorldPvpSeek enabled.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { "pvp" }; }
#endif
    };
}
