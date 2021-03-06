
ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

ifeq ($(RTE_ODP),)
$(error "Please define RTE_ODP environment variable")
endif

# Default target, can be overriden by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

ODP_PATH = $(RTE_ODP)

INCFLAGS = -I/usr/local/include/ -I./src/

CPP = g++
CPPFLAGS = -g -O3 $(INCFLAGS)  -fopenmp -Wno-strict-aliasing 

CPPFLAGS += -I$(ODP_PATH)/librte_netdp/include 
CPPFLAGS += -I$(ODP_PATH)/librte_netdpsock/include 

LINKERFLAGS = -lz -lrt -pthread  -ldl
LINKERFLAGS += -L$(ODP_PATH)/librte_netdpsock -lrte_netdpsock \
 -L$(RTE_SDK)/x86_64-native-linuxapp-gcc/lib -lrte_eal \
 -L$(RTE_SDK)/x86_64-native-linuxapp-gcc/lib -lrte_mempool \
 -L$(RTE_SDK)/x86_64-native-linuxapp-gcc/lib -lrte_ring


DEBUGFLAGS = -g -rdynamic -ggdb $(INCFLAGS)
HEADERS=$(wildcard *.h**)


all: apps
apps: example_apps/pagerank example_apps/sssp


clean:
	@rm -rf bin/*
	cd toolkits/collaborative_filtering/; make clean; cd ../../
	cd toolkits/parsers/; make clean; cd ../../
	cd toolkits/graph_analytics/; make clean; cd ../../

blocksplitter: src/preprocessing/blocksplitter.cpp $(HEADERS)
	$(CPP) $(CPPFLAGS) src/preprocessing/blocksplitter.cpp -o bin/blocksplitter $(LINKERFLAGS)

sharder_basic: src/preprocessing/sharder_basic.cpp $(HEADERS)
	@mkdir -p bin
	$(CPP) $(CPPFLAGS) src/preprocessing/sharder_basic.cpp -o bin/sharder_basic $(LINKERFLAGS)

example_apps/% : example_apps/%.cpp $(HEADERS)
	@mkdir -p bin/$(@D)
	$(CPP) $(CPPFLAGS) $(DEBUGFLAGS)  -Iexample_apps/ $@.cpp -o bin/$@ $(LINKERFLAGS)



myapps/% : myapps/%.cpp $(HEADERS)
	@mkdir -p bin/$(@D)
	$(CPP) $(CPPFLAGS) -Imyapps/ $@.cpp -o bin/$@ $(LINKERFLAGS)

tests/%: src/tests/%.cpp $(HEADERS)
	@mkdir -p bin/$(@D)
	$(CPP) $(CPPFLAGS) src/$@.cpp -o bin/$@	$(LINKERFLAGS)


graphlab_als: example_apps/matrix_factorization/graphlab_gas/als_graphlab.cpp
	$(CPP) $(CPPFLAGS) example_apps/matrix_factorization/graphlab_gas/als_graphlab.cpp -o bin/graphlab_als

cf:
	cd toolkits/collaborative_filtering/; bash ./test_eigen.sh; 
	if [ $$? -ne 0 ]; then exit 1; fi
	cd toolkits/collaborative_filtering/; make 
cf_test:
	cd toolkits/collaborative_filtering/; make test; 
cfd:
	cd toolkits/collaborative_filtering/; make -f Makefile.debug

parsers:
	cd toolkits/parsers/; make
parsersd:
	cd toolkits/parsers/; make -f Makefile.debug
ga:
	cd toolkits/graph_analytics/; make
ta:
	cd toolkits/text_analysis/; make
docs: */**
	doxygen conf/doxygen/doxygen.config

