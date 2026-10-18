#include "suit_switch_adapter.h"
#include "suit_switch.h"
#include "scene_freeze_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RECOMP_FULL_PROGRAM
#include "d3d_frame_adapter.h"

/* Swimsuit switch: puts another swimsuit on a character in the scene on
   screen.

   The game loads characters through a loader task, 0x000A0530, which moves
   the four slots' loads on in each of its turns. Slot n is character n.
   0x000A2060 asks for a suit: EAX holds the suit, EBX the character number
   at +1 of her root block (0x4D6FF0 + n * 0xBC) and the one stack argument
   the slot. 0x000A2260 lets the slot's suit go (EAX holds the slot). Each
   slot's request record at 0x5DEBD0 + n * 16 holds its phase at +0 (0xFF
   when idle), the suit asked for at +2 and the suit loaded at +4. When the
   suit has been read the loader binds it to the character (0x00062A70),
   sets +4 to the suit and the phase back to 0xFF.

   A slot's suit is loaded into the same memory as the suit it replaces, so
   nothing may draw her while the loader works. The game itself only changes
   suits between scenes. Here the switch starts in the camera build, which
   the free camera already wraps, in the middle of a drawing task's turn:
   letting the old suit go only marks its memory free, so the rest of that
   turn still draws it intact. From then on the scene runs only the loader
   task (scene_freeze_adapter.c), so the picture holds for the few frames
   the load takes; then the scene, frozen or not, carries on with the new
   suit. The phases are in suit_switch.c.

   Direct3D keeps the last texture set on each stage and the last vertex
   buffer set on each stream, and drops a reference to it when the next is
   set. Those still point into the old suit's memory, now the new suit's, so
   before the scene draws again they are cleared without dropping anything:
   a seam into the game's statically linked Direct3D (device + 0xB38 and the
   stream table at 0x1F2E28), to go when Direct3D is replaced wholesale.

   Debug: RECOMP_SUIT_SWITCH_AT=<frame>[,<frame>...] switches the first
   character to her next suit at those frames, so a scripted run can test it.

   Swimsuits are items: the item table at 0x3003E8 points at 28-byte
   records whose first byte is 2 for a swimsuit; the 16-bit mask at +16 has
   a bit for each character who can wear it, the bit for character c being
   the word at 0x2F5F78 + c * 4. Each item's names are pointed to from
   0x3095F8: a block of one name pointer every 8 bytes, Japanese first, then
   one for each of the other languages, indexed by the language byte at
   0x4D56D9. The names mark where a line may break with "@|". */
enum {
    LOADER_TASK = 0x000A0530u,
    REQUEST_SUIT = 0x000A2060u,
    RELEASE_SUIT = 0x000A2260u,
    RECORD_FIRST = 0x005DEBD0u,
    RECORD_SIZE = 16u,
    RECORD_PHASE = 0u,
    RECORD_PHASE_IDLE = 0xFFu,
    RECORD_LOADED = 4u,
    SLOT_COUNT = 4u,
    D3D_DEVICE = 0x001F2978u,
    D3D_TEXTURE_SLOTS = 0xB38u,
    D3D_TEXTURE_STATES = 0x0Cu,
    D3D_TEXTURE_DISABLED = 0x80000000u,
    D3D_DIRTY = 0x001F2984u,
    D3D_DIRTY_TEXTURES = 0x4800u,
    D3D_TEXTURE_STAGES = 4u,
    D3D_STREAM_BUFFERS = 0x001F2E28u,
    D3D_STREAM_SIZE = 12u,
    D3D_STREAMS = 16u,
    ACTOR_ACTIVE = 0x004D6FD0u,
    ROOT_FIRST = 0x004D6FF0u,
    ROOT_STRIDE = 0xBCu,
    ROOT_CHARACTER = 1u,
    ROOT_SUIT = 6u,
    ITEM_TABLE = 0x003003E8u,
    ITEM_SIZE = 28u,
    ITEM_KIND_SWIMSUIT = 2u,
    ITEM_WEARERS = 16u,
    CHARACTER_BITS = 0x002F5F78u,
    CHARACTER_LIMIT = 64u,
    /* The girls' bits; other numbers are other people. */
    GIRL_BITS = 0xFFu,
    SUIT_END = 0x149u,
    ITEM_NAMES = 0x003095F8u,
    NAME_STRIDE = 8u,
    LANGUAGE = 0x004D56D9u,
    LANGUAGE_ENGLISH = 1u,
    LANGUAGE_COUNT = 6u,
    NAME_LIMIT = 64u,
};

