
export CFGDIR:=$(abspath .)
include config.mk

.PHONY: all clean distclean check qcheck


all: tags
	+@make -C lib all
	+@make -C root all
	+@make -C sys all

clean:
	@rm -f *.o $(CLEAN_PATS)
	+@make -C sys clean
	+@make -C root clean
	+@make -C lib clean


distclean: clean
	@rm -f tags
	@rm -rf .deps
	+@make -C sys distclean
	+@make -C root distclean
	+@make -C lib distclean
	@find . -name ".deps" -type d -print|xargs rm -rf


tags: $(shell find . -iname "*.[ch]" -or -iname "*.p[lm]")
	@ctags -R *
