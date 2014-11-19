/* ******************************************************************************
 * Copyright (c) 2010-2014 Google, Inc.  All rights reserved.
 * Copyright (c) 2010 Massachusetts Institute of Technology  All rights reserved.
 * Copyright (c) 2000-2010 VMware, Inc.  All rights reserved.
 * ******************************************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */
/* Copyright (c) 2001-2003 Massachusetts Institute of Technology */
/* Copyright (c) 2000-2001 Hewlett-Packard Company */

/* file "mangle_shared.c" */

#include "../globals.h"
#include "arch.h"
#include "instr_create.h"
#include "instrument.h" /* for insert_get_mcontext_base */
#include "decode_fast.h" /* for decode_next_pc */

/* make code more readable by shortening long lines
 * we mark everything we add as a meta-instr to avoid hitting
 * client asserts on setting translation fields
 */
#define POST instrlist_meta_postinsert
#define PRE  instrlist_meta_preinsert

/* the stack size of a full context switch for clean call */
int
get_clean_call_switch_stack_size(void)
{
    return sizeof(priv_mcontext_t);
}

/* extra temporarily-used stack usage beyond
 * get_clean_call_switch_stack_size()
 */
int
get_clean_call_temp_stack_size(void)
{
    return XSP_SZ; /* for eflags clear code: push 0; popf */
}

/* utility routines for inserting clean calls to an instrumentation routine
 * strategy is very similar to fcache_enter/return
 * FIXME: try to share code with fcache_enter/return?
 *
 * first swap stacks to DynamoRIO stack:
 *      SAVE_TO_UPCONTEXT %xsp,xsp_OFFSET
 *      RESTORE_FROM_DCONTEXT dstack_OFFSET,%xsp
 * swap peb/teb fields
 * now save app eflags and registers, being sure to lay them out on
 * the stack in priv_mcontext_t order:
 *      push $0 # for priv_mcontext_t.pc; wasted, for now
 *      pushf
 *      pusha # xsp is dstack-XSP_SZ*2; rest are app values
 * clear the eflags for our usage
 * ASSUMPTION (also made in x86.asm): 0 ok, reserved bits are not set by popf,
 *                                    and clearing, not preserving, is good enough
 *      push   $0
 *      popf
 * make the call
 *      call routine
 * restore app regs and eflags
 *      popa
 *      popf
 *      lea XSP_SZ(xsp),xsp # clear priv_mcontext_t.pc slot
 * swap peb/teb fields
 * restore app stack
 *      RESTORE_FROM_UPCONTEXT xsp_OFFSET,%xsp
 */

void
insert_get_mcontext_base(dcontext_t *dcontext, instrlist_t *ilist,
                         instr_t *where, reg_id_t reg)
{
    PRE(ilist, where, instr_create_restore_from_tls
        (dcontext, reg, TLS_DCONTEXT_SLOT));

    /* An extra level of indirection with SELFPROT_DCONTEXT */
    if (TEST(SELFPROT_DCONTEXT, dynamo_options.protect_mask)) {
        ASSERT_NOT_TESTED();
        PRE(ilist, where, XINST_CREATE_load
            (dcontext, opnd_create_reg(reg),
             OPND_CREATE_MEMPTR(reg, offsetof(dcontext_t, upcontext))));
    }
}

/* prepare_for and cleanup_after assume that the stack looks the same after
 * the call to the instrumentation routine, since it stores the app state
 * on the stack.
 * Returns the size of the data stored on the DR stack.
 * WARNING: this routine does NOT save the fp/mmx/sse state, to do that the
 * instrumentation routine should call proc_save_fpstate() and then
 * proc_restore_fpstate()
 * (This is because of expense:
 *   fsave takes 118 cycles!
 *   frstor (separated by 6 instrs from fsave) takes 89 cycles
 *   fxsave and fxrstor are not available on HP machine!
 *   supposedly they came out in PII
 *   on balrog: fxsave 91 cycles, fxrstor 173)
 *
 * For x64, changes the stack pointer by a multiple of 16.
 *
 * NOTE: The client interface's get/set mcontext functions and the
 * hotpatching gateway rely on the app's context being available
 * on the dstack in a particular format.  Do not corrupt this data
 * unless you update all users of this data!
 *
 * NOTE : this routine clobbers TLS_XAX_SLOT and the XSP mcontext slot.
 * We guarantee to clients that all other slots (except the XAX mcontext slot)
 * will remain untouched.
 *
 * N.B.: insert_parameter_preparation (and our documentation for
 * dr_prepare_for_call) assumes that this routine only modifies xsp
 * and xax and no other registers.
 */
