
CCAN_DIR=~/src/ccan
MUNG_DIR=$(CFGDIR)/../mung
MUIDL_DIR=$(CFGDIR)/../muidl
LFHT_DIR=$(CFGDIR)/../lfht

vpath %.idl $(CFGDIR)/idl/sys $(CFGDIR)/idl/api

LD=ld.gold

# TODO: become independent of the mung includes
CFLAGS=-O2 -Wall -march=native -std=gnu11 \
	-m32 -mno-avx -mno-sse2 \
	-I $(CFGDIR)/include -I $(MUNG_DIR)/include \
	-I . -I $(MUIDL_DIR)/include -I $(LFHT_DIR) -I $(CCAN_DIR) \
	-D_GNU_SOURCE \
	-fno-pic -fuse-ld=gold -fno-builtin -nostdlib -ffreestanding \
	-Wno-frame-address \
	-DBUILD_SELFTEST=1 \
	#-DDEBUG_ME_HARDER #-D_L4_DEBUG_ME_HARDER #-DCCAN_LIST_DEBUG
	# (add -DNDEBUG for benchmarks without assert overhead)

LDFLAGS=-L /usr/lib32 -L /usr/lib/i386-linux-gnu

MUIDL:=$(abspath $(MUIDL_DIR)/muidl)
MUIDLFLAGS=-I $(MUIDL_DIR)/share/idl -I $(MUNG_DIR)/idl -I $(CFGDIR)/idl \
	$(filter -DNDEBUG,$(CFLAGS)) $(filter -DBUILD_%,$(CFLAGS))

CLEAN_PATS=*-service.s *-client.s *-common.s *-defs.h ccan-*.a


.deps: $(shell $(CFGDIR)/scripts/find-idl-defs.pl)
	@mkdir -p .deps


# pattern rules below this line.

%.o: %.c
	@echo "  CC $@"
	@$(CC) -c -o $@ $< $(CFLAGS) -nostartfiles -nodefaultlibs -MMD
	@test -d .deps || mkdir -p .deps
	@mv $(<:.c=.d) .deps/


# build CCAN sources as guests.
ccan-%.o ::
	@echo "  CC $@ <ccan>"
	@$(CC) -c -o $@ $(CCAN_DIR)/ccan/$*/$*.c $(CFLAGS) -nostartfiles \
		-nodefaultlibs

ccan-crypto-%.o ::
	@echo "  CC $@ <ccan/crypto>"
	@$(CC) -c -o $@ $(CCAN_DIR)/ccan/crypto/$*/$*.c $(CFLAGS) \
		-nostartfiles -nodefaultlibs


# also complex CCAN modules. build-ccan-module.pl emits CC lines as
# appropriate.
ccan-%.a ::
	+@$(CFGDIR)/scripts/build-ccan-module.pl $(CCAN_DIR)/ccan/$* \
		$(CFLAGS) -nostartfiles -nodefaultlibs


%.o: %-32.S
	@echo "  AS $@"
	@gcc -c -o $@ $< $(CFLAGS) -DIN_ASM_SOURCE -MMD
	@test -d .deps || mkdir -p .deps
	@mv $(<:-32.S=.d) .deps/


%.o: %.s
	@echo "  AS $@ <generated>"
	@as --32 -o $@ $<


# IDL service implementations.
%-impl-service.s %-impl-common.s: %-impl.idl
	@echo "  IDL $< <impl>"
	@$(MUIDL) $(MUIDLFLAGS) --service --common $<


%-impl-defs.h: %-impl.idl
	@echo "  IDL $< <impl-defs>"
	@$(MUIDL) $(MUIDLFLAGS) --defs $<
