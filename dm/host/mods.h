// Mods: folders of replacement game files (romfs/) and code patches (exefs/), in the layout emulators and Atmosphère use (hle_mods.c).
// Plain C with no host types, so the iOS launcher (Launcher.swift, through DmIOS-Bridging-Header.h) lists the same mods the game then loads.
#pragma once

enum {
    DM_MOD_FILES,        // replacement game files only: on unless DM_MODS_OFF names it
    DM_MOD_BUILT_IN,     // its code patches were applied when the app was built (dm_code.tsv beside its romfs/): always on
    DM_MOD_NEEDS_BUILD,  // has code patches (exefs/) this build was not made with: skipped whole, as half a mod can break the game
    DM_MOD_EMPTY,        // neither romfs/ nor exefs/ holds a file
};

int dm_mods_scan(void);                // (re)reads DM_MODS (else <DM_ROOT>/mods); returns how many mods were found
const char *dm_mod_name(int i);        // the folder's name, which DM_MODS_OFF uses
const char *dm_mod_dir(int i);         // the folder that holds romfs/ and exefs/
int dm_mod_kind(int i);                // DM_MOD_*
int dm_mod_on(int i);                  // used by the game: a files mod not named by DM_MODS_OFF, or a built-in one
const char *dm_mods_overlay(char *buf, unsigned n);   // DM_ROMFS_OVERLAY then the romfs/ folders of the mods that are on, ':'-separated (fresh scan; logs the list)
