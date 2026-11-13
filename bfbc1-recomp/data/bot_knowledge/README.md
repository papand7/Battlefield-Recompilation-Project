# What Bad Company's bots know of each map

As bf1943's `data/bot_knowledge`: one file per map (`<map>.txt`: on 8 m cells,
per team, where players went, where they died and where they killed from) and
the bots' tuned behaviour (`<map>.tune`, from training servers' self-play).
The build copies this folder next to the exe (`bot_knowledge/`); every server
starts from it and adds what it learns in `<user>/bot_knowledge/`. Positions on
the maps and behaviour settings only, no game data.

To put what servers learned into the next version:

    python ../bf1943-recomp/tools/bot_knowledge.py merge --clear --to data/bot_knowledge <user folder>/bot_knowledge
    python ../bf1943-recomp/tools/bot_knowledge.py info data/bot_knowledge
