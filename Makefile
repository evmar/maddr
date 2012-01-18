CFLAGS=-Wall -g

maddr: maddr.cc
	g++ $(CFLAGS) -o $@ $<

clean:
	rm -f maddr *.o
