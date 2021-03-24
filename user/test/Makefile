
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/user/crt/static.ld
LDFLAGS:=$(LDFLAGS) -e_start -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/user/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -D__sneks__=1 -D__l4x2__=1 -mstackrealign \
	-DTESTDIR="\"/\""	# leaning toothpicks in a grassy meadow

OUTPUTS=testsuite hostsuite

# here, we actually want the two-part path shown.
TEST_SOURCE:=$(wildcard */*.c)
TEST_OBJS:=$(TEST_SOURCE:%.c=%.o)

TESTSUITE_OBJS:=main.o harness.o optcompat.o $(TEST_OBJS) \
	ccan-talloc.o

HOSTSUITE_TESTDIR:=/tmp/sneks-hostsuite-$(shell stat -c "u%u_i%i" .)/
HOSTSUITE_OBJS:=$(filter-out optcompat.%,$(TESTSUITE_OBJS:%.o=%.host.o))
HOSTSUITE_CFLAGS=-O2 -Wall -march=native -std=gnu11 -m32 -D_GNU_SOURCE \
	-I $(CFGDIR)/user/test/hostinclude -I $(CCAN_DIR) -I $(LFHT_DIR) \
	-Wno-frame-address -DTESTDIR="\"$(HOSTSUITE_TESTDIR)\""
	#-DDEBUG_ME_HARDER #-DCCAN_LIST_DEBUG

vpath %.idl $(CFGDIR)/idl/api


all: $(OUTPUTS)


clean:
	@rm -f *.o *.a $(CLEAN_PATS) $(TEST_OBJS) $(HOSTSUITE_OBJS)


distclean:
	@rm -rf .deps $(OUTPUTS)


testsuite: $(LDSCRIPT) $(TESTSUITE_OBJS) $(CFGDIR)/lib/tap.o ccan-opt.a
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) \
		-lsneks-user-crt -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


initrd-install: testsuite
	@mkdir -p $(INITRD)/user
	@install $< $(INITRD)/user/
	+@make test-files-install

test-files-install:
	@mkdir -p $(INITRD)/user/test/io/reg
	@echo -n "0123456789abcdef" >$(INITRD)/user/test/io/reg/testfile
	@mkdir -p $(INITRD)/user/test/io/dir
	@for i in a b c d e f g h i j k l; do \
		mkdir -p $(INITRD)/user/test/io/dir/$$i; \
	done
	@for i in 0 1 2 3 4 5 6 7 8 9; do \
		echo "shoop da woop $$i" >$(INITRD)/user/test/io/dir/$$i; \
	done


# hostsuite stuff. perhaps dirtier than necessary.

hostsuite: $(HOSTSUITE_OBJS) $(CFGDIR)/lib/tap.c $(CFGDIR)/lib/strscpy.c \
		ccan-htable.host.o ccan-autodata.host.o ccan-opt.host.a
	@echo "  INSTALL $(HOSTSUITE_TESTDIR)"
	@mkdir -p $(HOSTSUITE_TESTDIR)
	@rm -rf $(HOSTSUITE_TESTDIR)/user/test
	+@make INITRD=$(HOSTSUITE_TESTDIR) test-files-install
	@echo "  LD $@ <impure!>"
	@$(CC) -o $@ $^ $(HOSTSUITE_CFLAGS)

# build CCAN sources as guests, for the host.
ccan-%.host.o ::
	@echo "  CC $@ <ccan>"
	@$(CC) -c -o $@ $(CCAN_DIR)/ccan/$*/$*.c $(HOSTSUITE_CFLAGS)


# also complex CCAN modules. build-ccan-module.pl emits CC lines as
# appropriate.
ccan-%.host.a ::
	+@HOSTSUITE=1 $(CFGDIR)/scripts/build-ccan-module.pl \
		$(CCAN_DIR)/ccan/$* $(HOSTSUITE_CFLAGS)

%.host.o: %.c
	@echo "  CC $@"
	@$(CC) -c -o $@ $< $(HOSTSUITE_CFLAGS)


include $(wildcard .deps/*.d)
