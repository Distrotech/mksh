all: mksh


mksh:
	./Build.sh

install: all
	install -d $(DESTDIR)/bin
	install -m 755 mksh $(DESTDIR)/bin
	ln -sr $(DESTDIR)/bin/mksh $(DESTDIR)/bin/ksh

clean:
	@rm -f *.o mksh Rebuild.sh conftest.c lft.c mksh.cat1 rlimits.gen sh_flags.gen signames.inc test.sh

distclean: clean
