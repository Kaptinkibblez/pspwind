/*
 * fmt.h - binary asset formats produced by tools/demake_cooker and consumed
 * by the PSP runtime. This header is the single source of truth: the Python
 * writers (dmcommon.py, tex_demaker.py, dmesh.py) mirror these layouts byte
 * for byte, and tools/demake_cooker/tests check the sizes against it.
 *
 * Conventions for every format:
 *   - little endian (Allegrex is little endian, files are mmap-style loaded)
 *   - all offsets are byte offsets from the start of the file / pack entry
 *   - every data block is 16-byte aligned so it can be handed to the GE or
 *     loaded into VFPU registers (lv.q) without copying
 *   - asset names are stored as pw_hash_name() hashes plus an optional
 *     string for debugging
 *
 * See docs/FORMATS.md for prose descriptions.
 */
#ifndef PSPWIND_FMT_H
#define PSPWIND_FMT_H

#include "common.h"

#define FMT_FOURCC(a, b, c, d)                                                \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) |                 \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

/* ------------------------------------------------------------------------ */
/* DPAK - generic asset pack (textures.dpak, meshes.dpak, ...)              */
/* ------------------------------------------------------------------------ */
#define DPAK_MAGIC FMT_FOURCC('D', 'P', 'A', 'K')
#define DPAK_VERSION 1
#define DPAK_ENTRY_ALIGN 64 /* entries start on 64-byte boundaries */

enum {
    DPAK_KIND_GENERIC = 0,
    DPAK_KIND_TEXTURE = 1,
    DPAK_KIND_MESH = 2
};

typedef struct {
    uint32_t magic;       /* DPAK_MAGIC */
    uint16_t version;     /* DPAK_VERSION */
    uint16_t kind;        /* DPAK_KIND_* */
    uint32_t entry_count;
    uint32_t dir_ofs;     /* DPakEntry[entry_count], sorted by hash */
    uint32_t names_ofs;   /* NUL-terminated names, DPakEntry.name_ofs relative */
    uint32_t names_size;
    uint32_t reserved[2];
} DPakHeader;

typedef struct {
    uint32_t hash;     /* pw_hash_name(name) */
    uint32_t ofs;      /* from start of pack file */
    uint32_t size;
    uint32_t name_ofs; /* relative to DPakHeader.names_ofs */
} DPakEntry;

/* ------------------------------------------------------------------------ */
/* DTEX - paletted, optionally swizzled PSP texture                         */
/* ------------------------------------------------------------------------ */
#define DTEX_MAGIC FMT_FOURCC('D', 'T', 'E', 'X')
#define DTEX_VERSION 1
#define DTEX_MAX_LEVELS 4

/* Pixel storage modes, numerically equal to GU_PSM_T4 / GU_PSM_T8. */
enum { DTEX_PSM_T4 = 4, DTEX_PSM_T8 = 5 };

enum {
    DTEX_F_SWIZZLED = 1 << 0,    /* levels stored in GE swizzle order */
    DTEX_F_ALPHA_TEST = 1 << 1,  /* palette index 0 is transparent cutout */
    DTEX_F_ALPHA_BLEND = 1 << 2, /* palette carries graded alpha */
    DTEX_F_ATLAS_PAGE = 1 << 3   /* page of a DATL atlas */
};

typedef struct {
    uint16_t width;      /* pixels */
    uint16_t height;     /* pixels */
    uint16_t buf_width;  /* GE texture buffer width in pixels */
    uint16_t reserved;
    uint32_t ofs;        /* from start of DTEX */
    uint32_t size;       /* bytes */
} DTexLevel;

typedef struct {
    uint32_t magic;        /* DTEX_MAGIC */
    uint16_t version;      /* DTEX_VERSION */
    uint8_t psm;           /* DTEX_PSM_T4 / DTEX_PSM_T8 */
    uint8_t flags;         /* DTEX_F_* */
    uint16_t width;        /* level 0 */
    uint16_t height;
    uint8_t level_count;   /* 1..DTEX_MAX_LEVELS */
    uint8_t alpha_ref;     /* alpha-test threshold (0..255) */
    uint16_t clut_entries; /* 16 or 256 */
    uint32_t clut_ofs;     /* ABGR8888 entries (GU_PSM_8888 CLUT) */
    uint32_t file_size;
    DTexLevel level[DTEX_MAX_LEVELS];
    uint32_t name_hash;
    uint32_t reserved;
} DTexHeader;

/* ------------------------------------------------------------------------ */
/* DATL - UI texture atlas index (pages are separate DTEX pack entries      */
/* named "atlas/<atlas>/<page>")                                            */
/* ------------------------------------------------------------------------ */
#define DATL_MAGIC FMT_FOURCC('D', 'A', 'T', 'L')
#define DATL_VERSION 1

typedef struct {
    uint32_t magic;       /* DATL_MAGIC */
    uint16_t version;
    uint16_t page_count;
    uint32_t entry_count;
    uint16_t page_width;
    uint16_t page_height;
    uint32_t pages_ofs;   /* uint32_t page_hash[page_count] */
    uint32_t entries_ofs; /* DAtlasEntry[entry_count], sorted by hash */
    uint32_t reserved[2];
} DAtlasHeader;

typedef struct {
    uint32_t hash; /* pw_hash_name(icon name) */
    uint16_t page;
    uint16_t x, y, w, h;
    uint16_t reserved;
} DAtlasEntry;

/* ------------------------------------------------------------------------ */
/* DMESH - demade model: node hierarchy + quantised GE vertex batches       */
/* ------------------------------------------------------------------------ */
/*
 * Spaces: "model space" is the NIF file's space - node 0's local transform
 * already applied. Header bounds and collision are in model space; batch
 * vertices are local to their node (world = REFR placement * node world).
 */
#define DMESH_MAGIC FMT_FOURCC('D', 'M', 'S', 'H')
#define DMESH_VERSION 1
#define DMESH_NO_TEXTURE 0xFFFFu
#define DMESH_NO_PARENT (-1)

enum {
    DMESH_F_SKELETAL = 1 << 0,  /* segmented rigid-limb character */
    DMESH_F_COLLISION = 1 << 1, /* collision block present */
    DMESH_F_ANIMATED = 1 << 2,  /* has nodes driven by controllers */
    DMESH_F_PARTICLES = 1 << 3  /* has particle emitters */
};