/* number of extra slots in addition to register slots. */
#define NUM_EXTRA_SLOTS 2 /* pc, aflags */
uint
prepare_for_clean_call(dcontext_t *dcontext, clean_call_info_t *cci,
                       instrlist_t *ilist, instr_t *instr)
{
    uint dstack_offs = 0;

    if (cci == NULL)
        cci = &default_clean_call_info;

    /* Swap stacks.  For thread-shared, we need to get the dcontext
     * dynamically rather than use the constant passed in here.  Save
     * away xax in a TLS slot and then load the dcontext there.
     */
    if (SCRATCH_ALWAYS_TLS()) {
        PRE(ilist, instr, instr_create_save_to_tls
            (dcontext, SCRATCH_REG0, TLS_SLOT_REG0));

        insert_get_mcontext_base(dcontext, ilist, instr, SCRATCH_REG0);

        PRE(ilist, instr, instr_create_save_to_dc_via_reg
            (dcontext, SCRATCH_REG0, REG_XSP, XSP_OFFSET));

        /* DSTACK_OFFSET isn't within the upcontext so if it's separate this won't
         * work right.  FIXME - the dcontext accessing routines are a mess of shared
         * vs. no shared support, separate context vs. no separate context support etc. */
        ASSERT_NOT_IMPLEMENTED(!TEST(SELFPROT_DCONTEXT, dynamo_options.protect_mask));

#if defined(WINDOWS) && defined(CLIENT_INTERFACE)
        /* i#249: swap PEB pointers while we have dcxt in reg.  We risk "silent
         * death" by using xsp as scratch but don't have simple alternative.
         * We don't support non-SCRATCH_ALWAYS_TLS.
         */
        /* XXX: should use clean callee analysis to remove pieces of this
         * such as errno preservation
         */
        if (INTERNAL_OPTION(private_peb) && should_swap_peb_pointer() &&
            !cci->out_of_line_swap) {
            preinsert_swap_peb(dcontext, ilist, instr, !SCRATCH_ALWAYS_TLS(),
                               REG_XAX/*dc*/, REG_XSP/*scratch*/, true/*to priv*/);
        }
#endif
        PRE(ilist, instr, instr_create_restore_from_dc_via_reg
            (dcontext, SCRATCH_REG0, REG_XSP, DSTACK_OFFSET));

        /* restore xax before pushing the context on the dstack */
        PRE(ilist, instr, instr_create_restore_from_tls
            (dcontext, SCRATCH_REG0, TLS_SLOT_REG0));
    }
    else {
        PRE(ilist, instr, instr_create_save_to_dcontext(dcontext, REG_XSP, XSP_OFFSET));
#if defined(WINDOWS) && defined(CLIENT_INTERFACE)
        if (INTERNAL_OPTION(private_peb) && should_swap_peb_pointer() &&
            !cci->out_of_line_swap) {
            preinsert_swap_peb(dcontext, ilist, instr, !SCRATCH_ALWAYS_TLS(),
                               REG_XAX/*unused*/, REG_XSP/*scratch*/, true/*to priv*/);
        }
#endif
        PRE(ilist, instr, instr_create_restore_dynamo_stack(dcontext));
    }

    /* Save flags and all registers, in priv_mcontext_t order.
     * We're at base of dstack so should be nicely aligned.
     */
    ASSERT(ALIGNED(dcontext->dstack, PAGE_SIZE));

    /* Note that we do NOT bother to put the correct pre-push app xsp value on the
     * stack here, as an optimization for callees who never ask for it: instead we
     * rely on dr_[gs]et_mcontext() to fix it up if asked for.  We can get away w/
     * this while hotpatching cannot (hotp_inject_gateway_call() fixes it up every
     * time) b/c the callee has to ask for the priv_mcontext_t.
     */
#ifdef X86
    if (cci->out_of_line_swap) {
        dstack_offs +=
            insert_out_of_line_context_switch(dcontext, ilist, instr, true);
    } else {
        dstack_offs +=
            insert_push_all_registers(dcontext, cci, ilist, instr, PAGE_SIZE,
                                      INSTR_CREATE_push_imm
                                      (dcontext, OPND_CREATE_INT32(0)));
        insert_clear_eflags(dcontext, cci, ilist, instr);
    }
#elif defined(ARM)
        /* FIXME i#1551: NYI on ARM */
        ASSERT_NOT_IMPLEMENTED(false);
#endif

    /* We no longer need to preserve the app's errno on Windows except
     * when using private libraries, so its preservation is in
     * preinsert_swap_peb().
     * We do not need to preserve DR's Linux errno across app execution.
     */

#if defined(X64) || defined(MACOS)
    /* PR 218790: maintain 16-byte rsp alignment.
     * insert_parameter_preparation() currently assumes we leave rsp aligned.
     */
    /* check if need adjust stack for alignment. */
    if (cci->should_align) {
        uint num_slots = NUM_GP_REGS + NUM_EXTRA_SLOTS;
        if (cci->skip_save_aflags)
            num_slots -= 2;
        num_slots -= cci->num_regs_skip; /* regs that not saved */
        if ((num_slots % 2) == 1) {
            ASSERT((dstack_offs % 16) == 8);
            PRE(ilist, instr, INSTR_CREATE_lea
                (dcontext, opnd_create_reg(REG_XSP),
                 OPND_CREATE_MEM_lea(REG_XSP, REG_NULL, 0, -(int)XSP_SZ)));
            dstack_offs += XSP_SZ;
        } else {
            ASSERT((dstack_offs % 16) == 0);
        }
    }
#endif
    ASSERT(cci->skip_save_aflags   ||
           cci->num_xmms_skip != 0 ||
           cci->num_regs_skip != 0 ||
           dstack_offs == sizeof(priv_mcontext_t) + clean_call_beyond_mcontext());
    return dstack_offs;
}

