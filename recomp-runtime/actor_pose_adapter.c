#include "actor_pose_adapter.h"
#include "actor_pose.h"

#include <math.h>
#include <stdio.h>

#ifdef RECOMP_FULL_PROGRAM
#include "runtime.h"
#include "animation_probe.h"

/* Animation select: plays a chosen clip on a character and holds it.

   Up to four characters animate at once. 0x4D6FD0 + n is nonzero while
   character n is active. Each has two animation layers of 0x80 bytes, the
   first at 0x5E0800 + n * 0x100. Each frame 0x000AC100 updates a character's
   layers; when the first layer's byte +0x74 is set, it clears it and calls
   0x000AEC80, which starts the clip numbered at +0x62 (+0x60 carries the same
   number). Bit 0x2000 of the number picks the shared bank; otherwise the
   layer's byte +0x6E picks the character's bank. The bank table at 0x5DD844
   has 12-byte entries whose first word is the bank, the shared bank fifth.
   A bank is 1000 clip pointers; a clip the scene did not load points at the
   placeholder 0xFA4 bytes into the bank.

   A clip carries its own root position, so a new clip would move the
   character. The character's root block at 0x4D6FF0 + n * 0xBC holds its
   position at +8; it is put back each frame while a clip is held, after the
   game's animation update and before the frame is drawn. While held, a clip
   the game's own logic starts in its place is replaced again.

   The root block does not hold everything: many clips also move the body
   away from the root. 0x000B01E0 builds a character's 32 bone matrices from
   the sampled clip, row-vector world matrices of 64 bytes at
   0x4D3650 + n * 0x800 with the translation in floats 12 to 14. After it
   builds a held character, every bone is moved across the ground so the first
   bone, the hips, stays where it stood when the hold began. Height is left
   alone, so sitting and standing clips keep their own.

   A character can also be hidden: after her skeleton is built every bone is
   moved HIDE_DEPTH below where it stood, out of any view above the ground.
   A frozen scene builds no skeletons, so hiding and showing also move the
   bones already built; each build leaves a hidden character's bones that
   far down, so the two agree. */
enum {
    ACTOR_COUNT = 4u,
    ACTOR_ACTIVE = 0x004D6FD0u,
    LAYER_FIRST = 0x005E0800u,
    LAYER_STRIDE = 0x100u,
    LAYER_CLIP_ALIAS = 0x60u,
    LAYER_CLIP = 0x62u,
    LAYER_BANK = 0x6Eu,
    LAYER_START = 0x74u,
    BANK_TABLE = 0x005DD844u,
    BANK_ENTRY_SIZE = 12u,
    SHARED_BANK_ENTRY = 4u,
    BANK_PLACEHOLDER = 0xFA4u,
    ROOT_FIRST = 0x004D6FF0u,
    ROOT_STRIDE = 0xBCu,
    ROOT_POSITION = 0x08u,
    BUILD_SKELETON = 0x000B01E0u,
    BONE_FIRST = 0x004D3650u,
    BONE_STRIDE = 0x800u,
    BONE_COUNT = 32u,
    BONE_SIZE = 64u,
};

static const float HIDE_DEPTH = 100.0f;

void sub_000B01E0(void);

static struct {
    uint32_t actor;
    bool holding;
    uint16_t clip;
    float position[3];
    /* The hips' place on the ground when the hold began. */
    float hips[2];
    bool hips_known;
    uint32_t hidden;
} pose;

static bool active(uint32_t actor)
{
    return actor < ACTOR_COUNT && *recomp_memory_i8(ACTOR_ACTIVE + actor) != 0;
}

static uint32_t layer_of(uint32_t actor)
{
    return LAYER_FIRST + actor * LAYER_STRIDE;
}

static uint32_t root_of(uint32_t actor)
{
    return ROOT_FIRST + actor * ROOT_STRIDE;
}

static uint32_t bone_of(uint32_t actor, uint32_t bone)
{
    return BONE_FIRST + actor * BONE_STRIDE + bone * BONE_SIZE;
}