enum {
    DMESH_NODE_F_BONE = 1 << 0,      /* skeleton bone (skin influence) */
    DMESH_NODE_F_BILLBOARD = 1 << 1, /* faces the camera */
    DMESH_NODE_F_ANIMATED = 1 << 2,  /* has keyframe controller in source */
    DMESH_NODE_F_HIDDEN = 1 << 3,    /* hidden in source (attach points) */
    DMESH_NODE_F_ATTACH = 1 << 4,    /* attach point (lights, weapons) */
    DMESH_NODE_F_EMITTER = 1 << 5    /* particle emitter origin */
};

enum {
    DMESH_BATCH_F_ALPHA_BLEND = 1 << 0,
    DMESH_BATCH_F_ALPHA_TEST = 1 << 1,
    DMESH_BATCH_F_DOUBLE_SIDED = 1 << 2,
    DMESH_BATCH_F_VERTEX_ALPHA = 1 << 3,
    DMESH_BATCH_F_EMISSIVE = 1 << 4,
    DMESH_BATCH_F_ADDITIVE = 1 << 5,
    DMESH_BATCH_F_CLAMP_U = 1 << 6,
    DMESH_BATCH_F_CLAMP_V = 1 << 7,
    DMESH_BATCH_F_SEGMENT = 1 << 8   /* rigid limb segment of a skin */
};

enum { DMESH_COLL_F_INDEX32 = 1 << 0 };

typedef struct {
    uint32_t magic;   /* DMESH_MAGIC */
    uint16_t version; /* DMESH_VERSION */
    uint16_t flags;   /* DMESH_F_* */
    uint32_t file_size;
    uint16_t node_count;
    uint16_t batch_count;
    uint16_t texture_count;
    uint16_t emitter_count;
    uint32_t nodes_ofs;     /* DMeshNode[node_count], parents precede children */
    uint32_t batches_ofs;   /* DMeshBatch[batch_count], sorted opaque first */
    uint32_t textures_ofs;  /* DMeshTexture[texture_count] */
    uint32_t emitters_ofs;  /* DMeshEmitter[emitter_count] */
    uint32_t collision_ofs; /* DMeshCollision or 0 */
    uint32_t strings_ofs;   /* NUL-terminated strings */
    uint32_t total_vertices;
    uint32_t total_triangles;
    float bound_center[3];
    float bound_radius;
    float bbox_min[3];
    float bbox_max[3];
    uint32_t reserved;
} DMeshHeader;

/* Local bind transform relative to parent: p' = R(rot) * (scale * p) + pos */
typedef struct {
    int16_t parent;    /* DMESH_NO_PARENT for the root */
    uint16_t flags;    /* DMESH_NODE_F_* */
    uint32_t name_hash;
    uint32_t name_ofs; /* relative to strings_ofs */
    float scale;
    float rot[4];      /* quaternion x, y, z, w */
    float pos[3];
    uint32_t reserved;
} DMeshNode;

/*
 * Vertex layout handed straight to sceGuDrawArray with DMESH_VERTEX_FORMAT:
 *   GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_NORMAL_8BIT | GU_VERTEX_16BIT
 * Decoding (done by the GE + per-batch texture/model scale):
 *   uv  = (u16 / 32768) * uv_scale + uv_offset   (sceGuTexScale/TexOffset)
 *   pos = (s16 / 32768) * pos_scale + pos_offset (folded into model matrix)
 * Cooked u/v never exceed 32768 so decoding is valid for [0,1] or [0,2).
 * Triangles are counter-clockwise front facing.
 */
typedef struct {
    uint16_t u, v;
    uint32_t color; /* ABGR8888 (R in low byte) */
    int8_t nx, ny, nz, pad0;
    int16_t x, y, z, pad1;
} DMeshVertex;

#define DMESH_VERTEX_GU_FORMAT_BITS                                           \
    ((1u << 1) /* TEXTURE_16BIT */ | (7u << 2) /* COLOR_8888 */ |             \
     (1u << 5) /* NORMAL_8BIT */ | (2u << 7) /* VERTEX_16BIT */)

typedef struct {
    uint16_t node;      /* index into nodes; vertices are node-local */
    uint16_t texture;   /* index into textures or DMESH_NO_TEXTURE */
    uint16_t flags;     /* DMESH_BATCH_F_* */
    uint8_t alpha_ref;  /* alpha test threshold */
    uint8_t blend;      /* NiAlphaProperty src (low nibble) / dst (high) */
    uint32_t vertex_count;
    uint32_t index_count; /* uint16_t indices, triangle list */
    uint32_t vertex_ofs;
    uint32_t index_ofs;
    float pos_offset[3];
    float pos_scale[3];
    float uv_offset[2];
    float uv_scale[2];
    uint32_t diffuse;   /* ABGR8888 material colour (already baked into verts) */
    uint32_t emissive;  /* ABGR8888 */
    float bound[4];     /* node-local bounding sphere xyz + radius */
} DMeshBatch;

typedef struct {
    uint32_t hash;     /* pw_hash_name(texture key) - DPAK texture entry */
    uint32_t name_ofs; /* relative to strings_ofs */
} DMeshTexture;

typedef struct {
    uint16_t node;          /* emitter origin node */
    uint16_t texture;       /* DMESH_NO_TEXTURE if untextured */
    uint16_t max_particles;
    uint16_t flags;         /* DMESH_BATCH_F_* blend flags for the sprites */
    uint32_t color;         /* ABGR8888 start colour */
    float size;
    float emit_rate;        /* particles per second */
    float lifetime;
    float lifetime_random;
    float speed;
    float speed_random;
    float vertical_dir;
    float vertical_angle;
    float horizontal_dir;
    float horizontal_angle;
    float gravity[3];       /* node-local acceleration, units/s^2 */
    float grow_time;
    float fade_time;
} DMeshEmitter;

typedef struct {
    uint32_t vertex_count;
    uint32_t tri_count;
    uint32_t vertex_ofs; /* float xyz[vertex_count], model space */
    uint32_t index_ofs;  /* uint16_t (or uint32_t with INDEX32) [tri_count*3] */
    uint16_t flags;      /* DMESH_COLL_F_* */
    uint16_t reserved0;
    float bbox_min[3];
    float bbox_max[3];
    uint32_t reserved1;
} DMeshCollision;

