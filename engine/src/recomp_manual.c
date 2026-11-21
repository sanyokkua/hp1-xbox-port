/**
 * Manual function overrides and ICALL diagnostics -- Harry Potter and the
 * Sorcerer's Stone (Xbox, NTSC-US). The XDK glue (CRT memmove, D3D fence wait
 * and interrupt handlers, DSOUND DSP post, AC'97 reset) comes from the NFSU1
 * port (nfsu1-xbox-port), its addresses moved to HP1's by masked byte match.
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * Two copies of the MSVC CRT memmove are linked (0x00186D70 and 0x002A9450;
 * memcpy is the same body). Their tail and backward-copy paths dispatch
 * through UnwindDown-style vectors indexed with a *negative* register -- the
 * table address in the instruction is the end of the table -- which the
 * lifter's forward table reader cannot follow, so those jumps were lowered
 * to indirect tail calls into the middle of the function and failed to
 * resolve: every copy whose length was not a multiple of 4, and every
 * overlapping backward copy, silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. Generated bodies are
 * skipped by tools.recomp --exclude-manual reading this file. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_00186D70(void) { crt_memmove(); }

/* ── D3D fence wait, D3D_BlockOnTime (0x00195DB0) ────────────
 *
 * stdcall (fence, flags), ret 8. HP1's XDK 5659 D3D keeps its device at
 * 0x001A1400, reached through the global at 0x001A13F8 (found by
 * signature from HP1/NFSU2; HP-PS-Xbox-Recomp DEVELOPING.md 6.1 gives PAL 0x1A13D8, +0x20 on NTSC):
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) that would let D3D
 * reuse ring space and vertex memory the executor has not read yet (see the
 * toolkit's d3d8ltcg-device-context.md), so instead the real semaphore is
 * handed to the executor once and the original wait runs against it. */
#define HP1_D3D_DEVICE_PTR 0x001A13F8u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_00195DB0_gen(void);

