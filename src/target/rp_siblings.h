/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2026 Rijesh Augustine
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Fork patch (mylonics/blackmagic), not part of upstream Black Magic Debug.
 *
 * RP2040 and RP2350 execute from QSPI Flash through XIP, which probing and programming the Flash takes away. A
 * core still executing from XIP at that point crashes, so the core we work through halts its sibling first, and
 * once the Flash contents may have changed, restarts it through the boot ROM as a chip reset would.
 *
 * Siblings are not attached, so this deliberately works on DHCSR directly rather than through target_halt_poll()
 * and friends: those are the stop-event handlers for the attached core. They clear DFSR, may unwind faults, and
 * service semihosting requests, which dereferences the target controller that a sibling does not have.
 *
 * A sibling is held by a reference on its AP rather than by its target: after a comms failure, the target list
 * is freed in list order, so the sibling's target may already be gone when the core we work through is detached,
 * and that detach still has to put the sibling back.
 */

#ifndef TARGET_RP_SIBLINGS_H
#define TARGET_RP_SIBLINGS_H

#include "general.h"
#include "target_internal.h"
#include "cortexm.h"
#include "adi.h"

/* Both parts have two Arm cores, so a core has at most one sibling */
#define RP_SIBLINGS_MAX 1U
/* How long to wait for a sibling to halt, or to come out of reset */
#define RP_SIBLING_TIMEOUT_MS 250U

typedef struct rp_sibling {
	adiv5_access_port_s *ap;
	/* Whether halting debug was enabled before we got to the core, so it can be put back */
	bool debug_enabled;
	/* Whether we halted the core. If not, it was already stopped, and stays so unless it gets restarted */
	bool halted_by_us;
} rp_sibling_s;

typedef struct rp_siblings {
	rp_sibling_s core[RP_SIBLINGS_MAX];
	size_t count;
} rp_siblings_s;

/* Tells whether `other` is a sibling core, on the same chip, of `target` */
typedef bool (*rp_is_sibling_f)(target_s *target, target_s *other);

/* Report a problem with the sibling to the debug log and to the GDB console */
static inline void rp_siblings_report(target_s *const target, const char *const message)
{
	DEBUG_WARN("%s: %s\n", target->driver, message);
	tc_printf(target, "%s: %s\n", target->driver, message);
}

/* Write a word in a core's address space through its AP */
static inline void rp_core_write32(adiv5_access_port_s *const ap, const target_addr32_t addr, const uint32_t value)
{
	adiv5_mem_write(ap, addr, &value, sizeof(value));
}

/* Make the wire talk to this core's DP: RP2040 cores sit behind separate multi-drop DPs (see cortexm_attach()) */
static inline void rp_core_select(adiv5_access_port_s *const ap)
{
	ap->dp->fault = 1;
	adiv5_dp_error(ap->dp);
}

/* Read a core's DHCSR, returning false if the access did not go through */
static inline bool rp_core_read_dhcsr(adiv5_access_port_s *const ap, uint32_t *const dhcsr)
{
	*dhcsr = adi_mem_read32(ap, CORTEXM_DHCSR);
	return adiv5_dp_error(ap->dp) == 0U;
}

/* Let a halted core run, putting halting debug back how we found it */
static inline void rp_core_release(adiv5_access_port_s *const ap, const bool debug_enabled)
{
	rp_core_write32(ap, CORTEXM_DHCSR, CORTEXM_DHCSR_DBGKEY | CORTEXM_DHCSR_C_DEBUGEN);
	if (!debug_enabled)
		rp_core_write32(ap, CORTEXM_DHCSR, CORTEXM_DHCSR_DBGKEY);
}

/*
 * Restart a halted core through the boot ROM, for once the Flash it was running from may have changed. Resetting
 * the core we work through need not have reset this one: on RP2040, AIRCR.SYSRESETREQ resets only the core that
 * requests it (datasheet section 2.4.2.9). This core's DHCSR.S_RESET_ST, sticky since we read it when halting the
 * core, tells whether it went through a reset since. If not, reset it through its own AIRCR.
 */
static inline void rp_core_restart(adiv5_access_port_s *const ap)
{
	if (!(adi_mem_read32(ap, CORTEXM_DHCSR) & CORTEXM_DHCSR_S_RESET_ST)) {
		rp_core_write32(ap, CORTEXM_AIRCR, CORTEXM_AIRCR_VECTKEY | CORTEXM_AIRCR_SYSRESETREQ);
		/* S_RESET_ST reads back set for the reset just requested, then clear once the core is out of reset */
		platform_timeout_s timeout;
		platform_timeout_set(&timeout, RP_SIBLING_TIMEOUT_MS);
		while ((adi_mem_read32(ap, CORTEXM_DHCSR) & CORTEXM_DHCSR_S_RESET_ST) && !platform_timeout_is_expired(&timeout))
			continue;
	}
	/* The reset vector catch the scan arms may have stopped the core again, so clear the stop reasons */
	rp_core_write32(ap, CORTEXM_DFSR, CORTEXM_DFSR_RESETALL);
}

