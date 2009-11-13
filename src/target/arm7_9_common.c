/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by Hongtao Zheng                                   *
 *   hontor@126.com                                                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "embeddedice.h"
#include "target_request.h"
#include "arm7_9_common.h"
#include "time_support.h"
#include "arm_simulator.h"


int arm7_9_debug_entry(struct target *target);

/**
 * Clear watchpoints for an ARM7/9 target.
 *
 * @param arm7_9 Pointer to the common struct for an ARM7/9 target
 * @return JTAG error status after executing queue
 */
static int arm7_9_clear_watchpoints(struct arm7_9_common *arm7_9)
{
	LOG_DEBUG("-");
	embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], 0x0);
	embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], 0x0);
	arm7_9->sw_breakpoint_count = 0;
	arm7_9->sw_breakpoints_added = 0;
	arm7_9->wp0_used = 0;
	arm7_9->wp1_used = arm7_9->wp1_used_default;
	arm7_9->wp_available = arm7_9->wp_available_max;

	return jtag_execute_queue();
}

/**
 * Assign a watchpoint to one of the two available hardware comparators in an
 * ARM7 or ARM9 target.
 *
 * @param arm7_9 Pointer to the common struct for an ARM7/9 target
 * @param breakpoint Pointer to the breakpoint to be used as a watchpoint
 */
static void arm7_9_assign_wp(struct arm7_9_common *arm7_9, struct breakpoint *breakpoint)
{
	if (!arm7_9->wp0_used)
	{
		arm7_9->wp0_used = 1;
		breakpoint->set = 1;
		arm7_9->wp_available--;
	}
	else if (!arm7_9->wp1_used)
	{
		arm7_9->wp1_used = 1;
		breakpoint->set = 2;
		arm7_9->wp_available--;
	}
	else
	{
		LOG_ERROR("BUG: no hardware comparator available");
	}
	LOG_DEBUG("BPID: %d (0x%08" PRIx32 ") using hw wp: %d",
			  breakpoint->unique_id,
			  breakpoint->address,
			  breakpoint->set );
}

/**
 * Setup an ARM7/9 target's embedded ICE registers for software breakpoints.
 *
 * @param arm7_9 Pointer to common struct for ARM7/9 targets
 * @return Error codes if there is a problem finding a watchpoint or the result
 *         of executing the JTAG queue
 */
static int arm7_9_set_software_breakpoints(struct arm7_9_common *arm7_9)
{
	if (arm7_9->sw_breakpoints_added)
	{
		return ERROR_OK;
	}
	if (arm7_9->wp_available < 1)
	{
		LOG_WARNING("can't enable sw breakpoints with no watchpoint unit available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	arm7_9->wp_available--;

	/* pick a breakpoint unit */
	if (!arm7_9->wp0_used)
	{
		arm7_9->sw_breakpoints_added = 1;
		arm7_9->wp0_used = 3;
	} else if (!arm7_9->wp1_used)
	{
		arm7_9->sw_breakpoints_added = 2;
		arm7_9->wp1_used = 3;
	}
	else
	{
		LOG_ERROR("BUG: both watchpoints used, but wp_available >= 1");
		return ERROR_FAIL;
	}

	if (arm7_9->sw_breakpoints_added == 1)
	{
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_VALUE], arm7_9->arm_bkpt);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], 0x0);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], 0xffffffffu);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
	}
	else if (arm7_9->sw_breakpoints_added == 2)
	{
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_VALUE], arm7_9->arm_bkpt);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_MASK], 0x0);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_MASK], 0xffffffffu);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
	}
	else
	{
		LOG_ERROR("BUG: both watchpoints used, but wp_available >= 1");
		return ERROR_FAIL;
	}
	LOG_DEBUG("SW BP using hw wp: %d",
			  arm7_9->sw_breakpoints_added );

	return jtag_execute_queue();
}

/**
 * Setup the common pieces for an ARM7/9 target after reset or on startup.
 *
 * @param target Pointer to an ARM7/9 target to setup
 * @return Result of clearing the watchpoints on the target
 */
int arm7_9_setup(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	return arm7_9_clear_watchpoints(arm7_9);
}

/**
 * Retrieves the architecture information pointers for ARMv4/5 and ARM7/9
 * targets.  A return of ERROR_OK signifies that the target is a valid target
 * and that the pointers have been set properly.
 *
 * @param target Pointer to the target device to get the pointers from
 * @param armv4_5_p Pointer to be filled in with the common struct for ARMV4/5
 *                  targets
 * @param arm7_9_p Pointer to be filled in with the common struct for ARM7/9
 *                 targets
 * @return ERROR_OK if successful
 */
int arm7_9_get_arch_pointers(struct target *target, struct arm **armv4_5_p, struct arm7_9_common **arm7_9_p)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;

	/* FIXME stop using this routine; just target_to_arm7_9() and
	 * verify the resulting pointer using a replacement routine
	 * that emits a usage message.
	 */
	if (armv4_5->common_magic != ARMV4_5_COMMON_MAGIC)
		return ERROR_TARGET_INVALID;

	if (arm7_9->common_magic != ARM7_9_COMMON_MAGIC)
		return ERROR_TARGET_INVALID;

	*armv4_5_p = armv4_5;
	*arm7_9_p = arm7_9;

	return ERROR_OK;
}

/**
 * Set either a hardware or software breakpoint on an ARM7/9 target.  The
 * breakpoint is set up even if it is already set.  Some actions, e.g. reset,
 * might have erased the values in Embedded ICE.
 *
 * @param target Pointer to the target device to set the breakpoints on
 * @param breakpoint Pointer to the breakpoint to be set
 * @return For hardware breakpoints, this is the result of executing the JTAG
 *         queue.  For software breakpoints, this will be the status of the
 *         required memory reads and writes
 */
int arm7_9_set_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	int retval = ERROR_OK;

	LOG_DEBUG("BPID: %d, Address: 0x%08" PRIx32 ", Type: %d" ,
			  breakpoint->unique_id,
			  breakpoint->address,
			  breakpoint->type);

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (breakpoint->type == BKPT_HARD)
	{
		/* either an ARM (4 byte) or Thumb (2 byte) breakpoint */
		uint32_t mask = (breakpoint->length == 4) ? 0x3u : 0x1u;

		/* reassign a hw breakpoint */
		if (breakpoint->set == 0)
		{
			arm7_9_assign_wp(arm7_9, breakpoint);
		}

		if (breakpoint->set == 1)
		{
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_VALUE], breakpoint->address);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], mask);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], 0xffffffffu);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
		}
		else if (breakpoint->set == 2)
		{
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_VALUE], breakpoint->address);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_MASK], mask);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_MASK], 0xffffffffu);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
		}
		else
		{
			LOG_ERROR("BUG: no hardware comparator available");
			return ERROR_OK;
		}

		retval = jtag_execute_queue();
	}
	else if (breakpoint->type == BKPT_SOFT)
	{
		/* did we already set this breakpoint? */
		if (breakpoint->set)
			return ERROR_OK;

		if (breakpoint->length == 4)
		{
			uint32_t verify = 0xffffffff;
			/* keep the original instruction in target endianness */
			if ((retval = target_read_memory(target, breakpoint->address, 4, 1, breakpoint->orig_instr)) != ERROR_OK)
			{
				return retval;
			}
			/* write the breakpoint instruction in target endianness (arm7_9->arm_bkpt is host endian) */
			if ((retval = target_write_u32(target, breakpoint->address, arm7_9->arm_bkpt)) != ERROR_OK)
			{
				return retval;
			}

			if ((retval = target_read_u32(target, breakpoint->address, &verify)) != ERROR_OK)
			{
				return retval;
			}
			if (verify != arm7_9->arm_bkpt)
			{
				LOG_ERROR("Unable to set 32 bit software breakpoint at address %08" PRIx32 " - check that memory is read/writable", breakpoint->address);
				return ERROR_OK;
			}
		}
		else
		{
			uint16_t verify = 0xffff;
			/* keep the original instruction in target endianness */
			if ((retval = target_read_memory(target, breakpoint->address, 2, 1, breakpoint->orig_instr)) != ERROR_OK)
			{
				return retval;
			}
			/* write the breakpoint instruction in target endianness (arm7_9->thumb_bkpt is host endian) */
			if ((retval = target_write_u16(target, breakpoint->address, arm7_9->thumb_bkpt)) != ERROR_OK)
			{
				return retval;
			}

			if ((retval = target_read_u16(target, breakpoint->address, &verify)) != ERROR_OK)
			{
				return retval;
			}
			if (verify != arm7_9->thumb_bkpt)
			{
				LOG_ERROR("Unable to set thumb software breakpoint at address %08" PRIx32 " - check that memory is read/writable", breakpoint->address);
				return ERROR_OK;
			}
		}

		if ((retval = arm7_9_set_software_breakpoints(arm7_9)) != ERROR_OK)
			return retval;

		arm7_9->sw_breakpoint_count++;

		breakpoint->set = 1;
	}

	return retval;
}

/**
 * Unsets an existing breakpoint on an ARM7/9 target.  If it is a hardware
 * breakpoint, the watchpoint used will be freed and the Embedded ICE registers
 * will be updated.  Otherwise, the software breakpoint will be restored to its
 * original instruction if it hasn't already been modified.
 *
 * @param target Pointer to ARM7/9 target to unset the breakpoint from
 * @param breakpoint Pointer to breakpoint to be unset
 * @return For hardware breakpoints, this is the result of executing the JTAG
 *         queue.  For software breakpoints, this will be the status of the
 *         required memory reads and writes
 */
int arm7_9_unset_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval = ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	LOG_DEBUG("BPID: %d, Address: 0x%08" PRIx32,
			  breakpoint->unique_id,
			  breakpoint->address );

	if (!breakpoint->set)
	{
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD)
	{
		LOG_DEBUG("BPID: %d Releasing hw wp: %d",
				  breakpoint->unique_id,
				  breakpoint->set );
		if (breakpoint->set == 1)
		{
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], 0x0);
			arm7_9->wp0_used = 0;
			arm7_9->wp_available++;
		}
		else if (breakpoint->set == 2)
		{
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], 0x0);
			arm7_9->wp1_used = 0;
			arm7_9->wp_available++;
		}
		retval = jtag_execute_queue();
		breakpoint->set = 0;
	}
	else
	{
		/* restore original instruction (kept in target endianness) */
		if (breakpoint->length == 4)
		{
			uint32_t current_instr;
			/* check that user program as not modified breakpoint instruction */
			if ((retval = target_read_memory(target, breakpoint->address, 4, 1, (uint8_t*)&current_instr)) != ERROR_OK)
			{
				return retval;
			}
			if (current_instr == arm7_9->arm_bkpt)
				if ((retval = target_write_memory(target, breakpoint->address, 4, 1, breakpoint->orig_instr)) != ERROR_OK)
				{
					return retval;
				}
		}
		else
		{
			uint16_t current_instr;
			/* check that user program as not modified breakpoint instruction */
			if ((retval = target_read_memory(target, breakpoint->address, 2, 1, (uint8_t*)&current_instr)) != ERROR_OK)
			{
				return retval;
			}
			if (current_instr == arm7_9->thumb_bkpt)
				if ((retval = target_write_memory(target, breakpoint->address, 2, 1, breakpoint->orig_instr)) != ERROR_OK)
				{
					return retval;
				}
		}

		if (--arm7_9->sw_breakpoint_count==0)
		{
			/* We have removed the last sw breakpoint, clear the hw breakpoint we used to implement it */
			if (arm7_9->sw_breakpoints_added == 1)
			{
				embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], 0);
			}
			else if (arm7_9->sw_breakpoints_added == 2)
			{
				embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], 0);
			}
		}

		breakpoint->set = 0;
	}

	return retval;
}