void sub_00195DB0(void)
{
    static int exec = -1;
    uint32_t dev = MEM32(HP1_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (exec < 0)
        exec = getenv("RECOMP_PB_EXEC") != NULL;
    if (exec) {
        static uint32_t registered;
        if (sem && sem != registered) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
            registered = sem;
        }
        if (0 /* NFSU2 main-loop return addresses */) {
            /* The main loop (sub_000AEA90) calls BlockOnFence on the fence
             * of the frame it just built, right before Present: the whole
             * frame has to be through the GPU before the next one starts,
             * so the game and the pushbuffer executor take turns instead
             * of overlapping (Eden race: game waited ~50% of the time,
             * executor ~20% idle). RECOMP_FRAME_LAG=1 waits for the
             * previous frame's fence instead: one frame in flight, as on a
             * PC. Race frames checked on Linux, no corruption. */
            static int lag = -1;
            static uint32_t prev;
            if (lag < 0) {
                const char *e = getenv("RECOMP_FRAME_LAG");
                lag = e && *e == '1';
                fprintf(stderr, "[D3D] frame fence: %s\n",
                        lag ? "previous frame (RECOMP_FRAME_LAG=1)" : "this frame");
            }
            if (lag) {
                uint32_t mine = MEM32(esp + 4);
                if (prev)
                    MEM32(esp + 4) = prev;
                prev = mine;
            }
        }
        sub_00195DB0_gen();
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 8;                           /* return address, 2 stdcall args */
}

/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC (0x00199350) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x00198BA0) and the PGRAPH handler
 * (0x00199110), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take in ecx (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, thiscall. It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_00198BA0_gen(void);

void sub_00198BA0(void)
{
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    /* Read under the dispatch lock: raising IRQL can drop the guest lock and
     * let the other handler retire a flip, which we would count again. */
    uint32_t head = MEM32(blk + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_00198BA0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (sub_002F2080). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_00199110_gen(void);
extern void nv2a_pb_trap_lock(void);
/* The frame-rate cap. Present turns D3D's presentation interval (1, 2, 4
 * vblanks, bit 31 = flip at once when late) into a NOP trap word,
 * (bits << 5) | 1 -- bits 0..2 = 1/2/3 for 1/2/4 vblanks, bit 3 =
 * immediate -- and the flip queue schedules the flip that many vblanks
 * after the last one (same D3D code and trap format as NFSU1/NFSU2). HP1
 * keeps the title's interval by default: unlike NFSU, parts of it step per
 * frame (HP-PS-Xbox-Recomp had to fix the aiming circle and the Lumos moths
 * at other rates). HP1_FPS=60 rewrites the interval to 1 as the trap is
 * taken, so a frame ready in time shows on the next vblank. */
static int fps_cap_30(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("HP1_FPS");
        v = !(e && atoi(e) == 60);
        fprintf(stderr, "[fps] flip interval: %s\n", v ? "the title's (HP1_FPS=60 for 1 vblank)" : "1 vblank (HP1_FPS=60)");
    }
    return v;
}
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_00199110(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    uint32_t nsource, head;

    nv2a_pb_trap_lock();
    head = MEM32(blk + 0x1BC);   /* under the locks, as in sub_00198BA0 */
    nsource = nv[0x400108 / 4];
    if (nsource && (nv[0x400704 / 4] & 0xFFFFu) == 0x0100u && !fps_cap_30()) {
        uint32_t data = nv[0x400708 / 4];
        if ((data & 0x1Fu) == 1u && ((data >> 5) & 7u) > 1u) {
            static int told;
            nv[0x400708 / 4] = (data & ~(7u << 5)) | (1u << 5);   /* flip: 1 vblank */
            if (!told++)
                fprintf(stderr, "[fps] flip interval %u -> 1 vblank (uncapped)\n", (data >> 5) & 7u);
        }
    }
    sub_00199110_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}

/* ── DirectSound DSP command post (0x001A9612) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_001A9612_gen(void);

void sub_001A9612(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_001A9612_gen();
}

/* ── DirectSound AC'97 channel reset (0x001AFCF0) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through sub_001A5D30/sub_001A5D52, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_001AFAFA re-arm -- is as the original does it. */
void sub_001AFCF0(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x001AFD04u); RECOMP_ABI_CALL(0x001A5D30u, sub_001A5D30);

    nabm = MEM32(MEM32(chan) * 4 + 0x001C2F40u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x001AFD57u); RECOMP_ABI_CALL(0x001AFAFAu, sub_001AFAFA);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x001AFD5Fu); RECOMP_ABI_CALL(0x001A5D52u, sub_001A5D52);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}

/* ── Debug: which resource a load wait is waiting for (HP1_LOAD_TRACE=1) ──
 *
 * sub_001726E0(item), cdecl: looks up [item+0x30] with sub_001686B0 and
 * returns 1 once it is resident; the load-wait loop (0x000D4277, a loading
 * screen per frame) runs until every item of the list says so. Once a second
 * this prints an item that still says 0, with the first bytes of its
 * descriptor and any string they point at. */
extern void sub_001726E0_gen(void);

static void load_trace_dump(uint32_t va, const char *what)
{
    int k;
    fprintf(stderr, "[load]   %s %08X:", what, va);
    if (va < 0x10000u || va >= 0x84000000u) { fprintf(stderr, " (not RAM)\n"); return; }
    for (k = 0; k < 16; k++) fprintf(stderr, " %08X", MEM32(va + 4u * k));
    fprintf(stderr, "\n");
    for (k = 0; k < 16; k++) {
        uint32_t p = MEM32(va + 4u * k);
        if (p >= 0x10000u && p < 0x84000000u) {
            const char *str = (const char *)XBOX_PTR(p);
            int n = 0;
            while (n < 48 && str[n] >= 0x20 && str[n] < 0x7F) n++;
            if (n >= 4 && (str[n] == 0))
                fprintf(stderr, "[load]     +%02X -> \"%.*s\"\n", k * 4, n, str);
        }
    }
}

