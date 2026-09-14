// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// Unified virtual ammunition logic for population combat shells.

#include "population_shell_ammo.hpp"

#include <algorithm>
#include <common/timer.hpp>

#include "../../clif.hpp"
#include "../../itemdb.hpp"
#include "../../log.hpp"
#include "../../map.hpp"
#include "../../mob.hpp"
#include "../../pc.hpp"
#include "../../skill.hpp"
#include "../../status.hpp"

namespace {

constexpr int kAmmoElementBonus = 500;
constexpr int kAmmoRefillThreshold = 100;
constexpr int kAmmoStockAmount = 500;

struct ShellAmmoChoice {
	t_itemid id;
	e_ammo_type subtype;
	uint16 element;
	uint16 attack;
	uint16 min_level;
};

static constexpr ShellAmmoChoice kArrows[] = {
	{ 1750, AMMO_ARROW, ELE_NEUTRAL, 25, 1 }, { 1751, AMMO_ARROW, ELE_HOLY, 30, 1 },
	{ 1752, AMMO_ARROW, ELE_FIRE, 30, 1 }, { 1753, AMMO_ARROW, ELE_NEUTRAL, 40, 1 },
	{ 1754, AMMO_ARROW, ELE_WATER, 30, 1 }, { 1755, AMMO_ARROW, ELE_WIND, 30, 1 },
	{ 1756, AMMO_ARROW, ELE_EARTH, 30, 1 }, { 1757, AMMO_ARROW, ELE_GHOST, 30, 1 },
	{ 1762, AMMO_ARROW, ELE_NEUTRAL, 30, 1 }, { 1765, AMMO_ARROW, ELE_POISON, 50, 1 },
	{ 1766, AMMO_ARROW, ELE_HOLY, 50, 1 }, { 1767, AMMO_ARROW, ELE_DARK, 30, 1 },
	{ 1770, AMMO_ARROW, ELE_NEUTRAL, 30, 1 }, { 1772, AMMO_ARROW, ELE_HOLY, 50, 1 },
	{ 1773, AMMO_ARROW, ELE_NEUTRAL, 45, 1 }, { 1774, AMMO_ARROW, ELE_NEUTRAL, 35, 1 },
};

// Deliberately excludes Slug_Bullet_1 (13210). It is marked Classes: All:
// false, and the old forced-equip path caused rAthena to remove it on pc_setpos.
static constexpr ShellAmmoChoice kBullets[] = {
	{ 13200, AMMO_BULLET, ELE_NEUTRAL, 25, 1 }, { 13201, AMMO_BULLET, ELE_HOLY, 15, 1 },
	{ 13215, AMMO_BULLET, ELE_NEUTRAL, 50, 100 }, { 13216, AMMO_BULLET, ELE_FIRE, 40, 100 },
	{ 13217, AMMO_BULLET, ELE_WATER, 40, 100 }, { 13218, AMMO_BULLET, ELE_WIND, 40, 100 },
	{ 13219, AMMO_BULLET, ELE_EARTH, 40, 100 }, { 13220, AMMO_BULLET, ELE_HOLY, 40, 100 },
	{ 13221, AMMO_BULLET, ELE_HOLY, 15, 1 }, { 13228, AMMO_BULLET, ELE_FIRE, 20, 1 },
	{ 13229, AMMO_BULLET, ELE_WIND, 20, 1 }, { 13230, AMMO_BULLET, ELE_WATER, 20, 1 },
	{ 13231, AMMO_BULLET, ELE_POISON, 20, 1 }, { 13232, AMMO_BULLET, ELE_DARK, 20, 1 },
};

static constexpr ShellAmmoChoice kShuriken[] = {
	{ 13250, AMMO_SHURIKEN, ELE_NEUTRAL, 10, 1 }, { 13251, AMMO_SHURIKEN, ELE_NEUTRAL, 30, 20 },
	{ 13252, AMMO_SHURIKEN, ELE_NEUTRAL, 45, 40 }, { 13253, AMMO_SHURIKEN, ELE_NEUTRAL, 70, 60 },
	{ 13254, AMMO_SHURIKEN, ELE_NEUTRAL, 100, 80 },
};

static constexpr ShellAmmoChoice kKunai[] = {
	{ 13255, AMMO_KUNAI, ELE_WATER, 30, 1 }, { 13256, AMMO_KUNAI, ELE_EARTH, 30, 1 },
	{ 13257, AMMO_KUNAI, ELE_WIND, 30, 1 }, { 13258, AMMO_KUNAI, ELE_FIRE, 30, 1 },
	{ 13259, AMMO_KUNAI, ELE_POISON, 30, 1 },
};

template <size_t N>
static void pe_shell_stock_ammo(map_session_data *sd, const ShellAmmoChoice (&choices)[N])
{
	if (!sd)
		return;

	for (const ShellAmmoChoice &choice : choices) {
		if (sd->status.base_level < choice.min_level)
			continue;
		int16 index = pc_search_inventory(sd, choice.id);
		int amount = index >= 0 ? sd->inventory.u.items_inventory[index].amount : 0;
		if (amount >= kAmmoRefillThreshold)
			continue;

		item_data *data = itemdb_search(choice.id);
		if (!data || data->type != IT_AMMO || data->subtype != choice.subtype || !(data->equip & EQP_AMMO))
			continue;

		struct item ammo = {};
		ammo.nameid = choice.id;
		ammo.identify = 1;
		const int add_amount = kAmmoStockAmount - amount;
		if (pc_additem(sd, &ammo, add_amount, LOG_TYPE_NONE, false) != ADDITEM_SUCCESS)
			continue;

		index = pc_search_inventory(sd, choice.id);
		if (index >= 0 && pc_isequip(sd, index) != ITEM_EQUIP_ACK_OK)
			pc_delitem(sd, index, add_amount, 0, 0, LOG_TYPE_NONE);
	}
}

static bool pe_shell_is_ninja(const map_session_data *sd)
{
	return sd && (sd->class_ & MAPID_FIRSTMASK) == MAPID_NINJA;
}

static bool pe_shell_weapon_uses_arrows(int16 weapon)
{
	return weapon == W_BOW || weapon == W_MUSICAL || weapon == W_WHIP;
}

static bool pe_shell_weapon_uses_bullets(int16 weapon)
{
	return weapon == W_REVOLVER || weapon == W_RIFLE || weapon == W_GATLING ||
		weapon == W_SHOTGUN || weapon == W_GRENADE;
}

static bool pe_shell_elemstrong(const mob_data *md, int ele)
{
	if (!md)
		return false;

	const int def_ele = md->status.def_ele;
	const int ele_lv = md->status.ele_lv;

	switch (ele) {
	case ELE_GHOST:
		return (def_ele == ELE_UNDEAD && ele_lv >= 2) || (def_ele == ELE_GHOST);
	case ELE_FIRE:
		return def_ele == ELE_UNDEAD || def_ele == ELE_EARTH;
	case ELE_WATER:
		return (def_ele == ELE_UNDEAD && ele_lv >= 3) || (def_ele == ELE_FIRE);
	case ELE_WIND:
		return def_ele == ELE_WATER;
	case ELE_EARTH:
		return def_ele == ELE_WIND;
	case ELE_HOLY:
		return (def_ele == ELE_POISON && ele_lv >= 3) || (def_ele == ELE_DARK) || (def_ele == ELE_UNDEAD);
	case ELE_DARK:
		return def_ele == ELE_HOLY;
	case ELE_POISON:
		return (def_ele == ELE_UNDEAD && ele_lv >= 2) || (def_ele == ELE_GHOST) || (def_ele == ELE_NEUTRAL);
	case ELE_UNDEAD:
		return (def_ele == ELE_HOLY && ele_lv >= 2);
	case ELE_NEUTRAL:
	default:
		return false;
	}
}

static bool pe_shell_elemallowed(mob_data *md, int ele)
{
	if (!md)
		return true;

	const int def_ele = md->status.def_ele;
	const int ele_lv = md->status.ele_lv;

	if (md->sc.getSCE(SC_WHITEIMPRISON)) {
		if (ele != ELE_GHOST)
			return false;
	}

	switch (ele) {
	case ELE_GHOST:
		return !((def_ele == ELE_NEUTRAL && ele_lv >= 2) || (def_ele == ELE_FIRE && ele_lv >= 3) ||
				(def_ele == ELE_WATER && ele_lv >= 3) || (def_ele == ELE_WIND && ele_lv >= 3) ||
				(def_ele == ELE_EARTH && ele_lv >= 3) || (def_ele == ELE_POISON && ele_lv >= 3) ||
				(def_ele == ELE_HOLY && ele_lv >= 2) || (def_ele == ELE_DARK && ele_lv >= 2));
	case ELE_FIRE:
	case ELE_WATER:
	case ELE_WIND:
	case ELE_EARTH:
		if (def_ele == ele || (def_ele == ELE_HOLY && ele_lv >= 2) || (def_ele == ELE_DARK && ele_lv >= 3))
			return false;
		if (ele == ELE_EARTH && def_ele == ELE_UNDEAD && ele_lv >= 4)
			return false;
		return true;
	case ELE_HOLY:
		return def_ele != ELE_HOLY;
	case ELE_DARK:
		return !(def_ele == ELE_POISON || def_ele == ELE_DARK || def_ele == ELE_UNDEAD);
	case ELE_POISON:
		return !((def_ele == ELE_WATER && ele_lv >= 3) || (def_ele == ELE_GHOST && ele_lv >= 3) ||
				(def_ele == ELE_POISON) || (def_ele == ELE_UNDEAD) || (def_ele == ELE_HOLY && ele_lv >= 2) ||
				(def_ele == ELE_DARK));
	case ELE_UNDEAD:
		return !((def_ele == ELE_WATER && ele_lv >= 3) || (def_ele == ELE_FIRE && ele_lv >= 3) ||
				(def_ele == ELE_WIND && ele_lv >= 3) || (def_ele == ELE_EARTH && ele_lv >= 3) ||
				(def_ele == ELE_POISON && ele_lv >= 1) || (def_ele == ELE_UNDEAD) || (def_ele == ELE_DARK));
	case ELE_NEUTRAL:
		return !(def_ele == ELE_GHOST && ele_lv >= 2);
	default:
		return true;
	}
}

template <size_t N>
static bool pe_shell_ammochange(map_session_data *sd, mob_data *md, const ShellAmmoChoice (&choices)[N], int rqAmount)
{
	if (!sd)
		return false;
	if (DIFF_TICK(sd->canequip_tick, gettick()) > 0)
		return false;

	pe_shell_stock_ammo(sd, choices);

	int bestIndex = -1;
	int bestPriority = -1;

	for (const ShellAmmoChoice &choice : choices) {
		int16 index = pc_search_inventory(sd, choice.id);
		if (index < 0)
			continue;
		if (rqAmount > 0 && sd->inventory.u.items_inventory[index].amount < rqAmount)
			continue;
		if (sd->status.base_level < choice.min_level || pc_isequip(sd, index) != ITEM_EQUIP_ACK_OK)
			continue;

		int priority = static_cast<int>(choice.attack);
		if (pe_shell_elemstrong(md, choice.element))
			priority += kAmmoElementBonus;

		if (pe_shell_elemallowed(md, choice.element) && priority > bestPriority) {
			bestPriority = priority;
			bestIndex = index;
		}
	}

	if (bestIndex < 0)
		return false;
	if (sd->equip_index[EQI_AMMO] == bestIndex)
		return true;
	return pc_equipitem(sd, bestIndex, EQP_AMMO, false);

}

template <size_t N>
static bool pe_shell_try_skill_ammo(map_session_data *sd, mob_data *md, int ammo_mask, int amount,
	const ShellAmmoChoice (&choices)[N])
{
	if (!(ammo_mask & (1 << choices[0].subtype)))
		return false;
	return pe_shell_ammochange(sd, md, choices, amount);
}

static bool pe_shell_equip_default_for_weapon(map_session_data *sd)
{
	if (pe_shell_weapon_uses_arrows(sd->status.weapon))
		return pe_shell_ammochange(sd, nullptr, kArrows, 1);
	if (pe_shell_weapon_uses_bullets(sd->status.weapon))
		return pe_shell_ammochange(sd, nullptr, kBullets, 1);
	return true;
}


} // namespace

