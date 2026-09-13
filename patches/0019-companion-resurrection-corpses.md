# Keep dead PCs addressable for resurrection

`ZC_NOTIFY_VANISH` with `VT_DEAD` does not remove a player character from the
server. The character remains on the map as a corpse, and resurrection later
refers to the same GID.

roBrowserLegacy removed that GID from `EntityManager` as soon as the death
packet arrived, while deliberately retaining the corpse in the render list.
`ZC_RESURRECTION` could no longer find the actor. When the revived character
subsequently moved or spawned, the client created another actor and left the
old, sometimes clickable corpse behind.

The client patch retains the lookup for dead PCs until a real exit, teleport,
or out-of-sight packet removes it. Non-PC entities keep the original immediate
cleanup because their GIDs may be reused while their death animation finishes.

This fixes Population Engine companions and also makes the client follow the
normal rAthena lifecycle for every resurrectable player character.
