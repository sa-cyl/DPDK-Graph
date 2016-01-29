
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
    
    VertexDataType * vertex_values;
    
    vid_t neighbor_value(CE_Graph_edge<EdgeDataType> * edge) {
        return vertex_values[edge->vertex_id()];
    }
    
    void set_data(CE_Graph_vertex<VertexDataType, EdgeDataType> &vertex, vid_t value) {
        vertex_values[vertex.id()] = value;
        vertex.set_data(value);
    }
    
    /**
     *  Vertex update function.
     *  On first iteration ,each vertex chooses a label = the vertex id.
     *  On subsequent iterations, each vertex chooses the minimum of the neighbor's
     *  label (and itself).
     */
    void update(CE_Graph_vertex<VertexDataType, EdgeDataType> &vertex, CE_Graph_context &gcontext) {
        /* This program requires selective scheduling. */
        assert(gcontext.scheduler != NULL);
        
        /* On subsequent iterations, find the minimum label of my neighbors */
        vid_t curmin = vertex.get_data();
        for(int i=0; i < vertex.num_edges(); i++) {
            vid_t nblabel = neighbor_value(vertex.edge(i));
            curmin = std::min(nblabel, curmin);
        }
        
        /* If my label changes, schedule neighbors */
        if (vertex.get_data() != curmin) {
            vid_t newlabel = curmin;
            
            for(int i=0; i < vertex.num_edges(); i++) {
                if (newlabel < neighbor_value(vertex.edge(i))) {
                    /* Schedule neighbor for update */
                    gcontext.scheduler->add_task(vertex.edge(i)->vertex_id());
                }
            }
        }
        set_data(vertex, curmin);
    }
    
    /**
     * Called before an iteration starts.
     */
    void before_iteration(int iteration, CE_Graph_context &ctx) {
        if (iteration == 0) {
            /* initialize  each vertex with its own lable */
            vertex_values = new VertexDataType[ctx.nvertices];
            for(int i=0; i < (int)ctx.nvertices; i++) {
                vertex_values[i] = i;
            }
        }
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
    metrics m("connected-components-inmem");
    
    /* Basic arguments for application */
    std::string filename = get_option_string("file");  // Base filename
    int niters           = get_option_int("niters", 10); // Number of iterations (max)
    bool scheduler       = true;    // Always run with scheduler
    
    /* Process input file - if not already preprocessed */
    int nshards             = (int) convert_if_notexists<EdgeDataType>(filename, get_option_string("nshards", "auto"));
    
    if (get_option_int("onlyresult", 0) == 0) {
        /* Run */
        ConnectedComponentsProgram program;
        CE_Graph_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m);
        engine.set_modifies_inedges(false); // Improves I/O performance.
        engine.set_modifies_outedges(false); // Improves I/O performance.

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

