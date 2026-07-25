#include "ScriptMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "GameObject.h"
#include "MotionMaster.h"
#include "Spell.h"
#include "LootMgr.h"

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace
{
constexpr uint32 FISHING_SPELL_ID = 7620;
constexpr uint32 FISHING_BOBBER_ENTRY = 35591;
constexpr uint32 FISHING_POLE_ITEM = 6256;

struct AutoFishState
{
    bool enabled = false;
    bool armed = false;
    bool lootOpened = false;
    uint32 nextActionMs = 0;
    uint32 lastSeenBobberMs = 0;
    uint32 stuckLogMs = 0;
};

std::unordered_map<ObjectGuid, AutoFishState> s_AutoFishStates;

bool g_AutoFishEnabled = false;
bool g_AutoFishDebug = false;
uint32 g_AutoFishDelayMin = 700;
uint32 g_AutoFishDelayMax = 1600;
std::vector<uint32> g_AutoFishAllowedGuids;

uint32 RollDelay()
{
    if (g_AutoFishDelayMax <= g_AutoFishDelayMin)
        return g_AutoFishDelayMin;
    return urand(g_AutoFishDelayMin, g_AutoFishDelayMax);
}

bool IsAllowed(Player* player)
{
    if (!g_AutoFishEnabled || !player)
        return false;

    if (player->GetSession() == nullptr || player->GetSession()->IsBot())
        return false;

    if (g_AutoFishAllowedGuids.empty())
        return true;

    uint32 guid = player->GetGUID().GetCounter();
    return std::find(g_AutoFishAllowedGuids.begin(), g_AutoFishAllowedGuids.end(), guid) != g_AutoFishAllowedGuids.end();
}

bool HasFishingPole(Player* player)
{
    if (!player)
        return false;

    if (Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
    {
        if (mainHand->GetTemplate() && mainHand->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
            mainHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            return true;
    }

    if (Item* offHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
    {
        if (offHand->GetTemplate() && offHand->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
            offHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            return true;
    }

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            if (item->GetTemplate() && item->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                return true;
        }
    }

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = player->GetBagByPos(bag))
        {
            for (uint32 i = 0; i < pBag->GetBagSize(); ++i)
            {
                if (Item* item = pBag->GetItemByPos(i))
                {
                    if (item->GetTemplate() && item->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                        item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                        return true;
                }
            }
        }
    }

    return false;
}

bool IsFishingBobberReady(Player* player)
{
    if (GameObject* go = player ? player->GetGameObject(FISHING_SPELL_ID) : nullptr)
        return go->GetEntry() == FISHING_BOBBER_ENTRY && go->getLootState() == GO_READY;

    return false;
}

bool IsFishing(Player* player)
{
    if (!player)
        return false;

    Spell* spell = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    return spell != nullptr && spell->GetSpellInfo() != nullptr && spell->GetSpellInfo()->Id == FISHING_SPELL_ID;
}

void LogState(Player* player, char const* msg);

bool AutoLootFishingBobber(Player* player, GameObject* bobber)
{
    if (!player || !bobber)
        return false;

    player->SendLoot(bobber->GetGUID(), LOOT_FISHING);

    Loot* loot = &bobber->loot;
    bool lootedAny = false;
    uint32 const maxLootSlot = loot->GetMaxSlotInLootFor(player);

    LogState(player, "opened fishing loot");

    for (uint32 slot = 0; slot < maxLootSlot; ++slot)
    {
        InventoryResult msg = EQUIP_ERR_OK;
        if (LootItem* lootItem = player->StoreLootItem(static_cast<uint8>(slot), loot, msg))
        {
            lootedAny = true;
            LogState(player, "stored loot item from fishing bobber");
        }

        if (msg == EQUIP_ERR_ITEM_NOT_FOUND || msg == EQUIP_ERR_ALREADY_LOOTED)
            continue;

        if (loot->isLooted())
            break;
    }

    return lootedAny;
}

void StopAutoFishing(Player* player, char const* reason)
{
    if (!player)
        return;

    AutoFishState& state = s_AutoFishStates[player->GetGUID()];
    state.armed = false;
    state.lootOpened = false;
    state.nextActionMs = 0;
    state.lastSeenBobberMs = 0;
    state.stuckLogMs = 0;
    LogState(player, reason);
}

GameObject* FindOwnFishingBobber(Player* player)
{
    GameObject* go = player ? player->GetGameObject(FISHING_SPELL_ID) : nullptr;
    if (go && go->GetEntry() == FISHING_BOBBER_ENTRY)
        return go;
    return nullptr;
}

void LogState(Player* player, char const* msg)
{
    if (!g_AutoFishDebug || !player)
        return;

    LOG_INFO("module", "AutoFishing [{}:{}] {}", player->GetName(), player->GetGUID().GetCounter(), msg);
}
}

class AutoFishingPlayerScript : public PlayerScript
{
public:
    AutoFishingPlayerScript() : PlayerScript("AutoFishingPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_AFTER_UPDATE }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        if (player->GetSession() == nullptr || player->GetSession()->IsBot())
            return;

        AutoFishState& state = s_AutoFishStates[player->GetGUID()];
        state = AutoFishState{};
        state.enabled = g_AutoFishEnabled && IsAllowed(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        if (player->GetSession() == nullptr || player->GetSession()->IsBot())
            return;

        s_AutoFishStates.erase(player->GetGUID());
    }

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        if (!player || !player->IsInWorld() || !IsAllowed(player))
            return;

        AutoFishState& state = s_AutoFishStates[player->GetGUID()];
        if (!state.enabled)
            return;

        if (player->GetSession() == nullptr || !player->GetSession()->PlayerLoading())
        {
            if (player->isDead() || player->IsInCombat() || player->isMoving() || player->IsBeingTeleported() || !player->IsAlive())
            {
                StopAutoFishing(player, "cancelling current fishing cycle because movement/combat/death/teleport");
                return;
            }

            if (!HasFishingPole(player))
            {
                if (state.stuckLogMs == 0 || state.stuckLogMs > 5000)
                {
                    LogState(player, "stopping because no fishing pole is equipped or carried");
                    state.stuckLogMs = 1;
                }
                else
                {
                    state.stuckLogMs += diff;
                }
                return;
            }

            state.stuckLogMs = 0;

            if (state.nextActionMs > diff)
            {
                state.nextActionMs -= diff;
                LogState(player, "waiting for next action window");
                return;
            }

            if (GameObject* bobber = FindOwnFishingBobber(player))
            {
                if (bobber->getLootState() == GO_READY)
                {
                    if (AutoLootFishingBobber(player, bobber))
                        LogState(player, "looted fishing bobber");
                    else
                        LogState(player, "fishing bobber ready but nothing was looted");
                    state.armed = true;
                    state.lootOpened = true;
                    state.nextActionMs = RollDelay();
                    state.lastSeenBobberMs = 0;
                    return;
                }

                if (!IsFishing(player))
                {
                    LogState(player, "bobber is stale after fishing ended; forcing recast");
                    state.lastSeenBobberMs = 0;
                    player->CastSpell(player, FISHING_SPELL_ID, false);
                    state.armed = true;
                    state.lootOpened = false;
                    state.nextActionMs = 1200;
                    return;
                }

                if (state.lastSeenBobberMs == 0)
                    state.lastSeenBobberMs = diff;
                else
                    state.lastSeenBobberMs += diff;

                if (state.lastSeenBobberMs > 15000)
                {
                    LogState(player, "bobber existed too long without becoming ready; resetting cast state");
                    state.nextActionMs = 0;
                    state.lastSeenBobberMs = 0;
                }

                return;
            }

            if (!state.armed)
            {
                if (IsFishing(player))
                {
                    state.armed = true;
                    LogState(player, "armed by manual fishing cast");
                }
                return;
            }

            if (state.lootOpened && player->GetLootGUID().IsGameObject())
            {
                state.lootOpened = false;
                state.nextActionMs = 250;
                LogState(player, "waiting briefly after opening loot");
                return;
            }

            if (!IsFishing(player))
            {
                player->CastSpell(player, FISHING_SPELL_ID, false);
                state.armed = true;
                state.lootOpened = false;
                state.nextActionMs = 1200;
                LogState(player, "cast fishing");
                return;
            }
        }
    }
};

class AutoFishingCommandScript : public CommandScript
{
public:
    AutoFishingCommandScript() : CommandScript("AutoFishingCommandScript") {}

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        static Acore::ChatCommands::ChatCommandTable autofishTable = {
            {"on", HandleAutoFishOn, SEC_PLAYER, Acore::ChatCommands::Console::No},
            {"off", HandleAutoFishOff, SEC_PLAYER, Acore::ChatCommands::Console::No},
            {"status", HandleAutoFishStatus, SEC_PLAYER, Acore::ChatCommands::Console::No},
        };

        static Acore::ChatCommands::ChatCommandTable commandTable = {
            {"autofish", autofishTable},
        };

        return commandTable;
    }

    static bool HandleAutoFishOn(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        s_AutoFishStates[player->GetGUID()].enabled = true;
        s_AutoFishStates[player->GetGUID()].armed = false;
        handler->PSendSysMessage("AutoFishing enabled.");
        return true;
    }

    static bool HandleAutoFishOff(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        s_AutoFishStates[player->GetGUID()].enabled = false;
        s_AutoFishStates[player->GetGUID()].armed = false;
        handler->PSendSysMessage("AutoFishing disabled.");
        return true;
    }

    static bool HandleAutoFishStatus(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        bool enabled = s_AutoFishStates[player->GetGUID()].enabled;
        handler->PSendSysMessage("AutoFishing is {}.", enabled ? "enabled" : "disabled");
        return true;
    }
};

class AutoFishingWorldScript : public WorldScript
{
public:
    AutoFishingWorldScript() : WorldScript("AutoFishingWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_AutoFishEnabled = sConfigMgr->GetOption<bool>("AutoFishing.Enabled", false);
    g_AutoFishDebug = sConfigMgr->GetOption<bool>("AutoFishing.Debug", true);
        g_AutoFishDelayMin = sConfigMgr->GetOption<uint32>("AutoFishing.DelayMsMin", 700);
        g_AutoFishDelayMax = sConfigMgr->GetOption<uint32>("AutoFishing.DelayMsMax", 1600);

        g_AutoFishAllowedGuids.clear();
        std::string allowed = sConfigMgr->GetOption<std::string>("AutoFishing.AllowedGUIDs", "");
        std::stringstream ss(allowed);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (token.empty())
                continue;
            g_AutoFishAllowedGuids.push_back(static_cast<uint32>(std::stoul(token)));
        }

        LOG_INFO("module", "AutoFishing loaded: enabled={}, debug={}, allowed_guids={}", g_AutoFishEnabled, g_AutoFishDebug, g_AutoFishAllowedGuids.size());
    }
};

void Addmod_autofishingScripts()
{
    new AutoFishingWorldScript();
    new AutoFishingPlayerScript();
    new AutoFishingCommandScript();
}