void sub_001726E0(void)
{
    static int on = -1;
    static time_t last;
    uint32_t item = MEM32(esp + 4);
    if (on < 0) {
        const char *e = getenv("HP1_LOAD_TRACE");
        on = e && *e == '1';
    }
    sub_001726E0_gen();
    if (on && (eax & 0xFF) == 0 && time(NULL) != last) {
        last = time(NULL);
        fprintf(stderr, "[load] waiting for item %08X\n", item);
        load_trace_dump(item, "item");
        load_trace_dump(MEM32(item + 0x30), "res");
    }
}

/* Debug: HP1_SCRIPT_TRACE=1 -- what a level script waits on. sub_0014D790(ctx)
 * runs a script's commands in order from [ctx+0x18] (index into the script
 * [ctx+0x10]: count word at +6, the command list at [+0x28], 8 bytes each, the
 * table index a word at +0); a command answering 0 holds the script to the next
 * frame. The command table [0x356FB0] has 0x48-byte entries, the name at +0.
 * Printed when a script's held command changes, with the letterbox state. */
extern void sub_0014D790_gen(void);
void sub_0014D790(void)
{
    static int on = -1;
    uint32_t ctx = MEM32(esp + 4);
    if (on < 0) { const char *e = getenv("HP1_SCRIPT_TRACE"); on = e && *e == '1'; }
    sub_0014D790_gen();
    if (on && ctx) {
        static uint32_t last_ctx[64], last_cmd[64];
        uint32_t scr = MEM32(ctx + 0x10), idx = MEM32(ctx + 0x18), k, slot = 64;
        if (scr && idx < MEM16(scr + 6)) {
            uint32_t ti = MEM16(MEM32(scr + 0x28) + idx * 8u);
            uint32_t entry = MEM32(0x00356FB0u) + ti * 0x48u;
            for (k = 0; k < 64; k++) if (last_ctx[k] == ctx || !last_ctx[k]) { slot = k; break; }
            if (slot < 64 && (last_ctx[slot] != ctx || last_cmd[slot] != (idx << 16 | ti))) {
                last_ctx[slot] = ctx; last_cmd[slot] = idx << 16 | ti;
                struct timespec ts;
                float gt;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                memcpy(&gt, (const void *)XBOX_PTR(0x003565B8u), 4);
                fprintf(stderr, "[script] %7.2f s game %8.2f ctx %08X held at #%u %.32s (letterbox %u, speech %u)\n",
                        ts.tv_sec + ts.tv_nsec / 1e9, gt, ctx, idx,
                        (const char *)XBOX_PTR(entry), MEM32(0x0032E224u), MEM32(0x00331798u));
            }
        }
    }
}

/* Debug: archive (.hog) lookups, sub_00184950(archive, name, len?) cdecl
 * (HP1_HOG_TRACE=<substring of the name>): the archive's first fields and
 * name, the name asked for and the entry it returned. */
extern void sub_00184950_gen(void);
void sub_00184950(void)
{
    static const char *want = (const char *)1;
    uint32_t arc = MEM32(esp + 4), name = MEM32(esp + 8), a3 = MEM32(esp + 12);
    if (want == (const char *)1) want = getenv("HP1_HOG_TRACE");
    sub_00184950_gen();
    if (want && name >= 0x10000u && name < 0x84000000u
            && strstr((const char *)XBOX_PTR(name), want)) {
        static int n;
        if (n++ < 40) {
            uint32_t an = MEM32(arc);
            fprintf(stderr, "[hog] lookup \"%.60s\" (a3 %08X) in archive %08X [0]=%08X%s%.40s count %u -> %08X\n",
                    (const char *)XBOX_PTR(name), a3, arc, an, an >= 0x10000u && an < 0x84000000u ? " " : "",
                    an >= 0x10000u && an < 0x84000000u ? (const char *)XBOX_PTR(an) : "", MEM32(arc + 0x14), eax);
        }
    }
}

