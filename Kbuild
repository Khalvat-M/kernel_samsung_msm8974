#
# Kbuild for top-level directory of the kernel
# This file takes care of the following:
# 1) Generate bounds.h
# 2) Generate asm-offsets.h (may need bounds.h)
# 3) Check for missing system calls
# 5) Generate constants.py (may need bounds.h)

#####
# 1) Generate bounds.h

bounds-file := include/generated/bounds.h

always  := $(bounds-file)
targets := kernel/bounds.s

# We use internal kbuild rules to avoid the "is up to date" message from make
kernel/bounds.s: kernel/bounds.c FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed_dep,cc_s_c)

$(bounds-file): kernel/bounds.s FORCE
	$(call filechk,offsets,__LINUX_BOUNDS_H__)

#####
# 2) Generate asm-offsets.h
#

offsets-file := include/generated/asm-offsets.h

always  += $(offsets-file)
targets += arch/$(SRCARCH)/kernel/asm-offsets.s

# We use internal kbuild rules to avoid the "is up to date" message from make
arch/$(SRCARCH)/kernel/asm-offsets.s: arch/$(SRCARCH)/kernel/asm-offsets.c \
                                      $(obj)/$(bounds-file) FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed_dep,cc_s_c)

$(offsets-file): arch/$(SRCARCH)/kernel/asm-offsets.s FORCE
	$(call filechk,offsets,__ASM_OFFSETS_H__)

#####
# 3) Check for missing system calls
#

always += missing-syscalls
targets += missing-syscalls

quiet_cmd_syscalls = CALL    $<
      cmd_syscalls = $(CONFIG_SHELL) $< $(CC) $(c_flags) $(missing_syscalls_flags)

missing-syscalls: scripts/checksyscalls.sh $(offsets-file) FORCE
	$(call cmd,syscalls)

#####
# 5) Generate constants for Python GDB integration
#

extra-$(CONFIG_GDB_SCRIPTS) += build_constants_py

build_constants_py: $(timeconst-file) $(bounds-file)
	@$(MAKE) $(build)=scripts/gdb/linux $@

# Keep these two files during make clean
no-clean-files := $(bounds-file) $(offsets-file)