/**
 * Add a breakpoint to an ARM7/9 target.  This makes sure that there are no
 * dangling breakpoints and that the desired breakpoint can be added.
 *
 * @param target Pointer to the target ARM7/9 device to add a breakpoint to
 * @param breakpoint Pointer to the breakpoint to be added
 * @return An error status if there is a problem adding the breakpoint or the
 *         result of setting the breakpoint
 */
int arm7_9_add_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (arm7_9->breakpoint_count == 0)
	{
		/* make sure we don't have any dangling breakpoints. This is vital upon
		 * GDB connect/disconnect
		 */
		arm7_9_clear_watchpoints(arm7_9);
	}

	if ((breakpoint->type == BKPT_HARD) && (arm7_9->wp_available < 1))
	{
		LOG_INFO("no watchpoint unit available for hardware breakpoint");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if ((breakpoint->length != 2) && (breakpoint->length != 4))
	{
		LOG_INFO("only breakpoints of two (Thumb) or four (ARM) bytes length supported");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
	{
		arm7_9_assign_wp(arm7_9, breakpoint);
	}

	arm7_9->breakpoint_count++;

	return arm7_9_set_breakpoint(target, breakpoint);
}

/**
 * Removes a breakpoint from an ARM7/9 target.  This will make sure there are no
 * dangling breakpoints and updates available watchpoints if it is a hardware
 * breakpoint.
 *
 * @param target Pointer to the target to have a breakpoint removed
 * @param breakpoint Pointer to the breakpoint to be removed
 * @return Error status if there was a problem unsetting the breakpoint or the
 *         watchpoints could not be cleared
 */
int arm7_9_remove_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval = ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	if ((retval = arm7_9_unset_breakpoint(target, breakpoint)) != ERROR_OK)
	{
		return retval;
	}

	if (breakpoint->type == BKPT_HARD)
		arm7_9->wp_available++;

	arm7_9->breakpoint_count--;
	if (arm7_9->breakpoint_count == 0)
	{
		/* make sure we don't have any dangling breakpoints */
		if ((retval = arm7_9_clear_watchpoints(arm7_9)) != ERROR_OK)
		{
			return retval;
		}
	}

	return ERROR_OK;
}

/**
 * Sets a watchpoint for an ARM7/9 target in one of the watchpoint units.  It is
 * considered a bug to call this function when there are no available watchpoint
 * units.
 *
 * @param target Pointer to an ARM7/9 target to set a watchpoint on
 * @param watchpoint Pointer to the watchpoint to be set
 * @return Error status if watchpoint set fails or the result of executing the
 *         JTAG queue
 */
int arm7_9_set_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	int retval = ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	int rw_mask = 1;
	uint32_t mask;

	mask = watchpoint->length - 1;

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (watchpoint->rw == WPT_ACCESS)
		rw_mask = 0;
	else
		rw_mask = 1;

	if (!arm7_9->wp0_used)
	{
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_VALUE], watchpoint->address);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], mask);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], watchpoint->mask);
		if (watchpoint->mask != 0xffffffffu)
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_VALUE], watchpoint->value);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], 0xff & ~EICE_W_CTRL_nOPC & ~rw_mask);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], EICE_W_CTRL_ENABLE | EICE_W_CTRL_nOPC | (watchpoint->rw & 1));

		if ((retval = jtag_execute_queue()) != ERROR_OK)
		{
			return retval;
		}
		watchpoint->set = 1;
		arm7_9->wp0_used = 2;
	}
	else if (!arm7_9->wp1_used)
	{
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_VALUE], watchpoint->address);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_MASK], mask);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_MASK], watchpoint->mask);
		if (watchpoint->mask != 0xffffffffu)
			embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_VALUE], watchpoint->value);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_MASK], 0xff & ~EICE_W_CTRL_nOPC & ~rw_mask);
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], EICE_W_CTRL_ENABLE | EICE_W_CTRL_nOPC | (watchpoint->rw & 1));

		if ((retval = jtag_execute_queue()) != ERROR_OK)
		{
			return retval;
		}
		watchpoint->set = 2;
		arm7_9->wp1_used = 2;
	}
	else
	{
		LOG_ERROR("BUG: no hardware comparator available");
		return ERROR_OK;
	}

	return ERROR_OK;
}

/**
 * Unset an existing watchpoint and clear the used watchpoint unit.
 *
 * @param target Pointer to the target to have the watchpoint removed
 * @param watchpoint Pointer to the watchpoint to be removed
 * @return Error status while trying to unset the watchpoint or the result of
 *         executing the JTAG queue
 */
int arm7_9_unset_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	int retval = ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!watchpoint->set)
	{
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (watchpoint->set == 1)
	{
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], 0x0);
		if ((retval = jtag_execute_queue()) != ERROR_OK)
		{
			return retval;
		}
		arm7_9->wp0_used = 0;
	}
	else if (watchpoint->set == 2)
	{
		embeddedice_set_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], 0x0);
		if ((retval = jtag_execute_queue()) != ERROR_OK)
		{
			return retval;
		}
		arm7_9->wp1_used = 0;
	}
	watchpoint->set = 0;

	return ERROR_OK;
}

/**
 * Add a watchpoint to an ARM7/9 target.  If there are no watchpoint units
 * available, an error response is returned.
 *
 * @param target Pointer to the ARM7/9 target to add a watchpoint to
 * @param watchpoint Pointer to the watchpoint to be added
 * @return Error status while trying to add the watchpoint
 */
int arm7_9_add_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (arm7_9->wp_available < 1)
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if ((watchpoint->length != 1) && (watchpoint->length != 2) && (watchpoint->length != 4))
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	arm7_9->wp_available--;

	return ERROR_OK;
}

/**
 * Remove a watchpoint from an ARM7/9 target.  The watchpoint will be unset and
 * the used watchpoint unit will be reopened.
 *
 * @param target Pointer to the target to remove a watchpoint from
 * @param watchpoint Pointer to the watchpoint to be removed
 * @return Result of trying to unset the watchpoint
 */
int arm7_9_remove_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	int retval = ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	if (watchpoint->set)
	{
		if ((retval = arm7_9_unset_watchpoint(target, watchpoint)) != ERROR_OK)
		{
			return retval;
		}
	}

	arm7_9->wp_available++;

	return ERROR_OK;
}

/**
 * Restarts the target by sending a RESTART instruction and moving the JTAG
 * state to IDLE.  This includes a timeout waiting for DBGACK and SYSCOMP to be
 * asserted by the processor.
 *
 * @param target Pointer to target to issue commands to
 * @return Error status if there is a timeout or a problem while executing the
 *         JTAG queue
 */
int arm7_9_execute_sys_speed(struct target *target)
{
	int retval;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct arm_jtag *jtag_info = &arm7_9->jtag_info;
	struct reg *dbg_stat = &arm7_9->eice_cache->reg_list[EICE_DBG_STAT];

	/* set RESTART instruction */
	jtag_set_end_state(TAP_IDLE);
	if (arm7_9->need_bypass_before_restart) {
		arm7_9->need_bypass_before_restart = 0;
		arm_jtag_set_instr(jtag_info, 0xf, NULL);
	}
	arm_jtag_set_instr(jtag_info, 0x4, NULL);

	long long then = timeval_ms();
	int timeout;
	while (!(timeout = ((timeval_ms()-then) > 1000)))
	{
		/* read debug status register */
		embeddedice_read_reg(dbg_stat);
		if ((retval = jtag_execute_queue()) != ERROR_OK)
			return retval;
		if ((buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_DBGACK, 1))
				   && (buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_SYSCOMP, 1)))
			break;
		if (debug_level >= 3)
		{
			alive_sleep(100);
		} else
		{
			keep_alive();
		}
	}
	if (timeout)
	{
		LOG_ERROR("timeout waiting for SYSCOMP & DBGACK, last DBG_STATUS: %" PRIx32 "", buf_get_u32(dbg_stat->value, 0, dbg_stat->size));
		return ERROR_TARGET_TIMEOUT;
	}

	return ERROR_OK;
}

/**
 * Restarts the target by sending a RESTART instruction and moving the JTAG
 * state to IDLE.  This validates that DBGACK and SYSCOMP are set without
 * waiting until they are.
 *
 * @param target Pointer to the target to issue commands to
 * @return Always ERROR_OK
 */
int arm7_9_execute_fast_sys_speed(struct target *target)
{
	static int set = 0;
	static uint8_t check_value[4], check_mask[4];

	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct arm_jtag *jtag_info = &arm7_9->jtag_info;
	struct reg *dbg_stat = &arm7_9->eice_cache->reg_list[EICE_DBG_STAT];

	/* set RESTART instruction */
	jtag_set_end_state(TAP_IDLE);
	if (arm7_9->need_bypass_before_restart) {
		arm7_9->need_bypass_before_restart = 0;
		arm_jtag_set_instr(jtag_info, 0xf, NULL);
	}
	arm_jtag_set_instr(jtag_info, 0x4, NULL);

	if (!set)
	{
		/* check for DBGACK and SYSCOMP set (others don't care) */

		/* NB! These are constants that must be available until after next jtag_execute() and
		 * we evaluate the values upon first execution in lieu of setting up these constants
		 * during early setup.
		 * */
		buf_set_u32(check_value, 0, 32, 0x9);
		buf_set_u32(check_mask, 0, 32, 0x9);
		set = 1;
	}

	/* read debug status register */
	embeddedice_read_reg_w_check(dbg_stat, check_value, check_mask);

	return ERROR_OK;
}

/**
 * Get some data from the ARM7/9 target.
 *
 * @param target Pointer to the ARM7/9 target to read data from
 * @param size The number of 32bit words to be read
 * @param buffer Pointer to the buffer that will hold the data
 * @return The result of receiving data from the Embedded ICE unit
 */
int arm7_9_target_request_data(struct target *target, uint32_t size, uint8_t *buffer)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct arm_jtag *jtag_info = &arm7_9->jtag_info;
	uint32_t *data;
	int retval = ERROR_OK;
	uint32_t i;

	data = malloc(size * (sizeof(uint32_t)));

	retval = embeddedice_receive(jtag_info, data, size);

	/* return the 32-bit ints in the 8-bit array */
	for (i = 0; i < size; i++)
	{
		h_u32_to_le(buffer + (i * 4), data[i]);
	}

	free(data);

	return retval;
}

/**
 * Handles requests to an ARM7/9 target.  If debug messaging is enabled, the
 * target is running and the DCC control register has the W bit high, this will
 * execute the request on the target.
 *
 * @param priv Void pointer expected to be a struct target pointer
 * @return ERROR_OK unless there are issues with the JTAG queue or when reading
 *                  from the Embedded ICE unit
 */
int arm7_9_handle_target_request(void *priv)
{
	int retval = ERROR_OK;
	struct target *target = priv;
	if (!target_was_examined(target))
		return ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct arm_jtag *jtag_info = &arm7_9->jtag_info;
	struct reg *dcc_control = &arm7_9->eice_cache->reg_list[EICE_COMMS_CTRL];

	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING)
	{
		/* read DCC control register */
		embeddedice_read_reg(dcc_control);
		if ((retval = jtag_execute_queue()) != ERROR_OK)
		{
			return retval;
		}

		/* check W bit */
		if (buf_get_u32(dcc_control->value, 1, 1) == 1)
		{
			uint32_t request;

			if ((retval = embeddedice_receive(jtag_info, &request, 1)) != ERROR_OK)
			{
				return retval;
			}
			if ((retval = target_request(target, request)) != ERROR_OK)
			{
				return retval;
			}
		}
	}

	return ERROR_OK;
}

