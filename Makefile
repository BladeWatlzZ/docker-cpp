C = gcc
CXX = g++
C_LIB = network/network.c network/nl.c
C_LINK = network.o nl.o
MAIN = main.cpp
LD = -std=c++11
OUT = mydocker

all:
	make container
container:
	$(C) -c $(C_LIB)
	$(CXX) $(LD) -o $(OUT) $(MAIN) $(C_LINK)
clean:
	rm *.o $(OUT)