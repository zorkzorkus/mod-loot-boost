#include "LootBoost.h"

#include "LootMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

// Reads the config and scans the loot tables once at startup
class LootBoostWorldScript : public WorldScript
{
public:
    LootBoostWorldScript() : WorldScript("LootBoostWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        sLootBoost->LoadConfig(reload);
    }

    void OnStartup() override
    {
        sLootBoost->BuildData();
    }
};

// Adds boss bonus loot and multiplies emblems after a loot window is generated
class LootBoostMiscScript : public MiscScript
{
public:
    LootBoostMiscScript() : MiscScript("LootBoostMiscScript", {
        MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS
    }) { }

    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner,
        bool /*personal*/, bool /*noEmptyError*/, uint16 lootMode) override
    {
        sLootBoost->OnLootGenerated(loot, tab, store, lootOwner, lootMode);
    }
};

// Raises the chance of BoE blues and epics on every roll
class LootBoostGlobalScript : public GlobalScript
{
public:
    LootBoostGlobalScript() : GlobalScript("LootBoostGlobalScript", {
        GLOBALHOOK_ON_ITEM_ROLL
    }) { }

    bool OnItemRoll(Player const* /*player*/, LootStoreItem const* item, float& chance, Loot& /*loot*/, LootStore const& store) override
    {
        sLootBoost->AdjustRollChance(item, chance, store);
        return true;
    }
};

void AddLootBoostScripts()
{
    new LootBoostWorldScript();
    new LootBoostMiscScript();
    new LootBoostGlobalScript();
}
