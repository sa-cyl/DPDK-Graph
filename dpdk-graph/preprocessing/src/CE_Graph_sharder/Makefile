INCFLAGS = -I/usr/local/include/ -I./

CPP = g++
CPPFLAGS = -g -O3 $(INCFLAGS)  -w -fopenmp -Wno-strict-aliasing 
LINKERFLAGS = -lz
DEBUGFLAGS = -g -ggdb $(INCFLAGS)
HEADERS=$(wildcard *.h**)


all: apps tests 
apps: example_apps/connectedcomponents example_apps/pagerank example_apps/pagerank_functional example_apps/communitydetection example_apps/unionfind_connectedcomps example_apps/stronglyconnectedcomponents example_apps/trianglecounting example_apps/randomwalks example_apps/minimumspanningforest
als: example_apps/matrix_factorization/als_edgefactors  example_apps/matrix_factorization/als_vertices_inmem
tests: tests/basic_smoketest tests/bulksync_functional_test tests/dynamicdata_smoketest tests/test_dynamicedata_loader


clean:
	@rm -rf bin/*
	cd toolkits/collaborative_filtering/; make clean; cd ../../
	cd toolkits/parsers/; make clean; cd ../../
	cd toolkits/graph_analytics/; make clean; cd ../../

blocksplitter: preprocessing/blocksplitter.cpp $(HEADERS)
	$(CPP) $(CPPFLAGS) preprocessing/blocksplitter.cpp -o bin/blocksplitter $(LINKERFLAGS)

sharder_basic:
	make sharder_basic_normal;
	make sharder_basic_dynamic;
	
sharder_basic_normal: preprocessing/sharder_basic_normal.cpp $(HEADERS)
	@mkdir -p bin
	$(CPP) $(CPPFLAGS) preprocessing/sharder_basic_normal.cpp -o bin/sharder_basic_normal $(LINKERFLAGS)

sharder_basic_dynamic: preprocessing/sharder_basic_dynamic.cpp $(HEADERS)
	@mkdir -p bin
	$(CPP) $(CPPFLAGS) preprocessing/sharder_basic_dynamic.cpp -o bin/sharder_basic_dynamic $(LINKERFLAGS)
	
example_apps/% : example_apps/%.cpp $(HEADERS)
	@mkdir -p bin/$(@D)
	$(CPP) $(CPPFLAGS) -Iexample_apps/ $@.cpp -o bin/$@ $(LINKERFLAGS) 



myapps/% : myapps/%.cpp $(HEADERS)
	@mkdir -p bin/$(@D)
	$(CPP) $(CPPFLAGS) -Imyapps/ $@.cpp -o bin/$@ $(LINKERFLAGS)

tests/%: tests/%.cpp $(HEADERS)
	@mkdir -p bin/$(@D)
	$(CPP) $(CPPFLAGS) $@.cpp -o bin/$@	$(LINKERFLAGS)


graphlab_als: example_apps/matrix_factorization/graphlab_gas/als_graphlab.cpp
	$(CPP) $(CPPFLAGS) example_apps/matrix_factorization/graphlab_gas/als_graphlab.cpp -o bin/graphlab_als $(LINKERFLAGS)

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


	

	