static RecompSuitSwitch state;
/* A suit being looked for and not yet worn. */
static bool browsing;
static uint32_t browse_actor;
static uint16_t browse_suit;

/* Each slot's suit before its first switch, and whose it was; 0 when none. */
static uint16_t original[SLOT_COUNT];
static uint8_t original_character[SLOT_COUNT];

static uint32_t root(uint32_t actor)
{
    return ROOT_FIRST + actor * ROOT_STRIDE;
}

static uint32_t character_bit(uint32_t character)
{
    return character < CHARACTER_LIMIT ? *recomp_memory_u32(CHARACTER_BITS + character * 4u) : 0u;
}

static bool wearable(uint16_t suit, void *context)
{
    uint32_t character = *(const uint32_t *)context;
    uint32_t item = *recomp_memory_u32(ITEM_TABLE + (uint32_t)suit * 4u);
    if (item == 0u) return false;
    const uint8_t *record = recomp_memory(item, ITEM_SIZE);
    if (record == NULL || record[0] != ITEM_KIND_SWIMSUIT || record[1] != 0u) return false;
    uint16_t wearers;
    memcpy(&wearers, record + ITEM_WEARERS, sizeof wearers);
    return (wearers & character_bit(character)) != 0u;
}

/* Calls a guest function with the given EAX and EBX and stack arguments,
   leaving every register as it was. */
static void call_guest(uint32_t address, uint32_t eax, uint32_t ebx,
    uint32_t a1, uint32_t a2)
{
    const RecompRegisters registers = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);
    recomp_runtime.registers.esp -= 12u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    *recomp_memory_u32(recomp_runtime.registers.esp + 4u) = a1;
    *recomp_memory_u32(recomp_runtime.registers.esp + 8u) = a2;
    recomp_runtime.registers.eax = eax;
    recomp_runtime.registers.ebx = ebx;
    recomp_dispatch(address);
    recomp_runtime.registers = registers;
    recomp_fpu_context_restore(&fpu);
}

static void load(void)
{
    uint32_t slot = state.slot;
    uint32_t character = *recomp_memory(root(slot) + ROOT_CHARACTER, 1u);
    call_guest(RELEASE_SUIT, slot, 0u, 0u, 0u);
    *recomp_memory_u16(root(slot) + ROOT_SUIT) = state.suit;
    call_guest(REQUEST_SUIT, state.suit, character, slot, 0u);
    recomp_scene_freeze_run_only(LOADER_TASK);
    fprintf(stderr, "recomp suit switch: slot %u character %u loading suit %u\n",
        slot, character, state.suit);
}

static bool loaded(void)
{
    const uint32_t record = RECORD_FIRST + state.slot * RECORD_SIZE;
    return *recomp_memory(record + RECORD_PHASE, 1u) == RECORD_PHASE_IDLE &&
        *recomp_memory_u16(record + RECORD_LOADED) == state.suit;
}

/* Forgets Direct3D's bindings without dropping their references. */
static void forget_bindings(void)
{
    uint32_t device = *recomp_memory_u32(D3D_DEVICE);
    if (device != 0u) {
        for (uint32_t stage = 0u; stage < D3D_TEXTURE_STAGES; ++stage) {
            *recomp_memory_u32(device + D3D_TEXTURE_SLOTS + stage * 4u) = 0u;
            *recomp_memory_u32(device + D3D_TEXTURE_STATES + stage * 4u) = D3D_TEXTURE_DISABLED;
        }
        *recomp_memory_u32(D3D_DIRTY) |= D3D_DIRTY_TEXTURES;
    }
    for (uint32_t stream = 0u; stream < D3D_STREAMS; ++stream) {
        *recomp_memory_u32(D3D_STREAM_BUFFERS + stream * D3D_STREAM_SIZE) = 0u;
    }
}

static bool girl(uint32_t actor, uint32_t *character);