/* ------------------------------------------------------------------------ */
/* DZONE - one loadable zone: a block of exterior cells (plus a fringe ring */
/* of terrain-only neighbours) or one interior cell. zones.dpak entries are */
/* named "zone/ext_<zx>_<zy>" and "zone/int/<lower-case cell name>".        */
/* All positions are world space (Morrowind units, +Z up).                  */
/* ------------------------------------------------------------------------ */
#define DZONE_MAGIC FMT_FOURCC('D', 'Z', 'O', 'N')
#define DZONE_VERSION 2
#define DZONE_TERRAIN_GRID 17   /* vertices per patch side */
#define DZONE_NONE 0xFFFFu
#define DZONE_NO_ZONE 0xFFFFFFFFu

enum { DZONE_KIND_EXTERIOR = 0, DZONE_KIND_INTERIOR = 1 };

enum {
    DZONE_F_WATER = 1 << 0,   /* draw a water plane at water_height */
    DZONE_F_TERRAIN = 1 << 1, /* has terrain patches */
    DZONE_F_QUASI_EXTERIOR = 1 << 2
};

enum {
    DZONE_CELL_F_FRINGE = 1 << 0, /* neighbour shown as terrain only */
    DZONE_CELL_F_WATER = 1 << 1
};

/* Instance kinds (what the placed record is). */
enum {
    DZONE_INST_STATIC = 0,
    DZONE_INST_ACTIVATOR,
    DZONE_INST_DOOR,
    DZONE_INST_CONTAINER,
    DZONE_INST_LIGHT,
    DZONE_INST_ITEM,
    DZONE_INST_NPC,
    DZONE_INST_CREATURE,
    DZONE_INST_LEVELED,
    DZONE_INST_OTHER
};

enum {
    DZONE_IF_MERGED = 1 << 0,     /* geometry baked into cell batches; collision only */
    DZONE_IF_PERSISTENT = 1 << 1, /* persistent reference */
    DZONE_IF_NO_MESH = 1 << 2,    /* model missing */
    DZONE_IF_ACTOR = 1 << 3       /* drawn through its DZoneActor record */
};

#define DZONE_ACTOR_MAX_PARTS 16

enum {
    DZONE_ACTOR_F_FEMALE = 1 << 0,
    DZONE_ACTOR_F_BEAST = 1 << 1,
    DZONE_ACTOR_F_CREATURE = 1 << 2
};

/*
 * Animated actor (NPC or creature). The skeleton mesh supplies the bone
 * hierarchy (and, for creatures, the segmented geometry). NPC body parts are
 * separate meshes: their rigid-limb segments bind to skeleton bones by node
 * name; unskinned parts attach to the bone named by part_bone[i]
 * (pw_hash_name of e.g. "Head", "Right Hand"). part_flags bit 0 mirrors a
 * left-side part across the bone's X axis.
 */
typedef struct {
    uint32_t instance;
    uint16_t skeleton;   /* zone mesh index */
    uint16_t anim;       /* zone anim index or DZONE_NONE */
    uint16_t flags;      /* DZONE_ACTOR_F_* */
    uint16_t part_count;
    uint16_t parts[DZONE_ACTOR_MAX_PARTS]; /* zone mesh indices */
    uint32_t part_bone_ofs; /* uint32_t bone hash per part */
    uint32_t part_flags;    /* bit i: part i mirrored (left side) */
    uint32_t reserved[3];
} DZoneActor;

typedef struct {
    uint32_t magic; /* DZONE_MAGIC */
    uint16_t version;
    uint8_t kind;   /* DZONE_KIND_* */
    uint8_t flags;  /* DZONE_F_* */
    uint32_t file_size;
    uint32_t name_hash; /* hash of the pack entry name */
    uint32_t name_ofs;  /* display name, relative to strings_ofs */
    int16_t cell_x0, cell_y0, cell_x1, cell_y1; /* core exterior cells, inclusive */
    float water_height;
    uint32_t fog_color; /* ABGR8888 */
    float fog_near;
    float fog_far;
    uint32_t ambient;   /* ABGR8888 */
    uint32_t sun_color; /* ABGR8888 */
    float sun_dir[3];   /* direction light travels (normalised) */
    uint16_t cell_count;
    uint16_t mesh_count;
    uint16_t texture_count;
    uint16_t terrain_count;
    uint32_t batch_count;
    uint32_t instance_count;
    uint32_t door_count;
    uint32_t light_count;
    uint32_t cells_ofs;     /* DZoneCell[] */
    uint32_t meshes_ofs;    /* uint32_t mesh hash[] (meshes.dpak) */
    uint32_t textures_ofs;  /* DMeshTexture[] (textures.dpak) */
    uint32_t terrain_ofs;   /* DZoneTerrain[] */
    uint32_t batches_ofs;   /* DMeshBatch[]: node = cell slot, texture = zone texture */
    uint32_t instances_ofs; /* DZoneInstance[], grouped by cell */
    uint32_t doors_ofs;     /* DZoneDoor[] */
    uint32_t lights_ofs;    /* DZoneLight[] */
    uint32_t strings_ofs;
    float bbox_min[3];
    float bbox_max[3];
    uint16_t actor_count;
    uint16_t anim_count;
    uint32_t actors_ofs; /* DZoneActor[] */
    uint32_t anims_ofs;  /* uint32_t anim hash[] (anims.dpak) */
} DZoneHeader;

typedef struct {
    int16_t x, y;       /* exterior grid (0,0 for interiors) */
    uint16_t flags;     /* DZONE_CELL_F_* */
    uint16_t terrain;   /* index into terrain or DZONE_NONE */
    uint32_t name_hash;
    uint32_t name_ofs;
    uint32_t region_hash;
    uint32_t first_batch, batch_count;
    uint32_t first_instance, instance_count;
    float water_height;
    uint32_t reserved[2];
} DZoneCell;

/*
 * Terrain patch. The static batches render it at 512-unit quads; near the
 * camera the runtime re-tessellates quads from these grids instead (the GE
 * drops triangles whose clipped projection leaves its guard band).
 */
typedef struct {
    int16_t x, y;
    uint16_t flags;
    uint16_t reserved0;
    float origin[2];   /* world x,y of vertex (0,0) */
    float min_h, max_h;
    uint32_t heights_ofs;  /* float[17*17], row-major, rows run north (+Y) */
    uint32_t colors_ofs;   /* uint32_t[17*17] baked ABGR8888 vertex colours */
    uint32_t quad_tex_ofs; /* uint16_t[16*16] zone texture index per quad */
    uint32_t first_batch;  /* static terrain batches of this patch (zone batch ids) */
    uint32_t batch_count;
    uint32_t reserved1;
} DZoneTerrain;

