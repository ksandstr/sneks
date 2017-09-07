
CCAN_DIR=~/src/ccan
MUNG_DIR=$(CFGDIR)/../mung
MUIDL_DIR=$(CFGDIR)/../muidl

LD=ld.gold

# TODO: become independent of the mung includes
CFLAGS=-O2 -Wall -march=native -std=gnu99 \
	-m32 -mno-avx -mno-sse2 \
	-I $(CFGDIR)/include -I $(MUNG_DIR)/include \
	-I $(MUNG_DIR)/include/fake_clib \
	-I . -I $(MUIDL_DIR)/include -I $(CCAN_DIR) \
	-D_GNU_SOURCE \
	-fno-pic -fuse-ld=gold -fno-builtin -nostdlib \
	-Wno-frame-address \
	#-DDEBUG_ME_HARDER #-D_L4_DEBUG_ME_HARDER #-DCCAN_LIST_DEBUG

MUIDL:=$(abspath $(MUIDL_DIR)/muidl)
MUIDLFLAGS=-I $(MUIDL_DIR)/share/idl -I $(MUNG_DIR)/idl -I $(CFGDIR)/idl

CLEAN_PATS=*-service.s *-client.s *-common.s *-defs.h


%.o: %.c
	@echo "  CC $@"
	@$(CC) -c -o $@ $< $(CFLAGS) -nostartfiles -nodefaultlibs -MMD
	@test -d .deps || mkdir -p .deps
	@mv $(<:.c=.d) .deps/


# build CCAN sources as guests.
ccan-%.o ::
	@echo "  CC $@ <ccan>"
	@$(CC) -c -o $@ $(CCAN_DIR)/ccan/$*/$*.c $(CFLAGS) -nostartfiles -nodefaultlibs


%.o: %-32.S
	@echo "  AS $@"
	@gcc -c -o $@ $< $(CFLAGS) -DIN_ASM_SOURCE -MMD
	@test -d .deps || mkdir -p .deps
	@mv $(<:-32.S=.d) .deps/


%.o: %.s
	@echo "  AS $@ <generated>"
	@as --32 -o $@ $<


.deps:
	@mkdir -p .deps
