
CC ?= cc

-include config.mak

all: boomphf soomph goomph mph_builder

boomphf: boomphf.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

soomph: soomph.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

goomph: goomph.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

mph_builder: mph_builder.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

# Run the soomph test harness: generates an MPHF for a fixed key set,
# compiles it with -DTEST, and checks the output is a valid permutation.
# Pass the generator executable as $1 (defaults to ./soomph). N_INPUTS can
# be overridden via the environment (e.g. N_INPUTS=1000 make test).
test: boomphf soomph goomph mph_builder test_soomph.sh
	sh ./test_soomph.sh ./boomphf
	sh ./test_soomph.sh ./soomph
	sh ./test_soomph.sh ./goomph
	sh ./test_soomph.sh ./mph_builder

clean:
	rm -f boomphf soomph soomph.o goomph goomph.o mph_builder mph_builder.o

.PHONY: all test clean