typedef struct {
    uint16_t mesh;   /* zone mesh index or DZONE_NONE */
    uint8_t kind;    /* DZONE_INST_* */
    uint8_t flags;   /* DZONE_IF_* */
    uint16_t cell;   /* zone cell slot */
    uint16_t door;   /* index into doors or DZONE_NONE */
    uint32_t object_hash; /* pw_hash_name(record id) */
    uint32_t refnum;      /* content file << 24 | ref index */
    float pos[3];
    float rot[3];    /* Morrowind euler radians: R = Rx(-x) Ry(-y) Rz(-z) */
    float scale;
    float bound[4];  /* world-space bounding sphere */
    uint32_t owner;  /* pw_hash_name of the owning NPC or faction id, 0 = nobody */
} DZoneInstance;

typedef struct {
    uint32_t instance;
    uint32_t dest_zone;  /* DWORLD zone index or DZONE_NO_ZONE */
    float dest_pos[3];
    float dest_rot[3];
    uint32_t dest_cell_hash; /* interior cell name hash or 0 */
    int32_t lock_level;
    uint32_t key_hash;
    uint32_t trap_hash;
} DZoneDoor;

typedef struct {
    float pos[3];
    float radius;
    uint32_t color;  /* ABGR8888 */
    uint32_t flags;  /* LIGH LHDT flags */
    uint32_t instance;
    uint32_t reserved;
} DZoneLight;

/* ------------------------------------------------------------------------ */
/* DWORLD - zone directory, pack entry "world"                              */
/* ------------------------------------------------------------------------ */
#define DWORLD_MAGIC FMT_FOURCC('D', 'W', 'L', 'D')
#define DWORLD_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t zone_size;   /* exterior cells per zone side */
    uint32_t zone_count;
    uint32_t ext_count;
    uint32_t int_count;
    uint32_t zones_ofs;   /* DWorldZone[zone_count] */
    uint32_t ext_ofs;     /* DWorldExt[ext_count], sorted by (x, y) */
    uint32_t int_ofs;     /* DWorldInt[int_count], sorted by hash */
    uint32_t strings_ofs;
    uint32_t file_size;
    uint32_t reserved[2];
} DWorldHeader;

typedef struct {
    uint32_t entry_hash; /* zones.dpak entry */
    uint32_t cell_hash;  /* interior name hash, 0 for exterior zones */
    uint32_t name_ofs;
    uint8_t kind;
    uint8_t flags;
    uint16_t cell_count;
    int16_t x0, y0, x1, y1; /* exterior cell range (inclusive) */
    uint32_t bytes;      /* zone entry size */
    uint32_t reserved;
} DWorldZone;

typedef struct {
    int16_t x, y;
    uint32_t zone;
} DWorldExt;

typedef struct {
    uint32_t hash;
    uint32_t zone;
} DWorldInt;

/* ------------------------------------------------------------------------ */
/* DANIM - skeletal animation resampled at a fixed rate (anims.dpak entries */
/* named after the .kf key, e.g. "xbase_anim", "r/xrat")                    */
/* ------------------------------------------------------------------------ */
#define DANIM_MAGIC FMT_FOURCC('D', 'A', 'N', 'M')
#define DANIM_VERSION 1
#define DANIM_QUAT_SCALE 32767.0f

enum {
    DANIM_T_ROT_ANIMATED = 1 << 0,   /* one rotation per frame, else one constant */
    DANIM_T_TRANS_ANIMATED = 1 << 1, /* one translation per frame, else constant */
    DANIM_T_HAS_ROT = 1 << 2,
    DANIM_T_HAS_TRANS = 1 << 3
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t track_count;
    uint16_t group_count;
    uint16_t reserved0;
    float fps;
    uint32_t frame_count;
    uint32_t tracks_ofs;  /* DAnimTrack[track_count] */
    uint32_t groups_ofs;  /* DAnimGroup[group_count], sorted by name hash */
    uint32_t strings_ofs;
    uint32_t file_size;
    uint32_t reserved[3];
} DAnimHeader;

typedef struct {
    uint32_t bone_hash; /* pw_hash_name(node name) */
    uint16_t flags;     /* DANIM_T_* */
    uint16_t reserved;
    uint32_t rot_ofs;   /* int16 x,y,z,w (/32767) per frame or one */
    uint32_t trans_ofs; /* float x,y,z per frame or one */
} DAnimTrack;

typedef struct {
    uint32_t name_hash; /* pw_hash_name(lower-case group, e.g. "walkforward") */
    uint32_t name_ofs;
    uint16_t start, stop;           /* frames, inclusive */
    uint16_t loop_start, loop_stop; /* frames */
    float speed;  /* root (Bip01) horizontal speed over the group, units/s */
    uint32_t reserved;
} DAnimGroup;


/* ---------------------------------------------------------------- DRPG ---
 * rpg.dpak: "rpg/core" (resident tables below), "text/<n>" (raw DEFLATE
 * blocks of NUL-terminated text and dialogue result-script blobs; a blob id
 * is block << 16 | offset) and "script/<id>" (bytecode + string pool).
 * All tables are 16-byte aligned inside the core; hashes are pw_hash_name()
 * of the lower-case record id. */
#define DRPG_VERSION 3
#define DRPG_NONE 0xFFFFFFFFu
#define DRPG_NONE16 0xFFFFu

typedef struct { uint32_t ofs, count; } DRpgTable;