/* ── Controllers: XAPI's XInput answered from the host pad ─────────────
 *
 * From MiddleDucky/HP-PS-Xbox-Recomp's port/src/hle_input.c (MIT; itself from
 * SCDA-XBERecomp's xapi.cpp), addresses moved PAL -> NTSC (+0x20: XPP follows
 * .text, 32 bytes longer on this disc). Over the emulated OHCI the pad
 * enumerated up to SET_CONFIGURATION and HP1 never opened it; HP-PS also found
 * the stick arriving late that way. XInitDevices, XInputOpen/Close,
 * XInputGetCapabilities, XInputGetState and XInputSetState are answered here;
 * XGetDevices and XGetDeviceChanges stay XAPI's and read the device-type record
 * kept below. The state is the USB pad's report (usb_gamepad_report): host
 * pad, keyboard, RECOMP_PAD_SCRIPT and the overlay's neutral pad all apply.
 * HP1_XINPUT_USB=1 keeps XAPI's own bodies on the emulated USB. */
#include <pthread.h>
#define XPP_GAMEPAD_TYPE 0x001F424Cu  /* g_DeviceType_Gamepad: Current, Change, Previous connected */
#define XPP_TYPES_BEGIN  0x001F4224u  /* the linked device types, begin .. end */
#define XPP_TYPES_END    0x001F422Cu
#define XI_HANDLE_BASE   0x5ADE0010u  /* opaque handles, never dereferenced by the title */
#define XI_MAX_HANDLES   32
#define XI_PORTS         2            /* the runtime's pads */
#define ARG(i)           MEM32(esp + 4u + 4u * (i))

extern int usb_gamepad_connected(int dev);
extern int usb_gamepad_report(int dev, uint8_t *out, int max);
extern void usb_gamepad_output(int dev, const uint8_t *data, int len);
extern void xbox_GuestSetEvent(uint32_t token);

static pthread_mutex_t s_xi_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; uint32_t port, packet; uint8_t last[16]; } s_xi[XI_MAX_HANDLES];

static int xi_hle(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HP1_XINPUT_USB");
        on = !(e && *e == '1');
        fprintf(stderr, "[input] XInput %s\n", on ? "from the host pad (direct)" : "through the emulated USB");
    }
    return on;
}

static void xi_update_ports(void)
{
    uint32_t cur = 0, old;
    int p;
    for (p = 0; p < XI_PORTS; p++)
        if (usb_gamepad_connected(p))
            cur |= 1u << p;
    pthread_mutex_lock(&s_xi_lock);
    old = MEM32(XPP_GAMEPAD_TYPE);
    if (cur != old) {
        MEM32(XPP_GAMEPAD_TYPE + 4) |= cur ^ old;
        MEM32(XPP_GAMEPAD_TYPE) = cur;
        fprintf(stderr, "[input] controller ports now %X (was %X)\n", cur, old);
    }
    pthread_mutex_unlock(&s_xi_lock);
}

static void *xi_poller(void *unused)
{
    (void)unused;
    for (;;) {
        usleep(250000);
        xi_update_ports();
    }
    return NULL;
}

static int xi_index(uint32_t h)
{
    uint32_t i = (h - XI_HANDLE_BASE) / 16u;
    if (h < XI_HANDLE_BASE || (h - XI_HANDLE_BASE) % 16u || i >= XI_MAX_HANDLES || !s_xi[i].used)
        return -1;
    return (int)i;
}

/* XInitDevices(dwPreallocTypeCount, PreallocTypes). The pads are reported by
 * the poller 250 ms in, as insertions, as on the console (HP-PS: a pad already
 * present here was consumed by a boot-time XGetDevices and never "arrived"). */
extern void sub_001F4563_gen(void);
void sub_001F4563(void)
{
    static int poller;
    uint32_t t;
    int i;
    if (!xi_hle()) { sub_001F4563_gen(); return; }
    for (t = XPP_TYPES_BEGIN; t < XPP_TYPES_END; t += 4) {
        uint32_t info = MEM32(t), type = info ? MEM32(info + 4) : 0;
        if (type)
            for (i = 0; i < 3; i++) MEM32(type + 4u * i) = 0;
    }
    for (i = 0; i < 3; i++) MEM32(XPP_GAMEPAD_TYPE + 4u * i) = 0;
    if (!poller) {
        pthread_t th;
        poller = 1;
        pthread_create(&th, NULL, xi_poller, NULL);
        pthread_detach(th);
    }
    esp += 4 + 8;
}

