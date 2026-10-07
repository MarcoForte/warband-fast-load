// Redirect a function in the game binary to a replacement.
//
// Preferred: the binary was pre-patched on disk by make_patched.py, so the
// function starts with `jmp [rip+slot]` and we only write the replacement's
// address into the slot (a data write).  Rosetta then keeps using its cached
// ahead-of-time translation of the game.
//
// Fallback (unpatched binary): overwrite the 13-byte frame-setup prologue in
// memory with "movabs rax, replacement; jmp rax".  This works, but modifying
// code makes Rosetta drop to JIT translation, costing ~170 ms at startup.
//
// Either way the returned trampoline holds the original prologue plus a jump
// back into the function, so the replacement can call the original.

#include "patch.h"

#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <string.h>
#include <sys/mman.h>

#define PROLOGUE_LEN 13
static const uint8_t standard_prologue[PROLOGUE_LEN] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
#define DISK_PATCH_LEN 6  // jmp [rip+disp32] replaces push rbp; mov rbp,rsp; push r15 (or r14)

// Must match HOOKS / SLOT_BASE in make_patched.py.  The first DISK_PATCH_LEN
// bytes of each prologue are what the trampoline re-executes; the rest is
// only checked, to make sure this is the expected game build.
static const struct {
    uint64_t addr;
    uint8_t prologue[PROLOGUE_LEN];
} hooks[] = {
    {0x100904054, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}},
    {0x1009E1750, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}},
    {0x100923954, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}},
    {0x100A454B4, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}},
    // script "player id" validator (takeover.c); push rbp; mov rbp,rsp; push r14; push rbx; sub rsp,..
    {0x100AE454C, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x10, 0x02, 0x00}},
};
#define SLOT_BASE 0x103B92B90ULL

void *game_addr(uint64_t unslid) {
    return (void *)(unslid + _dyld_get_image_vmaddr_slide(0));
}

// Trampoline: the first `len` original bytes, then jmp [rip+0] to target+len.
static void *make_trampoline(const uint8_t *target, const uint8_t *prologue, size_t len) {
    uint8_t *tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (tramp == MAP_FAILED) return NULL;
    memcpy(tramp, prologue, len);
    uint8_t *j = tramp + len;
    j[0] = 0xff, j[1] = 0x25, j[2] = j[3] = j[4] = j[5] = 0;
    uint64_t back = (uint64_t)(target + len);
    memcpy(j + 6, &back, 8);
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);
    return tramp;
}

static void *use_disk_patch(uint8_t *target, uint64_t unslid, void *replacement) {
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++) {
        if (hooks[i].addr != unslid) continue;
        int32_t disp = (int32_t)(SLOT_BASE + 8 * i - (unslid + DISK_PATCH_LEN));
        uint8_t expect[DISK_PATCH_LEN] = {0xff, 0x25};
        memcpy(expect + 2, &disp, 4);
        if (memcmp(target, expect, DISK_PATCH_LEN) != 0 ||
            memcmp(target + DISK_PATCH_LEN, hooks[i].prologue + DISK_PATCH_LEN, PROLOGUE_LEN - DISK_PATCH_LEN) != 0)
            return NULL;
        void *tramp = make_trampoline(target, hooks[i].prologue, DISK_PATCH_LEN);
        if (tramp) *(void **)game_addr(SLOT_BASE + 8 * i) = replacement;
        return tramp;
    }
    return NULL;
}

static void *patch_in_memory(uint8_t *target, void *replacement) {
    if (memcmp(target, standard_prologue, PROLOGUE_LEN) != 0) return NULL;  // different game build
    void *tramp = make_trampoline(target, standard_prologue, PROLOGUE_LEN);
    if (!tramp) return NULL;
    uint8_t patch[PROLOGUE_LEN] = {0x48, 0xb8};  // movabs rax, imm64
    uint64_t dest = (uint64_t)replacement;
    memcpy(patch + 2, &dest, 8);
    patch[10] = 0xff, patch[11] = 0xe0;  // jmp rax
    patch[12] = 0x90;                    // nop (never reached)
    vm_address_t page = (vm_address_t)target & ~(vm_address_t)(vm_page_size - 1);
    vm_size_t len = (vm_address_t)(target + PROLOGUE_LEN) - page;
    if (vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
        return NULL;
    memcpy(target, patch, PROLOGUE_LEN);
    vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    return tramp;
}

// Runs before the hook installers: point every slot of a disk-patched binary
// at a pass-through trampoline, so hooks that are disabled (or fail to
// install) still reach the original function.
__attribute__((constructor(101))) static void fill_slots(void) {
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++) {
        uint8_t *target = game_addr(hooks[i].addr);
        if (target[0] != 0xff || target[1] != 0x25) continue;
        void *tramp = make_trampoline(target, hooks[i].prologue, DISK_PATCH_LEN);
        if (tramp) *(void **)game_addr(SLOT_BASE + 8 * i) = tramp;
    }
}

void *patch_function(uint64_t unslid, void *replacement) {
    uint8_t *target = game_addr(unslid);
    if (target[0] == 0xff && target[1] == 0x25) return use_disk_patch(target, unslid, replacement);
    return patch_in_memory(target, replacement);
}
