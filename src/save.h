/*
 * save.h - saved games.
 *
 * One file per slot in the save folder (slot 0 = quicksave):
 *
 *   DSaveHeader                magic, version, total size, CRC-32 of
 *                              everything after the header, a fingerprint
 *                              of the cooked rpg.dpak, and a summary for
 *                              the slot list (name, level, cell, game date,
 *                              play time, real time)
 *   sections                   {tag, size} + payload:
 *     VMGL globals  VMTI time, randomness, player name/race/class
 *     VMIN script instances + locals pool
 *     VMPR used stat-store slots (slot index kept: the table is open
 *          addressing, lookups resolve to the same slots)
 *     VMIT used inventory slots (index kept: equipment refers to stacks)
 *     VMJR quest state, journal entries, known topics, heard infos,
 *          faction ranks / expulsions / reputation
 *     RPGS mechanics state (effects, equipment, player-made objects,
 *          character, mark, explored cells)
 *     GAME zone, player body and view, controls, menus
 *     AIST stolen goods, spawn serial, every actor of the current zone
 *          (position, state, package, spawned creatures)
 *
 * Files are written to <slot>.tmp and renamed over the old one, so a power
 * loss never leaves a half-written save. Loading validates the header, the
 * CRC, the database fingerprint and every count and index before touching
 * the game, so a damaged file is rejected without side effects.
 */
#ifndef PSPWIND_SAVE_H
#define PSPWIND_SAVE_H

#include "common.h"
#include "game.h"

#define SAVE_SLOTS 8
#define SAVE_MAGIC 0x56415344u /* "DSAV" */
#define SAVE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t size;          /* whole file */
    uint32_t crc;           /* CRC-32 of bytes [sizeof(DSaveHeader), size) */
    uint32_t db_size;       /* rpg/core fingerprint: file size */
    uint32_t db_objects;    /*   object and info counts */
    uint32_t db_infos;
    uint32_t struct_sizes;  /* sizeof(Rpg) ^ sizeof(AiActor) << 16: build check */
    /* summary */
    char name[32];
    char cell[48];
    int16_t level;
    uint16_t month;
    uint16_t day;
    uint16_t pad;
    float hour;
    float playtime;         /* seconds */
    int64_t real_time;      /* seconds since 1970 (time()) */
    uint32_t reserved[4];
} DSaveHeader;

typedef struct {
    int used;
    DSaveHeader hdr;
} SaveSlotInfo;

/* Writes the current game to slot (0..SAVE_SLOTS-1). Refused while the
 * player is dead or a message box / dialogue is open. 0 on success. */
int save_game(Game *g, const char *dir, int slot, float playtime, char *err, uint32_t errlen);
/* Restores a slot: enters its zone and places everything. 0 on success;
 * on failure nothing of the running game has changed. */
int load_game(Game *g, const char *dir, int slot, float *playtime, char *err, uint32_t errlen);
/* Header of a slot (used = 0 when missing or unreadable). */
void save_slot_info(const char *dir, int slot, SaveSlotInfo *out);
/* Slot of the newest save or -1. */
int save_latest(const char *dir);
uint32_t save_crc32(uint32_t crc, const void *data, uint32_t size);

#endif /* PSPWIND_SAVE_H */