void population_shell_prepare_ammo(map_session_data *sd)
{
	if (!sd)
		return;
	if (pe_shell_weapon_uses_arrows(sd->status.weapon))
		pe_shell_stock_ammo(sd, kArrows);
	if (pe_shell_weapon_uses_bullets(sd->status.weapon))
		pe_shell_stock_ammo(sd, kBullets);
	if (pe_shell_is_ninja(sd)) {
		pe_shell_stock_ammo(sd, kShuriken);
		pe_shell_stock_ammo(sd, kKunai);
	}
	pe_shell_equip_default_for_weapon(sd);
}

bool population_shell_equip_best_ammo_for_target(map_session_data *sd, mob_data *md)
{
	if (!sd)
		return false;
	if (pe_shell_weapon_uses_arrows(sd->status.weapon))
		return pe_shell_ammochange(sd, md, kArrows, 1);
	if (pe_shell_weapon_uses_bullets(sd->status.weapon))
		return pe_shell_ammochange(sd, md, kBullets, 1);
	return true;
}

bool population_shell_equip_ammo_for_skill(map_session_data *sd, mob_data *md, uint16 skill_id, uint16 skill_lv)
{
	if (!sd || skill_id == 0)
		return false;
	const int ammo_mask = skill_get_ammotype(skill_id);
	if (ammo_mask == AMMO_NONE)
		return true;
	const int amount = std::max(1, skill_get_ammo_qty(skill_id, skill_lv));

	if (pe_shell_try_skill_ammo(sd, md, ammo_mask, amount, kArrows))
		return true;
	if (pe_shell_try_skill_ammo(sd, md, ammo_mask, amount, kBullets))
		return true;
	if (pe_shell_try_skill_ammo(sd, md, ammo_mask, amount, kShuriken))
		return true;
	if (pe_shell_try_skill_ammo(sd, md, ammo_mask, amount, kKunai))
		return true;
	return false;
}
