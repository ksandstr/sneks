
SUBDIRS=crt sysmem vm test msg io fs.squashfs chrdev pipe null

# systest has special linkage because it should go last; so it's excluded from
# the main list.
NOTEST_SUBDIRS:=$(filter-out test,$(SUBDIRS))


all:
	+@$(foreach d,$(NOTEST_SUBDIRS),make -C $d all;)


clean:
	+@$(foreach d,$(SUBDIRS),make -C $d clean;)


distclean:
	+@$(foreach d,$(SUBDIRS),make -C $d distclean;)
