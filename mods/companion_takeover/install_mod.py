#!/usr/bin/env python3
"""Companion takeover: when you fall in battle, continue as the nearest companion.

Adds two triggers to the battle mission templates of a compiled module
(mission_templates.txt) and one message to quick_strings.txt.  The originals
are kept as *.takeover-orig and every run starts from them, so rerunning is
safe.  Needs fastswap.dylib with takeover.c (the engine only allows
player_control_agent in multiplayer; the dylib makes it work in single player).

usage: install_mod.py <module dir> [--uninstall] [--test]
  --test  also takes over plain soldiers (for custom battles, which have no
          companions) and makes F9 knock the player out instantly.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mstfile  # noqa: E402

# Opcodes (Warband 1.171+ header_operations.py)
try_end, try_begin, try_for_agents = 3, 4, 12
ge, eq, lt = 30, 31, 0x80000000 | 30
neq = 0x80000000 | 31
neg = 0x80000000
key_clicked = 71
player_control_agent = 421
get_distance_between_positions = 710
display_message = 1106
troop_is_hero = 1507
get_player_agent_no, agent_is_alive, agent_is_human, agent_is_ally = 1700, 1702, 1704, 1706
agent_get_position, agent_get_party_id, agent_get_troop_id = 1710, 1716, 1718
agent_deliver_damage_to_agent = 1722
mission_cam_set_mode = 2001
store_trigger_param_1 = 2071
assign = 2133
str_store_troop_name = 2322

LOCAL = 17 << 56
QUICK_STRING = 22 << 56
TI_ON_AGENT_KILLED_OR_WOUNDED = "-26.000000"
KEY_F9 = 0x43
P_MAIN_PARTY = 0

BATTLES = [
    "lead_charge", "village_attack_bandits", "village_raid",
    "besiege_inner_battle_castle", "besiege_inner_battle_town_center",
    "castle_attack_walls_defenders_sally", "castle_attack_walls_belfry", "castle_attack_walls_ladder",
    "bandit_lair", "entrenched_encounter", "ship_battle",
    "quick_battle_battle", "quick_battle_siege",
]
QSTR_ID, QSTR_TEXT = "qstr_ctakeover_now_as", "You_continue_the_fight_as_{s1}."


def block(stmts):
    """Assemble statements, turning ':name' operands into local variables."""
    names, out = [], []
    for st in stmts:
        ops = [st[0]]
        for a in st[1:]:
            if isinstance(a, str) and a.startswith(":"):
                if a not in names:
                    names.append(a)
                a = LOCAL | names.index(a)
            ops.append(a)
        out.append(ops)
    return out


def takeover_body(qstr, test):
    """Switch control to the living companion nearest to the fallen player agent."""
    companion_checks = [] if test else [
        (agent_get_troop_id, ":troop", ":agent"),
        (troop_is_hero, ":troop"),
        (agent_get_party_id, ":party", ":agent"),
        (eq, ":party", P_MAIN_PARTY),
    ]
    return [
        (get_player_agent_no, ":player"),
        (ge, ":player", 0),
        (agent_get_position, 1, ":player"),
        (assign, ":best", -1),
        (assign, ":best_dist", 1000000000),
        (try_for_agents, ":agent"),
            (neq, ":agent", ":player"),
            (agent_is_alive, ":agent"),
            (agent_is_human, ":agent"),
            (agent_is_ally, ":agent"),
            *companion_checks,
            (agent_get_position, 2, ":agent"),
            (get_distance_between_positions, ":dist", 1, 2),
            (lt, ":dist", ":best_dist"),
            (assign, ":best_dist", ":dist"),
            (assign, ":best", ":agent"),
        (try_end,),
        (ge, ":best", 0),
        # single-player form (see src/takeover.c): first argument is the agent
        (player_control_agent, ":best", ":best"),
        (mission_cam_set_mode, 0, 0, 0),
        (agent_get_troop_id, ":troop", ":best"),
        (str_store_troop_name, 1, ":troop"),
        (display_message, QUICK_STRING | qstr, 0xFFDD8844),
    ]


def triggers(qstr, test):
    body = block(takeover_body(qstr, test))
    out = [
        # the moment the player is killed or knocked out
        [(TI_ON_AGENT_KILLED_OR_WOUNDED, "0.000000", "0.000000"),
         block([(store_trigger_param_1, ":dead"), (get_player_agent_no, ":player"), (eq, ":dead", ":player")]),
         body],
        # fallback, and picks up companions that arrive later (reinforcements)
        [("1.000000", "0.000000", "0.000000"),
         block([(get_player_agent_no, ":player"), (ge, ":player", 0), (neg | agent_is_alive, ":player")]),
         body],
    ]
    if test:
        out.append([("0.000000", "0.000000", "0.000000"),
                    block([(key_clicked, KEY_F9)]),
                    block([(get_player_agent_no, ":player"), (ge, ":player", 0),
                           (agent_deliver_damage_to_agent, ":player", ":player", 1000)])])
    return out


def main(argv):
    mod = argv[0]
    mt, qs = os.path.join(mod, "mission_templates.txt"), os.path.join(mod, "quick_strings.txt")
    for f in (mt, qs):
        if not os.path.exists(f + ".takeover-orig"):
            shutil.copy2(f, f + ".takeover-orig")
    if "--uninstall" in argv:
        for f in (mt, qs):
            shutil.copy2(f + ".takeover-orig", f)
        print("companion takeover removed")
        return

    lines = open(qs + ".takeover-orig").read().split("\n")
    strings = [l for l in lines[1:] if l.strip()]
    qstr = len(strings)
    strings.append(f"{QSTR_ID} {QSTR_TEXT}")
    open(qs, "w").write("%d\n" % len(strings) + "\n".join(strings) + "\n")

    tpls = mstfile.parse(open(mt + ".takeover-orig").read())
    done = []
    for t in tpls:
        if t["name"] in BATTLES:
            t["triggers"] += triggers(qstr, "--test" in argv)
            done.append(t["name"])
    open(mt, "w").write(mstfile.write(tpls))
    print(f"companion takeover added to {len(done)} battle types" + (" (TEST MODE)" if "--test" in argv else ""))


if __name__ == "__main__":
    main(sys.argv[1:])
