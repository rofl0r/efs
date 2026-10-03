-include config.mak

# Select the MPH implementation: MPH=JMPH (default, Jenkins, jmph.h) or
# MPH=BBHASH (bbhash.h, the oomph/ BBHash library). One algorithm is compiled
# into the image; there is nothing in the image that identifies the algorithm.
MPH ?= JMPH

MPH_DEF_JMPH   =
MPH_DEF_BBHASH = -DEFS_MPH_OOMPH
MPH_DEP_JMPH   = jmph.h
MPH_DEP_BBHASH = bbhash.h

MPH_DEF = $(MPH_DEF_$(MPH))
MPH_DEP = $(MPH_DEP_$(MPH))

all: efsbuilder efsreader

efsbuilder.o: efsbuilder.c efs.h efs_mph.h $(MPH_DEP)
	$(CC) $(CFLAGS) $(MPH_DEF) -c -o $@ efsbuilder.c

efsreader.o: efsreader.c efs.h efs_mph.h $(MPH_DEP)
	$(CC) $(CFLAGS) $(MPH_DEF) -c -o $@ efsreader.c

# efstest links the builder as a library (EFS_NO_MAIN drops its main()).
efstest_builder.o: efsbuilder.c efs.h efs_mph.h $(MPH_DEP)
	$(CC) $(CFLAGS) $(MPH_DEF) -DEFS_NO_MAIN -c -o $@ efsbuilder.c

efstest.o: efstest.c efs.h efs_mph.h $(MPH_DEP)
	$(CC) $(CFLAGS) $(MPH_DEF) -c -o $@ efstest.c

efstest: efstest.o efstest_builder.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

efsbuilder: efsbuilder.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

efsreader: efsreader.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

# Self-check driver for the jmph.h MPH builder: builds an MPHF from stdin
# keys and emits "<key>\t<rank>" so the test harness can verify it is a
# valid permutation of 1..N.
jmph_test: jmph_test.c jmph.h
	$(CC) $(CFLAGS) -o $@ jmph_test.c

# Run the jmph MPHF test harness: generates N_INPUTS distinct keys, builds
# the MPHF, and checks the output is a valid permutation of 1..N_INPUTS.
# N_INPUTS can be overridden, e.g. `make test N_INPUTS=50000`.
test: jmph_test
	sh ./test_jmph.sh ./jmph_test

# Directory sizes to exercise (each is the number of generated files).
CHECK_N ?= 0 1 2 3 7 12 50 100 255 256 257 300 500 1000 2000

# End-to-end correctness sweep for ONE MPH implementation (the currently
# built one): generates a temp tree of N Markov-named files, builds the EFS
# image, reads every entry back, and compares names + contents. Generated
# dirs are removed on success and kept for inspection on failure.
check: efstest
	@set -e; for n in $(CHECK_N); do \
		printf 'MPH=%s N=%s: ' "$(MPH)" "$$n"; \
		./efstest -n $$n -s 1 || exit 1; \
	done

# Run the sweep for BOTH MPH implementations, rebuilding in between. This is
# the top-level "validate everything" target.
check-all:
	$(MAKE) clean >/dev/null; $(MAKE) MPH=JMPH check
	$(MAKE) clean >/dev/null; $(MAKE) MPH=BBHASH check

clean:
	rm -f efsbuilder efsreader efsbuilder.o efsreader.o jmph_test \
		efstest efstest.o efstest_builder.o

.PHONY: all test check check-all clean