/**
 * Polls an ARM7/9 target for its current status.  If DBGACK is set, the target
 * is manipulated to the right halted state based on its current state.  This is
 * what happens:
 *
 * <table>
 * 		<tr><th > State</th><th > Action</th></tr>
 * 		<tr><td > TARGET_RUNNING | TARGET_RESET</td><td > Enters debug mode.  If TARGET_RESET, pc may be checked</td></tr>
 * 		<tr><td > TARGET_UNKNOWN</td><td > Warning is logged</td></tr>
 * 		<tr><td > TARGET_DEBUG_RUNNING</td><td > Enters debug mode</td></tr>
 * 		<tr><td > TARGET_HALTED</td><td > Nothing</td></tr>
 * </table>
 *
 * If the target does not end up in the halted state, a warning is produced.  If
 * DBGACK is cleared, then the target is expected to either be running or
 * running in debug.
 *
 * @param target Pointer to the ARM7/9 target to poll
 * @return ERROR_OK or an error status if a command fails
 */
int arm7_9_poll(struct target *target)
{
	int retval;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct reg *dbg_stat = &arm7_9->eice_cache->reg_list[EICE_DBG_STAT];

	/* read debug status register */
	embeddedice_read_reg(dbg_stat);
	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		return retval;
	}

	if (buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_DBGACK, 1))
	{
/*		LOG_DEBUG("DBGACK set, dbg_state->value: 0x%x", buf_get_u32(dbg_stat->value, 0, 32));*/
		if (target->state == TARGET_UNKNOWN)
		{
			/* Starting OpenOCD with target in debug-halt */
			target->state = TARGET_RUNNING;
			LOG_DEBUG("DBGACK already set during server startup.");
		}
		if ((target->state == TARGET_RUNNING) || (target->state == TARGET_RESET))
		{
			int check_pc = 0;
			if (target->state == TARGET_RESET)
			{
				if (target->reset_halt)
				{
					enum reset_types jtag_reset_config = jtag_get_reset_config();
					if ((jtag_reset_config & RESET_SRST_PULLS_TRST) == 0)
					{
						check_pc = 1;
					}
				}
			}

			target->state = TARGET_HALTED;

			if ((retval = arm7_9_debug_entry(target)) != ERROR_OK)
				return retval;

			if (check_pc)
			{
				struct reg *reg = register_get_by_name(target->reg_cache, "pc", 1);
				uint32_t t=*((uint32_t *)reg->value);
				if (t != 0)
				{
					LOG_ERROR("PC was not 0. Does this target need srst_pulls_trst?");
				}
			}

			if ((retval = target_call_event_callbacks(target, TARGET_EVENT_HALTED)) != ERROR_OK)
			{
				return retval;
			}
		}
		if (target->state == TARGET_DEBUG_RUNNING)
		{
			target->state = TARGET_HALTED;
			if ((retval = arm7_9_debug_entry(target)) != ERROR_OK)
				return retval;

			if ((retval = target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED)) != ERROR_OK)
			{
				return retval;
			}
		}
		if (target->state != TARGET_HALTED)
		{
			LOG_WARNING("DBGACK set, but the target did not end up in the halted state %d", target->state);
		}
	}
	else
	{
		if (target->state != TARGET_DEBUG_RUNNING)
			target->state = TARGET_RUNNING;
	}

	return ERROR_OK;
}

/**
 * Asserts the reset (SRST) on an ARM7/9 target.  Some -S targets (ARM966E-S in
 * the STR912 isn't affected, ARM926EJ-S in the LPC3180 and AT91SAM9260 is
 * affected) completely stop the JTAG clock while the core is held in reset
 * (SRST).  It isn't possible to program the halt condition once reset is
 * asserted, hence a hook that allows the target to set up its reset-halt
 * condition is setup prior to asserting reset.
 *
 * @param target Pointer to an ARM7/9 target to assert reset on
 * @return ERROR_FAIL if the JTAG device does not have SRST, otherwise ERROR_OK
 */
int arm7_9_assert_reset(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	LOG_DEBUG("target->state: %s",
		  target_state_name(target));

	enum reset_types jtag_reset_config = jtag_get_reset_config();
	if (!(jtag_reset_config & RESET_HAS_SRST))
	{
		LOG_ERROR("Can't assert SRST");
		return ERROR_FAIL;
	}

	/* At this point trst has been asserted/deasserted once. We would
	 * like to program EmbeddedICE while SRST is asserted, instead of
	 * depending on SRST to leave that module alone.  However, many CPUs
	 * gate the JTAG clock while SRST is asserted; or JTAG may need
	 * clock stability guarantees (adaptive clocking might help).
	 *
	 * So we assume JTAG access during SRST is off the menu unless it's
	 * been specifically enabled.
	 */
	bool srst_asserted = false;

	if (((jtag_reset_config & RESET_SRST_PULLS_TRST) == 0)
			&& (jtag_reset_config & RESET_SRST_NO_GATING))
	{
		jtag_add_reset(0, 1);
		srst_asserted = true;
	}

	if (target->reset_halt)
	{
		/*
		 * Some targets do not support communication while SRST is asserted. We need to
		 * set up the reset vector catch here.
		 *
		 * If TRST is asserted, then these settings will be reset anyway, so setting them
		 * here is harmless.
		 */
		if (arm7_9->has_vector_catch)
		{
			/* program vector catch register to catch reset vector */
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_VEC_CATCH], 0x1);

			/* extra runtest added as issues were found with certain ARM9 cores (maybe more) - AT91SAM9260 and STR9 */
			jtag_add_runtest(1, jtag_get_end_state());
		}
		else
		{
			/* program watchpoint unit to match on reset vector address */
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_VALUE], 0x0);
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], 0x3);
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], 0xffffffff);
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
		}
	}

	/* here we should issue an SRST only, but we may have to assert TRST as well */
	if (jtag_reset_config & RESET_SRST_PULLS_TRST)
	{
		jtag_add_reset(1, 1);
	} else if (!srst_asserted)
	{
		jtag_add_reset(0, 1);
	}

	target->state = TARGET_RESET;
	jtag_add_sleep(50000);

	armv4_5_invalidate_core_regs(target);

	if ((target->reset_halt) && ((jtag_reset_config & RESET_SRST_PULLS_TRST) == 0))
	{
		/* debug entry was already prepared in arm7_9_assert_reset() */
		target->debug_reason = DBG_REASON_DBGRQ;
	}

	return ERROR_OK;
}

/**
 * Deassert the reset (SRST) signal on an ARM7/9 target.  If SRST pulls TRST
 * and the target is being reset into a halt, a warning will be triggered
 * because it is not possible to reset into a halted mode in this case.  The
 * target is halted using the target's functions.
 *
 * @param target Pointer to the target to have the reset deasserted
 * @return ERROR_OK or an error from polling or halting the target
 */
int arm7_9_deassert_reset(struct target *target)
{
	int retval = ERROR_OK;
	LOG_DEBUG("target->state: %s",
		target_state_name(target));

	/* deassert reset lines */
	jtag_add_reset(0, 0);

	enum reset_types jtag_reset_config = jtag_get_reset_config();
	if (target->reset_halt && (jtag_reset_config & RESET_SRST_PULLS_TRST) != 0)
	{
		LOG_WARNING("srst pulls trst - can not reset into halted mode. Issuing halt after reset.");
		/* set up embedded ice registers again */
		if ((retval = target_examine_one(target)) != ERROR_OK)
			return retval;

		if ((retval = target_poll(target)) != ERROR_OK)
		{
			return retval;
		}

		if ((retval = target_halt(target)) != ERROR_OK)
		{
			return retval;
		}

	}
	return retval;
}

/**
 * Clears the halt condition for an ARM7/9 target.  If it isn't coming out of
 * reset and if DBGRQ is used, it is progammed to be deasserted.  If the reset
 * vector catch was used, it is restored.  Otherwise, the control value is
 * restored and the watchpoint unit is restored if it was in use.
 *
 * @param target Pointer to the ARM7/9 target to have halt cleared
 * @return Always ERROR_OK
 */
int arm7_9_clear_halt(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct reg *dbg_ctrl = &arm7_9->eice_cache->reg_list[EICE_DBG_CTRL];

	/* we used DBGRQ only if we didn't come out of reset */
	if (!arm7_9->debug_entry_from_reset && arm7_9->use_dbgrq)
	{
		/* program EmbeddedICE Debug Control Register to deassert DBGRQ
		 */
		buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGRQ, 1, 0);
		embeddedice_store_reg(dbg_ctrl);
	}
	else
	{
		if (arm7_9->debug_entry_from_reset && arm7_9->has_vector_catch)
		{
			/* if we came out of reset, and vector catch is supported, we used
			 * vector catch to enter debug state
			 * restore the register in that case
			 */
			embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_VEC_CATCH]);
		}
		else
		{
			/* restore registers if watchpoint unit 0 was in use
			 */
			if (arm7_9->wp0_used)
			{
				if (arm7_9->debug_entry_from_reset)
				{
					embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_VALUE]);
				}
				embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK]);
				embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK]);
				embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK]);
			}
			/* control value always has to be restored, as it was either disabled,
			 * or enabled with possibly different bits
			 */
			embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE]);
		}
	}

	return ERROR_OK;
}

/**
 * Issue a software reset and halt to an ARM7/9 target.  The target is halted
 * and then there is a wait until the processor shows the halt.  This wait can
 * timeout and results in an error being returned.  The software reset involves
 * clearing the halt, updating the debug control register, changing to ARM mode,
 * reset of the program counter, and reset of all of the registers.
 *
 * @param target Pointer to the ARM7/9 target to be reset and halted by software
 * @return Error status if any of the commands fail, otherwise ERROR_OK
 */
int arm7_9_soft_reset_halt(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	struct reg *dbg_stat = &arm7_9->eice_cache->reg_list[EICE_DBG_STAT];
	struct reg *dbg_ctrl = &arm7_9->eice_cache->reg_list[EICE_DBG_CTRL];
	int i;
	int retval;

	/* FIX!!! replace some of this code with tcl commands
	 *
	 * halt # the halt command is synchronous
	 * armv4_5 core_state arm
	 *
	 */

	if ((retval = target_halt(target)) != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	int timeout;
	while (!(timeout = ((timeval_ms()-then) > 1000)))
	{
		if (buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_DBGACK, 1) != 0)
			break;
		embeddedice_read_reg(dbg_stat);
		if ((retval = jtag_execute_queue()) != ERROR_OK)
			return retval;
		if (debug_level >= 3)
		{
			alive_sleep(100);
		} else
		{
			keep_alive();
		}
	}
	if (timeout)
	{
		LOG_ERROR("Failed to halt CPU after 1 sec");
		return ERROR_TARGET_TIMEOUT;
	}
	target->state = TARGET_HALTED;

	/* program EmbeddedICE Debug Control Register to assert DBGACK and INTDIS
	 * ensure that DBGRQ is cleared
	 */
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGACK, 1, 1);
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGRQ, 1, 0);
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_INTDIS, 1, 1);
	embeddedice_store_reg(dbg_ctrl);

	if ((retval = arm7_9_clear_halt(target)) != ERROR_OK)
	{
		return retval;
	}

	/* if the target is in Thumb state, change to ARM state */
	if (buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_ITBIT, 1))
	{
		uint32_t r0_thumb, pc_thumb;
		LOG_DEBUG("target entered debug from Thumb state, changing to ARM");
		/* Entered debug from Thumb mode */
		armv4_5->core_state = ARMV4_5_STATE_THUMB;
		arm7_9->change_to_arm(target, &r0_thumb, &pc_thumb);
	}

	/* all register content is now invalid */
	if ((retval = armv4_5_invalidate_core_regs(target)) != ERROR_OK)
	{
		return retval;
	}

	/* SVC, ARM state, IRQ and FIQ disabled */
	buf_set_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8, 0xd3);
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty = 1;
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].valid = 1;

	/* start fetching from 0x0 */
	buf_set_u32(armv4_5->core_cache->reg_list[15].value, 0, 32, 0x0);
	armv4_5->core_cache->reg_list[15].dirty = 1;
	armv4_5->core_cache->reg_list[15].valid = 1;

	armv4_5->core_mode = ARMV4_5_MODE_SVC;
	armv4_5->core_state = ARMV4_5_STATE_ARM;

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	/* reset registers */
	for (i = 0; i <= 14; i++)
	{
		buf_set_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).value, 0, 32, 0xffffffff);
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).dirty = 1;
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).valid = 1;
	}

	if ((retval = target_call_event_callbacks(target, TARGET_EVENT_HALTED)) != ERROR_OK)
	{
		return retval;
	}

	return ERROR_OK;
}

