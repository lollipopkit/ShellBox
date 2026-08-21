#ifndef EMU_CPU_H
#define EMU_CPU_H

#include "misc.h"
#include "emu/mmu.h"

#include <stddef.h>

// Include architecture-specific CPU state definition
#include "emu/arch/arm64/cpu.h"

// Common CPU interface
struct cpu_state;
struct tlb;
int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb);
void cpu_poke(struct cpu_state *cpu);

#define CPU_OFFSET(field) offsetof(struct cpu_state, field)

#endif
