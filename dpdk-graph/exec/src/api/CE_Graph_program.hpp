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

#ifndef CE_Graph_PROGRAM_DEF
#define CE_Graph_PROGRAM_DEF

#include "api/graph_objects.hpp"
#include "api/CE_Graph_context.hpp"

namespace CE_Graph {
    
    template <typename VertexDataType_, typename EdgeDataType_,
                typename vertex_t = CE_Graph_vertex<VertexDataType_, EdgeDataType_> >
    class CE_GraphProgram {
        
    public:
        typedef VertexDataType_ VertexDataType;
        typedef EdgeDataType_ EdgeDataType;
        
        virtual ~CE_GraphProgram() {}
        
        /**
         * Called before an iteration starts.
         */
        virtual void before_iteration(int iteration, CE_Graph_context &gcontext) {
        }
        
        /**
         * Called after an iteration has finished.
         */
        virtual void after_iteration(int iteration, CE_Graph_context &gcontext) {
        }
        
        /**
         * Support for the new "rinse" method. An app can ask the vertices currently in
         * memory be updated again before moving to new interval or iteration.
         */
        virtual bool repeat_updates(CE_Graph_context &gcontext) {
            return false;
        }
        
        
        
        /**
         * Called before an execution interval is started.
         */
        virtual void before_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &gcontext) {        
        }
        
        /**
         * Called after an execution interval has finished.
         */
        virtual void after_exec_interval(vid_t window_st, vid_t window_en, CE_Graph_context &gcontext) {        
        }
        
        /**
         * Update function.
         */
        virtual void update(vertex_t &v, CE_Graph_context &gcontext)  = 0;    
    };

}

#endif

