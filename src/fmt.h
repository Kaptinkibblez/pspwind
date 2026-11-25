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
#define DZONE_VERSION 1
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
    DZONE_IF_NO_MESH = 1 << 2     /* model missing / actor assembled at runtime */
};

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
    uint32_t reserved[3];
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
    uint32_t reserved;
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

#endif /* PSPWIND_FMT_H */
