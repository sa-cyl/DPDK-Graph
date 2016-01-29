INCFLAGS = -I/usr/local/include/ -I.

CPP = g++
CPPFLAGS = -g -O3 $(INCFLAGS)  -fopenmp -Wno-strict-aliasing
LINKERFLAGS = -lz
DEBUGFLAGS = -g -ggdb $(INCFLAGS)
HEADERS=$(wildcard *.h**)

ceMaster: ceMaster.cpp $(HEADERS)
	$(CPP) $(CPPFLAGS) ceMaster.cpp -o ceMaster $(LINKERFLAGS)
	mv ceMaster ../bin

clean:
	@rm -rf ceMaster