void recomp_suit_switch_camera_build(void)
{
    if (state.phase != RECOMP_SUIT_WAITING) return;
    /* The scene may have moved on since the switch was asked for. */
    uint32_t character;
    bool ready = girl(state.slot, &character) &&
        *recomp_memory(RECORD_FIRST + state.slot * RECORD_SIZE + RECORD_PHASE, 1u) == RECORD_PHASE_IDLE;
    if (recomp_suit_switch_camera_built(&state, recomp_d3d_frame_adapter_swap_counter(), ready) ==
        RECOMP_SUIT_ACTION_LOAD) {
        load();
    } else {
        fprintf(stderr, "recomp suit switch: slot %u not ready; suit %u dropped\n",
            state.slot, state.suit);
    }
}

static void scripted_switch(uint32_t frame)
{
    const char *at = getenv("RECOMP_SUIT_SWITCH_AT");
    if (at == NULL) return;
    for (const char *next = at; *next != '\0';) {
        char *end;
        uint32_t when = (uint32_t)strtoul(next, &end, 10);
        if (end == next) break;
        if (when == frame) recomp_suit_switch_step(0u, 1);
        next = *end == ',' ? end + 1 : end;
    }
}

void recomp_suit_switch_frame(uint32_t frame)
{
    scripted_switch(frame);
    switch (recomp_suit_switch_tick(&state, frame, loaded())) {
    case RECOMP_SUIT_ACTION_NONE:
    case RECOMP_SUIT_ACTION_LOAD:
        break;
    case RECOMP_SUIT_ACTION_FINISH:
        forget_bindings();
        recomp_scene_freeze_run_only(0u);
        fprintf(stderr, "recomp suit switch: suit %u on\n", state.suit);
        break;
    case RECOMP_SUIT_ACTION_GIVE_UP:
        forget_bindings();
        recomp_scene_freeze_run_only(0u);
        fprintf(stderr, "recomp suit switch: suit %u gave up\n", state.suit);
        break;
    }
}

static bool begin(uint32_t actor, uint16_t suit)
{
    uint8_t character = *recomp_memory(root(actor) + ROOT_CHARACTER, 1u);
    uint16_t current = *recomp_memory_u16(root(actor) + ROOT_SUIT);
    if (state.phase != RECOMP_SUIT_IDLE) return false;
    if (original[actor] == 0u || original_character[actor] != character) {
        original[actor] = current;
        original_character[actor] = character;
    }
    return recomp_suit_switch_begin(&state, actor, suit, recomp_d3d_frame_adapter_swap_counter());
}

bool recomp_suit_switch_restore(uint32_t actor)
{
    if (actor >= SLOT_COUNT || original[actor] == 0u || state.phase != RECOMP_SUIT_IDLE) return false;
    if (*recomp_memory(root(actor) + ROOT_CHARACTER, 1u) != original_character[actor]) return false;
    if (*recomp_memory_u16(root(actor) + ROOT_SUIT) == original[actor]) return false;
    return recomp_suit_switch_begin(&state, actor, original[actor], recomp_d3d_frame_adapter_swap_counter());
}

void recomp_suit_switch_forget(void)
{
    memset(original, 0, sizeof original);
    browsing = false;
    /* A switch not yet started belongs to the scene being left. */
    if (state.phase == RECOMP_SUIT_WAITING) state.phase = RECOMP_SUIT_IDLE;
}

bool recomp_suit_switch_step(uint32_t actor, int step)
{
    if (actor >= SLOT_COUNT || step == 0 || state.phase != RECOMP_SUIT_IDLE) return false;
    if (*recomp_memory(ACTOR_ACTIVE + actor, 1u) == 0u) return false;
    uint32_t character = *recomp_memory(root(actor) + ROOT_CHARACTER, 1u);
    if ((character_bit(character) & GIRL_BITS) == 0u) return false;
    uint16_t current = *recomp_memory_u16(root(actor) + ROOT_SUIT);
    uint16_t suit = recomp_suit_switch_next(current, step, SUIT_END, wearable, &character);
    if (suit == current) return false;
    return begin(actor, suit);
}

static bool girl(uint32_t actor, uint32_t *character)
{
    if (actor >= SLOT_COUNT || *recomp_memory(ACTOR_ACTIVE + actor, 1u) == 0u) return false;
    *character = *recomp_memory(root(actor) + ROOT_CHARACTER, 1u);
    return (character_bit(*character) & GIRL_BITS) != 0u;
}