void
cleanup_after_clean_call(dcontext_t *dcontext, clean_call_info_t *cci,
                         instrlist_t *ilist, instr_t *instr)
{
    if (cci == NULL)
        cci = &default_clean_call_info;
    /* saved error code is currently on the top of the stack */

#if defined(X64) || defined(MACOS)
    /* PR 218790: remove the padding we added for 16-byte rsp alignment */
    if (cci->should_align) {
        uint num_slots = NUM_GP_REGS + NUM_EXTRA_SLOTS;
        if (cci->skip_save_aflags)
            num_slots += 2;
        num_slots -= cci->num_regs_skip; /* regs that not saved */
        if ((num_slots % 2) == 1) {
            PRE(ilist, instr, INSTR_CREATE_lea
                (dcontext, opnd_create_reg(REG_XSP),
                 OPND_CREATE_MEM_lea(REG_XSP, REG_NULL, 0, XSP_SZ)));
        }
    }
#endif

    /* now restore everything */
    if (cci->out_of_line_swap) {
#ifdef X86
        insert_out_of_line_context_switch(dcontext, ilist, instr, false);
#elif defined(ARM)
        /* FIXME i#1551: NYI on ARM */
        ASSERT_NOT_REACHED();
#endif
    } else {
        insert_pop_all_registers(dcontext, cci, ilist, instr,
                                 /* see notes in prepare_for_clean_call() */
                                 PAGE_SIZE);
    }

    /* Swap stacks back.  For thread-shared, we need to get the dcontext
     * dynamically.  Save xax in TLS so we can use it as scratch.
     */
    if (SCRATCH_ALWAYS_TLS()) {
        PRE(ilist, instr, instr_create_save_to_tls
            (dcontext, SCRATCH_REG0, TLS_SLOT_REG0));

        insert_get_mcontext_base(dcontext, ilist, instr, SCRATCH_REG0);

#if defined(WINDOWS) && defined(CLIENT_INTERFACE)
        /* i#249: swap PEB pointers while we have dcxt in reg.  We risk "silent
         * death" by using xsp as scratch but don't have simple alternative.
         * We don't support non-SCRATCH_ALWAYS_TLS.
         */
        if (INTERNAL_OPTION(private_peb) && should_swap_peb_pointer() &&
            !cci->out_of_line_swap) {
            preinsert_swap_peb(dcontext, ilist, instr, !SCRATCH_ALWAYS_TLS(),
                               REG_XAX/*dc*/, REG_XSP/*scratch*/, false/*to app*/);
        }
#endif

        PRE(ilist, instr, instr_create_restore_from_dc_via_reg
            (dcontext, SCRATCH_REG0, REG_XSP, XSP_OFFSET));

        PRE(ilist, instr, instr_create_restore_from_tls
            (dcontext, SCRATCH_REG0, TLS_SLOT_REG0));
    }
    else {
#if defined(WINDOWS) && defined(CLIENT_INTERFACE)
        if (INTERNAL_OPTION(private_peb) && should_swap_peb_pointer() &&
            !cci->out_of_line_swap) {
            preinsert_swap_peb(dcontext, ilist, instr, !SCRATCH_ALWAYS_TLS(),
                               REG_XAX/*unused*/, REG_XSP/*scratch*/, false/*to app*/);
        }
#endif
        PRE(ilist, instr,
            instr_create_restore_from_dcontext(dcontext, REG_XSP, XSP_OFFSET));
    }
}