/* XInputOpen(DeviceType, dwPort, dwSlot, pPollingParameters) -> HANDLE */
extern void sub_001F5002_gen(void);
void sub_001F5002(void)
{
    uint32_t type = ARG(0), port = ARG(1);
    int i;
    if (!xi_hle()) { sub_001F5002_gen(); return; }
    eax = 0;
    if (type == XPP_GAMEPAD_TYPE && port < XI_PORTS)
        for (i = 0; i < XI_MAX_HANDLES; i++)
            if (!s_xi[i].used) {
                memset(&s_xi[i], 0, sizeof s_xi[i]);
                s_xi[i].used = 1;
                s_xi[i].port = port;
                eax = XI_HANDLE_BASE + 16u * (uint32_t)i;
                break;
            }
    fprintf(stderr, "[input] XInputOpen(port %u) = %08X\n", port, eax);
    esp += 4 + 16;
}

/* XInputClose(hDevice) */
extern void sub_001F5058_gen(void);
void sub_001F5058(void)
{
    int i;
    if (!xi_hle()) { sub_001F5058_gen(); return; }
    i = xi_index(ARG(0));
    if (i >= 0) s_xi[i].used = 0;
    esp += 4 + 4;
}

/* XInputGetCapabilities(hDevice, pCapabilities): SubType, Reserved, then the
 * gamepad's In (every button and axis) and Out (both motors). */
extern void sub_001F5064_gen(void);
void sub_001F5064(void)
{
    uint32_t caps = ARG(1);
    int i = xi_hle() ? xi_index(ARG(0)) : -2, k;
    if (i == -2) { sub_001F5064_gen(); return; }
    if (i < 0) {
        eax = 6;                                    /* ERROR_INVALID_HANDLE */
    } else if (!usb_gamepad_connected((int)s_xi[i].port)) {
        eax = 1167;                                 /* ERROR_DEVICE_NOT_CONNECTED */
    } else {
        MEM8(caps) = 1;                             /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        MEM8(caps + 1) = 0;
        MEM16(caps + 2) = 0;
        MEM16(caps + 4) = 0x00FF;
        for (k = 0; k < 8; k++) MEM8(caps + 6 + k) = 0xFF;
        for (k = 0; k < 4; k++) MEM16(caps + 14 + 2 * k) = 0xFFFF;
        MEM16(caps + 22) = 0xFFFF;
        MEM16(caps + 24) = 0xFFFF;
        eax = 0;
    }
    esp += 4 + 8;
}

/* ── Skip Cutscenes (overlay Tweaks, HP1_SKIP_CUTSCENES=1) ───────────────
 * Story scenes come two ways. Cuts (cut/hpmNN.hog sub-levels) have the game's
 * own skip, answered below (sub_000D1550). Scenes inside a level are level
 * scripts with the letterbox up: [0x32E224] is 2 (bars coming in) or 1 (in),
 * set by the script command setletterbox (0x4CAD0 -> 0x2A5A0 on, 0x2A5F0 off:
 * 3, then 0). They have no skip, so they are fast-forwarded: their commands
 * hold on game time (Wait: [0x3565B8], frames counted by the tick
 * sub_00140CE0 over [0x21B7D8]), on speech (WaitForSpeech: [0x331798] != 0) or
 * on animations, all advanced by the tick. The game ticks twice a frame; while
 * such a scene plays each tick runs HP1_FF_TICKS times (8), so a 15 s scene
 * passes in about 2 s at full frame rate. Sound is muted and a line of speech is
 * stopped with the game's own stop (0x4F5F0: frees the stream and sound, state
 * [0x331798] to 0). (The first version ran the vblank 8x and drew one frame in
 * 8: the game loop is not paced by the vblank, so the scene kept its length and
 * only the frame rate dropped.) Checked once a frame from the pad read. */
