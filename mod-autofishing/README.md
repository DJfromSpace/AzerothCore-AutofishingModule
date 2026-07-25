# mod-autofishing

Minimal standalone autofishing module for WoW servers running AzerothCore.

This module does not require `mod-playerbots` to work. It only handles the
controlled player character’s fishing loop.

`mod-playerbots` is optional and will remain optional. It is only relevant if
you also want companion bot behavior in your own setup.

Features:

- Per-character toggle via `.autofish on|off|status`
- Manual start, then repeated fishing loop
- Opens and loots the bobber when ready
- Stops on movement, combat, death, logout, or missing fishing pole
- Configurable allowed GUID list
- Configurable delay between catches

Works best with:

- Real players
- Playerbots, if you already use `mod-playerbots`

For playerbot-heavy setups, autofishing may already be part of your broader bot
workflow. For real players, this module works as long as the character’s GUID is
allowed in config.

This repository is only the standalone module package. To use it, copy the
`mod-autofishing` folder into an AzerothCore server’s `modules/` directory.

Install:

1. Copy the `mod-autofishing` folder into your AzerothCore `modules/` directory.
2. Re-run CMake so the module is included in your build.
3. Rebuild `worldserver` after adding the module to `modules/`.
4. Build the server.
5. Copy `conf/mod_autofishing.conf.dist` to your active module config location and
   rename it to `mod_autofishing.conf`.
6. Set `AutoFishing.Enabled = 1` and, if desired, restrict it with
   `AutoFishing.AllowedGUIDs`.
7. In game, use `.pinfo` on the character you want to allow. The worldserver
   chat output shows the character’s GUID.
8. Add that GUID to `AutoFishing.AllowedGUIDs` in `mod_autofishing.conf`, using
   commas if you want to allow multiple characters.
9. Restart worldserver so the updated allowlist is loaded.
10. Start worldserver and use `.autofish on`.

Notes:

- The module starts only after you manually cast Fishing once.
- Debug logging is controlled by `AutoFishing.Debug`.
- If you want to allow everyone, leave `AutoFishing.AllowedGUIDs` empty.

Future direction:

- Better compatibility with Playerbots
- Cleaner permission handling for real players and companion bots
- A friendlier allow/deny system for larger servers
