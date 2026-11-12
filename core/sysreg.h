#ifndef HENGEVM_SYSREG_H
#define HENGEVM_SYSREG_H

#include <stdint.h>

#include "cpu.h"

/* MRS, MSR (register and immediate), and SYS (DC, IC, TLBI, AT).
   Each returns 1 if the block must leave. */
int hv_sysreg_mrs(HvCpu *cpu, uint32_t insn, uint64_t pc);
int hv_sysreg_msr(HvCpu *cpu, uint32_t insn, uint64_t pc);
int hv_sysreg_sys(HvCpu *cpu, uint32_t insn, uint64_t pc);

/* The guest's counter (CNTPCT): 24 MHz monotonic tick counter (mach_absolute_time on Apple, scaled cntvct_el0 on Linux/Android). */
uint64_t hv_counter(void);
#ifndef __APPLE__
extern uint64_t hv_cnt_base;
extern uint64_t hv_cnt_mult;
void hv_counter_ensure_init(void);
#endif

/* One TLBI on one CPU's own TLB and branch cache. */
void hv_tlbi_apply(HvCpu *cpu, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint64_t xt);

#endif