typedef struct {
    char magic[4];         /* "DRPG" */
    uint16_t version;
    uint16_t flags;
    uint32_t player;       /* object index of the "player" NPC or DRPG_NONE */
    uint32_t names_ofs, names_size; /* NUL-terminated name pool, ofs 0 = "" */
    DRpgTable objects;     /* DRpgObject, sorted by hash */
    DRpgTable actors;      /* DRpgActor */
    DRpgTable lists;       /* DRpgListItem */
    DRpgTable effects;     /* DRpgEffect */
    DRpgTable spells;      /* DRpgSpell (SPEL + ENCH), sorted by hash */
    DRpgTable factions;    /* DRpgFaction, sorted by hash */
    DRpgTable reactions;   /* DRpgReaction */
    DRpgTable globals;     /* DRpgGlobal, sorted by hash = bytecode index */
    DRpgTable gmsts;       /* DRpgGmst, sorted by hash */
    DRpgTable scripts;     /* DRpgScript, sorted by hash */
    DRpgTable vars;        /* DRpgVar, per script sorted by hash */
    DRpgTable starts;      /* uint16 script indices (SSCR) */
    DRpgTable topics;      /* DRpgTopic, sorted by name (case-insensitive) */
    DRpgTable infos;       /* DRpgInfo, grouped per topic in load order */
    DRpgTable conds;       /* DRpgCond */
    DRpgTable blocks;      /* DRpgBlock: text/<i> */
    DRpgTable skills;      /* DRpgSkill[27], skill index order */
    DRpgTable mgefs;       /* DRpgMgef[143], magic effect index order */
    DRpgTable races;       /* DRpgRace, sorted by hash */
    DRpgTable travel;      /* DRpgTravel (NPC stat[1..2] = first, count) */
    DRpgTable markers;     /* DRpgMarker (divine / temple intervention, prison) */
    DRpgTable aipkgs;      /* DRpgAiPackage (NPC_/CREA stat[3] = first, stat[4] = count) */
    DRpgTable sounds;      /* DRpgSound, sorted by hash */
    DRpgTable soundgens;   /* DRpgSoundGen, sorted by (creature, type) */
    uint32_t topic_hash_ofs; /* uint32 topic indices sorted by topic hash */
    uint32_t file_size;
    uint32_t reserved;
} DRpgHeader;

enum {
    DRPG_OT_NPC = 0, DRPG_OT_CREA, DRPG_OT_WEAP, DRPG_OT_ARMO, DRPG_OT_CLOT, DRPG_OT_BOOK,
    DRPG_OT_ALCH, DRPG_OT_INGR, DRPG_OT_MISC, DRPG_OT_APPA, DRPG_OT_LOCK, DRPG_OT_PROB,
    DRPG_OT_REPA, DRPG_OT_LIGH, DRPG_OT_CONT, DRPG_OT_LEVI, DRPG_OT_LEVC, DRPG_OT_DOOR,
    DRPG_OT_ACTI, DRPG_OT_STAT, DRPG_OT_CLAS, DRPG_OT_RACE, DRPG_OT_REGN, DRPG_OT_BSGN,
    DRPG_OT_COUNT
};

enum {
    DRPG_OF_UNIQUE = 1 << 0,   /* exactly one placed reference */
    DRPG_OF_FEMALE = 1 << 1,
    DRPG_OF_ESSENTIAL = 1 << 2,
    DRPG_OF_AUTOCALC = 1 << 3,
    DRPG_OF_KEY = 1 << 4,
    DRPG_OF_SCROLL = 1 << 5,
    DRPG_OF_CARRY = 1 << 6,
    DRPG_OF_ORGANIC = 1 << 7,
    DRPG_OF_RESPAWN = 1 << 8,
    DRPG_OF_PLAYABLE = 1 << 9,
    DRPG_OF_BEAST = 1 << 10,
    DRPG_OF_ALL_LEVELS = 1 << 11,
    DRPG_OF_EACH = 1 << 12,
    DRPG_OF_AUTOCALC_ITEM = 1 << 13
};

/* Type specific fields (see docs/FORMATS.md):
 *   NPC_  link = race, class, faction; stat[0] = actor; list = inventory,
 *         list2 = spells
 *   CREA  stat[0] = actor, stat[1] = creature type, stat[2] = flags,
 *         link[0] = "x" variant key hash (animation / preferred skeleton)
 *   NPC_/CREA  stat[3], stat[4] = AI package range
 *   WEAP  link[0] = enchantment, link[1] = chop/slash min,max bytes,
 *         link[2] = thrust min,max; stat = type, health, speed (float bits),
 *         reach (float bits), enchant points, flags
 *   ARMO  link[0] = ench; stat = type, health, enchant points, rating
 *   CLOT  link[0] = ench; stat[0] = type, stat[2] = enchant points
 *   BOOK  link[0] = ench, link[1] = text blob; stat[0] = skill, [2] = ench pts
 *   ALCH/INGR  list2 = DRpgEffect range
 *   LOCK/PROB/REPA  stat[0] = uses, stat[1] = quality (float bits)
 *   APPA  stat[0] = type, stat[1] = quality; LIGH stat = time, radius,
 *         colour, flags; CONT stat[0] = capacity (float bits), list = items
 *   LEVI/LEVC  list = {id, level}, stat[0] = chance none
 *   CLAS  stat[0] = attr0 | attr1 << 8 | specialisation << 16,
 *         stat[1..3] = 10 skill bytes (minor, major pairs), stat[4] = services
 *   RACE  stat[0..3] = height m/f, weight m/f (float bits); list2 = powers
 *   BSGN  link[1] = description blob, list2 = spells, icon = texture
 *   NPC_  stat[1], stat[2] = travel destination range */
typedef struct {
    uint32_t hash;
    uint32_t name;       /* name pool offset or DRPG_NONE */
    uint8_t type;        /* DRPG_OT_* */
    uint8_t sex;         /* NPC: 1 = female */
    uint16_t script;     /* script index or DRPG_NONE16 */
    uint32_t flags;      /* DRPG_OF_* */
    uint32_t icon;       /* pw_hash_name(icon key) or 0 */
    uint32_t model;      /* pw_hash_name(mesh key) or 0 */
    float weight;
    int32_t value;
    uint32_t link[3];
    uint16_t list_count, list2_count;
    uint32_t list_first, list2_first;
    int32_t stat[6];
} DRpgObject;

typedef struct {
    int16_t level;
    uint16_t health, magicka, fatigue;
    int32_t gold;
    int32_t soul;
    uint16_t attributes[8];
    uint16_t attack[6];    /* creatures: min, max x 3 */
    uint8_t skills[27];    /* NPC skills; creatures: combat, magic, stealth */
    uint8_t disposition, reputation;
    int8_t rank;
    uint8_t hello, fight, flee, alarm;
    uint8_t pad[2];
    uint32_t services;
} DRpgActor;

typedef struct { uint32_t hash; int32_t count; } DRpgListItem;

typedef struct {
    int16_t effect;
    int8_t skill, attribute;
    uint8_t range, pad;
    uint16_t area, duration, mag_min, mag_max;
    uint16_t pad2;
} DRpgEffect;

typedef struct {
    uint32_t hash;
    uint32_t name;
    uint8_t kind;          /* 0 spell, 1 enchantment */
    uint8_t type;
    uint16_t flags;
    int32_t cost;
    int32_t charge;
    uint32_t effect_first;
    uint16_t effect_count;
    uint16_t pad;
    uint32_t reserved;
} DRpgSpell;

