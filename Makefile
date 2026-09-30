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

clean:
	rm -f efsbuilder efsreader efsbuilder.o efsreader.o jmph_test

.PHONY: all test clean
