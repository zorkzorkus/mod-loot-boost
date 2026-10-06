/*
 * mod-loot-boost
 *
 * More loot from bosses (scaled per instance size), multiplied emblems,
 * and better odds for BoE blues and epics everywhere.
 *
 * Released under the GNU AGPL v3 license, like AzerothCore itself.
 */

#ifndef MOD_LOOT_BOOST_H
#define MOD_LOOT_BOOST_H

#include "Define.h"

#include <array>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LootStore;
class LootTemplate;
class Map;
class Player;
struct Loot;
struct LootStoreItem;

namespace LootBoost
{
    // Which multiplier applies to a boss, based on where it dies.
    enum SizeBucket : uint8
    {
        BUCKET_DUNGEON = 0,
        BUCKET_RAID10,
        BUCKET_RAID15,
        BUCKET_RAID20,
        BUCKET_RAID25,
        BUCKET_RAID40,
        BUCKET_OTHER,
        BUCKET_COUNT
    };

    // A guaranteed reference row that points to a boss gear pool.
    // Every bonus pass rolls the pool once more.
    struct BonusReference
    {
        uint32 referenceId;
        uint8 maxCount;
        uint16 lootMode;
    };

    // One item of a plain gear group (gear listed directly in a boss
    // loot table, like Archmage Arugal's).
    struct GroupEntry
    {
        uint32 itemId;
        float weight;   // effective drop chance in percent
        uint16 lootMode;
        uint8 minCount;
        uint8 maxCount;
        bool equippable;
    };

    struct GearGroup
    {
        uint8 groupId;
        float totalChance; // chance in percent that the group drops anything at all
        std::vector<GroupEntry> entries;
    };

    class Manager
    {
    public:
        static Manager* instance();

        // Called on startup and on ".reload config".
        void LoadConfig(bool reload);

        // Called once at startup, after the world database is loaded.
        void BuildData();

        // Called after the core generated a loot window.
        void OnLootGenerated(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner, uint16 lootMode);

        // Called for every chance roll. May change the chance.
        void AdjustRollChance(LootStoreItem const* item, float& chance, LootStore const& store) const;

    private:
        Manager() = default;

        // Bonus logic
        uint32 AddBonusPasses(Loot* loot, LootStore const& store, Player* lootOwner, uint16 lootMode,
            std::vector<BonusReference> const& refs, float multiplier, uint32 maxItems) const;
        uint32 AddBonusPicks(Loot* loot, uint16 lootMode, std::vector<GearGroup> const& groups,
            float multiplier, uint32 maxItems) const;
        void MultiplyEmblems(Loot* loot) const;

        static SizeBucket GetBucket(Map const* map);
        static char const* GetBucketName(SizeBucket bucket);
        static uint32 RandomRound(float value);

        // Startup data helpers
        bool IsEmblem(uint32 itemId) const;
        bool IsBoostedStore(LootStore const& store) const;

        // Config (atomics, because loot is generated in parallel map threads)
        std::atomic<bool> _bossEnabled{true};
        std::atomic<bool> _logKills{true};
        std::atomic<uint32> _maxItems{15};
        std::array<std::atomic<float>, BUCKET_COUNT> _multipliers{};
        std::atomic<float> _emblemMultiplier{1.0f};
        std::atomic<bool> _boeEnabled{true};
        std::atomic<float> _boeRareMultiplier{1.0f};
        std::atomic<float> _boeEpicMultiplier{1.0f};
        std::atomic<float> _boeMaxChance{5.0f};

        // Read only after startup
        std::unordered_set<uint32> _emblemItems;
        bool _emblemListLoaded = false;

        std::unordered_map<uint32, std::vector<BonusReference>> _creatureBonusRefs; // by creature loot id
        std::unordered_map<uint32, std::vector<BonusReference>> _chestBonusRefs;    // by chest loot id
        std::unordered_map<uint32, std::vector<GearGroup>> _creatureGearGroups;     // by creature loot id

        std::unordered_map<uint32, uint32> _boePools;       // reference id -> item quality (3 or 4)
        std::unordered_set<uint32> _boeExcludedItems;       // BoE items that sit in crowded groups
        std::unordered_set<uint32> _boeExcludedPools;       // BoE pools that sit in crowded groups
        bool _dataBuilt = false;
    };
}

#define sLootBoost LootBoost::Manager::instance()

#endif