/*
 * Halt one sibling and hold on to it. One that is already halted is held too, so that it gets restarted if the
 * Flash changes, but is otherwise left alone. One that cannot be reached or halted is reported and left running:
 * it may then crash if it executes from XIP while the Flash is unavailable, which is no worse than not halting it,
 * and better than refusing to attach or program.
 */
static inline void rp_sibling_halt(target_s *const target, rp_siblings_s *const siblings, adiv5_access_port_s *const ap)
{
	for (size_t i = 0; i < siblings->count; ++i) {
		if (siblings->core[i].ap == ap)
			return;
	}

	rp_core_select(ap);
	uint32_t dhcsr = 0U;
	if (!rp_core_read_dhcsr(ap, &dhcsr)) {
		rp_siblings_report(target, "the other core could not be reached, it may crash while the Flash is in use");
		return;
	}
	if (siblings->count == RP_SIBLINGS_MAX) {
		rp_siblings_report(target, "the other core could not be halted, it may crash while the Flash is in use");
		return;
	}

	/* Hold the core before touching it, so it is put back even if an exception cuts this short */
	rp_sibling_s *const sibling = &siblings->core[siblings->count++];
	adiv5_ap_ref(ap);
	sibling->ap = ap;
	sibling->debug_enabled = dhcsr & CORTEXM_DHCSR_C_DEBUGEN;
	sibling->halted_by_us = !(dhcsr & CORTEXM_DHCSR_S_HALT);
	if (!sibling->halted_by_us)
		return;

	rp_core_write32(ap, CORTEXM_DHCSR, CORTEXM_DHCSR_DBGKEY | CORTEXM_DHCSR_C_DEBUGEN | CORTEXM_DHCSR_C_HALT);
	platform_timeout_s timeout;
	platform_timeout_set(&timeout, RP_SIBLING_TIMEOUT_MS);
	while (!platform_timeout_is_expired(&timeout)) {
		if (!rp_core_read_dhcsr(ap, &dhcsr))
			break;
		if (dhcsr & CORTEXM_DHCSR_S_HALT)
			return;
	}

	/* Withdraw the request so the core does not stop later on with nobody to release it, and let it go */
	rp_core_release(ap, sibling->debug_enabled);
	--siblings->count;
	adiv5_ap_unref(ap);
	rp_siblings_report(target, "the other core could not be halted, it may crash while the Flash is in use");
}

/* Halt every sibling of the core we work through, then switch the wire back to that core */
static inline void rp_siblings_halt(
	target_s *const target, rp_siblings_s *const siblings, const rp_is_sibling_f is_sibling)
{
	bool selected_sibling = false;
	for (target_s *core = target_list; core; core = core->next) {
		if (!is_sibling(target, core))
			continue;
		rp_sibling_halt(target, siblings, cortex_ap(core));
		selected_sibling = true;
	}
	if (selected_sibling)
		rp_core_select(cortex_ap(target));
}

/*
 * Let the siblings go again. With `restart`, the Flash may have changed under them, so every held sibling, one that
 * was already halted included, restarts through the boot ROM as on a chip reset. Otherwise, those we halted carry on
 * from where they stopped. Each is dropped only once dealt with, so that if an exception cuts this short, a later
 * call (from detach) picks up the rest.
 */
static inline void rp_siblings_release(target_s *const target, rp_siblings_s *const siblings, const bool restart)
{
	if (!siblings->count)
		return;
	while (siblings->count) {
		const rp_sibling_s *const sibling = &siblings->core[siblings->count - 1U];
		adiv5_access_port_s *const ap = sibling->ap;
		if (restart || sibling->halted_by_us) {
			rp_core_select(ap);
			if (restart)
				rp_core_restart(ap);
			rp_core_release(ap, sibling->debug_enabled);
		}
		--siblings->count;
		adiv5_ap_unref(ap);
	}
	rp_core_select(cortex_ap(target));
}

/* Drop the siblings without touching the chip, leaving them halted: for when it can no longer be reached */
static inline void rp_siblings_forget(rp_siblings_s *const siblings)
{
	while (siblings->count)
		adiv5_ap_unref(siblings->core[--siblings->count].ap);
}

#endif /* TARGET_RP_SIBLINGS_H */
