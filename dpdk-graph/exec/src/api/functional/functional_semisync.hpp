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
 * Semi-synchronous implementation of the functional API.
 */


#ifndef CE_Graph_FUNCTIONAL_SEMISYNC_DEF
#define CE_Graph_FUNCTIONAL_SEMISYNC_DEF

#include <assert.h>


#include "api/graph_objects.hpp"
#include "api/CE_Graph_context.hpp"
#include "api/functional/functional_defs.hpp"

#include "metrics/metrics.hpp"
#include "CE_Graph_types.hpp"

namespace CE_Graph {

template <typename KERNEL>
class functional_vertex_unweighted_semisync : public CE_Graph_vertex<typename KERNEL::VertexDataType, typename KERNEL::EdgeDataType> {
public:
    
    typedef typename KERNEL::VertexDataType VT;
    typedef typename KERNEL::EdgeDataType ET;
    
    VT cumval;
    
    KERNEL kernel;
    vertex_info vinfo;
    CE_Graph_context * gcontext;
    
    functional_vertex_unweighted_semisync() : CE_Graph_vertex<VT, ET> () {}
    
    functional_vertex_unweighted_semisync(CE_Graph_context &ginfo, vid_t _id, int indeg, int outdeg) : 
    CE_Graph_vertex<VT, ET> (_id, NULL, NULL, indeg, outdeg) { 
        vinfo.indegree = indeg;
        vinfo.outdegree = outdeg;
        vinfo.vertexid = _id;
        cumval = kernel.reset();
        gcontext = &ginfo;
    }
    
    functional_vertex_unweighted_semisync(vid_t _id, 
                                 CE_Graph_edge<ET> * iptr, 
                                 CE_Graph_edge<ET> * optr, 
                                 int indeg, 
                                 int outdeg) {
        assert(false); // This should never be called.
    }
    
    void first_iteration(CE_Graph_context &gcontext_) {
        this->set_data(kernel.initial_value(gcontext_, vinfo));
    }
    
    // Optimization: as only memshard (not streaming shard) creates inedgers,
    // we do not need atomic instructions here!
    inline void add_inedge(vid_t src, ET * ptr, bool special_edge) {
        if (gcontext->iteration > 0) {
            cumval = kernel.plus(cumval, kernel.op_neighborval(*gcontext, vinfo, src, *ptr));
        } 
    }
    
    void ready(CE_Graph_context &gcontext_) {
        this->set_data(kernel.compute_vertexvalue(gcontext_, vinfo, cumval));
    }
    
    inline void add_outedge(vid_t dst, ET * ptr, bool special_edge) {
        *ptr = kernel.value_to_neighbor(*gcontext, vinfo, dst, this->get_data());
    }
    
    bool computational_edges() {
        return true;
    }
    
    /* Outedges do not need to be read, they just need to be written */
    static bool read_outedges() {
        return false;
    }
    
    
};



template <typename KERNEL>
    class FunctionalProgramProxySemisync : public CE_GraphProgram<typename KERNEL::VertexDataType, typename  KERNEL::EdgeDataType, functional_vertex_unweighted_semisync<KERNEL> > {
    public:  
    typedef typename KERNEL::VertexDataType VertexDataType;
    typedef typename KERNEL::EdgeDataType EdgeDataType;
    typedef functional_vertex_unweighted_semisync<KERNEL> fvertex_t;
    
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
     * Pagerank update function.
     */
    void update(fvertex_t &v, CE_Graph_context &ginfo) {
        if (ginfo.iteration == 0) {
            v.first_iteration(ginfo);
        } else { 
            v.ready(ginfo);
        }   
    }
    
};

}


#endif