#define HP1_LETTERBOX     0x0032E224u
#define HP1_SPEECH_STATE  0x00331798u
extern void xa2_set_mute(int on);
static int s_cutscene_ff;               /* a letterboxed scene is being fast-forwarded */
static int ff_ticks(void)               /* how much faster: HP1_FF_TICKS, 8 */
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("HP1_FF_TICKS");
        n = e && atoi(e) > 0 ? atoi(e) : 8;
    }
    return n;
}
static void stop_speech(void)
{
    uint32_t sa = eax, sc = ecx, sd = edx, sb = ebx, ss = esi, sdi = edi, ssp = esp;
    PUSH32(esp, 0x0004F5F0u); RECOMP_ABI_CALL(0x0004F5F0u, sub_0004F5F0);
    eax = sa; ecx = sc; edx = sd; ebx = sb; esi = ss; edi = sdi; esp = ssp;
}
/* The game's own cutscene skip (sub_000D1550, called by the cut update
 * sub_000D4660 while a cut, a cut/hpmNN.hog sub-level, plays): with either of
 * two pad actions held ([0x356C04], [0x356C34] in the action table) and the
 * cut allowed by [0x329DB4 + cut id] (only cuts already seen), it sets
 * [0x33FF34] and the cut ends at its next update (sub_000D42F0); cut 0x28 is
 * never skipped. Skip Cutscenes answers "pressed and allowed" once for every cut. */
extern void sub_000D1550_gen(void);
static int skip_cutscenes_on(void);
void sub_000D1550(void)
{
    /* Once per cut, as a button press is: each "pressed" restarts the cut's
     * 0.5 s fade-out (sub_000DC7B0), and the cut ends only when it is done. */
    if (skip_cutscenes_on() && !MEM8(0x0033FF34u)) {
        uint32_t id = MEM32(0x0033FF14u);
        static uint32_t told = 0xFFFFFFFFu;
        if (id != 0x28u) {
            MEM8(0x0033FF34u) = 1;
            if (told != id) { told = id; fprintf(stderr, "[cutscene] cut %u skipped (Skip Cutscenes)\n", id); }
        }
        eax = (eax & ~0xFFu) | 1u;
        esp += 4;
        return;
    }
    sub_000D1550_gen();
}

volatile int hp1_skip_cutscenes = -1;   /* the overlay's row sets it live; -1: HP1_SKIP_CUTSCENES */
static int skip_cutscenes_on(void)
{
    if (hp1_skip_cutscenes < 0) {
        const char *e = getenv("HP1_SKIP_CUTSCENES");
        hp1_skip_cutscenes = e && *e == '1';
    }
    return hp1_skip_cutscenes;
}
static void cutscene_skip_tick(void)
{
    static int on;
    uint32_t lb = MEM32(HP1_LETTERBOX);
    int want = skip_cutscenes_on() && (lb == 1 || lb == 2);
    if (want != on) {
        on = want;
        s_cutscene_ff = on;
        xa2_set_mute(on);
        fprintf(stderr, "[cutscene] %s\n", on ? "fast-forward (Skip Cutscenes)" : "back to normal speed");
    }
    if (on && MEM32(HP1_SPEECH_STATE) != 0u) {
        static int told;
        if (told++ < 50)
            fprintf(stderr, "[cutscene] line of speech stopped\n");
        stop_speech();
    }
}

/* The game tick (see Skip Cutscenes above), run HP1_FF_TICKS times while a
 * letterboxed scene is fast-forwarded. */
extern void sub_00140CE0_gen(void);
void sub_00140CE0(void)
{
    int k;
    for (k = s_cutscene_ff ? ff_ticks() : 1; k > 0; k--) {
        uint32_t ssp = esp;
        sub_00140CE0_gen();
        if (k > 1) esp = ssp;
    }
}

/* The wand shuffle in Ollivanders (script command muddlewands, 0x4CB30, holds
 * until [0x340291] is 2) is animated per drawn frame, not per tick: the frame
 * driver sub_000D5760 updates each of the three boxes with sub_000D4EF0(box),
 * stepping by the frame time [0x21B7DC] (a constant the title only reads). While
 * a scene is fast-forwarded the step is HP1_FF_TICKS times longer for this
 * update only. */