static void lower_bones(uint32_t actor, float depth);

void recomp_actor_pose_choose(uint32_t actor)
{
    if (actor >= ACTOR_COUNT) return;
    if (actor != pose.actor) recomp_actor_pose_release();
    pose.actor = actor;
}

void recomp_actor_pose_set_hidden(uint32_t mask)
{
    for (uint32_t actor = 0u; actor < ACTOR_COUNT; ++actor) {
        uint32_t bit = 1u << actor;
        if ((mask & bit) != 0u && (pose.hidden & bit) == 0u) lower_bones(actor, HIDE_DEPTH);
        if ((mask & bit) == 0u && (pose.hidden & bit) != 0u) lower_bones(actor, -HIDE_DEPTH);
    }
    pose.hidden = mask;
}

bool recomp_actor_pose_focus(float point[3])
{
    if (!active(pose.actor)) recomp_actor_pose_next_actor();
    if (!active(pose.actor)) return false;
    recomp_guest_load(point, bone_of(pose.actor, 0u) + 12u * sizeof(float), 3u * sizeof(float));
    return true;
}

bool recomp_actor_pose_facing(float direction[3])
{
    if (!active(pose.actor)) return false;
    float matrix[16];
    recomp_guest_load(matrix, bone_of(pose.actor, 0u), sizeof matrix);
    /* The hips' third axis points the way she faces. */
    direction[0] = matrix[8];
    direction[1] = 0.0f;
    direction[2] = matrix[10];
    float length = sqrtf(direction[0] * direction[0] + direction[2] * direction[2]);
    if (!(length > 1e-4f)) return false;
    direction[0] /= length;
    direction[2] /= length;
    return true;
}

static void lower_bones(uint32_t actor, float depth)
{
    for (uint32_t bone = 0u; bone < BONE_COUNT; ++bone) {
        float matrix[16];
        uint32_t address = bone_of(actor, bone);
        recomp_guest_load(matrix, address, sizeof matrix);
        if (matrix[15] == 0.0f) continue;
        matrix[13] -= depth;
        recomp_guest_store(address, matrix, sizeof matrix);
    }
}

static void build_skeleton(void)
{
    uint32_t actor = recomp_runtime.registers.eax;
    RecompFunction observed = recomp_animation_probe_lookup_manual(BUILD_SKELETON);
    if (observed != NULL) observed();
    else sub_000B01E0();
    if (actor < ACTOR_COUNT && (pose.hidden & (1u << actor)) != 0u) {
        lower_bones(actor, HIDE_DEPTH);
        return;
    }
    if (!pose.holding || actor != pose.actor || !pose.hips_known) return;

    float hips[3];
    recomp_guest_load(hips, bone_of(actor, 0u) + 12u * sizeof(float), sizeof hips);
    float dx = pose.hips[0] - hips[0], dz = pose.hips[1] - hips[2];
    for (uint32_t bone = 0u; bone < BONE_COUNT; ++bone) {
        float matrix[16];
        uint32_t address = bone_of(actor, bone);
        recomp_guest_load(matrix, address, sizeof matrix);
        if (matrix[15] == 0.0f) continue;
        matrix[12] += dx;
        matrix[14] += dz;
        recomp_guest_store(address, matrix, sizeof matrix);
    }
}

RecompFunction recomp_actor_pose_lookup_manual(uint32_t guest_address)
{
    return guest_address == BUILD_SKELETON ? build_skeleton : NULL;
}

static bool load_bank(uint32_t entry, uint32_t *bank, uint32_t entries[RECOMP_ACTOR_POSE_BANK_SIZE])
{
    *bank = *recomp_memory_u32(BANK_TABLE + entry * BANK_ENTRY_SIZE);
    if (*bank == 0u) return false;
    recomp_guest_load(entries, *bank, RECOMP_ACTOR_POSE_BANK_SIZE * sizeof *entries);
    return true;
}