/**
 * Halt an ARM7/9 target.  This is accomplished by either asserting the DBGRQ
 * line or by programming a watchpoint to trigger on any address.  It is
 * considered a bug to call this function while the target is in the
 * TARGET_RESET state.
 *
 * @param target Pointer to the ARM7/9 target to be halted
 * @return Always ERROR_OK
 */
int arm7_9_halt(struct target *target)
{
	if (target->state == TARGET_RESET)
	{
		LOG_ERROR("BUG: arm7/9 does not support halt during reset. This is handled in arm7_9_assert_reset()");
		return ERROR_OK;
	}

	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct reg *dbg_ctrl = &arm7_9->eice_cache->reg_list[EICE_DBG_CTRL];

	LOG_DEBUG("target->state: %s",
		  target_state_name(target));

	if (target->state == TARGET_HALTED)
	{
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
	{
		LOG_WARNING("target was in unknown state when halt was requested");
	}

	if (arm7_9->use_dbgrq)
	{
		/* program EmbeddedICE Debug Control Register to assert DBGRQ
		 */
		if (arm7_9->set_special_dbgrq) {
			arm7_9->set_special_dbgrq(target);
		} else {
			buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGRQ, 1, 1);
			embeddedice_store_reg(dbg_ctrl);
		}
	}
	else
	{
		/* program watchpoint unit to match on any address
		 */
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
	}

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

/**
 * Handle an ARM7/9 target's entry into debug mode.  The halt is cleared on the
 * ARM.  The JTAG queue is then executed and the reason for debug entry is
 * examined.  Once done, the target is verified to be halted and the processor
 * is forced into ARM mode.  The core registers are saved for the current core
 * mode and the program counter (register 15) is updated as needed.  The core
 * registers and CPSR and SPSR are saved for restoration later.
 *
 * @param target Pointer to target that is entering debug mode
 * @return Error code if anything fails, otherwise ERROR_OK
 */
int arm7_9_debug_entry(struct target *target)
{
	int i;
	uint32_t context[16];
	uint32_t* context_p[16];
	uint32_t r0_thumb, pc_thumb;
	uint32_t cpsr;
	int retval;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	struct reg *dbg_stat = &arm7_9->eice_cache->reg_list[EICE_DBG_STAT];
	struct reg *dbg_ctrl = &arm7_9->eice_cache->reg_list[EICE_DBG_CTRL];

#ifdef _DEBUG_ARM7_9_
	LOG_DEBUG("-");
#endif

	/* program EmbeddedICE Debug Control Register to assert DBGACK and INTDIS
	 * ensure that DBGRQ is cleared
	 */
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGACK, 1, 1);
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGRQ, 1, 0);
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_INTDIS, 1, 1);
	embeddedice_store_reg(dbg_ctrl);

	if ((retval = arm7_9_clear_halt(target)) != ERROR_OK)
	{
		return retval;
	}

	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		return retval;
	}

	if ((retval = arm7_9->examine_debug_reason(target)) != ERROR_OK)
		return retval;


	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* if the target is in Thumb state, change to ARM state */
	if (buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_ITBIT, 1))
	{
		LOG_DEBUG("target entered debug from Thumb state");
		/* Entered debug from Thumb mode */
		armv4_5->core_state = ARMV4_5_STATE_THUMB;
		arm7_9->change_to_arm(target, &r0_thumb, &pc_thumb);
		LOG_DEBUG("r0_thumb: 0x%8.8" PRIx32 ", pc_thumb: 0x%8.8" PRIx32 "", r0_thumb, pc_thumb);
	}
	else
	{
		LOG_DEBUG("target entered debug from ARM state");
		/* Entered debug from ARM mode */
		armv4_5->core_state = ARMV4_5_STATE_ARM;
	}

	for (i = 0; i < 16; i++)
		context_p[i] = &context[i];
	/* save core registers (r0 - r15 of current core mode) */
	arm7_9->read_core_regs(target, 0xffff, context_p);

	arm7_9->read_xpsr(target, &cpsr, 0);

	if ((retval = jtag_execute_queue()) != ERROR_OK)
		return retval;

	/* if the core has been executing in Thumb state, set the T bit */
	if (armv4_5->core_state == ARMV4_5_STATE_THUMB)
		cpsr |= 0x20;

	buf_set_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 32, cpsr);
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty = 0;
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].valid = 1;

	armv4_5->core_mode = cpsr & 0x1f;

	if (armv4_5_mode_to_number(armv4_5->core_mode) == -1)
	{
		target->state = TARGET_UNKNOWN;
		LOG_ERROR("cpsr contains invalid mode value - communication failure");
		return ERROR_TARGET_FAILURE;
	}

	LOG_DEBUG("target entered debug state in %s mode", armv4_5_mode_strings[armv4_5_mode_to_number(armv4_5->core_mode)]);

	if (armv4_5->core_state == ARMV4_5_STATE_THUMB)
	{
		LOG_DEBUG("thumb state, applying fixups");
		context[0] = r0_thumb;
		context[15] = pc_thumb;
	} else if (armv4_5->core_state == ARMV4_5_STATE_ARM)
	{
		/* adjust value stored by STM */
		context[15] -= 3 * 4;
	}

	if ((target->debug_reason != DBG_REASON_DBGRQ) || (!arm7_9->use_dbgrq))
		context[15] -= 3 * ((armv4_5->core_state == ARMV4_5_STATE_ARM) ? 4 : 2);
	else
		context[15] -= arm7_9->dbgreq_adjust_pc * ((armv4_5->core_state == ARMV4_5_STATE_ARM) ? 4 : 2);

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	for (i = 0; i <= 15; i++)
	{
		LOG_DEBUG("r%i: 0x%8.8" PRIx32 "", i, context[i]);
		buf_set_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).value, 0, 32, context[i]);
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).dirty = 0;
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).valid = 1;
	}

	LOG_DEBUG("entered debug state at PC 0x%" PRIx32 "", context[15]);

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	/* exceptions other than USR & SYS have a saved program status register */
	if ((armv4_5->core_mode != ARMV4_5_MODE_USR) && (armv4_5->core_mode != ARMV4_5_MODE_SYS))
	{
		uint32_t spsr;
		arm7_9->read_xpsr(target, &spsr, 1);
		if ((retval = jtag_execute_queue()) != ERROR_OK)
		{
			return retval;
		}
		buf_set_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 16).value, 0, 32, spsr);
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 16).dirty = 0;
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 16).valid = 1;
	}

	/* r0 and r15 (pc) have to be restored later */
	ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 0).dirty = ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 0).valid;
	ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 15).dirty = ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, 15).valid;

	if ((retval = jtag_execute_queue()) != ERROR_OK)
		return retval;

	if (arm7_9->post_debug_entry)
		arm7_9->post_debug_entry(target);

	return ERROR_OK;
}

/**
 * Validate the full context for an ARM7/9 target in all processor modes.  If
 * there are any invalid registers for the target, they will all be read.  This
 * includes the PSR.
 *
 * @param target Pointer to the ARM7/9 target to capture the full context from
 * @return Error if the target is not halted, has an invalid core mode, or if
 *         the JTAG queue fails to execute
 */
int arm7_9_full_context(struct target *target)
{
	int i;
	int retval;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;

	LOG_DEBUG("-");

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	/* iterate through processor modes (User, FIQ, IRQ, SVC, ABT, UND)
	 * SYS shares registers with User, so we don't touch SYS
	 */
	for (i = 0; i < 6; i++)
	{
		uint32_t mask = 0;
		uint32_t* reg_p[16];
		int j;
		int valid = 1;

		/* check if there are invalid registers in the current mode
		 */
		for (j = 0; j <= 16; j++)
		{
			if (ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j).valid == 0)
				valid = 0;
		}

		if (!valid)
		{
			uint32_t tmp_cpsr;

			/* change processor mode (and mask T bit) */
			tmp_cpsr = buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & 0xE0;
			tmp_cpsr |= armv4_5_number_to_mode(i);
			tmp_cpsr &= ~0x20;
			arm7_9->write_xpsr_im8(target, tmp_cpsr & 0xff, 0, 0);

			for (j = 0; j < 15; j++)
			{
				if (ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j).valid == 0)
				{
					reg_p[j] = (uint32_t*)ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j).value;
					mask |= 1 << j;
					ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j).valid = 1;
					ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j).dirty = 0;
				}
			}

			/* if only the PSR is invalid, mask is all zeroes */
			if (mask)
				arm7_9->read_core_regs(target, mask, reg_p);

			/* check if the PSR has to be read */
			if (ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), 16).valid == 0)
			{
				arm7_9->read_xpsr(target, (uint32_t*)ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), 16).value, 1);
				ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), 16).valid = 1;
				ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), 16).dirty = 0;
			}
		}
	}

	/* restore processor mode (mask T bit) */
	arm7_9->write_xpsr_im8(target, buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & ~0x20, 0, 0);

	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		return retval;
	}
	return ERROR_OK;
}

/**
 * Restore the processor context on an ARM7/9 target.  The full processor
 * context is analyzed to see if any of the registers are dirty on this end, but
 * have a valid new value.  If this is the case, the processor is changed to the
 * appropriate mode and the new register values are written out to the
 * processor.  If there happens to be a dirty register with an invalid value, an
 * error will be logged.
 *
 * @param target Pointer to the ARM7/9 target to have its context restored
 * @return Error status if the target is not halted or the core mode in the
 *         armv4_5 struct is invalid.
 */
