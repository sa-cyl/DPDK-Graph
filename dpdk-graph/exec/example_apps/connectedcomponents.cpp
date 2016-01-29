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


#include <cmath>
#include <string>
#define CE_Graph_DISABLE_COMPRESSION

#include "CE_Graph_basic_includes.hpp"
#include "util/labelanalysis.hpp"

using namespace CE_Graph;



/**
 * Type definitions. Remember to create suitable graph shards using the
 * Sharder-program. 
 */
typedef vid_t VertexDataType;       // vid_t is the vertex id type
typedef vid_t EdgeDataType;

/**
 * CE_Graph programs need to subclass CE_GraphProgram<vertex-type, edge-type> 
 * class. The main logic is usually in the update function.
 */
struct ConnectedComponentsProgram : public CE_GraphProgram<VertexDataType, EdgeDataType> {
    
    /**
     *  Vertex update function.
     *  On first iteration ,each vertex chooses a label = the vertex id.
     *  On subsequent iterations, each vertex chooses the minimum of the neighbor's
     *  label (and itself). 
     */
    void update(CE_Graph_vertex<VertexDataType, EdgeDataType> &vertex, CE_Graph_context &gcontext) {
        /* This program requires selective scheduling. */
        assert(gcontext.scheduler != NULL);
        
        if (gcontext.iteration == 0) {
            vertex.set_data(vertex.id());
            gcontext.scheduler->add_task(vertex.id()); 
        }
        
        /* On subsequent iterations, find the minimum label of my neighbors */
        vid_t curmin = vertex.get_data();
	vid_t nb;
	int first=1;
        for(int i=0; i < vertex.num_edges(); i++) {
            vid_t nblabel = vertex.edge(i)->get_data();
            if (gcontext.iteration == 0) nblabel = vertex.edge(i)->vertex_id();  // Note!
	    if(first==1){//for convergence
		nb=nblabel;
		first=0;
	    }else{
		nb = std::min(nblabel, nb);
	    } 
            curmin = std::min(nblabel, curmin); 
        }

        if(nb==vertex.get_data()) //for convergence.
               prev_bitset->clear_bit(vertex.id()-interval_st);
        else prev_bitset->set_bit(vertex.id()-interval_st);        

        /* Set my label */
        vertex.set_data(curmin);
        
        /** 
         * Broadcast new label to neighbors by writing the value
         * to the incident edges.
         * Note: on first iteration, write only to out-edges to avoid
         * overwriting data (this is kind of a subtle point)
         */
        vid_t label = vertex.get_data();
        
        if (gcontext.iteration > 0) {
            for(int i=0; i < vertex.num_edges(); i++) {
                if (label < vertex.edge(i)->get_data()) {
                    vertex.edge(i)->set_data(label);
                    /* Schedule neighbor for update */
                    gcontext.scheduler->add_task(vertex.edge(i)->vertex_id()); 
                }
            }
        } else if (gcontext.iteration == 0) {
            for(int i=0; i < vertex.num_outedges(); i++) {
                vertex.outedge(i)->set_data(label);
            }
        }
    }    
    /**
     * Called before an iteration starts.
     */
    void before_iteration(int iteration, CE_Graph_context &info) {
    }
    
    /**
     * Called after an iteration has finished.
     */
    void after_iteration(int iteration, CE_Graph_context &ginfo) {
    }
    
    /**
     * Called before an execution interval is started.
     */
    void before_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &ginfo) {        
    }
    
    /**
     * Called after an execution interval has finished.
     */
    void after_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &ginfo) {        
    }
    
};

int main(int argc, const char ** argv) {
    /* CE_Graph initialization will read the command line 
     arguments and the configuration file. */
    CE_Graph_init(argc, argv);
    
    /* Metrics object for keeping track of performance counters
     and other information. Currently required. */
    metrics m("connected-components");
    
    /* Basic arguments for application */
    std::string filename = get_option_string("file");  // Base filename
    int niters = get_option_int("niters", -1);
    int nshards = get_option_int("nshards", -1);
    if(nshards == -1){
         std::cout<<"Please input nshards...." <<std::endl;
         assert(nshards != -1);
    }
    bool scheduler       = true;    // Always run with scheduler
    
    
    if (get_option_int("onlyresult", 0) == 0) {
        /* Run */
        ConnectedComponentsProgram program;
        CE_Graph_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m); 
        engine.run(program, niters);
    }
    
    /* Run analysis of the connected components  (output is written to a file) */
    m.start_time("label-analysis");
    
    analyze_labels<vid_t>(filename);
    
    m.stop_time("label-analysis");
    
    /* Report execution metrics */
    metrics_report(m);
    return 0;
}