typedef struct {
    uint32_t hash;
    uint32_t name;
    uint32_t rank_name[10];   /* name pool offsets or DRPG_NONE */
    int8_t attribute[2];
    int8_t skill[7];
    uint8_t hidden;
    uint16_t reaction_count;
    uint32_t reaction_first;
    int16_t rank_req[10][5];  /* attribute1, attribute2, skill1, skill2, reputation */
} DRpgFaction;

typedef struct { uint32_t faction; int32_t value; } DRpgReaction;

enum { DRPG_VT_SHORT = 0, DRPG_VT_LONG = 1, DRPG_VT_FLOAT = 2 };
enum { DRPG_GMST_INT = 0, DRPG_GMST_FLOAT = 1, DRPG_GMST_STRING = 2 };

typedef struct {
    uint32_t hash;
    uint8_t type;          /* DRPG_VT_* */
    uint8_t pad[3];
    uint32_t value;        /* int32 or float bits */
} DRpgGlobal;

typedef struct {
    uint32_t hash;
    uint8_t type;          /* DRPG_GMST_* */
    uint8_t pad[3];
    uint32_t value;        /* int32, float bits or name pool offset */
} DRpgGmst;

enum { DRPG_SF_ONACTIVATE = 1 << 0, DRPG_SF_DYNAMIC = 1 << 1 };

typedef struct {
    uint32_t hash;
    uint32_t name;
    uint16_t num_shorts, num_longs, num_floats;
    uint16_t flags;        /* DRPG_SF_* */
    uint32_t var_first;
    uint16_t var_count;
    uint16_t pad;
    uint32_t code_size;    /* "script/<id>" = code[code_size] + strings */
    uint32_t strings_size;
} DRpgScript;

typedef struct {
    uint32_t hash;
    uint8_t type;          /* DRPG_VT_* */
    uint8_t pad;
    uint16_t slot;         /* locals are [shorts..., longs..., floats...] */
} DRpgVar;

enum { DRPG_TT_TOPIC = 0, DRPG_TT_VOICE, DRPG_TT_GREETING, DRPG_TT_PERSUASION, DRPG_TT_JOURNAL };

typedef struct {
    uint32_t hash;
    uint32_t name;
    uint8_t type;          /* DRPG_TT_* */
    uint8_t flags;
    uint8_t pad[2];
    uint32_t info_first;
    uint32_t info_count;
} DRpgTopic;

enum {
    DRPG_IF_QUEST_NAME = 1 << 0,
    DRPG_IF_QUEST_FINISHED = 1 << 1,
    DRPG_IF_QUEST_RESTART = 1 << 2,
    DRPG_IF_HAS_ACTOR = 1 << 3
};

typedef struct {
    uint32_t text;         /* blob id of the NUL-terminated response */
    uint32_t script;       /* blob id of the result script or DRPG_NONE */
    uint32_t sound;        /* pw_hash_name(voice file) or 0 */
    uint32_t cond_first;
    uint16_t cond_count;
    uint8_t flags;         /* DRPG_IF_* */
    int8_t pad;
    int32_t data;          /* minimum disposition, or the journal index */
} DRpgInfo;

/* Condition functions: 0..73 are the dialogue SCVR functions (RankLow ...
 * PcWerewolfKills, in the original order), then: */
enum {
    DRPG_CF_GLOBAL = 74, DRPG_CF_LOCAL = 75, DRPG_CF_JOURNAL = 76, DRPG_CF_ITEM = 77,
    DRPG_CF_DEAD = 78, DRPG_CF_NOT_ID = 79, DRPG_CF_NOT_FACTION = 80, DRPG_CF_NOT_CLASS = 81,
    DRPG_CF_NOT_RACE = 82, DRPG_CF_NOT_CELL = 83, DRPG_CF_NOT_LOCAL = 84,
    /* INFO header filters, always first in an info's list */
    DRPG_CF_ACTOR = 100, DRPG_CF_RACE = 101, DRPG_CF_CLASS = 102, DRPG_CF_FACTION = 103,
    DRPG_CF_FACTIONLESS = 104, DRPG_CF_RANK = 105, DRPG_CF_GENDER = 106, DRPG_CF_PCFACTION = 107,
    DRPG_CF_PCRANK = 108, DRPG_CF_CELL = 109
};
enum { DRPG_CMP_EQ = 0, DRPG_CMP_NE, DRPG_CMP_GT, DRPG_CMP_GE, DRPG_CMP_LT, DRPG_CMP_LE };

typedef struct {
    uint8_t func;          /* DRPG_CF_* or SCVR function */
    uint8_t comp;          /* DRPG_CMP_* */
    uint8_t is_float;      /* value is float bits */
    uint8_t pad;
    uint32_t key;          /* id/variable hash; name pool offset for cells */
    int32_t value;         /* compared value; rank for faction filters */
} DRpgCond;

typedef struct {
    uint32_t entry_hash;   /* pw_hash_name("text/<i>") */
    uint32_t raw_size;
    uint32_t packed_size;
    uint32_t reserved;
} DRpgBlock;

typedef struct {
    uint32_t name;         /* name pool */
    uint8_t attribute;     /* governing attribute */
    uint8_t specialization; /* 0 combat, 1 magic, 2 stealth */
    uint16_t pad;
    float use[4];          /* skill gain per use type */
    uint32_t desc;         /* blob id or DRPG_NONE */
    uint32_t reserved;
} DRpgSkill;

enum {
    DRPG_MF_TARGET_SKILL = 0x1, DRPG_MF_TARGET_ATTRIBUTE = 0x2, DRPG_MF_NO_DURATION = 0x4,
    DRPG_MF_NO_MAGNITUDE = 0x8, DRPG_MF_HARMFUL = 0x10, DRPG_MF_CAST_SELF = 0x40,
    DRPG_MF_CAST_TOUCH = 0x80, DRPG_MF_CAST_TARGET = 0x100, DRPG_MF_SPELLMAKING = 0x200,
    DRPG_MF_ENCHANTING = 0x400, DRPG_MF_APPLIED_ONCE = 0x1000, DRPG_MF_NON_RECASTABLE = 0x4000,
    DRPG_MF_UNREFLECTABLE = 0x10000, DRPG_MF_CASTER_LINKED = 0x20000
};