int arm7_9_restore_context(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	struct reg *reg;
	struct armv4_5_core_reg *reg_arch_info;
	enum armv4_5_mode current_mode = armv4_5->core_mode;
	int i, j;
	int dirty;
	int mode_change;

	LOG_DEBUG("-");

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (arm7_9->pre_restore_context)
		arm7_9->pre_restore_context(target);

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	/* iterate through processor modes (User, FIQ, IRQ, SVC, ABT, UND)
	 * SYS shares registers with User, so we don't touch SYS
	 */
	for (i = 0; i < 6; i++)
	{
		LOG_DEBUG("examining %s mode", armv4_5_mode_strings[i]);
		dirty = 0;
		mode_change = 0;
		/* check if there are dirty registers in the current mode
		*/
		for (j = 0; j <= 16; j++)
		{
			reg = &ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j);
			reg_arch_info = reg->arch_info;
			if (reg->dirty == 1)
			{
				if (reg->valid == 1)
				{
					dirty = 1;
					LOG_DEBUG("examining dirty reg: %s", reg->name);
					if ((reg_arch_info->mode != ARMV4_5_MODE_ANY)
						&& (reg_arch_info->mode != current_mode)
						&& !((reg_arch_info->mode == ARMV4_5_MODE_USR) && (armv4_5->core_mode == ARMV4_5_MODE_SYS))
						&& !((reg_arch_info->mode == ARMV4_5_MODE_SYS) && (armv4_5->core_mode == ARMV4_5_MODE_USR)))
					{
						mode_change = 1;
						LOG_DEBUG("require mode change");
					}
				}
				else
				{
					LOG_ERROR("BUG: dirty register '%s', but no valid data", reg->name);
				}
			}
		}

		if (dirty)
		{
			uint32_t mask = 0x0;
			int num_regs = 0;
			uint32_t regs[16];

			if (mode_change)
			{
				uint32_t tmp_cpsr;

				/* change processor mode (mask T bit) */
				tmp_cpsr = buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & 0xE0;
				tmp_cpsr |= armv4_5_number_to_mode(i);
				tmp_cpsr &= ~0x20;
				arm7_9->write_xpsr_im8(target, tmp_cpsr & 0xff, 0, 0);
				current_mode = armv4_5_number_to_mode(i);
			}

			for (j = 0; j <= 14; j++)
			{
				reg = &ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), j);
				reg_arch_info = reg->arch_info;


				if (reg->dirty == 1)
				{
					regs[j] = buf_get_u32(reg->value, 0, 32);
					mask |= 1 << j;
					num_regs++;
					reg->dirty = 0;
					reg->valid = 1;
					LOG_DEBUG("writing register %i of mode %s with value 0x%8.8" PRIx32 "", j, armv4_5_mode_strings[i], regs[j]);
				}
			}

			if (mask)
			{
				arm7_9->write_core_regs(target, mask, regs);
			}

			reg = &ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_number_to_mode(i), 16);
			reg_arch_info = reg->arch_info;
			if ((reg->dirty) && (reg_arch_info->mode != ARMV4_5_MODE_ANY))
			{
				LOG_DEBUG("writing SPSR of mode %i with value 0x%8.8" PRIx32 "", i, buf_get_u32(reg->value, 0, 32));
				arm7_9->write_xpsr(target, buf_get_u32(reg->value, 0, 32), 1);
			}
		}
	}

	if ((armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty == 0) && (armv4_5->core_mode != current_mode))
	{
		/* restore processor mode (mask T bit) */
		uint32_t tmp_cpsr;

		tmp_cpsr = buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & 0xE0;
		tmp_cpsr |= armv4_5_number_to_mode(i);
		tmp_cpsr &= ~0x20;
		LOG_DEBUG("writing lower 8 bit of cpsr with value 0x%2.2x", (unsigned)(tmp_cpsr));
		arm7_9->write_xpsr_im8(target, tmp_cpsr & 0xff, 0, 0);
	}
	else if (armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty == 1)
	{
		/* CPSR has been changed, full restore necessary (mask T bit) */
		LOG_DEBUG("writing cpsr with value 0x%8.8" PRIx32 "", buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 32));
		arm7_9->write_xpsr(target, buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 32) & ~0x20, 0);
		armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty = 0;
		armv4_5->core_cache->reg_list[ARMV4_5_CPSR].valid = 1;
	}

	/* restore PC */
	LOG_DEBUG("writing PC with value 0x%8.8" PRIx32 "", buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32));
	arm7_9->write_pc(target, buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32));
	armv4_5->core_cache->reg_list[15].dirty = 0;

	if (arm7_9->post_restore_context)
		arm7_9->post_restore_context(target);

	return ERROR_OK;
}

/**
 * Restart the core of an ARM7/9 target.  A RESTART command is sent to the
 * instruction register and the JTAG state is set to TAP_IDLE causing a core
 * restart.
 *
 * @param target Pointer to the ARM7/9 target to be restarted
 * @return Result of executing the JTAG queue
 */
int arm7_9_restart_core(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct arm_jtag *jtag_info = &arm7_9->jtag_info;

	/* set RESTART instruction */
	jtag_set_end_state(TAP_IDLE);
	if (arm7_9->need_bypass_before_restart) {
		arm7_9->need_bypass_before_restart = 0;
		arm_jtag_set_instr(jtag_info, 0xf, NULL);
	}
	arm_jtag_set_instr(jtag_info, 0x4, NULL);

	jtag_add_runtest(1, jtag_set_end_state(TAP_IDLE));
	return jtag_execute_queue();
}

/**
 * Enable the watchpoints on an ARM7/9 target.  The target's watchpoints are
 * iterated through and are set on the target if they aren't already set.
 *
 * @param target Pointer to the ARM7/9 target to enable watchpoints on
 */
void arm7_9_enable_watchpoints(struct target *target)
{
	struct watchpoint *watchpoint = target->watchpoints;

	while (watchpoint)
	{
		if (watchpoint->set == 0)
			arm7_9_set_watchpoint(target, watchpoint);
		watchpoint = watchpoint->next;
	}
}

/**
 * Enable the breakpoints on an ARM7/9 target.  The target's breakpoints are
 * iterated through and are set on the target.
 *
 * @param target Pointer to the ARM7/9 target to enable breakpoints on
 */
void arm7_9_enable_breakpoints(struct target *target)
{
	struct breakpoint *breakpoint = target->breakpoints;

	/* set any pending breakpoints */
	while (breakpoint)
	{
		arm7_9_set_breakpoint(target, breakpoint);
		breakpoint = breakpoint->next;
	}
}

int arm7_9_resume(struct target *target, int current, uint32_t address, int handle_breakpoints, int debug_execution)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	struct breakpoint *breakpoint = target->breakpoints;
	struct reg *dbg_ctrl = &arm7_9->eice_cache->reg_list[EICE_DBG_CTRL];
	int err, retval = ERROR_OK;

	LOG_DEBUG("-");

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!debug_execution)
	{
		target_free_all_working_areas(target);
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	if (!current)
		buf_set_u32(armv4_5->core_cache->reg_list[15].value, 0, 32, address);

	uint32_t current_pc;
	current_pc = buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32);

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints)
	{
		if ((breakpoint = breakpoint_find(target, buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32))))
		{
			LOG_DEBUG("unset breakpoint at 0x%8.8" PRIx32 " (id: %d)", breakpoint->address, breakpoint->unique_id );
			if ((retval = arm7_9_unset_breakpoint(target, breakpoint)) != ERROR_OK)
			{
				return retval;
			}

			/* calculate PC of next instruction */
			uint32_t next_pc;
			if ((retval = arm_simulate_step(target, &next_pc)) != ERROR_OK)
			{
				uint32_t current_opcode;
				target_read_u32(target, current_pc, &current_opcode);
				LOG_ERROR("Couldn't calculate PC of next instruction, current opcode was 0x%8.8" PRIx32 "", current_opcode);
				return retval;
			}

			LOG_DEBUG("enable single-step");
			arm7_9->enable_single_step(target, next_pc);

			target->debug_reason = DBG_REASON_SINGLESTEP;

			if ((retval = arm7_9_restore_context(target)) != ERROR_OK)
			{
				return retval;
			}

			if (armv4_5->core_state == ARMV4_5_STATE_ARM)
				arm7_9->branch_resume(target);
			else if (armv4_5->core_state == ARMV4_5_STATE_THUMB)
			{
				arm7_9->branch_resume_thumb(target);
			}
			else
			{
				LOG_ERROR("unhandled core state");
				return ERROR_FAIL;
			}

			buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGACK, 1, 0);
			embeddedice_write_reg(dbg_ctrl, buf_get_u32(dbg_ctrl->value, 0, dbg_ctrl->size));
			err = arm7_9_execute_sys_speed(target);

			LOG_DEBUG("disable single-step");
			arm7_9->disable_single_step(target);

			if (err != ERROR_OK)
			{
				if ((retval = arm7_9_set_breakpoint(target, breakpoint)) != ERROR_OK)
				{
					return retval;
				}
				target->state = TARGET_UNKNOWN;
				return err;
			}

			arm7_9_debug_entry(target);
			LOG_DEBUG("new PC after step: 0x%8.8" PRIx32 "", buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32));

			LOG_DEBUG("set breakpoint at 0x%8.8" PRIx32 "", breakpoint->address);
			if ((retval = arm7_9_set_breakpoint(target, breakpoint)) != ERROR_OK)
			{
				return retval;
			}
		}
	}

	/* enable any pending breakpoints and watchpoints */
	arm7_9_enable_breakpoints(target);
	arm7_9_enable_watchpoints(target);

	if ((retval = arm7_9_restore_context(target)) != ERROR_OK)
	{
		return retval;
	}

	if (armv4_5->core_state == ARMV4_5_STATE_ARM)
	{
		arm7_9->branch_resume(target);
	}
	else if (armv4_5->core_state == ARMV4_5_STATE_THUMB)
	{
		arm7_9->branch_resume_thumb(target);
	}
	else
	{
		LOG_ERROR("unhandled core state");
		return ERROR_FAIL;
	}

	/* deassert DBGACK and INTDIS */
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGACK, 1, 0);
	/* INTDIS only when we really resume, not during debug execution */
	if (!debug_execution)
		buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_INTDIS, 1, 0);
	embeddedice_write_reg(dbg_ctrl, buf_get_u32(dbg_ctrl->value, 0, dbg_ctrl->size));

	if ((retval = arm7_9_restart_core(target)) != ERROR_OK)
	{
		return retval;
	}

	target->debug_reason = DBG_REASON_NOTHALTED;

	if (!debug_execution)
	{
		/* registers are now invalid */
		armv4_5_invalidate_core_regs(target);
		target->state = TARGET_RUNNING;
		if ((retval = target_call_event_callbacks(target, TARGET_EVENT_RESUMED)) != ERROR_OK)
		{
			return retval;
		}
	}
	else
	{
		target->state = TARGET_DEBUG_RUNNING;
		if ((retval = target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED)) != ERROR_OK)
		{
			return retval;
		}
	}

	LOG_DEBUG("target resumed");

	return ERROR_OK;
}

void arm7_9_enable_eice_step(struct target *target, uint32_t next_pc)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	uint32_t current_pc;
	current_pc = buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32);

	if (next_pc != current_pc)
	{
		/* setup an inverse breakpoint on the current PC
		* - comparator 1 matches the current address
		* - rangeout from comparator 1 is connected to comparator 0 rangein
		* - comparator 0 matches any address, as long as rangein is low */
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], ~(EICE_W_CTRL_RANGE | EICE_W_CTRL_nOPC) & 0xff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_VALUE], current_pc);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_MASK], 0);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], 0x0);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
	}
	else
	{
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE], 0x0);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK], 0xff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_VALUE], next_pc);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_MASK], 0);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_MASK], 0xffffffff);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE], EICE_W_CTRL_ENABLE);
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_MASK], ~EICE_W_CTRL_nOPC & 0xff);
	}
}

void arm7_9_disable_eice_step(struct target *target)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_ADDR_MASK]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_DATA_MASK]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_VALUE]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W0_CONTROL_MASK]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_VALUE]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W1_ADDR_MASK]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W1_DATA_MASK]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_MASK]);
	embeddedice_store_reg(&arm7_9->eice_cache->reg_list[EICE_W1_CONTROL_VALUE]);
}