bool
parameters_stack_padded(void)
{
    return (REGPARM_MINSTACK > 0 || REGPARM_END_ALIGN > XSP_SZ);
}

/* Inserts a complete call to callee with the passed-in arguments.
 * For x64, assumes the stack pointer is currently 16-byte aligned.
 * Clean calls ensure this by using clean base of dstack and having
 * dr_prepare_for_call pad to 16 bytes.
 * Returns whether the call is direct.
 */
bool
insert_meta_call_vargs(dcontext_t *dcontext, instrlist_t *ilist, instr_t *instr,
                       bool clean_call, byte *encode_pc, void *callee,
                       uint num_args, opnd_t *args)
{
    instr_t *in = (instr == NULL) ? instrlist_last(ilist) : instr_get_prev(instr);
    bool direct;
    uint stack_for_params =
        insert_parameter_preparation(dcontext, ilist, instr,
                                     clean_call, num_args, args);
    IF_X64(ASSERT(ALIGNED(stack_for_params, 16)));
    /* If we need an indirect call, we use r11 as the last of the scratch regs.
     * We document this to clients using dr_insert_call_ex() or DR_CLEANCALL_INDIRECT.
     */
    direct = insert_reachable_cti(dcontext, ilist, instr, encode_pc, (byte *)callee,
                                  false/*call*/, false/*!precise*/, DR_REG_R11, NULL);
    if (stack_for_params > 0) {
        /* XXX PR 245936: let user decide whether to clean up?
         * i.e., support calling a stdcall routine?
         */
#ifdef X86
        PRE(ilist, instr,
            INSTR_CREATE_lea(dcontext, opnd_create_reg(REG_XSP),
                             opnd_create_base_disp(REG_XSP, REG_NULL, 0,
                                                   stack_for_params, OPSZ_lea)));
#elif defined(ARM)
        /* FIXME i#1551: NYI on ARM */
        ASSERT_NOT_IMPLEMENTED(false);
#endif
    }
    /* mark it all meta */
    if (in == NULL)
        in = instrlist_first(ilist);
    else
        in = instr_get_next(in);
    while (in != instr) {
        instr_set_meta(in);
        in = instr_get_next(in);
    }
    return direct;
}

/*###########################################################################
 *###########################################################################
 *
 *   M A N G L I N G   R O U T I N E S
 */

void
insert_mov_immed_ptrsz(dcontext_t *dcontext, ptr_int_t val, opnd_t dst,
                       instrlist_t *ilist, instr_t *instr,
                       instr_t **first, instr_t **second)
{
    insert_mov_immed_arch(dcontext, NULL, NULL, val, dst,
                          ilist, instr, first, second);
}

void
insert_mov_instr_addr(dcontext_t *dcontext, instr_t *src, byte *encode_estimate,
                      opnd_t dst, instrlist_t *ilist, instr_t *instr,
                      instr_t **first, instr_t **second)
{
    insert_mov_immed_arch(dcontext, src, encode_estimate, 0, dst,
                          ilist, instr, first, second);
}

