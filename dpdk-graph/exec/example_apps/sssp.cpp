/**
 * @file
 * @author  YongLi Cheng <ChengYongLi@hust.edu.cn>
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2015] [YongLi Cheng]
 * 
 *
 * Simple sssp implementation.
 */

#include <string>
#include <fstream>
#include <cmath>

#define CE_Graph_DISABLE_COMPRESSION

#include "CE_Graph_basic_includes.hpp"

using namespace CE_Graph;
 
#define THRESHOLD 1e-1    


typedef float VertexDataType;
typedef float EdgeDataType;
//for a test
int c;
struct ssspProgram : public CE_GraphProgram<VertexDataType, EdgeDataType> {
    
    /**
      * Called before an iteration starts. Not implemented.
      */
    void before_iteration(int iteration, CE_Graph_context &info) {
    }
    
    /**
      * Called after an iteration has finished. Not implemented.
      */
    void after_iteration(int iteration, CE_Graph_context &ginfo) {
    }
    
    /**
      * Called before an execution interval is started. Not implemented.
      */
    void before_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &ginfo) {        
    }

    
    /**
      * sssp update function.
      */
    void update(CE_Graph_vertex<VertexDataType, EdgeDataType> &v, CE_Graph_context &ginfo) {
        float sum=0;
        if (ginfo.iteration == 0) {
	    /* Select a source vertex */
	    if(((M/2) == exec_interval) && (v.num_outedges() > 200)) sel_v = v.id();
	    v.set_data(-1);
	    for(int i=0; i<v.num_outedges(); i++){
		CE_Graph_edge<float> *edge = v.outedge(i);
		edge->set_data(-1);
	   }
	} else if(ginfo.iteration ==1){
		if((sel_v == v.id()) && (sel_v != 0)){
			v.set_data(0);
	    		v.scheduled = true;
			prev_bitset->clear_bit(v.id()-interval_st);
			for(int i=0; i<v.num_outedges(); i++){
				CE_Graph_edge<float> *edge = v.outedge(i);
				edge->set_data(0);
			}
			
		} else {
			//std::cout<<"=====other id: "<<v.id()<<" value: "<<v.get_data()<<std::endl;
		}
	}else{
	if(!((sel_v==v.id()) &&(sel_v!=0))){
	    float min = -1;
	    //std::cout<<"vid: "<<v.id()<<std::endl;
            for(int i=0; i < v.num_inedges(); i++) {
                if(v.inedge(i)->get_data() > -1){
		//std::cout<<"edge>-1, v.id: "<< v.id()<<"edge value: "<<v.inedge(i)->get_data()<<std::endl;
			if(min == -1) min = v.inedge(i)->get_data();
			else{
				 if(min > v.inedge(i)->get_data()) min = v.inedge(i)->get_data();
			}
		}
            }

	    if(min > -1){
		if((min+1)==v.get_data()) //for convergence.
			prev_bitset->clear_bit(v.id()-interval_st);
		else prev_bitset->set_bit(v.id()-interval_st);

		if((v.get_data()==-1) || ((min+1)<v.get_data())){
			v.set_data(min+1);
			//if(min+1 >1) std::cout<<"min+1 : "<<min+1<<" v: "<<v.id()<<" v.value:"<< v.get_data()<<std::endl;
                	for(int i=0; i < v.num_outedges(); i++) {
                    		CE_Graph_edge<float> * edge = v.outedge(i);
                    		edge->set_data(min+1);
			}
			
		}
	    }
        }
	}
    }
    
};
int main(int argc, const char ** argv) {
    CE_Graph_init(argc, argv);
    metrics m("sssp");
    
    /* Parameters */
    std::stringstream ss;
    std::string fn;
    std::string filename    = get_option_string("file"); // Base filename
    int niters              = get_option_int("niters", 4);
    bool scheduler          = false;                    // Non-dynamic version of pagerank.
    int ntop                = get_option_int("top", 20);
    
    /* Process input file - if not already preprocessed */
    int nshards             = get_option_int("nshards", -1);
    if(nshards == -1){
        std::cout<<"Please input nshards...." <<std::endl;
        assert(nshards != -1);
    }

    /* Run */
    sel_v = 0;
    CE_Graph_engine<float, float> engine(filename, 0, scheduler, m); 
    engine.set_modifies_inedges(false); // Improves I/O performance.
    ssspProgram program;
    engine.run(program, niters);
        
    /* Output top ranked vertices */
    ss<<filename<<"_this_interval";
    fn = ss.str();
    std::vector< vertex_value<float> > top = get_top_vertices<float>(fn, ntop);
    std::cout << "Print top " << ntop << " vertices:" << std::endl;
    for(int i=0; i < (int)top.size(); i++) {
        std::cout << (i+1) << ". " << top[i].vertex << "\t" << top[i].value << std::endl;
    }
    metrics_report(m);    
    return 0;
}