int arm7_9_step(struct target *target, int current, uint32_t address, int handle_breakpoints)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	struct breakpoint *breakpoint = NULL;
	int err, retval;

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	if (!current)
		buf_set_u32(armv4_5->core_cache->reg_list[15].value, 0, 32, address);

	uint32_t current_pc;
	current_pc = buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32);

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints)
		if ((breakpoint = breakpoint_find(target, buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32))))
			if ((retval = arm7_9_unset_breakpoint(target, breakpoint)) != ERROR_OK)
			{
				return retval;
			}

	target->debug_reason = DBG_REASON_SINGLESTEP;

	/* calculate PC of next instruction */
	uint32_t next_pc;
	if ((retval = arm_simulate_step(target, &next_pc)) != ERROR_OK)
	{
		uint32_t current_opcode;
		target_read_u32(target, current_pc, &current_opcode);
		LOG_ERROR("Couldn't calculate PC of next instruction, current opcode was 0x%8.8" PRIx32 "", current_opcode);
		return retval;
	}

	if ((retval = arm7_9_restore_context(target)) != ERROR_OK)
	{
		return retval;
	}

	arm7_9->enable_single_step(target, next_pc);

	if (armv4_5->core_state == ARMV4_5_STATE_ARM)
	{
		arm7_9->branch_resume(target);
	}
	else if (armv4_5->core_state == ARMV4_5_STATE_THUMB)
	{
		arm7_9->branch_resume_thumb(target);
	}
	else
	{
		LOG_ERROR("unhandled core state");
		return ERROR_FAIL;
	}

	if ((retval = target_call_event_callbacks(target, TARGET_EVENT_RESUMED)) != ERROR_OK)
	{
		return retval;
	}

	err = arm7_9_execute_sys_speed(target);
	arm7_9->disable_single_step(target);

	/* registers are now invalid */
	armv4_5_invalidate_core_regs(target);

	if (err != ERROR_OK)
	{
		target->state = TARGET_UNKNOWN;
	} else {
		arm7_9_debug_entry(target);
		if ((retval = target_call_event_callbacks(target, TARGET_EVENT_HALTED)) != ERROR_OK)
		{
			return retval;
		}
		LOG_DEBUG("target stepped");
	}

	if (breakpoint)
		if ((retval = arm7_9_set_breakpoint(target, breakpoint)) != ERROR_OK)
		{
			return retval;
		}

	return err;
}

int arm7_9_read_core_reg(struct target *target, int num, enum armv4_5_mode mode)
{
	uint32_t* reg_p[16];
	uint32_t value;
	int retval;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	enum armv4_5_mode reg_mode = ((struct armv4_5_core_reg*)ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).arch_info)->mode;

	if ((num < 0) || (num > 16))
		return ERROR_INVALID_ARGUMENTS;

	if ((mode != ARMV4_5_MODE_ANY)
			&& (mode != armv4_5->core_mode)
			&& (reg_mode != ARMV4_5_MODE_ANY))
	{
		uint32_t tmp_cpsr;

		/* change processor mode (mask T bit) */
		tmp_cpsr = buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & 0xE0;
		tmp_cpsr |= mode;
		tmp_cpsr &= ~0x20;
		arm7_9->write_xpsr_im8(target, tmp_cpsr & 0xff, 0, 0);
	}

	if ((num >= 0) && (num <= 15))
	{
		/* read a normal core register */
		reg_p[num] = &value;

		arm7_9->read_core_regs(target, 1 << num, reg_p);
	}
	else
	{
		/* read a program status register
		 * if the register mode is MODE_ANY, we read the cpsr, otherwise a spsr
		 */
		struct armv4_5_core_reg *arch_info = ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).arch_info;
		int spsr = (arch_info->mode == ARMV4_5_MODE_ANY) ? 0 : 1;

		arm7_9->read_xpsr(target, &value, spsr);
	}

	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		return retval;
	}

	ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).valid = 1;
	ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).dirty = 0;
	buf_set_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).value, 0, 32, value);

	if ((mode != ARMV4_5_MODE_ANY)
			&& (mode != armv4_5->core_mode)
			&& (reg_mode != ARMV4_5_MODE_ANY))	{
		/* restore processor mode (mask T bit) */
		arm7_9->write_xpsr_im8(target, buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & ~0x20, 0, 0);
	}

	return ERROR_OK;
}

int arm7_9_write_core_reg(struct target *target, int num, enum armv4_5_mode mode, uint32_t value)
{
	uint32_t reg[16];
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	enum armv4_5_mode reg_mode = ((struct armv4_5_core_reg*)ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).arch_info)->mode;

	if ((num < 0) || (num > 16))
		return ERROR_INVALID_ARGUMENTS;

	if ((mode != ARMV4_5_MODE_ANY)
			&& (mode != armv4_5->core_mode)
			&& (reg_mode != ARMV4_5_MODE_ANY))	{
		uint32_t tmp_cpsr;

		/* change processor mode (mask T bit) */
		tmp_cpsr = buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & 0xE0;
		tmp_cpsr |= mode;
		tmp_cpsr &= ~0x20;
		arm7_9->write_xpsr_im8(target, tmp_cpsr & 0xff, 0, 0);
	}

	if ((num >= 0) && (num <= 15))
	{
		/* write a normal core register */
		reg[num] = value;

		arm7_9->write_core_regs(target, 1 << num, reg);
	}
	else
	{
		/* write a program status register
		* if the register mode is MODE_ANY, we write the cpsr, otherwise a spsr
		*/
		struct armv4_5_core_reg *arch_info = ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).arch_info;
		int spsr = (arch_info->mode == ARMV4_5_MODE_ANY) ? 0 : 1;

		/* if we're writing the CPSR, mask the T bit */
		if (!spsr)
			value &= ~0x20;

		arm7_9->write_xpsr(target, value, spsr);
	}

	ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).valid = 1;
	ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, mode, num).dirty = 0;

	if ((mode != ARMV4_5_MODE_ANY)
			&& (mode != armv4_5->core_mode)
			&& (reg_mode != ARMV4_5_MODE_ANY))	{
		/* restore processor mode (mask T bit) */
		arm7_9->write_xpsr_im8(target, buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & ~0x20, 0, 0);
	}

	return jtag_execute_queue();
}

int arm7_9_read_memory(struct target *target, uint32_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	uint32_t reg[16];
	uint32_t num_accesses = 0;
	int thisrun_accesses;
	int i;
	uint32_t cpsr;
	int retval;
	int last_reg = 0;

	LOG_DEBUG("address: 0x%8.8" PRIx32 ", size: 0x%8.8" PRIx32 ", count: 0x%8.8" PRIx32 "", address, size, count);

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* sanitize arguments */
	if (((size != 4) && (size != 2) && (size != 1)) || (count == 0) || !(buffer))
		return ERROR_INVALID_ARGUMENTS;

	if (((size == 4) && (address & 0x3u)) || ((size == 2) && (address & 0x1u)))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	/* load the base register with the address of the first word */
	reg[0] = address;
	arm7_9->write_core_regs(target, 0x1, reg);

	int j = 0;

	switch (size)
	{
		case 4:
			while (num_accesses < count)
			{
				uint32_t reg_list;
				thisrun_accesses = ((count - num_accesses) >= 14) ? 14 : (count - num_accesses);
				reg_list = (0xffff >> (15 - thisrun_accesses)) & 0xfffe;

				if (last_reg <= thisrun_accesses)
					last_reg = thisrun_accesses;

				arm7_9->load_word_regs(target, reg_list);

				/* fast memory reads are only safe when the target is running
				 * from a sufficiently high clock (32 kHz is usually too slow)
				 */
				if (arm7_9->fast_memory_access)
					retval = arm7_9_execute_fast_sys_speed(target);
				else
					retval = arm7_9_execute_sys_speed(target);
				if (retval != ERROR_OK)
					return retval;

				arm7_9->read_core_regs_target_buffer(target, reg_list, buffer, 4);

				/* advance buffer, count number of accesses */
				buffer += thisrun_accesses * 4;
				num_accesses += thisrun_accesses;

				if ((j++%1024) == 0)
				{
					keep_alive();
				}
			}
			break;
		case 2:
			while (num_accesses < count)
			{
				uint32_t reg_list;
				thisrun_accesses = ((count - num_accesses) >= 14) ? 14 : (count - num_accesses);
				reg_list = (0xffff >> (15 - thisrun_accesses)) & 0xfffe;

				for (i = 1; i <= thisrun_accesses; i++)
				{
					if (i > last_reg)
						last_reg = i;
					arm7_9->load_hword_reg(target, i);
					/* fast memory reads are only safe when the target is running
					 * from a sufficiently high clock (32 kHz is usually too slow)
					 */
					if (arm7_9->fast_memory_access)
						retval = arm7_9_execute_fast_sys_speed(target);
					else
						retval = arm7_9_execute_sys_speed(target);
					if (retval != ERROR_OK)
					{
						return retval;
					}

				}

				arm7_9->read_core_regs_target_buffer(target, reg_list, buffer, 2);

				/* advance buffer, count number of accesses */
				buffer += thisrun_accesses * 2;
				num_accesses += thisrun_accesses;

				if ((j++%1024) == 0)
				{
					keep_alive();
				}
			}
			break;
		case 1:
			while (num_accesses < count)
			{
				uint32_t reg_list;
				thisrun_accesses = ((count - num_accesses) >= 14) ? 14 : (count - num_accesses);
				reg_list = (0xffff >> (15 - thisrun_accesses)) & 0xfffe;

				for (i = 1; i <= thisrun_accesses; i++)
				{
					if (i > last_reg)
						last_reg = i;
					arm7_9->load_byte_reg(target, i);
					/* fast memory reads are only safe when the target is running
					 * from a sufficiently high clock (32 kHz is usually too slow)
					 */
					if (arm7_9->fast_memory_access)
						retval = arm7_9_execute_fast_sys_speed(target);
					else
						retval = arm7_9_execute_sys_speed(target);
					if (retval != ERROR_OK)
					{
						return retval;
					}
				}

				arm7_9->read_core_regs_target_buffer(target, reg_list, buffer, 1);

				/* advance buffer, count number of accesses */
				buffer += thisrun_accesses * 1;
				num_accesses += thisrun_accesses;

				if ((j++%1024) == 0)
				{
					keep_alive();
				}
			}
			break;
		default:
			LOG_ERROR("BUG: we shouldn't get here");
			exit(-1);
			break;
	}

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	for (i = 0; i <= last_reg; i++)
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).dirty = ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).valid;

	arm7_9->read_xpsr(target, &cpsr, 0);
	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		LOG_ERROR("JTAG error while reading cpsr");
		return ERROR_TARGET_DATA_ABORT;
	}

	if (((cpsr & 0x1f) == ARMV4_5_MODE_ABT) && (armv4_5->core_mode != ARMV4_5_MODE_ABT))
	{
		LOG_WARNING("memory read caused data abort (address: 0x%8.8" PRIx32 ", size: 0x%" PRIx32 ", count: 0x%" PRIx32 ")", address, size, count);

		arm7_9->write_xpsr_im8(target, buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & ~0x20, 0, 0);

		return ERROR_TARGET_DATA_ABORT;
	}

	return ERROR_OK;
}

