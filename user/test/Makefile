
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/user/crt/static.ld
LDFLAGS:=$(LDFLAGS) -e_start -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/user/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -D__sneks__=1 -D__l4x2__=1 -mstackrealign \
	-DTESTDIR="\"/\""	# leaning toothpicks in a grassy meadow

# here, we actually want the two-part path shown.
TEST_SOURCE:=$(filter-out tools/%.c,$(wildcard */*.c))
TEST_OBJS:=$(TEST_SOURCE:%.c=%.o)

TESTSUITE_OBJS:=main.o harness.o util.o optcompat.o $(TEST_OBJS) \
	ccan-talloc.o

HOSTSUITE_TESTDIR:=/tmp/sneks-hostsuite-$(shell stat -c "u%u_i%i" .)/
HOSTSUITE_OBJS:=$(filter-out optcompat.%,$(TESTSUITE_OBJS:%.o=%.host.o))
HOSTSUITE_CFLAGS=-O2 -Wall -march=native -std=gnu11 -m32 -D_GNU_SOURCE \
	-I $(CFGDIR)/user/test/hostinclude -I $(CCAN_DIR) -I $(LFHT_DIR) \
	-Wno-frame-address -DTESTDIR="\"$(HOSTSUITE_TESTDIR)\""
	#-DDEBUG_ME_HARDER #-DCCAN_LIST_DEBUG

TOOLS:=$(patsubst tools/%.c,tools/%,$(wildcard tools/*.c))
SCRIPTS:=$(wildcard scripts/*)
HOSTSUITE_TOOLS:=$(patsubst tools/%,tools/%.host,$(TOOLS))

OUTPUTS=testsuite $(TOOLS) hostsuite $(HOSTSUITE_TOOLS)

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

tools/%: $(LDSCRIPT) tools/%.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) \
		-lsneks-user-crt -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)

initrd-install: testsuite $(TOOLS)
	@mkdir -p $(INITRD)/user
	@install $< $(INITRD)/user/
	@mkdir -p $(INITRD)/user/test/tools
	@install -t $(INITRD)/user/test/tools $(TOOLS)
	@mkdir -p $(INITRD)/user/test/scripts
	@install -t $(INITRD)/user/test/scripts $(SCRIPTS)
	+@make TARGET_INITRD=/initrd test-files-install

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
	@mkdir -p $(INITRD)/user/test/io/stat
	@for i in r x rx; do \
		echo "exit 123" >$(INITRD)/user/test/io/stat/$$i; \
		chmod u=$$i $(INITRD)/user/test/io/stat/$$i; \
	done
	@ln -s does_not_exist $(INITRD)/user/test/io/stat/exploding_symlink
	@mkdir -p $(INITRD)/user/test/io/symlink
	@ln -s foo/bar/zot $(INITRD)/user/test/io/symlink/teh_linkz0r
	@ln -s looping_terminal $(INITRD)/user/test/io/symlink/looping_terminal
	@ln -s looping_middle/deez/nutz $(INITRD)/user/test/io/symlink/looping_middle
	@ln -s ../reg/testfile $(INITRD)/user/test/io/symlink/terminal_relative
	@ln -s terminal_relative $(INITRD)/user/test/io/symlink/terminal_iterated_relative
	@ln -s $(TARGET_INITRD)/user/test/io/reg/testfile $(INITRD)/user/test/io/symlink/terminal_absolute
	@ln -s $(TARGET_INITRD)/user/test/io/symlink/terminal_relative $(INITRD)/user/test/io/symlink/terminal_iterated_absolute
	@ln -s ../reg $(INITRD)/user/test/io/symlink/nonterminal_relative
	@ln -s nonterminal_relative $(INITRD)/user/test/io/symlink/nonterminal_iterated_relative
	@ln -s $(TARGET_INITRD)/user/test/io/reg $(INITRD)/user/test/io/symlink/nonterminal_absolute
	@ln -s $(TARGET_INITRD)/user/test/io/symlink/nonterminal_relative $(INITRD)/user/test/io/symlink/nonterminal_iterated_absolute
	@mkdir -p $(INITRD)/user/test/exec/isdir/exit_with_0
	@mkdir -p $(INITRD)/user/test/exec/noexec_script
	@echo "#!$(INITRD)/user/test/exec/noexec/exit_with_0" \
		>>$(INITRD)/user/test/exec/noexec_script/exit_with_0
	@echo "exit 666" >>$(INITRD)/user/test/exec/noexec_script/exit_with_0
	@find $(INITRD)/user/test/exec -name exit_with_0 -exec chmod u+x {} +
	@mkdir -p $(INITRD)/user/test/exec/xnotset
	@echo "exit 667" >>$(INITRD)/user/test/exec/xnotset/exit_with_0


# hostsuite stuff. perhaps dirtier than necessary.

hostsuite: $(HOSTSUITE_OBJS) $(CFGDIR)/lib/tap.c $(CFGDIR)/lib/strscpy.c \
		ccan-htable.host.o ccan-autodata.host.o ccan-opt.host.a \
		$(HOSTSUITE_TOOLS)
	@echo "  INSTALL $(HOSTSUITE_TESTDIR)"
	@mkdir -p $(HOSTSUITE_TESTDIR)
	@rm -rf $(HOSTSUITE_TESTDIR)/user/test
	@mkdir -p $(HOSTSUITE_TESTDIR)/user/test/tools
	@install -t $(HOSTSUITE_TESTDIR)/user/test/tools $(HOSTSUITE_TOOLS)
	@mkdir -p $(HOSTSUITE_TESTDIR)/user/test/scripts
	@install -t $(HOSTSUITE_TESTDIR)/user/test/scripts $(SCRIPTS)
	@rename 's/\.host//' $(HOSTSUITE_TESTDIR)/user/test/tools/*.host
	+@make INITRD=$(HOSTSUITE_TESTDIR) TARGET_INITRD=$(HOSTSUITE_TESTDIR) \
		test-files-install
	@echo "  LD $@ <impure!>"
	@$(CC) -o $@ $(filter-out tools/%,$^) $(HOSTSUITE_CFLAGS)

tools/%.host: tools/%.host.o
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
