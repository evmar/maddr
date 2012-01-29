CFLAGS=-Wall -g
# -O2

maddr: maddr.cc
	g++ $(CFLAGS) -o $@ $<

clean:
	rm -f maddr *.o