int arm7_9_write_memory(struct target *target, uint32_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	struct armv4_5_common_s *armv4_5 = &arm7_9->armv4_5_common;
	struct reg *dbg_ctrl = &arm7_9->eice_cache->reg_list[EICE_DBG_CTRL];

	uint32_t reg[16];
	uint32_t num_accesses = 0;
	int thisrun_accesses;
	int i;
	uint32_t cpsr;
	int retval;
	int last_reg = 0;

#ifdef _DEBUG_ARM7_9_
	LOG_DEBUG("address: 0x%8.8x, size: 0x%8.8x, count: 0x%8.8x", address, size, count);
#endif

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* sanitize arguments */
	if (((size != 4) && (size != 2) && (size != 1)) || (count == 0) || !(buffer))
		return ERROR_INVALID_ARGUMENTS;

	if (((size == 4) && (address & 0x3u)) || ((size == 2) && (address & 0x1u)))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	/* load the base register with the address of the first word */
	reg[0] = address;
	arm7_9->write_core_regs(target, 0x1, reg);

	/* Clear DBGACK, to make sure memory fetches work as expected */
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGACK, 1, 0);
	embeddedice_store_reg(dbg_ctrl);

	switch (size)
	{
		case 4:
			while (num_accesses < count)
			{
				uint32_t reg_list;
				thisrun_accesses = ((count - num_accesses) >= 14) ? 14 : (count - num_accesses);
				reg_list = (0xffff >> (15 - thisrun_accesses)) & 0xfffe;

				for (i = 1; i <= thisrun_accesses; i++)
				{
					if (i > last_reg)
						last_reg = i;
					reg[i] = target_buffer_get_u32(target, buffer);
					buffer += 4;
				}

				arm7_9->write_core_regs(target, reg_list, reg);

				arm7_9->store_word_regs(target, reg_list);

				/* fast memory writes are only safe when the target is running
				 * from a sufficiently high clock (32 kHz is usually too slow)
				 */
				if (arm7_9->fast_memory_access)
					retval = arm7_9_execute_fast_sys_speed(target);
				else
					retval = arm7_9_execute_sys_speed(target);
				if (retval != ERROR_OK)
				{
					return retval;
				}

				num_accesses += thisrun_accesses;
			}
			break;
		case 2:
			while (num_accesses < count)
			{
				uint32_t reg_list;
				thisrun_accesses = ((count - num_accesses) >= 14) ? 14 : (count - num_accesses);
				reg_list = (0xffff >> (15 - thisrun_accesses)) & 0xfffe;

				for (i = 1; i <= thisrun_accesses; i++)
				{
					if (i > last_reg)
						last_reg = i;
					reg[i] = target_buffer_get_u16(target, buffer) & 0xffff;
					buffer += 2;
				}

				arm7_9->write_core_regs(target, reg_list, reg);

				for (i = 1; i <= thisrun_accesses; i++)
				{
					arm7_9->store_hword_reg(target, i);

					/* fast memory writes are only safe when the target is running
					 * from a sufficiently high clock (32 kHz is usually too slow)
					 */
					if (arm7_9->fast_memory_access)
						retval = arm7_9_execute_fast_sys_speed(target);
					else
						retval = arm7_9_execute_sys_speed(target);
					if (retval != ERROR_OK)
					{
						return retval;
					}
				}

				num_accesses += thisrun_accesses;
			}
			break;
		case 1:
			while (num_accesses < count)
			{
				uint32_t reg_list;
				thisrun_accesses = ((count - num_accesses) >= 14) ? 14 : (count - num_accesses);
				reg_list = (0xffff >> (15 - thisrun_accesses)) & 0xfffe;

				for (i = 1; i <= thisrun_accesses; i++)
				{
					if (i > last_reg)
						last_reg = i;
					reg[i] = *buffer++ & 0xff;
				}

				arm7_9->write_core_regs(target, reg_list, reg);

				for (i = 1; i <= thisrun_accesses; i++)
				{
					arm7_9->store_byte_reg(target, i);
					/* fast memory writes are only safe when the target is running
					 * from a sufficiently high clock (32 kHz is usually too slow)
					 */
					if (arm7_9->fast_memory_access)
						retval = arm7_9_execute_fast_sys_speed(target);
					else
						retval = arm7_9_execute_sys_speed(target);
					if (retval != ERROR_OK)
					{
						return retval;
					}

				}

				num_accesses += thisrun_accesses;
			}
			break;
		default:
			LOG_ERROR("BUG: we shouldn't get here");
			exit(-1);
			break;
	}

	/* Re-Set DBGACK */
	buf_set_u32(dbg_ctrl->value, EICE_DBG_CONTROL_DBGACK, 1, 1);
	embeddedice_store_reg(dbg_ctrl);

	if (armv4_5_mode_to_number(armv4_5->core_mode)==-1)
		return ERROR_FAIL;

	for (i = 0; i <= last_reg; i++)
		ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).dirty = ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i).valid;

	arm7_9->read_xpsr(target, &cpsr, 0);
	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		LOG_ERROR("JTAG error while reading cpsr");
		return ERROR_TARGET_DATA_ABORT;
	}

	if (((cpsr & 0x1f) == ARMV4_5_MODE_ABT) && (armv4_5->core_mode != ARMV4_5_MODE_ABT))
	{
		LOG_WARNING("memory write caused data abort (address: 0x%8.8" PRIx32 ", size: 0x%" PRIx32 ", count: 0x%" PRIx32 ")", address, size, count);

		arm7_9->write_xpsr_im8(target, buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8) & ~0x20, 0, 0);

		return ERROR_TARGET_DATA_ABORT;
	}

	return ERROR_OK;
}

static int dcc_count;
static uint8_t *dcc_buffer;

static int arm7_9_dcc_completion(struct target *target, uint32_t exit_point, int timeout_ms, void *arch_info)
{
	int retval = ERROR_OK;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);

	if ((retval = target_wait_state(target, TARGET_DEBUG_RUNNING, 500)) != ERROR_OK)
		return retval;

	int little = target->endianness == TARGET_LITTLE_ENDIAN;
	int count = dcc_count;
	uint8_t *buffer = dcc_buffer;
	if (count > 2)
	{
		/* Handle first & last using standard embeddedice_write_reg and the middle ones w/the
		 * core function repeated. */
		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_COMMS_DATA], fast_target_buffer_get_u32(buffer, little));
		buffer += 4;

		struct embeddedice_reg *ice_reg = arm7_9->eice_cache->reg_list[EICE_COMMS_DATA].arch_info;
		uint8_t reg_addr = ice_reg->addr & 0x1f;
		struct jtag_tap *tap;
		tap = ice_reg->jtag_info->tap;

		embeddedice_write_dcc(tap, reg_addr, buffer, little, count-2);
		buffer += (count-2)*4;

		embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_COMMS_DATA], fast_target_buffer_get_u32(buffer, little));
	} else
	{
		int i;
		for (i = 0; i < count; i++)
		{
			embeddedice_write_reg(&arm7_9->eice_cache->reg_list[EICE_COMMS_DATA], fast_target_buffer_get_u32(buffer, little));
			buffer += 4;
		}
	}

	if ((retval = target_halt(target))!= ERROR_OK)
	{
		return retval;
	}
	return target_wait_state(target, TARGET_HALTED, 500);
}

static const uint32_t dcc_code[] =
{
	/* r0 == input, points to memory buffer
	 * r1 == scratch
	 */

	/* spin until DCC control (c0) reports data arrived */
	0xee101e10,	/* w: mrc p14, #0, r1, c0, c0 */
	0xe3110001,	/*    tst r1, #1              */
	0x0afffffc,	/*    bne w                   */

	/* read word from DCC (c1), write to memory */
	0xee111e10,	/*    mrc p14, #0, r1, c1, c0 */
	0xe4801004,	/*    str r1, [r0], #4        */

	/* repeat */
	0xeafffff9	/*    b   w                   */
};

int armv4_5_run_algorithm_inner(struct target *target, int num_mem_params, struct mem_param *mem_params, int num_reg_params, struct reg_param *reg_params, uint32_t entry_point, uint32_t exit_point, int timeout_ms, void *arch_info, int (*run_it)(struct target *target, uint32_t exit_point, int timeout_ms, void *arch_info));

int arm7_9_bulk_write_memory(struct target *target, uint32_t address, uint32_t count, uint8_t *buffer)
{
	int retval;
	struct arm7_9_common *arm7_9 = target_to_arm7_9(target);
	int i;

	if (!arm7_9->dcc_downloads)
		return target_write_memory(target, address, 4, count, buffer);

	/* regrab previously allocated working_area, or allocate a new one */
	if (!arm7_9->dcc_working_area)
	{
		uint8_t dcc_code_buf[6 * 4];

		/* make sure we have a working area */
		if (target_alloc_working_area(target, 24, &arm7_9->dcc_working_area) != ERROR_OK)
		{
			LOG_INFO("no working area available, falling back to memory writes");
			return target_write_memory(target, address, 4, count, buffer);
		}

		/* copy target instructions to target endianness */
		for (i = 0; i < 6; i++)
		{
			target_buffer_set_u32(target, dcc_code_buf + i*4, dcc_code[i]);
		}

		/* write DCC code to working area */
		if ((retval = target_write_memory(target, arm7_9->dcc_working_area->address, 4, 6, dcc_code_buf)) != ERROR_OK)
		{
			return retval;
		}
	}

	struct armv4_5_algorithm armv4_5_info;
	struct reg_param reg_params[1];

	armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
	armv4_5_info.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);

	buf_set_u32(reg_params[0].value, 0, 32, address);

	dcc_count = count;
	dcc_buffer = buffer;
	retval = armv4_5_run_algorithm_inner(target, 0, NULL, 1, reg_params,
			arm7_9->dcc_working_area->address, arm7_9->dcc_working_area->address + 6*4, 20*1000, &armv4_5_info, arm7_9_dcc_completion);

	if (retval == ERROR_OK)
	{
		uint32_t endaddress = buf_get_u32(reg_params[0].value, 0, 32);
		if (endaddress != (address + count*4))
		{
			LOG_ERROR("DCC write failed, expected end address 0x%08" PRIx32 " got 0x%0" PRIx32 "", (address + count*4), endaddress);
			retval = ERROR_FAIL;
		}
	}

	destroy_reg_param(&reg_params[0]);

	return retval;
}

int arm7_9_checksum_memory(struct target *target, uint32_t address, uint32_t count, uint32_t* checksum)
{
	struct working_area *crc_algorithm;
	struct armv4_5_algorithm armv4_5_info;
	struct reg_param reg_params[2];
	int retval;

	static const uint32_t arm7_9_crc_code[] = {
		0xE1A02000,				/* mov		r2, r0 */
		0xE3E00000,				/* mov		r0, #0xffffffff */
		0xE1A03001,				/* mov		r3, r1 */
		0xE3A04000,				/* mov		r4, #0 */
		0xEA00000B,				/* b		ncomp */
								/* nbyte: */
		0xE7D21004,				/* ldrb	r1, [r2, r4] */
		0xE59F7030,				/* ldr		r7, CRC32XOR */
		0xE0200C01,				/* eor		r0, r0, r1, asl 24 */
		0xE3A05000,				/* mov		r5, #0 */
								/* loop: */
		0xE3500000,				/* cmp		r0, #0 */
		0xE1A06080,				/* mov		r6, r0, asl #1 */
		0xE2855001,				/* add		r5, r5, #1 */
		0xE1A00006,				/* mov		r0, r6 */
		0xB0260007,				/* eorlt	r0, r6, r7 */
		0xE3550008,				/* cmp		r5, #8 */
		0x1AFFFFF8,				/* bne		loop */
		0xE2844001,				/* add		r4, r4, #1 */
								/* ncomp: */
		0xE1540003,				/* cmp		r4, r3 */
		0x1AFFFFF1,				/* bne		nbyte */
								/* end: */
		0xEAFFFFFE,				/* b		end */
		0x04C11DB7				/* CRC32XOR:	.word 0x04C11DB7 */
	};

	uint32_t i;

	if (target_alloc_working_area(target, sizeof(arm7_9_crc_code), &crc_algorithm) != ERROR_OK)
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* convert flash writing code into a buffer in target endianness */
	for (i = 0; i < (sizeof(arm7_9_crc_code)/sizeof(uint32_t)); i++)
	{
		if ((retval = target_write_u32(target, crc_algorithm->address + i*sizeof(uint32_t), arm7_9_crc_code[i])) != ERROR_OK)
		{
			return retval;
		}
	}

	armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
	armv4_5_info.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);

	buf_set_u32(reg_params[0].value, 0, 32, address);
	buf_set_u32(reg_params[1].value, 0, 32, count);

	/* 20 second timeout/megabyte */
	int timeout = 20000 * (1 + (count / (1024*1024)));

	if ((retval = target_run_algorithm(target, 0, NULL, 2, reg_params,
		crc_algorithm->address, crc_algorithm->address + (sizeof(arm7_9_crc_code) - 8), timeout, &armv4_5_info)) != ERROR_OK)
	{
		LOG_ERROR("error executing arm7_9 crc algorithm");
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		target_free_working_area(target, crc_algorithm);
		return retval;
	}

	*checksum = buf_get_u32(reg_params[0].value, 0, 32);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);

	target_free_working_area(target, crc_algorithm);

	return ERROR_OK;
}