void
insert_push_immed_ptrsz(dcontext_t *dcontext, ptr_int_t val,
                        instrlist_t *ilist, instr_t *instr,
                        instr_t **first, instr_t **second)
{
    insert_push_immed_arch(dcontext, NULL, NULL, val,
                           ilist, instr, first, second);
}

void
insert_push_instr_addr(dcontext_t *dcontext, instr_t *src_inst, byte *encode_estimate,
                       instrlist_t *ilist, instr_t *instr,
                       instr_t **first, instr_t **second)
{
    insert_push_immed_arch(dcontext, src_inst, encode_estimate, 0,
                           ilist, instr, first, second);
}

ptr_uint_t
get_call_return_address(dcontext_t *dcontext, instrlist_t *ilist, instr_t *instr)
{
    ptr_uint_t retaddr, curaddr;

    ASSERT(instr_is_call(instr));
#ifdef CLIENT_INTERFACE
    /* i#620: provide API to set fall-through and retaddr targets at end of bb */
    if (instrlist_get_return_target(ilist) != NULL) {
        retaddr = (ptr_uint_t)instrlist_get_return_target(ilist);
        LOG(THREAD, LOG_INTERP, 3, "set return target "PFX" by client\n", retaddr);
        return retaddr;
    }
#endif
    /* For CI builds, use the translation field so we can handle cases
     * where the client has changed the target and invalidated the raw
     * bits.  We'll make sure the translation is always set for direct
     * calls.
     *
     * If a client changes an instr, or our own mangle_rel_addr() does,
     * the raw bits won't be valid but the translation should be.
     */
    curaddr = (ptr_uint_t) instr_get_translation(instr);
    if (curaddr == 0 && instr_raw_bits_valid(instr))
        curaddr = (ptr_uint_t) instr_get_raw_bits(instr);
    ASSERT(curaddr != 0);
    /* we use the next app instruction as return address as the client
     * or DR may change the instruction and so its length.
     */
    if (instr_raw_bits_valid(instr) &&
        instr_get_translation(instr) == instr_get_raw_bits(instr)) {
        /* optimization, if nothing changes, use instr->length to avoid
         * calling decode_next_pc.
         */
        retaddr = curaddr + instr->length;
    } else {
        retaddr = (ptr_uint_t) decode_next_pc(dcontext, (byte *)curaddr);
    }
    return retaddr;
}

/* TOP-LEVEL MANGLE
 * This routine is responsible for mangling a fragment into the form
 * we'd like prior to placing it in the code cache
 * If mangle_calls is false, ignores calls
 * If record_translation is true, records translation target for each
 * inserted instr -- but this slows down encoding in current implementation
 */
