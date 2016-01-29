all:sharder_basic tran make-edgelist

clean:
	@rm -rf bin/*
	
sharder_basic:
	@mkdir -p bin
	cd ./src/CE_Graph_sharder;make sharder_basic;cd ../../
	mv src/CE_Graph_sharder/bin/sharder_basic_normal bin/sharder_basic_normal;
	mv src/CE_Graph_sharder/bin/sharder_basic_dynamic bin/sharder_basic_dynamic;
	g++ ./src/sharder_basic.cpp -o ./bin/sharder_basic
	
tran:
	@mkdir -p bin
	g++ ./src/tran.cpp -o ./bin/tran
   
make-edgelist:
	@mkdir -p bin
	cd ./src/graph500;make make-edgelist;cd ../../
	mv src/graph500/make-edgelist bin/make-edgelist;