typedef struct {
    uint32_t name;         /* name pool (GMST sEffect...) */
    uint32_t flags;        /* DRPG_MF_* (hard-coded flags merged in) */
    float base_cost;
    float speed;
    uint8_t school;        /* 0 alteration .. 5 restoration */
    uint8_t present;       /* an MGEF record exists */
    int16_t resisted_by;   /* effect index or -1 */
    int16_t weakened_by;   /* effect index or -1 */
    uint16_t pad;
    uint32_t sound[4];     /* cast, bolt, hit, area sound hashes (school default) */
} DRpgMgef;

typedef struct {
    uint32_t hash;
    uint32_t name;
    int8_t bonus_skill[7];  /* -1 unused */
    int8_t pad0;
    uint8_t bonus[7];
    uint8_t pad1;
    uint16_t attribute[8][2]; /* male, female */
    float height[2], weight[2];
    uint32_t flags;        /* 1 playable, 2 beast */
    uint32_t desc;         /* blob id or DRPG_NONE */
} DRpgRace;

typedef struct {
    float pos[3];
    float rot_z;
    uint32_t cell;         /* hash of the lower-case interior name, 0 exterior */
} DRpgTravel;

typedef struct {
    float pos[3];
    float rot_z;
    uint32_t cell;         /* hash of the lower-case interior name, 0 exterior */
    uint8_t kind;          /* DRPG_MARKER_* */
    uint8_t pad[3];
} DRpgMarker;

enum { DRPG_MARKER_DIVINE = 0, DRPG_MARKER_TEMPLE = 1, DRPG_MARKER_PRISON = 2 };

/* AI packages (record order; the first is the actor's current package) */
enum { DRPG_AI_WANDER = 0, DRPG_AI_TRAVEL, DRPG_AI_ESCORT, DRPG_AI_FOLLOW, DRPG_AI_ACTIVATE };

typedef struct {
    float pos[3];          /* travel / escort destination */
    uint32_t target;       /* escort / follow / activate target id hash */
    uint32_t cell;         /* escort / follow destination cell hash (CNDT) or 0 */
    uint16_t distance;     /* wander radius */
    uint16_t duration;     /* wander / escort / follow hours, 0 = forever */
    uint8_t type;          /* DRPG_AI_* */
    uint8_t hour;          /* wander time of day */
    uint8_t idle[8];       /* wander idle2..idle9 chances */
    uint8_t pad[2];
} DRpgAiPackage;

/* SOUN: the sound file key is pw_hash_name("sound/" + path without extension) */
typedef struct {
    uint32_t hash;
    uint32_t file;         /* sounds.dpak / music.dpak entry hash, 0 = none */
    uint8_t volume;        /* 0..255 */
    uint8_t min_range;     /* Morrowind range units (x fAudioMinDistanceMult) */
    uint8_t max_range;
    uint8_t pad;
} DRpgSound;

/* SNDG: creature sound generators (creature 0 = default for everyone) */
enum { DRPG_SNDG_LEFT = 0, DRPG_SNDG_RIGHT, DRPG_SNDG_SWIM_LEFT, DRPG_SNDG_SWIM_RIGHT, DRPG_SNDG_MOAN,
       DRPG_SNDG_ROAR, DRPG_SNDG_SCREAM, DRPG_SNDG_LAND, DRPG_SNDG_COUNT };
typedef struct {
    uint32_t creature;
    uint32_t sound;        /* DRpgSound hash */
    uint8_t type;          /* DRPG_SNDG_* */
    uint8_t pad[3];
} DRpgSoundGen;

/* ---------------------------------------------------------------- DSND ---
 * sounds.dpak / music.dpak entries (sound_demaker.py), keyed by
 * pw_hash_name("sound/fx/foot/walk") or ("music/explore/mx_1"). */
#define DSND_MAGIC 0x444E5344u /* "DSND" */
enum { DSND_CODEC_ADPCM = 0, DSND_CODEC_MP3 = 1 };
typedef struct {
    uint32_t magic;
    uint16_t version;      /* 1 */
    uint8_t codec;         /* DSND_CODEC_* */
    uint8_t channels;      /* 1 or 2 */
    uint32_t rate;         /* Hz */
    uint32_t samples;      /* frames per channel */
    uint16_t block_bytes;  /* ADPCM: bytes per channel block (4-byte header + nibbles) */
    uint16_t block_samples;/* ADPCM: frames per block (first one in the header) */
    uint32_t data_size;    /* payload bytes after this header */
    uint32_t reserved[2];
} DSndHeader;

/* pgrd/<interior name> or pgrd/<x>,<y> in rpg.dpak: a cell's path grid in
 * world coordinates; edges are an adjacency list (edge_first[count + 1]). */
#define DPGR_MAGIC 0x52475044u /* "DPGR" */
typedef struct {
    uint32_t magic;
    uint16_t version;      /* 1 */
    uint16_t count;        /* points: float x, y, z each */
    uint32_t edge_count;   /* then uint16 edge_first[count + 1], uint16 edges[edge_count] */
} DPgrdHeader;

/* Dialogue result-script blob (inside a text block). */
typedef struct {
    uint16_t flags;
    uint16_t pad;
    uint32_t code_size;
    uint32_t strings_size;
} DRpgSnippet;

/* MWScript bytecode opcodes (tools/demake_cooker/mwscript.py). Operands are
 * little endian and unaligned. */
enum {
    MWBC_RET = 0x00,      /* end of program */
    MWBC_PUSHI8 = 0x01,   /* s8 */
    MWBC_PUSHI16 = 0x02,  /* s16 */
    MWBC_PUSHI32 = 0x03,  /* s32 */
    MWBC_PUSHF = 0x04,    /* f32 */
    MWBC_PUSHS = 0x05,    /* u16 string pool offset */
    MWBC_LOADL = 0x06,    /* u16 slot */
    MWBC_STOREL_S = 0x07, MWBC_STOREL_L = 0x08, MWBC_STOREL_F = 0x09,
    MWBC_LOADG = 0x0A,    /* u16 global index */
    MWBC_STOREG_S = 0x0B, MWBC_STOREG_L = 0x0C, MWBC_STOREG_F = 0x0D,
    MWBC_LOADM = 0x0E,    /* u32 ref hash, u16 slot, u8 bit0 global script | type << 1 */
    MWBC_STOREM = 0x0F,   /* same operands */
    MWBC_LOADD = 0x10,    /* u32 name hash (speaker local, as float) */
    MWBC_STORED = 0x11,   /* u32 name hash */
    MWBC_I2F = 0x12, MWBC_F2I = 0x13, MWBC_NEGI = 0x14, MWBC_NEGF = 0x15,
    MWBC_ADDI = 0x16, MWBC_SUBI = 0x17, MWBC_MULI = 0x18, MWBC_DIVI = 0x19,
    MWBC_ADDF = 0x1A, MWBC_SUBF = 0x1B, MWBC_MULF = 0x1C, MWBC_DIVF = 0x1D,
    MWBC_CMPI = 0x1E,     /* + EQ NE LT LE GT GE */
    MWBC_CMPF = 0x24,     /* + EQ NE LT LE GT GE */
    MWBC_JMP = 0x2A,      /* s16 relative to the next instruction */
    MWBC_JZ = 0x2B,       /* s16, pops an int */
    MWBC_POP = 0x2C,
    MWBC_CALL = 0x2D,     /* u16 function, u8 argc */
    MWBC_CALLX = 0x2E,    /* u16 function, u8 argc, u32 reference hash */
    MWBC_MSGBOX = 0x2F,   /* u16 fmt, u8 nargs, u8 nbuttons, u8 types[nargs], u16 buttons[] */
    MWBC_OP_COUNT = 0x30
};
enum { MWBC_ARG_INT = 0, MWBC_ARG_FLOAT = 1, MWBC_ARG_STRING = 2 };

