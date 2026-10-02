# TFC RTV — rock the vote, nominations, end-of-map vote (metamod-p)

This is a metamod-p plugin for Team Fortress Classic. It replaces the AMX Mod X `rtv.amxx`, and
the two must not run together.

## Building from source

The plugin builds against the metamod-p SDK (the `metamod/` and `hlsdk/` folders from
[Bots-United/metamod-p](https://github.com/Bots-United/metamod-p)). Put this repository inside
that folder, next to `metamod/` and `hlsdk/`. On a TFC install that is
`tfc\addons\metamod\metamod_sdk\`:

```
tfc/addons/metamod/metamod_sdk/
    metamod/
    hlsdk/
    tfc_rtv_mm/        <- this repository
```

- **Windows:** Visual Studio, `tfc_rtv_mm.sln`, **Release | Win32**. With the layout above, the
  build copies the DLL to `tfc\addons\tfc_rtv_mm\tfc_rtv_mm_mm.dll`.
- **Linux:** `make -f Makefile.linux` (32-bit g++: `apt install g++-multilib`). Elsewhere, point it at
  the SDK: `make -f Makefile.linux SDK=/path/to/metamod-p`.

## Install

1. The source folder goes in `tfc\addons\metamod\metamod_sdk\tfc_rtv_mm\`, next to `tfc_teams_mm`.
   Open `tfc_rtv_mm.sln` and build **Release | Win32**. The post-build step copies
   `tfc_rtv_mm_mm.dll` to `tfc\addons\tfc_rtv_mm\`. It fails if the game is running.
2. `configs\rtv.ini` goes in `tfc\addons\tfc_rtv_mm\configs\`.
3. In `tfc\addons\metamod\plugins.ini`:
   `win32 addons\tfc_rtv_mm\tfc_rtv_mm_mm.dll`
   `linux addons/tfc_rtv_mm/tfc_rtv_mm_mm_i386.so`
4. Turn off the AMX Mod X RTV: put a `;` in front of `rtv.amxx` in
   `addons\amxmodx\configs\plugins.ini`. Otherwise both answer `rtv` and both change the map.
   `mapchooser.amxx` and `nextmap.amxx` should stay off too.
5. Restart the server. `meta list` should show **TFC RTV (rock the vote + map vote)** as RUN.

### Linux server

1. Copy `tfc/addons/tfc_rtv_mm/` to the server: `tfc_rtv_mm_mm_i386.so`, `configs/rtv.ini` and
   `docs/`.
2. Add the `linux` line above to the server's `plugins.ini`. Linux paths are case-sensitive.

The `.so` needs nothing newer than glibc 2.2.4 and no C++ runtime. To build it yourself, install a
32-bit g++ (`apt install g++-multilib`) and run `make -f Makefile.linux` in this folder.

## Player chat

With or without a `!` or `/` in front:

| Say | What it does |
|---|---|
| `rtv` (`rockthevote`) | Rock the vote. It passes at `rtv_percent` of the players here. |
| `unrtv` | Take your rtv back |
| `nominate <map>` (`nom`) | Nominate a map. Part of the name works if it matches only one map. |
| just a map's name, e.g. `casbah` | Nominate it (`nominate_typed 1`). This only works when the whole message is exactly a map on the list. |
| `nominate`, `nominate list`, `maps` | A window listing the maps you can nominate, their nominations, and the recently played maps |
| `nextmap` | The next map, and how it was chosen |
| `revote` | Reopen the vote menu during a vote, to vote or change your vote |
| `!votekeys` | Sign up for **vote keys**: vote with keys you bind yourself (`vote1`–`vote9`, `vote0`), one press, and your number keys never get taken over. See below. |
| `!votemenu` (`!votekeys off`, `!novotekeys`) | Opt out of vote keys and go back to the number menu |
| `!rtvmenu` | Your vote settings in one menu (see below). The console command `rtvmenu` does the same, so it can go on a key. |
| `!rtvstatus` | RTV count, players here and time left |
| `!forcertv` | Admin only. Start the map vote now. |
| `!cancelvote` | Admin only. Cancel the countdown, the vote, or a pending map change. |

**The vote menu.** Press a number **twice** to vote. The first press marks the map ("press 3
again"), and the second press casts the vote. While the menu is up, the number keys pick from it
instead of switching weapons, so a single press made to change weapons can't cast a vote.

- **0** closes the menu without voting. `revote` opens it again.
- After you vote, the menu turns into the **results list** in the same spot: the counts and your
  vote marked. It's read-only, so a number key pressed on it is passed on as the weapon key it was
  (see the vote keys below). **0** hides it, and `!revote` brings back the menu to change your vote.
- If another menu replaces ours, such as the class menu or an AMX Mod X menu, ours comes back
  within 3 seconds for anyone who hasn't voted.
- The vote ends early once everyone has voted or closed the menu.

**Vote keys (opt-in).** Everyone starts with the number menu above. A player who says
`!votekeys` (or picks it in `!rtvmenu`) signs up instead. Like all of a player's vote settings, the
choice is remembered by SteamID in `rtv_prefs.txt` next to the DLL.

- **During a vote:** the vote shows as a read-only list in the game's menu, in the player's menu
  colours: the maps numbered 1–9, "0. No vote", the counts, and their own vote marked. While a menu
  is up the game sends every number key to it, so the plugin passes each number press back to the
  player as the `slotN` command it was. Their weapon switches, the menu steps aside, and it comes
  back 1.5 seconds later. This assumes the number keys are bound to `slot1`…`slot10` (the default).
- **Voting:** keys they bind themselves, one press each: `bind <key> vote1` … `vote9`, and
  `vote0` for no vote. `!votekeys` opens a window with the lines to copy. The number pad fits well:
  `KP_END` vote1, `KP_DOWNARROW` vote2, `KP_PGDN` vote3, `KP_LEFTARROW` vote4, `KP_5` vote5,
  `KP_RIGHTARROW` vote6, `KP_HOME` vote7, `KP_UPARROW` vote8, `KP_PGUP` vote9, `KP_INS` vote0.
  The older `rtv_vote <n>` still works.
- **Changing a vote:** press another key.
- **Number menu:** `!revote` opens the number menu for that one vote.
- **Opting out:** `!votemenu` (or `!votekeys off`) goes back to the number menu for good.
- **Why it's opt-in:** the server never binds keys itself, so a key only votes for a player who
  bound it.

**Your vote settings (`!rtvmenu`).** A number menu with everything a player can change for
themselves. One press picks an option, since opening it was on purpose.

| Option | What it does |
|---|---|
| 1. Vote with | The number menu, or your own vote keys. Turning vote keys on shows the bind window. |
| 2. Press twice to vote | On or off for you, in the number menu. The default comes from `vote_confirm`. |
| 3. Menu colours | Title, numbers, map names, vote counts, your vote, and hints (the grey help lines). Each press cycles that part through white, yellow, red and grey. Option 7 there previews it. Defaults: yellow, red, white, yellow, yellow, grey. |
| 4. Show the vote key binds | The bind window, with the lines to copy. |
| 5. Preview | A sample vote menu in your colours. Any key goes back. |
| 6. Reset | Back to the server's settings. |

- **Only four colours:** white, yellow, red and grey are the only colours the game's menu can draw.
  TFC's own menu code (`CHudMenu::Draw` in `client.dll`) knows `\w \y \r \d` and nothing else, so no
  server plugin can add more.
- **Where the colours apply:** the number menu, the results list, and the vote-key list.
- **During a vote:** the settings menu replaces the vote menu. Closing it (0) brings the vote menu
  back.
- **Saved settings:** everything is kept in `rtv_prefs.txt`. The first time it runs, the plugin
  takes over `rtv_votekeys.txt` from earlier versions.

A vote has up to 9 maps (`vote_maps 9`), because 1–9 are the map keys and 0 is "no vote".

## Chat tips

So players know the commands exist, the plugin puts tips in chat:

- **Every `advert_interval` seconds** (default 300, the first at `advert_first` = 120 s into the
  map), a line to everyone. It takes turns between "say rtv / nominate <map> or just type the
  map's name / !nominate list" and "next map is X / nominate / !votekeys". No tip goes out while a
  vote is running, or when no humans are on.
- **When someone joins** (`advert_join` = 25 s after they connect), two lines just to them on how
  to rtv and nominate. It's once per player per server session, not again at every map change.
- **Your own tips:** `advert "text"` lines in `rtv.ini` (up to 8) replace the built-in ones and
  are used in turn. Keep each under about 170 characters (longer is cut off). Text in quotes can
  contain `//`, so web addresses work.
- `advert_interval 0` or `advert_join 0` turns that part off.

The TFC Teams plugin has the same settings for `!scramble` / `!stats`. Its first tip comes at 270 s,
so the two plugins' tips alternate rather than arriving together.

## Server console / rcon

`rtv_force`, `rtv_cancel`, `rtv_status` (every player's AFK / RTV / nomination state),
`rtv_reload` (reloads the config and map list) and `rtv_nextmap [map]` (shows or sets the next
map).

## How it works

**Rock the vote.** It passes when `ceil(players here × rtv_percent / 100)` of them have said
`rtv`, with at least `rtv_min_players` present.

- **Players here:** humans (and bots with `rtv_include_bots 1`), spectators included, minus
  anyone AFK.
- **AFK:** no keys pressed and no mouse movement for `afk_time` seconds.
- **Going AFK:** your rtv is taken away and the total shrinks.
- **Coming back:** you're told you can say `rtv` again. Typing in chat also counts as being back.
- **Leaving:** the total shrinks. Every change of the count or the total checks the RTV again, so
  someone leaving can be what makes it pass.
- **Timing:** no RTV in the first `rtv_mapstart_delay` seconds of a map.
- **Admins:** with `rtv_admin_instant 1`, an admin's `rtv` starts the vote at once. On a listen
  server, the host is an admin.

**The map vote.** The steps:

1. A `vote_countdown` countdown, shown on screen with the voice counting down.
2. A `vote_duration` vote between `vote_maps` maps. Nominated maps come first, the most-nominated
   at the top, then random maps from the list fill the rest.
3. If the top two are within `runoff_percent` of the votes cast, a `runoff_duration` runoff
   between just those two.
4. The winner is the next map. A tie is broken at random.

**Nominations.** Each player has one nomination; nominating another map moves it.

- **Backing a map:** nominating a map someone already nominated tells you who nominated it first,
  and your nomination is added to it.
- **Before a vote:** a player's nomination leaves with them. If someone else also nominated that
  map, it stays nominated. Either way, everyone is told ("Alice left - casbah stays nominated").
- **Once a vote starts** (at the start of the countdown), the maps in it are fixed and
  everyone is told which nominations made it in. Nobody can nominate, and players leaving don't
  change anything. `!nominate list` then shows the maps in the vote.

What happens next:

- **After an RTV vote:** the map changes `change_delay` seconds later. If nobody voted, the map
  stays.
- **End-of-map vote:** the winner is used when the map ends. If nobody voted, the next map in the
  cycle is used.
- **RTV after the end-of-map vote:** if an RTV passes once the end-of-map vote has already chosen
  the next map, the server goes there right away, with no second vote.

**Recently played.** `rtv_history.txt` next to the DLL keeps the maps played, newest first. The
last `map_history` of them (the current map included, 0–50, default 3) can't be nominated or voted
for. The first time the plugin runs, it takes over the AMX Mod X RTV's
`addons/amxmodx/data/rtv_history.txt`, so the ban carries on across the switch.

**Immune maps.** `map_immune 2fort well` in `rtv.ini` lists maps that are never banned:

- They can always be nominated and voted for, even right after they were played.
- They don't use up a ban slot. With `map_history 3`, the 3 most recent *other* maps are banned,
  so a favourite that comes round often doesn't let other maps back early.
- Separate maps with spaces or commas. `*` matches anything (`cz*`), and the line can repeat.
- `!nominate list` shows them under "Always open". The trace log lists them when the config
  loads.

**The map list** is the server's `mapcyclefile`, or `maps_file` if one is set. Maps that aren't on
the server are left out, with a line in the trace.

**End of the map.** This uses `mp_timelimit`. TFC ends the map when the game time reaches
`mp_timelimit × 60` (`CHalfLifeMultiplay::Think`, tfc.so 0x8ccb0).

- **Warning:** at `endmap_warn_time` seconds left, the plugin tells everyone to nominate, in chat
  and on screen.
- **Vote:** at `endmap_vote_time` seconds left, the vote runs.
- **The change:** TFC then plays out the map as normal: intermission, the scoreboard, and other
  plugins' end-of-map screens. At the end it calls the engine's ChangeLevel with the next map from
  its cycle (`CHalfLifeMultiplay::ChangeLevel`, 0x8dcc0). This plugin hooks that call and sends
  the voted map instead.
- **`control_nextmap 1`:** with no vote, it still sends the next map of the list the plugin shows
  for `nextmap`, so what `nextmap` says is always where the server goes.
- **AMX Mod X:** `amx_nextmap` is kept up to date, so its `timeleft` and `nextmap` agree.

## Trace log

`rtv_trace.log` next to the DLL (`debug 1`) records every RTV, AFK change, nomination, vote
(options, each vote, the tally), and map change, including which map the game asked for and which
one was sent.

## Licence

GPL-3.0. See `LICENSE`.