bool recomp_suit_switch_browse(uint32_t actor, int step)
{
    uint32_t character;
    if (step == 0 || !girl(actor, &character)) return false;
    if (!browsing || browse_actor != actor) {
        browse_suit = state.phase != RECOMP_SUIT_IDLE && state.slot == actor ? state.suit :
            *recomp_memory_u16(root(actor) + ROOT_SUIT);
    }
    uint16_t suit = browse_suit;
    for (int moved = 0; moved < (step > 0 ? step : -step); ++moved) {
        suit = recomp_suit_switch_next(suit, step > 0 ? 1 : -1, SUIT_END, wearable, &character);
    }
    browsing = true;
    browse_actor = actor;
    browse_suit = suit;
    return true;
}

bool recomp_suit_switch_wear_browsed(void)
{
    if (!browsing) return false;
    browsing = false;
    if (state.phase != RECOMP_SUIT_IDLE) return false;
    if (*recomp_memory_u16(root(browse_actor) + ROOT_SUIT) == browse_suit) return false;
    return begin(browse_actor, browse_suit);
}

uint16_t recomp_suit_switch_shown(uint32_t actor)
{
    if (browsing && browse_actor == actor) return browse_suit;
    return recomp_suit_switch_current(actor);
}

void recomp_suit_switch_place(uint32_t actor, uint16_t suit, uint32_t *place, uint32_t *count)
{
    uint32_t character;
    *place = 0u;
    *count = 0u;
    if (!girl(actor, &character)) return;
    for (uint16_t id = 1u; id < SUIT_END; ++id) {
        if (!wearable(id, &character)) continue;
        ++*count;
        if (id == suit) *place = *count;
    }
}

bool recomp_suit_switch_busy(void)
{
    return state.phase != RECOMP_SUIT_IDLE;
}

uint16_t recomp_suit_switch_current(uint32_t actor)
{
    if (actor >= SLOT_COUNT) return 0u;
    if (state.phase != RECOMP_SUIT_IDLE && state.slot == actor) return state.suit;
    return *recomp_memory_u16(root(actor) + ROOT_SUIT);
}

void recomp_suit_switch_name(uint16_t suit, char *name, uint32_t size)
{
    if (size == 0u) return;
    snprintf(name, size, "Swimsuit %u", (unsigned)suit);
    if (suit == 0u || suit >= SUIT_END) return;
    /* The runtime's font has no Japanese: Japanese shows the English name. */
    uint32_t language = *recomp_memory(LANGUAGE, 1u);
    if (language == 0u || language >= LANGUAGE_COUNT) language = LANGUAGE_ENGLISH;
    uint32_t block = *recomp_memory_u32(ITEM_NAMES + (uint32_t)suit * 4u);
    if (block == 0u) return;
    uint32_t text = *recomp_memory_u32(block + language * NAME_STRIDE);
    const char *source = text != 0u ? (const char *)recomp_memory(text, NAME_LIMIT) : NULL;
    if (source == NULL || source[0] == '\0') return;
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < NAME_LIMIT && source[i] != '\0' && used + 1u < size; ++i) {
        if (source[i] == '@' && i + 1u < NAME_LIMIT && source[i + 1u] == '|') {
            ++i;
            continue;
        }
        name[used++] = source[i];
    }
    name[used] = '\0';
}

#else
bool recomp_suit_switch_step(uint32_t actor, int step)
{
    (void)actor;
    (void)step;
    return false;
}

bool recomp_suit_switch_busy(void)
{
    return false;
}

uint16_t recomp_suit_switch_current(uint32_t actor)
{
    (void)actor;
    return 0u;
}

void recomp_suit_switch_name(uint16_t suit, char *name, uint32_t size)
{
    snprintf(name, size, "Swimsuit %u", (unsigned)suit);
}

void recomp_suit_switch_camera_build(void)
{
}

bool recomp_suit_switch_restore(uint32_t actor)
{
    (void)actor;
    return false;
}

bool recomp_suit_switch_browse(uint32_t actor, int step)
{
    (void)actor;
    (void)step;
    return false;
}

bool recomp_suit_switch_wear_browsed(void)
{
    return false;
}

uint16_t recomp_suit_switch_shown(uint32_t actor)
{
    (void)actor;
    return 0u;
}

void recomp_suit_switch_place(uint32_t actor, uint16_t suit, uint32_t *place, uint32_t *count)
{
    (void)actor;
    (void)suit;
    *place = 0u;
    *count = 0u;
}

void recomp_suit_switch_forget(void)
{
}

void recomp_suit_switch_frame(uint32_t frame)
{
    (void)frame;
}
#endif
