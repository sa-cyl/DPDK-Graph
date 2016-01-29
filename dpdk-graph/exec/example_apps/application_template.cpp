
/**
 * @file
 * @author  Aapo Kyrola <akyrola@cs.cmu.edu>
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2012] [Aapo Kyrola, Guy Blelloch, Carlos Guestrin / Carnegie Mellon University]
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
 
 *
 * @section DESCRIPTION
 *
 * Template for CE_Graph applications. To create a new application, duplicate
 * this template.
 */



#include <string>

#include "CE_Graph_basic_includes.hpp"

using namespace CE_Graph;

/**
  * Type definitions. Remember to create suitable graph shards using the
  * Sharder-program. 
  */
typedef my_vertex_type VertexDataType;
typedef my_edge_type EdgeDataType;

/**
  * CE_Graph programs need to subclass CE_GraphProgram<vertex-type, edge-type> 
  * class. The main logic is usually in the update function.
  */
struct MyCE_GraphProgram : public CE_GraphProgram<VertexDataType, EdgeDataType> {
    
 
    /**
     *  Vertex update function.
     */
    void update(CE_Graph_vertex<VertexDataType, EdgeDataType> &vertex, CE_Graph_context &gcontext) {

        if (ginfo.iteration == 0) {
            /* On first iteration, initialize vertex (and its edges). This is usually required, because
               on each run, CE_Graph will modify the data files. To start from scratch, it is easiest
               do initialize the program in code. Alternatively, you can keep a copy of initial data files. */
            // vertex.set_data(init_value);
        
        } else {
            /* Do computation */ 

            /* Loop over in-edges (example) */
            for(int i=0; i < vertex.num_inedges(); i++) {
                // Do something
            //    value += vertex.inedge(i).get_data();
            }
            
            /* Loop over out-edges (example) */
            for(int i=0; i < vertex.num_outedges(); i++) {
                // Do something
                // vertex.outedge(i).set_data(x)
            }
            
            /* Loop over all edges (ignore direction) */
            for(int i=0; i < vertex.num_edges(); i++) {
                // vertex.edge(i).get_data() 
            }
            
            // v.set_data(new_value);
        }
    }
    
    /**
     * Called before an iteration starts.
     */
    void before_iteration(int iteration, CE_Graph_context &gcontext) {
    }
    
    /**
     * Called after an iteration has finished.
     */
    void after_iteration(int iteration, CE_Graph_context &gcontext) {
    }
    
    /**
     * Called before an execution interval is started.
     */
    void before_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &gcontext) {        
    }
    
    /**
     * Called after an execution interval has finished.
     */
    void after_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &gcontext) {        
    }
    
};

int main(int argc, const char ** argv) {
    /* CE_Graph initialization will read the command line 
       arguments and the configuration file. */
    CE_Graph_init(argc, argv);
    
    /* Metrics object for keeping track of performance counters
       and other information. Currently required. */
    metrics m("my-application-name");
    
    /* Basic arguments for application */
    std::string filename = get_option_string("file");  // Base filename
    int niters           = get_option_int("niters", 4); // Number of iterations
    bool scheduler       = get_option_int("scheduler", 0); // Whether to use selective scheduling
    
    /* Detect the number of shards or preprocess an input to create them */
    int nshards          = convert_if_notexists<EdgeDataType>(filename, 
                                                            get_option_string("nshards", "auto"));
    
    /* Run */
    MyCE_GraphProgram program;
    CE_Graph_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m); 
    engine.run(program, niters);
    
    /* Report execution metrics */
    metrics_report(m);
    return 0;
}
