#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H

// The guest's view of the vdso, in pages. These lived in
// tools/ptraceomatic-config.h, which named the tool that had to agree with the
// kernel about the layout rather than the thing being described; the tool is
// gone and the kernel is where they were always read.
#define VVAR_PAGES 4
#define VDSO_PAGES 2

extern const char vdso_data[VDSO_PAGES * (1 << 12)] __asm__("vdso_data");
int vdso_symbol(const char *name);

#endif