/* Layout contract shared with the Python cookers. */
PW_STATIC_ASSERT(sizeof(DPakHeader) == 32, dpak_header);
PW_STATIC_ASSERT(sizeof(DPakEntry) == 16, dpak_entry);
PW_STATIC_ASSERT(sizeof(DTexLevel) == 16, dtex_level);
PW_STATIC_ASSERT(sizeof(DTexHeader) == 96, dtex_header);
PW_STATIC_ASSERT(sizeof(DAtlasHeader) == 32, datl_header);
PW_STATIC_ASSERT(sizeof(DAtlasEntry) == 16, datl_entry);
PW_STATIC_ASSERT(sizeof(DMeshHeader) == 96, dmesh_header);
PW_STATIC_ASSERT(sizeof(DMeshNode) == 48, dmesh_node);
PW_STATIC_ASSERT(sizeof(DMeshVertex) == 20, dmesh_vertex);
PW_STATIC_ASSERT(sizeof(DMeshBatch) == 88, dmesh_batch);
PW_STATIC_ASSERT(sizeof(DMeshTexture) == 8, dmesh_texture);
PW_STATIC_ASSERT(sizeof(DMeshEmitter) == 72, dmesh_emitter);
PW_STATIC_ASSERT(sizeof(DMeshCollision) == 48, dmesh_collision);

PW_STATIC_ASSERT(sizeof(DZoneHeader) == 160, dzone_header);
PW_STATIC_ASSERT(sizeof(DZoneCell) == 48, dzone_cell);
PW_STATIC_ASSERT(sizeof(DZoneTerrain) == 48, dzone_terrain);
PW_STATIC_ASSERT(sizeof(DZoneInstance) == 64, dzone_instance);
PW_STATIC_ASSERT(sizeof(DZoneDoor) == 48, dzone_door);
PW_STATIC_ASSERT(sizeof(DZoneLight) == 32, dzone_light);
PW_STATIC_ASSERT(sizeof(DWorldHeader) == 48, dworld_header);
PW_STATIC_ASSERT(sizeof(DWorldZone) == 32, dworld_zone);
PW_STATIC_ASSERT(sizeof(DWorldExt) == 8, dworld_ext);
PW_STATIC_ASSERT(sizeof(DWorldInt) == 8, dworld_int);
PW_STATIC_ASSERT(sizeof(DAnimHeader) == 48, danim_header);
PW_STATIC_ASSERT(sizeof(DAnimTrack) == 16, danim_track);
PW_STATIC_ASSERT(sizeof(DAnimGroup) == 24, danim_group);
PW_STATIC_ASSERT(sizeof(DZoneActor) == 64, dzone_actor);
PW_STATIC_ASSERT(sizeof(DRpgHeader) == 224, drpgheader);
PW_STATIC_ASSERT(sizeof(DRpgObject) == 80, drpgobject);
PW_STATIC_ASSERT(sizeof(DRpgActor) == 84, drpgactor);
PW_STATIC_ASSERT(sizeof(DRpgListItem) == 8, drpglistitem);
PW_STATIC_ASSERT(sizeof(DRpgEffect) == 16, drpgeffect);
PW_STATIC_ASSERT(sizeof(DRpgSpell) == 32, drpgspell);
PW_STATIC_ASSERT(sizeof(DRpgFaction) == 164, drpgfaction);
PW_STATIC_ASSERT(sizeof(DRpgReaction) == 8, drpgreaction);
PW_STATIC_ASSERT(sizeof(DRpgGlobal) == 12, drpgglobal);
PW_STATIC_ASSERT(sizeof(DRpgGmst) == 12, drpggmst);
PW_STATIC_ASSERT(sizeof(DRpgScript) == 32, drpgscript);
PW_STATIC_ASSERT(sizeof(DRpgVar) == 8, drpgvar);
PW_STATIC_ASSERT(sizeof(DRpgTopic) == 20, drpgtopic);
PW_STATIC_ASSERT(sizeof(DRpgInfo) == 24, drpginfo);
PW_STATIC_ASSERT(sizeof(DRpgCond) == 12, drpgcond);
PW_STATIC_ASSERT(sizeof(DRpgBlock) == 16, drpgblock);
PW_STATIC_ASSERT(sizeof(DRpgSnippet) == 12, drpgsnippet);
PW_STATIC_ASSERT(sizeof(DRpgSkill) == 32, drpgskill);
PW_STATIC_ASSERT(sizeof(DRpgMgef) == 40, drpgmgef);
PW_STATIC_ASSERT(sizeof(DRpgRace) == 80, drpgrace);
PW_STATIC_ASSERT(sizeof(DRpgTravel) == 20, drpgtravel);
PW_STATIC_ASSERT(sizeof(DRpgMarker) == 24, drpgmarker);
PW_STATIC_ASSERT(sizeof(DRpgAiPackage) == 36, drpgaipackage);
PW_STATIC_ASSERT(sizeof(DRpgSound) == 12, drpgsound);
PW_STATIC_ASSERT(sizeof(DRpgSoundGen) == 12, drpgsoundgen);
PW_STATIC_ASSERT(sizeof(DPgrdHeader) == 12, dpgrdheader);
PW_STATIC_ASSERT(sizeof(DSndHeader) == 32, dsndheader);

#endif /* PSPWIND_FMT_H */