int arm7_9_blank_check_memory(struct target *target, uint32_t address, uint32_t count, uint32_t* blank)
{
	struct working_area *erase_check_algorithm;
	struct reg_param reg_params[3];
	struct armv4_5_algorithm armv4_5_info;
	int retval;
	uint32_t i;

	static const uint32_t erase_check_code[] =
	{
		/* loop: */
		0xe4d03001,		/* ldrb r3, [r0], #1 */
		0xe0022003,		/* and r2, r2, r3    */
		0xe2511001,		/* subs r1, r1, #1   */
		0x1afffffb,		/* bne loop          */
		/* end: */
		0xeafffffe		/* b end             */
	};

	/* make sure we have a working area */
	if (target_alloc_working_area(target, sizeof(erase_check_code), &erase_check_algorithm) != ERROR_OK)
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* convert flash writing code into a buffer in target endianness */
	for (i = 0; i < (sizeof(erase_check_code)/sizeof(uint32_t)); i++)
		if ((retval = target_write_u32(target, erase_check_algorithm->address + i*sizeof(uint32_t), erase_check_code[i])) != ERROR_OK)
		{
			return retval;
		}

	armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
	armv4_5_info.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	buf_set_u32(reg_params[0].value, 0, 32, address);

	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	buf_set_u32(reg_params[1].value, 0, 32, count);

	init_reg_param(&reg_params[2], "r2", 32, PARAM_IN_OUT);
	buf_set_u32(reg_params[2].value, 0, 32, 0xff);

	if ((retval = target_run_algorithm(target, 0, NULL, 3, reg_params,
			erase_check_algorithm->address, erase_check_algorithm->address + (sizeof(erase_check_code) - 4), 10000, &armv4_5_info)) != ERROR_OK)
	{
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		destroy_reg_param(&reg_params[2]);
		target_free_working_area(target, erase_check_algorithm);
		return 0;
	}

	*blank = buf_get_u32(reg_params[2].value, 0, 32);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	target_free_working_area(target, erase_check_algorithm);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_arm7_9_write_xpsr_command)
{
	uint32_t value;
	int spsr;
	int retval;
	struct target *target = get_current_target(cmd_ctx);
	struct arm *armv4_5;
	struct arm7_9_common *arm7_9;

	if (arm7_9_get_arch_pointers(target, &armv4_5, &arm7_9) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM7/ARM9 target");
		return ERROR_OK;
	}

	if (target->state != TARGET_HALTED)
	{
		command_print(cmd_ctx, "can't write registers while running");
		return ERROR_OK;
	}

	if (argc < 2)
	{
		command_print(cmd_ctx, "usage: write_xpsr <value> <not cpsr | spsr>");
		return ERROR_OK;
	}

	COMMAND_PARSE_NUMBER(u32, args[0], value);
	COMMAND_PARSE_NUMBER(int, args[1], spsr);

	/* if we're writing the CPSR, mask the T bit */
	if (!spsr)
		value &= ~0x20;

	arm7_9->write_xpsr(target, value, spsr);
	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		LOG_ERROR("JTAG error while writing to xpsr");
		return retval;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_arm7_9_write_xpsr_im8_command)
{
	uint32_t value;
	int rotate;
	int spsr;
	int retval;
	struct target *target = get_current_target(cmd_ctx);
	struct arm *armv4_5;
	struct arm7_9_common *arm7_9;

	if (arm7_9_get_arch_pointers(target, &armv4_5, &arm7_9) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM7/ARM9 target");
		return ERROR_OK;
	}

	if (target->state != TARGET_HALTED)
	{
		command_print(cmd_ctx, "can't write registers while running");
		return ERROR_OK;
	}

	if (argc < 3)
	{
		command_print(cmd_ctx, "usage: write_xpsr_im8 <im8> <rotate> <not cpsr | spsr>");
		return ERROR_OK;
	}

	COMMAND_PARSE_NUMBER(u32, args[0], value);
	COMMAND_PARSE_NUMBER(int, args[1], rotate);
	COMMAND_PARSE_NUMBER(int, args[2], spsr);

	arm7_9->write_xpsr_im8(target, value, rotate, spsr);
	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		LOG_ERROR("JTAG error while writing 8-bit immediate to xpsr");
		return retval;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_arm7_9_write_core_reg_command)
{
	uint32_t value;
	uint32_t mode;
	int num;
	struct target *target = get_current_target(cmd_ctx);
	struct arm *armv4_5;
	struct arm7_9_common *arm7_9;

	if (arm7_9_get_arch_pointers(target, &armv4_5, &arm7_9) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM7/ARM9 target");
		return ERROR_OK;
	}

	if (target->state != TARGET_HALTED)
	{
		command_print(cmd_ctx, "can't write registers while running");
		return ERROR_OK;
	}

	if (argc < 3)
	{
		command_print(cmd_ctx, "usage: write_core_reg <num> <mode> <value>");
		return ERROR_OK;
	}

	COMMAND_PARSE_NUMBER(int, args[0], num);
	COMMAND_PARSE_NUMBER(u32, args[1], mode);
	COMMAND_PARSE_NUMBER(u32, args[2], value);

	return arm7_9_write_core_reg(target, num, mode, value);
}

COMMAND_HANDLER(handle_arm7_9_dbgrq_command)
{
	struct target *target = get_current_target(cmd_ctx);
	struct arm *armv4_5;
	struct arm7_9_common *arm7_9;

	if (arm7_9_get_arch_pointers(target, &armv4_5, &arm7_9) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM7/ARM9 target");
		return ERROR_OK;
	}

	if (argc > 0)
	{
		if (strcmp("enable", args[0]) == 0)
		{
			arm7_9->use_dbgrq = 1;
		}
		else if (strcmp("disable", args[0]) == 0)
		{
			arm7_9->use_dbgrq = 0;
		}
		else
		{
			command_print(cmd_ctx, "usage: arm7_9 dbgrq <enable | disable>");
		}
	}

	command_print(cmd_ctx, "use of EmbeddedICE dbgrq instead of breakpoint for target halt %s", (arm7_9->use_dbgrq) ? "enabled" : "disabled");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_arm7_9_fast_memory_access_command)
{
	struct target *target = get_current_target(cmd_ctx);
	struct arm *armv4_5;
	struct arm7_9_common *arm7_9;

	if (arm7_9_get_arch_pointers(target, &armv4_5, &arm7_9) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM7/ARM9 target");
		return ERROR_OK;
	}

	if (argc > 0)
	{
		if (strcmp("enable", args[0]) == 0)
		{
			arm7_9->fast_memory_access = 1;
		}
		else if (strcmp("disable", args[0]) == 0)
		{
			arm7_9->fast_memory_access = 0;
		}
		else
		{
			command_print(cmd_ctx, "usage: arm7_9 fast_memory_access <enable | disable>");
		}
	}

	command_print(cmd_ctx, "fast memory access is %s", (arm7_9->fast_memory_access) ? "enabled" : "disabled");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_arm7_9_dcc_downloads_command)
{
	struct target *target = get_current_target(cmd_ctx);
	struct arm *armv4_5;
	struct arm7_9_common *arm7_9;

	if (arm7_9_get_arch_pointers(target, &armv4_5, &arm7_9) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM7/ARM9 target");
		return ERROR_OK;
	}

	if (argc > 0)
	{
		if (strcmp("enable", args[0]) == 0)
		{
			arm7_9->dcc_downloads = 1;
		}
		else if (strcmp("disable", args[0]) == 0)
		{
			arm7_9->dcc_downloads = 0;
		}
		else
		{
			command_print(cmd_ctx, "usage: arm7_9 dcc_downloads <enable | disable>");
		}
	}

	command_print(cmd_ctx, "dcc downloads are %s", (arm7_9->dcc_downloads) ? "enabled" : "disabled");

	return ERROR_OK;
}

int arm7_9_init_arch_info(struct target *target, struct arm7_9_common *arm7_9)
{
	int retval = ERROR_OK;
	struct arm *armv4_5 = &arm7_9->armv4_5_common;

	arm7_9->common_magic = ARM7_9_COMMON_MAGIC;

	if ((retval = arm_jtag_setup_connection(&arm7_9->jtag_info)) != ERROR_OK)
		return retval;

	/* caller must have allocated via calloc(), so everything's zeroed */

	arm7_9->wp_available_max = 2;

	arm7_9->fast_memory_access = fast_and_dangerous;
	arm7_9->dcc_downloads = fast_and_dangerous;

	armv4_5->arch_info = arm7_9;
	armv4_5->read_core_reg = arm7_9_read_core_reg;
	armv4_5->write_core_reg = arm7_9_write_core_reg;
	armv4_5->full_context = arm7_9_full_context;

	if ((retval = armv4_5_init_arch_info(target, armv4_5)) != ERROR_OK)
		return retval;

	return target_register_timer_callback(arm7_9_handle_target_request,
			1, 1, target);
}

int arm7_9_register_commands(struct command_context_s *cmd_ctx)
{
	command_t *arm7_9_cmd;

	arm7_9_cmd = register_command(cmd_ctx, NULL, "arm7_9",
			NULL, COMMAND_ANY, "arm7/9 specific commands");

	register_command(cmd_ctx, arm7_9_cmd, "write_xpsr",
			handle_arm7_9_write_xpsr_command, COMMAND_EXEC,
			"write program status register <value> <not cpsr | spsr>");
	register_command(cmd_ctx, arm7_9_cmd, "write_xpsr_im8",
			handle_arm7_9_write_xpsr_im8_command, COMMAND_EXEC,
			"write program status register "
			"<8bit immediate> <rotate> <not cpsr | spsr>");

	register_command(cmd_ctx, arm7_9_cmd, "write_core_reg",
			handle_arm7_9_write_core_reg_command, COMMAND_EXEC,
			"write core register <num> <mode> <value>");

	register_command(cmd_ctx, arm7_9_cmd, "dbgrq",
			handle_arm7_9_dbgrq_command, COMMAND_ANY,
			"use EmbeddedICE dbgrq instead of breakpoint "
			"for target halt requests <enable | disable>");
	register_command(cmd_ctx, arm7_9_cmd, "fast_memory_access",
			handle_arm7_9_fast_memory_access_command, COMMAND_ANY,
			"use fast memory accesses instead of slower "
			"but potentially safer accesses <enable | disable>");
	register_command(cmd_ctx, arm7_9_cmd, "dcc_downloads",
			handle_arm7_9_dcc_downloads_command, COMMAND_ANY,
			"use DCC downloads for larger memory writes <enable | disable>");

	armv4_5_register_commands(cmd_ctx);

	etm_register_commands(cmd_ctx);

	return ERROR_OK;
}