void
mangle(dcontext_t *dcontext, instrlist_t *ilist, uint *flags INOUT,
       bool mangle_calls, bool record_translation)
{
    instr_t *instr, *next_instr;
#ifdef WINDOWS
    bool ignorable_sysenter = DYNAMO_OPTION(ignore_syscalls) &&
        DYNAMO_OPTION(ignore_syscalls_follow_sysenter) &&
        (get_syscall_method() == SYSCALL_METHOD_SYSENTER) &&
        TEST(FRAG_HAS_SYSCALL, *flags);
#endif

    /* Walk through instr list:
     * -- convert exit branches to use near_rel form;
     * -- convert direct calls into 'push %eip', aka return address;
     * -- convert returns into 'pop %xcx (; add $imm, %xsp)';
     * -- convert indirect branches into 'save %xcx; lea EA, %xcx';
     * -- convert indirect calls as a combination of direct call and
     *    indirect branch conversion;
     * -- ifdef STEAL_REGISTER, steal edi for our own use.
     * -- ifdef UNIX, mangle seg ref and mov_seg
     */

    KSTART(mangling);
    instrlist_set_our_mangling(ilist, true); /* PR 267260 */
    for (instr = instrlist_first(ilist);
         instr != NULL;
         instr = next_instr) {

        /* don't mangle anything that mangle inserts! */
        next_instr = instr_get_next(instr);

        if (!instr_opcode_valid(instr))
            continue;

#ifdef ANNOTATIONS
        if (is_annotation_return_placeholder(instr)) {
            instrlist_remove(ilist, instr);
            instr_destroy(dcontext, instr);
            continue;
        }
#endif

        if (record_translation) {
            /* make sure inserted instrs translate to the original instr */
            app_pc xl8 = instr_get_translation(instr);
            if (xl8 == NULL)
                xl8 = instr_get_raw_bits(instr);
            instrlist_set_translation_target(ilist, xl8);
        }

#ifdef X86_64
        if (DYNAMO_OPTION(x86_to_x64) &&
            IF_WINDOWS_ELSE(is_wow64_process(NT_CURRENT_PROCESS), false) &&
            instr_get_x86_mode(instr))
            translate_x86_to_x64(dcontext, ilist, &instr);
#endif

#if defined(UNIX) && defined(X86)
        if (INTERNAL_OPTION(mangle_app_seg) && instr_is_app(instr)) {
            /* The instr might be changed by client, and we cannot rely on
             * PREFIX_SEG_FS/GS. So we simply call mangle_seg_ref on every
             * instruction and mangle it if necessary.
             */
            mangle_seg_ref(dcontext, ilist, instr, next_instr);
            if (instr_get_opcode(instr) == OP_mov_seg)
                mangle_mov_seg(dcontext, ilist, instr, next_instr);
        }
#endif

#ifdef X86
        if (instr_saves_float_pc(instr) && instr_is_app(instr)) {
            mangle_float_pc(dcontext, ilist, instr, next_instr, flags);
        }
#endif

#ifdef X64
        /* i#393: mangle_rel_addr might destroy the instr if it is a LEA,
         * which makes instr point to freed memory.
         * In such case, the control should skip later checks on the instr
         * for exit_cti and syscall.
         * skip the rest of the loop if instr is destroyed.
         */
        if (instr_has_rel_addr_reference(instr)) {
            if (mangle_rel_addr(dcontext, ilist, instr, next_instr))
                continue;
        }
#endif

        if (instr_is_exit_cti(instr)) {
#ifdef X86
            mangle_exit_cti_prefixes(dcontext, instr);
#endif

            /* to avoid reachability problems we convert all
             * 8-bit-offset jumps that exit the fragment to 32-bit.
             * Note that data16 jmps are implicitly converted via the
             * absolute target and loss of prefix info (xref PR 225937).
             */
            if (instr_is_cti_short(instr)) {
                /* convert short jumps */
                convert_to_near_rel(dcontext, instr);
            }
        }

#ifdef ANNOTATIONS
        if (is_annotation_label(instr)) {
            mangle_annotation_helper(dcontext, instr, ilist);
            continue;
        }
#endif

        /* PR 240258: wow64 call* gateway is considered is_syscall */
        if (instr_is_syscall(instr)) {
#ifdef WINDOWS
            /* For XP & 2003, which use sysenter, we process the syscall after all
             * mangling is completed, since we need to insert a reference to the
             * post-sysenter instruction. If that instruction is a 'ret', which
             * we've seen on both os's at multiple patch levels, we'd have a
             * dangling reference since it's deleted in mangle_return(). To avoid
             * that case, we defer syscall processing until mangling is completed.
             */
            if (!ignorable_sysenter)
#endif
                mangle_syscall(dcontext, ilist, *flags, instr, next_instr);
            continue;
        }
        else if (instr_is_interrupt(instr)) { /* non-syscall interrupt */
            mangle_interrupt(dcontext, ilist, instr, next_instr);
            continue;
        }
#ifdef FOOL_CPUID
        else if (instr_get_opcode(instr) == OP_cpuid) {
            mangle_cpuid(dcontext, ilist, instr, next_instr);
            continue;
        }
#endif

        if (!instr_is_cti(instr) || instr_is_meta(instr)) {
#ifdef STEAL_REGISTER
            steal_reg(dcontext, instr, ilist);
#endif
#ifdef CLIENT_INTERFACE
            if (TEST(INSTR_CLOBBER_RETADDR, instr->flags) && instr_is_label(instr)) {
                /* move the value to the note field (which the client cannot
                 * possibly use at this point) so we don't have to search for
                 * this label when we hit the ret instr
                 */
                dr_instr_label_data_t *data = instr_get_label_data_area(instr);
                instr_t *tmp;
                instr_t *ret = (instr_t *) data->data[0];
                CLIENT_ASSERT(ret != NULL,
                              "dr_clobber_retaddr_after_read()'s label is corrupted");
                /* avoid use-after-free if client removed the ret by ensuring
                 * this instr_t pointer does exist.
                 * note that we don't want to go searching based just on a flag
                 * as we want tight coupling w/ a pointer as a general way
                 * to store per-instr data outside of the instr itself.
                 */
                for (tmp = instr_get_next(instr); tmp != NULL; tmp = instr_get_next(tmp)) {
                    if (tmp == ret) {
                        tmp->note = (void *) data->data[1]; /* the value to use */
                        break;
                    }
                }
            }
#endif
            continue;
        }

#ifdef STEAL_REGISTER
        if (ilist->flags) {
            restore_state(dcontext, instr, ilist); /* end of edi calculation */
        }
#endif

        if (instr_is_call_direct(instr)) {
            /* mangle_direct_call may inline a call and remove next_instr, so
             * it passes us the updated next instr */
            next_instr = mangle_direct_call(dcontext, ilist, instr, next_instr,
                                            mangle_calls, *flags);
        } else if (instr_is_call_indirect(instr)) {
            mangle_indirect_call(dcontext, ilist, instr, next_instr, mangle_calls,
                                 *flags);
        } else if (instr_is_return(instr)) {
            mangle_return(dcontext, ilist, instr, next_instr, *flags);
        } else if (instr_is_mbr(instr)) {
            mangle_indirect_jump(dcontext, ilist, instr, next_instr, *flags);
#ifdef X86
        } else if (instr_get_opcode(instr) == OP_jmp_far) {
            mangle_far_direct_jump(dcontext, ilist, instr, next_instr, *flags);
#endif
        }
        /* else nothing to do, e.g. direct branches */
    }

#ifdef WINDOWS
    /* Do XP & 2003 ignore-syscalls processing now. */
    if (ignorable_sysenter) {
        /* Check for any syscalls and process them. */
        for (instr = instrlist_first(ilist);
             instr != NULL;
             instr = next_instr) {
            next_instr = instr_get_next(instr);
            if (instr_opcode_valid(instr) && instr_is_syscall(instr))
                mangle_syscall(dcontext, ilist, *flags, instr, next_instr);
        }
    }
#endif
    if (record_translation)
        instrlist_set_translation_target(ilist, NULL);
    instrlist_set_our_mangling(ilist, false); /* PR 267260 */

#if defined(X86) && defined(X64)
    if (!X64_CACHE_MODE_DC(dcontext)) {
        instr_t *in;
        for (in = instrlist_first(ilist); in != NULL; in = instr_get_next(in)) {
            if (instr_is_our_mangling(in)) {
                instr_set_x86_mode(in, true/*x86*/);
                instr_shrink_to_32_bits(in);
            }
        }
    }
#endif

    /* The following assertion should be guaranteed by fact that all
     * blocks end in some kind of branch, and the code above restores
     * the register state on a branch. */
    ASSERT(ilist->flags == 0);
    KSTOP(mangling);
}

/* END OF CONTROL-FLOW MANGLING ROUTINES
 *###########################################################################
 *###########################################################################
 */

void
clean_call_info_init(clean_call_info_t *cci, void *callee,
                     bool save_fpstate, uint num_args)
{
    memset(cci, 0, sizeof(*cci));
    cci->callee        = callee;
    cci->num_args      = num_args;
    cci->save_fpstate  = save_fpstate;
    cci->save_all_regs = true;
    cci->should_align  = true;
    cci->callee_info   = &default_callee_info;
}

void
mangle_init(void)
{
    /* create a default func_info for:
     * 1. clean call callee that cannot be analyzed.
     * 2. variable clean_callees will not be updated during the execution
     *    and can be set write protected.
     */
#ifdef CLIENT_INTERFACE
    clean_call_opt_init();
    clean_call_info_init(&default_clean_call_info, NULL, false, 0);
#endif
}

void
mangle_exit(void)
{
#ifdef CLIENT_INTERFACE
    clean_call_opt_exit();
#endif
}