extern void sub_000D4EF0_gen(void);
void sub_000D4EF0(void)
{
    if (s_cutscene_ff) {
        float dt, fast;
        memcpy(&dt, (const void *)XBOX_PTR(0x0021B7DCu), 4);
        fast = dt * (float)ff_ticks();
        memcpy((void *)XBOX_PTR(0x0021B7DCu), &fast, 4);
        sub_000D4EF0_gen();
        memcpy((void *)XBOX_PTR(0x0021B7DCu), &dt, 4);
        return;
    }
    sub_000D4EF0_gen();
}

/* XInputGetState(hDevice, pState): dwPacketNumber, then XINPUT_GAMEPAD
 * { wButtons, bAnalogButtons[8], sThumbLX, LY, RX, RY } -- the USB report's
 * bytes 2..19 laid out the same way. The packet number moves only on a
 * change, as XAPI's does. HP1_PAD_LOG=1 logs every change. */
extern void sub_001F523C_gen(void);
void sub_001F523C(void)
{
    uint32_t st = ARG(1);
    uint8_t rep[20];
    int i = xi_hle() ? xi_index(ARG(0)) : -2, k;
    cutscene_skip_tick();
    if (i == -2) { sub_001F523C_gen(); return; }
    if (i < 0) {
        eax = 6;
    } else if (!usb_gamepad_connected((int)s_xi[i].port)
               || usb_gamepad_report((int)s_xi[i].port, rep, (int)sizeof rep) < 20) {
        eax = 1167;
    } else {
        static int log = -1;
        {
            if (memcmp(rep + 2, s_xi[i].last, 16)) {
                s_xi[i].packet++;
                if (log < 0) { const char *e = getenv("HP1_PAD_LOG"); log = e && *e == '1'; }
                if (log)
                    fprintf(stderr, "[pad] port %u buttons %02X analog %02X %02X %02X %02X %02X %02X %02X %02X"
                            " LX %d LY %d RX %d RY %d\n", s_xi[i].port, rep[2], rep[4], rep[5], rep[6], rep[7],
                            rep[8], rep[9], rep[10], rep[11], (int16_t)(rep[12] | rep[13] << 8),
                            (int16_t)(rep[14] | rep[15] << 8), (int16_t)(rep[16] | rep[17] << 8),
                            (int16_t)(rep[18] | rep[19] << 8));
            }
            {   /* HP1_CAPTURE_ON_BACK=<n>: Back pressed logs the next n frames' draws (as F12) */
                static int cob = -1;
                extern volatile int nv2a_mtl_capture, nv2a_mtl_capture_frames;
                if (cob < 0) { const char *e = getenv("HP1_CAPTURE_ON_BACK"); cob = e ? atoi(e) : 0; }
                if (cob > 0 && (rep[2] & 0x20) && !(s_xi[i].last[0] & 0x20)) {
                    nv2a_mtl_capture_frames = cob;
                    nv2a_mtl_capture = cob + 1;
                    fprintf(stderr, "[capture] Back: %d frames\n", cob);
                }
            }
            memcpy(s_xi[i].last, rep + 2, 16);
        }
        MEM32(st) = s_xi[i].packet;
        MEM16(st + 4) = rep[2];
        for (k = 0; k < 8; k++) MEM8(st + 6 + k) = rep[4 + k];
        for (k = 0; k < 8; k++) MEM8(st + 14 + k) = rep[12 + k];
        eax = 0;
    }
    esp += 4 + 8;
}

/* XInputSetState(hDevice, pFeedback): the rumble goes to the host pad and the
 * request completes at once. XAPI would complete it from the pad's interrupt
 * OUT pipe, which never answers here: HP-PS found HP1 waiting on its event
 * with INFINITE the first time Harry was hit. XINPUT_FEEDBACK: header
 * { dwStatus, hEvent, ... } at 0, XINPUT_RUMBLE { left, right } at 0x42. */
