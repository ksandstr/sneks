
CCAN_DIR=~/src/ccan
MUNG_DIR=$(CFGDIR)/../mung
MUIDL_DIR=$(CFGDIR)/../muidl
LFHT_DIR=$(CFGDIR)/../lfht

LD=ld.gold

# TODO: become independent of the mung includes
CFLAGS=-O2 -Wall -march=native -std=gnu99 \
	-m32 -mno-avx -mno-sse2 \
	-I $(CFGDIR)/include -I $(MUNG_DIR)/include \
	-I $(MUNG_DIR)/include/fake_clib \
	-I . -I $(MUIDL_DIR)/include -I $(LFHT_DIR) -I $(CCAN_DIR) \
	-D_GNU_SOURCE \
	-fno-pic -fuse-ld=gold -fno-builtin -nostdlib -ffreestanding \
	-Wno-frame-address \
	#-DDEBUG_ME_HARDER #-D_L4_DEBUG_ME_HARDER #-DCCAN_LIST_DEBUG

MUIDL:=$(abspath $(MUIDL_DIR)/muidl)
MUIDLFLAGS=-I $(MUIDL_DIR)/share/idl -I $(MUNG_DIR)/idl -I $(CFGDIR)/idl

CLEAN_PATS=*-service.s *-client.s *-common.s *-defs.h ccan-*.a


# build patterns below this line.


%.o: %.c
	@echo "  CC $@"
	@$(CC) -c -o $@ $< $(CFLAGS) -nostartfiles -nodefaultlibs -MMD
	@test -d .deps || mkdir -p .deps
	@mv $(<:.c=.d) .deps/


# build CCAN sources as guests.
ccan-%.o ::
	@echo "  CC $@ <ccan>"
	@$(CC) -c -o $@ $(CCAN_DIR)/ccan/$*/$*.c $(CFLAGS) -nostartfiles -nodefaultlibs


# also complex CCAN modules. build-ccan-module.pl emits CC lines as
# appropriate.
ccan-%.a ::
	@$(CFGDIR)/stuff/build-ccan-module.pl $(CCAN_DIR)/ccan/$* \
		$(CFLAGS) -nostartfiles -nodefaultlibs


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


# IDL compiler outputs.
#
# TODO: we don't really need to hear first about the services being generated,
# and then the common bit, the client, and then the defs. there's AS chatter
# about the first three too.

vpath %.idl $(CFGDIR)/idl/sys

%-service.s: %.idl
	@echo "  IDL $< <service>"
	@$(MUIDL) $(MUIDLFLAGS) --service $<

%-client.s: %.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client $<

%-common.s: %.idl
	@echo "  IDL $< <common>"
	@$(MUIDL) $(MUIDLFLAGS) --common $<

%-defs.h: %.idl
	@echo "  IDL $< <defs>"
	@$(MUIDL) $(MUIDLFLAGS) --defs $<
