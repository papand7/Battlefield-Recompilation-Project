# What the bots know of each map

One file per map (`<map>.txt`), merged from what servers learned in their
matches (`tools/bot_knowledge.py merge`): on 8 m cells, per team, where players
went, where they died and where they killed from. The build copies this folder
next to the exe (`bot_knowledge/`); every server starts from it and adds what
it learns in `<user>/bot_knowledge/`. Positions on the maps only, no game data.

To put what servers learned into the next version:

    python tools/bot_knowledge.py merge --clear <user folder>/bot_knowledge [more servers' folders]
    python tools/bot_knowledge.py info data/bot_knowledge
