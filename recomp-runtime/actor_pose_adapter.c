#include "actor_pose_adapter.h"
#include "actor_pose.h"

#include <stdio.h>

#ifdef RECOMP_FULL_PROGRAM
#include "runtime.h"

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
   the game's own logic starts in its place is replaced again. */
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
};

static struct {
    uint32_t actor;
    bool holding;
    uint16_t clip;
    float position[3];
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
        recomp_guest_load(pose.position, root_of(pose.actor) + ROOT_POSITION, sizeof pose.position);
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
#endif