static void request(uint32_t actor, uint16_t clip)
{
    uint32_t layer = layer_of(actor);
    *recomp_memory_u16(layer + LAYER_CLIP_ALIAS) = clip;
    *recomp_memory_u16(layer + LAYER_CLIP) = clip;
    *recomp_memory_i8(layer + LAYER_START) = 1;
}

void recomp_actor_pose_next_actor(void)
{
    for (uint32_t step = 1u; step <= ACTOR_COUNT; ++step) {
        uint32_t actor = (pose.actor + step) % ACTOR_COUNT;
        if (active(actor)) {
            if (actor != pose.actor) recomp_actor_pose_release();
            pose.actor = actor;
            fprintf(stderr, "recomp pose: character %u\n", actor);
            return;
        }
    }
}

bool recomp_actor_pose_step_clip(int step)
{
    static uint32_t character[RECOMP_ACTOR_POSE_BANK_SIZE], shared[RECOMP_ACTOR_POSE_BANK_SIZE];
    static RecompActorPoseClips clips;
    uint32_t character_bank = 0u, shared_bank = 0u;

    if (!active(pose.actor)) recomp_actor_pose_next_actor();
    if (!active(pose.actor)) return false;
    uint32_t layer = layer_of(pose.actor);
    bool has_character = load_bank(*recomp_memory_i8(layer + LAYER_BANK) & 0xFFu, &character_bank, character);
    bool has_shared = load_bank(SHARED_BANK_ENTRY, &shared_bank, shared);
    recomp_actor_pose_list(&clips, has_character ? character : NULL, character_bank + BANK_PLACEHOLDER,
        has_shared ? shared : NULL, shared_bank + BANK_PLACEHOLDER);

    uint16_t current = pose.holding ? pose.clip : *recomp_memory_u16(layer + LAYER_CLIP);
    uint16_t next;
    uint32_t position;
    if (!recomp_actor_pose_next(&clips, current, step, &next, &position)) return false;
    if (!pose.holding) {
        float hips[3];
        recomp_guest_load(pose.position, root_of(pose.actor) + ROOT_POSITION, sizeof pose.position);
        recomp_guest_load(hips, bone_of(pose.actor, 0u) + 12u * sizeof(float), sizeof hips);
        pose.hips[0] = hips[0];
        pose.hips[1] = hips[2];
        pose.hips_known = true;
        pose.holding = true;
    }
    pose.clip = next;
    request(pose.actor, next);
    fprintf(stderr, "recomp pose: character %u clip %04x (%u of %u)\n",
        pose.actor, next, position + 1u, clips.count);
    return true;
}

void recomp_actor_pose_release(void)
{
    if (!pose.holding) return;
    pose.holding = false;
    fprintf(stderr, "recomp pose: character %u released\n", pose.actor);
}

void recomp_actor_pose_frame(void)
{
    if (!pose.holding) return;
    if (!active(pose.actor)) {
        recomp_actor_pose_release();
        return;
    }
    uint32_t layer = layer_of(pose.actor);
    if (*recomp_memory_u16(layer + LAYER_CLIP) != pose.clip) request(pose.actor, pose.clip);
    recomp_guest_store(root_of(pose.actor) + ROOT_POSITION, pose.position, sizeof pose.position);
}
#else
void recomp_actor_pose_next_actor(void)
{
}

bool recomp_actor_pose_step_clip(int step)
{
    (void)step;
    return false;
}

void recomp_actor_pose_release(void)
{
}

void recomp_actor_pose_frame(void)
{
}

bool recomp_actor_pose_focus(float point[3])
{
    (void)point;
    return false;
}

bool recomp_actor_pose_facing(float direction[3])
{
    (void)direction;
    return false;
}

void recomp_actor_pose_choose(uint32_t actor)
{
    (void)actor;
}

void recomp_actor_pose_set_hidden(uint32_t mask)
{
    (void)mask;
}

RecompFunction recomp_actor_pose_lookup_manual(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}
#endif
