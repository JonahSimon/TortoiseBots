#pragma once
#include "PassTroughStrategy.h"

namespace ai
{
    class DuelStrategy : public PassTroughStrategy
    {
    public:
        DuelStrategy(PlayerbotAI* ai) : PassTroughStrategy(ai) {}
        std::string getName() override { return "duel"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "duel"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy will allow bots to accept duels and attack their duel target.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { "start duel" }; }
#endif
    private:
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatTriggers(std::list<TriggerNode*> &triggers) override;
        void InitDeadTriggers(std::list<TriggerNode*>& triggers) override {}
    };

    class StartDuelStrategy : public Strategy
    {
    public:
        StartDuelStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "start duel"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "start duel"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy will allow bots to start duels with other bots if they are the current [h:value|rpg target].";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { "duel", "rpg", "rpg player" }; }
#endif
    };

    // Marker only: EnemyPlayersValue checks for it before scanning for open-world PvP targets.
    class WorldPvpStrategy : public Strategy
    {
    public:
        WorldPvpStrategy(PlayerbotAI* ai) : Strategy(ai) {}
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
