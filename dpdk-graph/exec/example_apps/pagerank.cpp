/*
@file
 * @author  YongLi Cheng <ChengYongLi@hust.edu.cn>
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2014] [Yongli Cheng , Xiuneng Wang / Huazhong University of Science and Technology]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <fstream>
#include <cmath>

#define CE_Graph_DISABLE_COMPRESSION


#include "CE_Graph_basic_includes.hpp"

using namespace CE_Graph;
 
#define THRESHOLD 1e-1    
#define RANDOMRESETPROB 0.15


typedef float VertexDataType;
typedef float EdgeDataType;
//for a test
int c;
struct PagerankProgram : public CE_GraphProgram<VertexDataType, EdgeDataType> {
    
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
      * Pagerank update function.
      */
    void update(CE_Graph_vertex<VertexDataType, EdgeDataType> &v, CE_Graph_context &ginfo) {
        float sum=0;
	//if(v.num_outedges()>0) std::cout<<"Vid:" << v.vertexid <<" v.num_outedges " << v.num_outedges() << " num_inedges" << v.num_inedges()<<std::endl;
        if (ginfo.iteration == 0) {
            /* On first iteration, initialize vertex and out-edges. 
               The initialization is important,
               because on every run, CE_Graph will modify the data in the edges on disk. 
             */
            for(int i=0; i < v.num_outedges(); i++) {
                CE_Graph_edge<float> * edge = v.outedge(i);
                edge->set_data(1.0 / v.num_outedges());
            }
            v.set_data(RANDOMRESETPROB); 
        } else {
            /* Compute the sum of neighbors' weighted pageranks by reading from the in-edges. */
            for(int i=0; i < v.num_inedges(); i++) {
                float val = v.inedge(i)->get_data();
                sum += val;
            }
            /* Compute my pagerank */
            float pagerank = RANDOMRESETPROB + (1 - RANDOMRESETPROB) * sum;
            
            /* Write my pagerank divided by the number of out-edges to
               each of my out-edges. */
            if (v.num_outedges() > 0) {
                float pagerankcont = pagerank / v.num_outedges();
                for(int i=0; i < v.num_outedges(); i++) {
                    CE_Graph_edge<float> * edge = v.outedge(i);
                    edge->set_data(pagerankcont);
                }
            }
                
            /* Keep track of the progression of the computation.
            CE_Graph engine writes a file filename.deltalog. */
            //ginfo.log_change(std::abs(pagerank - v.get_data()));
	    /* Check convergence */
	    if(abs(v.get_data() - pagerank) < THRESHOLD) 
		//v.scheduled = false;
		prev_bitset->clear_bit(v.id() - interval_st);
	    else
		prev_bitset->set_bit(v.id() - interval_st);
		//v.scheduled = true;	

            /* Set my new pagerank as the vertex value */
            v.set_data(pagerank); 
	    //if(v.get_data() <0) std::cout<<"v.id: " <<v.id()<<"   value:  " << v.get_data()<<std::endl;
        }
    }
    
};
int main(int argc, const char ** argv) {
    CE_Graph_init(argc, argv);
    metrics m("pagerank");
    
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
    CE_Graph_engine<float, float> engine(filename, nshards, scheduler, m); 
    engine.set_modifies_inedges(false); // Improves I/O performance.
    PagerankProgram program;
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

