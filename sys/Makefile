
# NOTE: no line for systest here. it should be linked last, so that's done from
# the toplevel Makefile instead.
all:
	+@make -C crt all
	+@make -C sysmem all


clean:
	+@make -C crt clean
	+@make -C sysmem clean
	+@make -C test clean


distclean:
	+@make -C crt distclean
	+@make -C sysmem distclean
	+@make -C test distclean