void sub_001F52AF(void)
{
    uint32_t fb = ARG(1);
    int i = xi_index(ARG(0));
    if (fb) {
        uint8_t out[6] = { 0x00, 0x06, MEM8(fb + 0x42), MEM8(fb + 0x43), MEM8(fb + 0x44), MEM8(fb + 0x45) };
        uint32_t event = MEM32(fb + 4);
        usb_gamepad_output(i >= 0 ? (int)s_xi[i].port : 0, out, 6);
        MEM32(fb) = 0;                              /* ERROR_SUCCESS */
        if (event)
            xbox_GuestSetEvent(event);
    }
    eax = 997;                                      /* ERROR_IO_PENDING, as XAPI returns */
    esp += 4 + 8;
}
#undef ARG

/* ── Debug: HP1_PEEK="addr:f,addr:x,..." -- guest words once a second ──
 * f = float, x = hex dword, d = int; printed from the vblank DPC hook (the
 * runtime's weak xbox_after_vblank_dpc). */
/* HP1_MEMSNAP=<dir>: .data (0x210F20-0x375330) to <dir>/s<vblank>.bin every 2 s
 * (find state flags by diffing snapshots taken in different game states). */
static void memsnap(void)
{
    static int on = -1; static char dir[512]; static unsigned t;
    if (on < 0) { const char *e = getenv("HP1_MEMSNAP"); on = e && *e; if (on) snprintf(dir, sizeof dir, "%s", e); }
    if (!on || ++t % 120)
        return;
    {
        char fn[600]; FILE *f;
        snprintf(fn, sizeof fn, "%s/s%07u.bin", dir, t);
        if ((f = fopen(fn, "wb"))) { fwrite((const void *)XBOX_PTR(0x210F20u), 1, 0x375330u - 0x210F20u, f); fclose(f); }
    }
}

void xbox_after_vblank_dpc(void)
{
    static int on = -1, n;
    memsnap();
    static uint32_t addr[16];
    static char kind[16];
    static unsigned ticks;
    if (on < 0) {
        const char *e = getenv("HP1_PEEK");
        on = 0;
        while (e && *e && n < 16) {
            char *end;
            addr[n] = (uint32_t)strtoul(e, &end, 16);
            kind[n] = (*end == ':' && end[1]) ? end[1] : 'x';
            n++;
            e = strchr(end, ',');
            if (e) e++;
        }
        on = n > 0;
    }
    if (!on || ++ticks % 60)
        return;
    {
        int k;
        fprintf(stderr, "[peek]");
        for (k = 0; k < n; k++) {
            if (kind[k] == 'f') fprintf(stderr, " %06X=%g", addr[k], MEMF(addr[k]));
            else if (kind[k] == 'd') fprintf(stderr, " %06X=%d", addr[k], (int32_t)MEM32(addr[k]));
            else fprintf(stderr, " %06X=%08X", addr[k], MEM32(addr[k]));
        }
        fprintf(stderr, "\n");
    }
}

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Skip the logo movies (overlay: Tweaks > Skip Logo Movies, HP1_SKIP_LOGOS=1) ──
 * From HP-PS-Xbox-Recomp (MIT, port/src/hle_video.c, PAL sub_00164450): the boot
 * sequence (sub_00023D50) sets [0x3253CC] = 1, then calls the movie player
 * sub_00164460(name, 0, 0, 0x1C330) (cdecl) for movies/splashwb.thp and the
 * language's movies/ea_<lang>.thp; a button that skips a movie clears the flag.
 * Not playing them leaves it at 1, as a movie that ran to its end: the next state
 * is the same. The callers do not read the return value. */
static int skip_logos(void)
{
    const char *e = getenv("HP1_SKIP_LOGOS");
    return e && *e == '1';
}

extern void sub_00164460_gen(void);
void sub_00164460(void)
{
    uint32_t name = MEM32(esp + 4u);   /* the name, cdecl argument 0 */
    if (name)
        fprintf(stderr, "[movie] %s\n", (const char *)XBOX_PTR(name));
    if (name && skip_logos()) {
        const char *n = (const char *)XBOX_PTR(name);
        if (!strcasecmp(n, "movies/splashwb.thp") || !strncasecmp(n, "movies/ea_", 10)) {
            fprintf(stderr, "[movie] skipped %s (Skip Logo Movies)\n", n);
            eax = 0;
            esp += 4;                           /* cdecl: only the return address */
            return;
        }
    }
    sub_00164460_gen();
}
