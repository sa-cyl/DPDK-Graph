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


#define maxi(a,b) ( ((a)>(b)) ? (a):(b) )
#define mini(a,b) ( ((a)>(b)) ? (b):(a) )

#ifndef DEF_CE_Graph_CE_Graph_ENGINE
#define DEF_CE_Graph_CE_Graph_ENGINE

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <omp.h>
#include <vector>
#include <sys/time.h>
#include <errno.h>

#include "api/chifilenames.hpp"
#include "api/graph_objects.hpp"
#include "api/CE_Graph_context.hpp"
#include "api/CE_Graph_program.hpp"
#include "engine/auxdata/degree_data.hpp"
#include "engine/auxdata/vertex_data.hpp"
#include "engine/bitset_scheduler.hpp"
#include "comm/xgworker.hpp"
#include "io/stripedio.hpp"
#include "logger/logger.hpp"
#include "metrics/metrics.hpp"
#include "shards/memoryshard.hpp"
#include "shards/slidingshard.hpp"
#include "util/pthread_tools.hpp"
#include "output/output.hpp"
#include "schedule/task.hpp"
#include "util/dense_bitset.hpp"
#include "util/toplist.hpp"
#include "shards/dynamicdata/dynamicblock.hpp"
#include"comm/threadpool.hpp"
/* Modified By YouLi Cheng */
static task *T;
static intervallock *iL;

namespace CE_Graph {
    
    template <typename VertexDataType, typename EdgeDataType,   
    typename svertex_t = CE_Graph_vertex<VertexDataType, EdgeDataType> >
    
    class CE_Graph_engine {
    public:     
        typedef sliding_shard<VertexDataType, EdgeDataType, svertex_t> slidingshard_t;
        typedef memory_shard<VertexDataType, EdgeDataType, svertex_t> memshard_t;
        
    protected:
        std::string base_filename;
        int nshards;
        
        /* IO manager */
        stripedio * iomgr;
        
        /* Shards */
        std::vector<slidingshard_t *> sliding_shards;
        memshard_t * memoryshard;
        std::vector<std::pair<vid_t, vid_t> > intervals;
        
        /* Auxilliary data handlers */
        degree_data * degree_handler;
        vertex_data_store<VertexDataType> * vertex_data_handler;
        
        /* Computational context */
        CE_Graph_context chicontext;
        
        /* Scheduler */
        bitset_scheduler * scheduler;
        
        /* Configuration */
        bool modifies_outedges;
        bool modifies_inedges;
        bool disable_outedges;
        bool only_adjacency;
        bool use_selective_scheduling;
        bool enable_deterministic_parallelism;
        bool store_inedges;
        bool disable_vertexdata_storage;
        bool preload_commit; //alow storing of modified edge data on preloaded data into memory
        bool randomization;
        bool initialize_edges_before_run;
        
        size_t blocksize;
        int membudget_mb;
        int load_threads;
        int exec_threads;
		std::queue<int> clientfds; 
        /* State */
        vid_t sub_interval_st;
        vid_t sub_interval_en;
        int iter;
        int niters;
        size_t nupdates;
        size_t nedges;
        size_t work; // work is the number of edges processed
        unsigned int maxwindow;
        mutex modification_lock;
        
        bool reset_vertexdata;
        bool save_edgesfiles_after_inmemmode;        
        /* Outputs */
        std::vector<ioutput<VertexDataType, EdgeDataType> *> outputs;
               /* Metrics */
        metrics &m;
        
        void print_config() {
            logstream(LOG_INFO) << "Engine configuration: " << std::endl;
            logstream(LOG_INFO) << " exec_threads = " << exec_threads << std::endl;
            logstream(LOG_INFO) << " load_threads = " << load_threads << std::endl;
            logstream(LOG_INFO) << " membudget_mb = " << membudget_mb << std::endl;
            logstream(LOG_INFO) << " blocksize = " << blocksize << std::endl;
            logstream(LOG_INFO) << " scheduler = " << use_selective_scheduling << std::endl;
        }
        
    public:
        
        /**
         * Initialize CE_Graph engine
         * @param base_filename prefix of the graph files
         * @param nshards number of shards
         * @param selective_scheduling if true, uses selective scheduling 
         */
        CE_Graph_engine(std::string _base_filename, int _nshards, bool _selective_scheduling, metrics &_m) : base_filename(_base_filename), nshards(_nshards), use_selective_scheduling(_selective_scheduling), m(_m) {
	    /*Modified by YouLi Cheng Start a server for this worker. */
            type_size = sizeof(EdgeDataType);
            inbuf = (char **) calloc(1024, sizeof(char*));
            obuf = (char **) calloc(1024, sizeof(char*));
            for(int i = 0; i<1024; i++){
                olength[i] = -1; 
                inlength[i] = -1;
                inbuf[i] = NULL;
                obuf[i] = NULL;
            }
            pthread_mutex_init(&tlock,NULL);  
            pthread_mutex_init(&olock,NULL);  
	    pthread_t ts1, ts2, ret;
	    ret = pthread_create(&ts1, NULL, &server5, &clientfds);
            assert(ret>=0);
			//ret = pthread_create(&ts2, NULL, &server4, &m_send);
			//assert(ret>=0);
            /* Initialize IO */
            m.start_time("iomgr_init");
            iomgr = new stripedio(m);
            if (disable_preloading()) {
                iomgr->set_disable_preloading(true);
            }
            m.stop_time("iomgr_init");
#ifndef DYNAMICEDATA
            logstream(LOG_INFO) << "Initializing CE_Graph_engine. This engine expects " << sizeof(EdgeDataType)
            << "-byte edge data. " << std::endl;
#else
            logstream(LOG_INFO) << "Initializing CE_Graph_engine with dynamic edge-data. This engine expects " << sizeof(int)
            << "-byte edge data. " << std::endl;

#endif
            /* If number of shards is unspecified - discover */
            if (nshards < 1) {
                nshards = get_option_int("nshards", 0);
                if (nshards < 1) {
                    logstream(LOG_WARNING) << "Number of shards was not specified (command-line argument 'nshards'). Trying to detect. " << std::endl;
                    nshards = discover_shard_num();
                }
            }
            
            /* Initialize a plenty of fields */
            memoryshard = NULL;
            modifies_outedges = true;
            modifies_inedges = true;
            save_edgesfiles_after_inmemmode = false;
            preload_commit = true;
            only_adjacency = false;
            disable_outedges = false;
            reset_vertexdata = false;
            initialize_edges_before_run = false;
            blocksize = get_option_long("blocksize", 4096 * 1024);
#ifndef DYNAMICEDATA
            while (blocksize % sizeof(EdgeDataType) != 0) blocksize++;
#endif
            
            disable_vertexdata_storage = false;

            membudget_mb = get_option_int("membudget_mb", 1024);
            nupdates = 0;
            iter = 0;
            work = 0;
            nedges = 0;
            scheduler = NULL;
            store_inedges = true;
            degree_handler = NULL;
            vertex_data_handler = NULL;
            enable_deterministic_parallelism = true;
            load_threads = get_option_int("loadthreads", 2);
            exec_threads = get_option_int("execthreads", omp_get_max_threads());
            maxwindow = 40000000;

            /* Load graph shard interval information */
            _load_vertex_intervals();
            
            _m.set("file", _base_filename);
            _m.set("engine", "default");
            _m.set("nshards", (size_t)nshards);
        }
        
        virtual ~CE_Graph_engine() {
            if (degree_handler != NULL) delete degree_handler;
            if (vertex_data_handler != NULL) delete vertex_data_handler;
            if (memoryshard != NULL) {
                delete memoryshard;
                memoryshard = NULL;
            }
            for(int i=0; i < (int)sliding_shards.size(); i++) {
                if (sliding_shards[i] != NULL) {
                    delete sliding_shards[i];
                }
                sliding_shards[i] = NULL;
            }
            degree_handler = NULL;
            vertex_data_handler = NULL;
            delete iomgr;
        }
        
        
        
    protected:
        
        virtual degree_data * create_degree_handler() {
            return new degree_data(base_filename, iomgr);
        }
        
        virtual bool disable_preloading() {
            return false;
        }
        
        
            
        /**
         * Try to find suitable shards by trying with different
         * shard numbers. Looks up to shard number 2000.
         */
        int discover_shard_num() {
#ifndef DYNAMICEDATA
            int _nshards = find_shards<EdgeDataType>(base_filename);
#else
            int _nshards = find_shards<int>(base_filename);
#endif
            if (_nshards == 0) {
                logstream(LOG_ERROR) << "Could not find suitable shards - maybe you need to run sharder to create them?" << std::endl;
                logstream(LOG_ERROR) << "Was looking with filename [" << base_filename << "]" << std::endl;
                logstream(LOG_ERROR) << "You need to create the shards with edge data-type of size " << sizeof(EdgeDataType) << " bytes." << std::endl;
                logstream(LOG_ERROR) << "To specify the number of shards, use command-line parameter 'nshards'" << std::endl;
                assert(0);
            }
            return _nshards;
        }
        
       //to be rewrited. 
        virtual void initialize_sliding_shards() {
            assert(sliding_shards.size() == 0);
            for(int p=0; p < nshards; p++) {
#ifndef DYNAMICEDATA
                std::string edata_filename = filename_block_edata<EdgeDataType>(base_filename, exec_interval, p, P, 0);
                std::string adj_filename = filename_block_adj(base_filename, exec_interval, p, P);
                /* Let the IO manager know that we will be reading these files, and
                 it should decide whether to preload them or not.
                 */
                iomgr->allow_preloading(edata_filename);
                iomgr->allow_preloading(adj_filename);
#else
                std::string edata_filename = filename_shard_edata<int>(base_filename, p, nshards); //todo:
                std::string adj_filename = filename_block_adj(base_filename, exec_interval, p, P);
#endif
                
                
                sliding_shards.push_back(
                                         new slidingshard_t(iomgr, edata_filename, 
                                                            adj_filename,
                                                            intervals[p].first, 
                                                            intervals[p].second, 
                                                            blocksize, 
                                                            m, 
                                                            !modifies_outedges, 
                                                            only_adjacency));
                //if (!only_adjacency) 
                //    nedges += sliding_shards[sliding_shards.size() - 1]->num_edges();
            }
            
        }
        
        /* rewrited by YongLi Cheng. */
        virtual void initialize_sliding_shards_XG() {
            for(int p=0; p < nshards; p++) {
#ifndef DYNAMICEDATA
                std::string edata_filename = filename_block_edata<EdgeDataType>(bname, exec_interval, p, P, 1);
                std::string adj_filename = filename_block_adj(bname, exec_interval, p, P);
                /* Let the IO manager know that we will be reading these files, and
                 it should decide whether to preload them or not.
                 */
                iomgr->allow_preloading(edata_filename);
                iomgr->allow_preloading(adj_filename);
#else
                std::string edata_filename = filename_shard_edata<int>(base_filename, p, nshards); //todo:
                std::string adj_filename = filename_block_adj(base_filename, exec_interval, p, P);
#endif
                
                
                sliding_shards.push_back(
                      new slidingshard_t(iomgr, edata_filename, 
                            adj_filename,
                            intervals[p].first, 
                            intervals[p].second, 
                            blocksize, 
                            m, 
                            !modifies_outedges, 
                            only_adjacency));
            }
            
        }

        virtual void initialize_scheduler() {
            if (use_selective_scheduling) {
                if (scheduler != NULL) delete scheduler;
                scheduler = new bitset_scheduler((vid_t) num_vertices());
                scheduler->add_task_to_all();
            } else {
                scheduler = NULL;
            }
        }
        
        /**
         * If the data is only in one shard, we can just
         * keep running from memory.
         */
        bool is_inmemory_mode() {
            return nshards == 1;
        }
        
        
        /**
         * Extends the window to fill the memory budget, but not over maxvid
         */
        virtual vid_t determine_next_window(vid_t iinterval, vid_t fromvid, vid_t maxvid, size_t membudget) {
            /* Load degrees */
            degree_handler->load(fromvid, maxvid);
            
            /* If is in-memory-mode, memory budget is not considered. */
            if (is_inmemory_mode() || svertex_t().computational_edges()) {
                return maxvid;
            } else {
                size_t memreq = 0;
                int max_interval = maxvid - fromvid;
                for(int i=0; i < max_interval; i++) {
                    degree deg = degree_handler->get_degree(fromvid + i);
                    int inc = deg.indegree;
                    int outc = deg.outdegree * (!disable_outedges);
                    
                    // Raw data and object cost included
                    memreq += sizeof(svertex_t) + (sizeof(EdgeDataType) + sizeof(vid_t) + sizeof(CE_Graph_edge<EdgeDataType>))*(outc + inc);
                    if (memreq > membudget) {
                        logstream(LOG_DEBUG) << "Memory budget exceeded with " << memreq << " bytes." << std::endl;
                        return fromvid + i - 1;  // Previous was enough
                    }
                }
                return maxvid;
            }
        }
        
        /** 
         * Calculates the exact number of edges
         * required to load in the subinterval.
         */
        size_t num_edges_subinterval(vid_t st, vid_t en) {
            size_t num_edges = 0;
            int nvertices = en - st + 1;
            if (scheduler != NULL) {
                for(int i=0; i < nvertices; i++) {
                    bool is_sched = scheduler->is_scheduled(st + i);
                    if (is_sched) {
                        degree d = degree_handler->get_degree(st + i);
                        num_edges += d.indegree * store_inedges + d.outdegree;
                    }
                }
            } else {
                for(int i=0; i < nvertices; i++) {
                    degree d = degree_handler->get_degree(st + i);
                    num_edges += d.indegree * store_inedges + d.outdegree;
                }
            }
            return num_edges;
        }
        
        virtual void load_before_updates(std::vector<svertex_t> &vertices) {
            volatile int done = 0;
            omp_set_num_threads(load_threads);
#pragma omp parallel for schedule(dynamic, 1)
            for(int p=-1; p < nshards; p++)  {
                if (p==(-1)) {
                    /* Load memory shard */
                    if (!memoryshard->loaded()) {
                        memoryshard->load_XG();
                    }
                    
                    /* Load vertex edges from memory shard */
                    memoryshard->load_vertices_XG(sub_interval_st, sub_interval_en, vertices, true, !disable_outedges);
                    
                    /* Load vertices */ 
                    vertex_data_handler->load(sub_interval_st, sub_interval_en);

                    /* Load vertices */
                    if (!disable_vertexdata_storage) {
                        vertex_data_handler->load(sub_interval_st, sub_interval_en);
                    }
                } else {
                    /* Load edges from a sliding shard */
                    if (!disable_outedges) {
                        if (p != exec_interval) {
                            if (randomization) {
                              sliding_shards[p]->set_disable_async_writes(true);   
                            }

                            sliding_shards[p]->read_next_vertices_XG(p,(int) vertices.size(), sub_interval_st, vertices,
                                                                  scheduler != NULL && chicontext.iteration == 0);
                            
                        }
                    }
                    __sync_add_and_fetch(&done, 1);
                }
            }
            
            /* Wait for all reads to complete */
            while(done < nshards) {}
            obuf_loaded = 1;
            iomgr->wait_for_reads();
        }
        
        void exec_updates(CE_GraphProgram<VertexDataType, EdgeDataType, svertex_t> &userprogram,
                          std::vector<svertex_t> &vertices) {
            metrics_entry me = m.start_time();
            size_t nvertices = vertices.size();
            if (!enable_deterministic_parallelism) {
                for(int i=0; i < (int)nvertices; i++) vertices[i].parallel_safe = true;
            }
            int sub_interval_len = sub_interval_en - sub_interval_st;

            std::vector<vid_t> random_order(randomization ? sub_interval_len + 1 : 0);
            if (randomization) {
                // Randomize vertex-vector
                for(int idx=0; idx <= (int)sub_interval_len; idx++) random_order[idx] = idx;
                std::random_shuffle(random_order.begin(), random_order.end());
            }
             
            do {
                omp_set_num_threads(exec_threads);
                
        #pragma omp parallel sections 
                    {
        #pragma omp section
                        {
        #pragma omp parallel for schedule(dynamic)
                            for(int idx=0; idx <= (int)sub_interval_len; idx++) {
                                vid_t vid = sub_interval_st + (randomization ? random_order[idx] : idx);
                                svertex_t & v = vertices[vid - sub_interval_st];
                                
                                if (exec_threads == 1 || v.parallel_safe) {
                                    if (!disable_vertexdata_storage)
                                        v.dataptr = vertex_data_handler->vertex_data_ptr(vid);
                                    if (v.scheduled){ 
                                        userprogram.update(v, chicontext);
				    }
                                }
                            }
                        }
        #pragma omp section
                        {
                            if (exec_threads > 1 && enable_deterministic_parallelism) {
                                int nonsafe_count = 0;
                                for(int idx=0; idx <= (int)sub_interval_len; idx++) {
                                    vid_t vid = sub_interval_st + (randomization ? random_order[idx] : idx);
                                    svertex_t & v = vertices[vid - sub_interval_st];
                                    if (!v.parallel_safe && v.scheduled) {
                                        if (!disable_vertexdata_storage)
                                            v.dataptr = vertex_data_handler->vertex_data_ptr(vid);
                                        userprogram.update(v, chicontext);
                                        nonsafe_count++;
                                    }
                                }
                                
                                m.add("serialized-updates", nonsafe_count);
                            }
                        }
                }
            } while (userprogram.repeat_updates(chicontext));
            m.stop_time(me, "execute-updates");
        }
        

        void prev_updates(CE_GraphProgram<VertexDataType, EdgeDataType, svertex_t> &userprogram,
                          std::vector<svertex_t> &vertices) {
            size_t nvertices = vertices.size();
            if (!enable_deterministic_parallelism) {
                for(int i=0; i < (int)nvertices; i++) vertices[i].parallel_safe = true;
            }
            int sub_interval_len = sub_interval_en - sub_interval_st;

            std::vector<vid_t> random_order(randomization ? sub_interval_len + 1 : 0);
            if (randomization) {
                // Randomize vertex-vector
                for(int idx=0; idx <= (int)sub_interval_len; idx++) random_order[idx] = idx;
                std::random_shuffle(random_order.begin(), random_order.end());
            }
             
            do {
                omp_set_num_threads(exec_threads);
                
        #pragma omp parallel sections 
                    {
        #pragma omp section
                        {
        #pragma omp parallel for schedule(dynamic)
                            for(int idx=0; idx <= (int)sub_interval_len; idx++) {
                                vid_t vid = sub_interval_st + (randomization ? random_order[idx] : idx);
                                svertex_t & v = vertices[vid - sub_interval_st];


if(v.crucial_flag == false){
                                
                                if (exec_threads == 1 || v.parallel_safe) {
                                    if (!disable_vertexdata_storage)
                                        v.dataptr = vertex_data_handler->vertex_data_ptr(vid);
                                    if (v.scheduled){ 
                                        userprogram.update(v, chicontext);
				                    }
                                }

}                                


                            }
                        }
        #pragma omp section
                        {
                            if (exec_threads > 1 && enable_deterministic_parallelism) {
                                int nonsafe_count = 0;
                                for(int idx=0; idx <= (int)sub_interval_len; idx++) {
                                    vid_t vid = sub_interval_st + (randomization ? random_order[idx] : idx);
                                    svertex_t & v = vertices[vid - sub_interval_st];
if(v.crucial_flag==false){

                                    if (!v.parallel_safe && v.scheduled) {
                                        if (!disable_vertexdata_storage)
                                            v.dataptr = vertex_data_handler->vertex_data_ptr(vid);
                                        userprogram.update(v, chicontext);
                                        nonsafe_count++;
                                    }

}


                                }
                                
                                m.add("serialized-updates", nonsafe_count);
                            }
                        }
                }
            } while (userprogram.repeat_updates(chicontext));
        }
        
        void crucial_updates(CE_GraphProgram<VertexDataType, EdgeDataType, svertex_t> &userprogram,
                          std::vector<svertex_t> &vertices) {
            metrics_entry me = m.start_time();
            size_t nvertices = vertices.size();
            if (!enable_deterministic_parallelism) {
                for(int i=0; i < (int)nvertices; i++) vertices[i].parallel_safe = true;
            }
            int sub_interval_len = sub_interval_en - sub_interval_st;

            std::vector<vid_t> random_order(randomization ? sub_interval_len + 1 : 0);
            if (randomization) {
                // Randomize vertex-vector
                for(int idx=0; idx <= (int)sub_interval_len; idx++) random_order[idx] = idx;
                std::random_shuffle(random_order.begin(), random_order.end());
            }
             
            do {
                omp_set_num_threads(exec_threads);
                
        	#pragma omp parallel for schedule(dynamic)
                for(int idx=0; idx <= (int)sub_interval_len; idx++) {
                      vid_t vid = sub_interval_st + (randomization ? random_order[idx] : idx);
                      svertex_t & v = vertices[vid - sub_interval_st];
			//std::cout<<"Vid:" << v.vertexid <<" v.num_outedges " << v.num_outedges() << " num_inedges" << v.num_inedges()<<std::endl;
                      if (exec_threads == 1 || v.parallel_safe) {
                              if (!disable_vertexdata_storage)
                                  v.dataptr = vertex_data_handler->vertex_data_ptr(vid);
                              if (v.scheduled){ 
                                  userprogram.update(v, chicontext);
		   	      }
                       }
                 }
            } while (userprogram.repeat_updates(chicontext));
            m.stop_time(me, "execute-updates");
        }
        


        /**
         Special method for running all iterations with the same vertex-vector.
         This is a hacky solution.

         FIXME:  this does not work well with deterministic parallelism. Needs a
         a separate analysis phase to check which vertices can be run in parallel, and
         then run it in chunks. Not difficult.
         **/
        void exec_updates_inmemory_mode(CE_GraphProgram<VertexDataType, EdgeDataType, svertex_t> &userprogram,
                                        std::vector<svertex_t> &vertices) {
            work = nupdates = 0;
            for(iter=0; iter<niters; iter++) {
                logstream(LOG_INFO) << "In-memory mode: Iteration " << iter << " starts. (" << chicontext.runtime() << " secs)" << std::endl;
                chicontext.iteration = iter;
                if (iter > 0) // First one run before -- ugly
                    userprogram.before_iteration(iter, chicontext);
                userprogram.before_exec_interval(0, (int)num_vertices(), chicontext);
                
                if (use_selective_scheduling) {
                    scheduler->new_iteration(iter);
                    if (iter > 0 && !scheduler->has_new_tasks) {
                        logstream(LOG_INFO) << "No new tasks to run!" << std::endl;
                        break;
                    }
                    for(int i=0; i < (int)vertices.size(); i++) { // Could, should parallelize
                        if (iter == 0 || scheduler->is_scheduled(i)) {
                            vertices[i].scheduled =  true;
                            nupdates++;
                            work += vertices[i].inc + vertices[i].outc;
                        } else {
                            vertices[i].scheduled = false;
                        }
                    }
                    
                    scheduler->has_new_tasks = false; // Kind of misleading since scheduler may still have tasks - but no new tasks.
                } else {
                    nupdates += num_vertices();
                    //work += num_edges();
                }
                
                exec_updates(userprogram, vertices);
                load_after_updates(vertices);
                
                userprogram.after_exec_interval(0, (int)num_vertices(), chicontext);
                userprogram.after_iteration(iter, chicontext);
                if (chicontext.last_iteration > 0 && chicontext.last_iteration <= iter){
                   logstream(LOG_INFO)<<"Stopping engine since last iteration was set to: " << chicontext.last_iteration << std::endl;
                   break;
                }

            }
            
            if (save_edgesfiles_after_inmemmode) {
                logstream(LOG_INFO) << "Saving memory shard..." << std::endl;
                
            }
        }
        

        virtual void init_vertices(std::vector<svertex_t> &vertices, CE_Graph_edge<EdgeDataType> * &edata) {
            size_t nvertices = vertices.size();
            
            /* Compute number of edges */
            size_t num_edges = num_edges_subinterval(sub_interval_st, sub_interval_en);
            
            /* Allocate edge buffer */
            edata = (CE_Graph_edge<EdgeDataType>*) malloc(num_edges * sizeof(CE_Graph_edge<EdgeDataType>));
            
            /* Assign vertex edge array pointers */
            size_t ecounter = 0;
            for(int i=0; i < (int)nvertices; i++) {
                degree d = degree_handler->get_degree(sub_interval_st + i);
                int inc = d.indegree;
                int outc = d.outdegree * (!disable_outedges);
                vertices[i] = svertex_t(sub_interval_st + i, &edata[ecounter], 
                                        &edata[ecounter + inc * store_inedges], inc, outc);
                if (scheduler != NULL) {
                    bool is_sched = ( scheduler->is_scheduled(sub_interval_st + i));
                    if (is_sched) {
                        vertices[i].scheduled =  true;
                        nupdates++;
                        ecounter += inc * store_inedges + outc;
                    }
                } else {
                    nupdates++; 
                    vertices[i].scheduled =  true;
                    ecounter += inc * store_inedges + outc;               
                }
            }                   
            work += ecounter;
            assert(ecounter <= num_edges);
        }
        
        
        void save_vertices(std::vector<svertex_t> &vertices) {
            if (disable_vertexdata_storage) return;
            size_t nvertices = vertices.size();
            bool modified_any_vertex = false;
            for(int i=0; i < (int)nvertices; i++) {
                if (vertices[i].modified) {
                    modified_any_vertex = true;
                    break;
                }
            }
            if (modified_any_vertex) {
                vertex_data_handler->save();
            }
        }
        
        virtual void load_after_updates(std::vector<svertex_t> &vertices) {
            // Do nothing.
        }   
        
        virtual void write_delta_log() {
            // Write delta log
            std::string deltafname = iomgr->multiplexprefix(0) + base_filename + ".deltalog";
            FILE * df = fopen(deltafname.c_str(), (chicontext.iteration == 0  ? "w" : "a"));
            fprintf(df, "%d,%lu,%lu,%lf\n", chicontext.iteration, nupdates, work, chicontext.get_delta()); 
            fclose(df);
        }
        
    public:
        
        virtual std::vector< std::pair<vid_t, vid_t> > get_intervals() {
            return intervals;
        }
        
        virtual std::pair<vid_t, vid_t> get_interval(int i) {
            return intervals[i];
        }
        
        /**
         * Returns first vertex of i'th interval.
         */
        vid_t get_interval_start(int i) {
            return get_interval(i).first;
        }
        
        /** 
         * Returns last vertex (inclusive) of i'th interval.
         */
        vid_t get_interval_end(int i) {
            return get_interval(i).second;
        }
        
        virtual size_t num_vertices() {
            return 1 + intervals[nshards - 1].second;
        }
        
        CE_Graph_context &get_context() {
            return chicontext;
        }
        
        virtual int get_nshards() {
            return nshards;
        }
        
        size_t num_updates() {
            return nupdates;
        }
        
        /**
         * Thread-safe version of num_edges
         */
        virtual size_t num_edges_safe() {
            return num_edges();
        }
        
        virtual size_t num_buffered_edges() {
            return 0;
        }
        
        /** 
         * Counts the number of edges from shard sizes.
         */
        virtual size_t num_edges() {
            if (sliding_shards.size() == 0) {
                logstream(LOG_ERROR) << "engine.num_edges() can be called only after engine has been started. To be fixed later. As a workaround, put the engine into a global variable, and query the number afterwards in begin_iteration(), for example." << std::endl;
                assert(false);
            }
            if (only_adjacency) {
                // TODO: fix.
                logstream(LOG_ERROR) << "Asked number of edges, but engine was run without edge-data." << std::endl; 
                return 0;
            }
            return nedges;
        }
        
        /**
         * Checks whether any vertex is scheduled in the given interval.
         * If no scheduler is configured, returns always true.
         */
        // TODO: support for a minimum fraction of scheduled vertices
        bool is_any_vertex_scheduled(vid_t st, vid_t en) {
            if (scheduler == NULL) return true;
            for(vid_t v=st; v<=en; v++) {
                if (scheduler->is_scheduled(v)) {
                    return true;
                }
            }
            return false;
        }
        
        virtual void initialize_iter() {
            // Do nothing
        }
        
        virtual void initialize_before_run() {
            if (reset_vertexdata) {
                vertex_data_handler->clear(num_vertices());
            }
        }
        
        virtual memshard_t * create_memshard(vid_t interval_st, vid_t interval_en) {
#ifndef DYNAMICEDATA
            return new memshard_t(this->iomgr,
                                  filename_shard_edata<EdgeDataType>(base_filename, exec_interval, nshards),  
                                  filename_shard_adj(base_filename, exec_interval, nshards),  
                                  interval_st, 
                                  interval_en,
                                  blocksize,
                                  m);
#else
            return new memshard_t(this->iomgr,
                                  filename_shard_edata<int>(base_filename, exec_interval, nshards),
                                  filename_shard_adj(base_filename, exec_interval, nshards),
                                  interval_st,
                                  interval_en,
                                  blocksize,
                                  m);
#endif
        }
      
	/*
	 * Rewrited by Yongli Cheng 2014/12/20
	 */
	void run(CE_GraphProgram<VertexDataType, EdgeDataType, svertex_t> &userprogram, int _niters) {
            m.start_time("runtime");
	    randomization = get_option_int("randomization", 0) == 1;
            
            std::cout<<"running...."<<std::endl;
            int fd_rw,c,m1;
	    int wret, len, ntop;
	    int c1=0;
	    char buf[1024 * 1024 + 1];
	    char workerIP[1024][15]; //save worker IP 
            int ret;

            while(fd_master < 0) {}//waiting for xgMaster connection. volatile int fd_master
            ret =  read(fd_master, buf, 1024 * 1024); //Receive 'S' Package
	        if(ret < 0) std::cout<<"xgMaster shutdown, exit...."<<std::endl;
            assert(ret > 0);

            memcpy(&c, buf+6, sizeof(int));
            memcpy(&M, buf+6+sizeof(int), sizeof(int)); //machine num
            memcpy(&N, buf+6+2*sizeof(int), sizeof(int)); //niters time
            memcpy(&P, buf+6+3*sizeof(int), sizeof(int)); // ==  machine num subgraph num
            memcpy(&CO, buf+6+4*sizeof(int), sizeof(int));
            memcpy(&ntop, buf+6+5*sizeof(int), sizeof(int));

            T = (task*)malloc(sizeof(task) * c);
            iL = (intervallock*)malloc(sizeof(intervallock) * P);

	    std::cout<<"C:"<<c<<"M: "<<M<<"N:  "<<N<<"P:  "<<P<<"  CO: "<<CO<<" ntop: "<<ntop<<std::endl;

	    /* worker IP */
	    for(int k = 0; k < M; k++) memcpy(workerIP[k], buf+6+6*sizeof(int)+k*15, 15);
	    for(int k = 0; k < M; k++) std::cout<<"worker: "<<workerIP[k]<<std::endl;
			    /* Init iL */
            for(int i=0; i<P; i++){
		        iL[i].interval = -1;
		        for(int j=0;j<2*P-1;j++) iL[i].lock[j] = '0';
            }


            /* send a message to xgMaster. */
            char* msg = (char*)"XGACKS";
            if (send(fd_master, msg, strlen(msg), 0) == -1)
                   std::cout<<"send back XGACKS fail!"<<std::endl;

            if (degree_handler == NULL)
                degree_handler = create_degree_handler();

            niters = N;   //todo:need or not
            logstream(LOG_INFO) << "FG starting" << std::endl;
            logstream(LOG_INFO) << "Licensed under the Apache License 2.0" << std::endl;
            logstream(LOG_INFO) << "Copyright YongLi Cheng et al., HuaZhong Technology University (2014)" << std::endl;

            if (vertex_data_handler == NULL)
                vertex_data_handler = new vertex_data_store<VertexDataType>(base_filename, num_vertices(), iomgr);

            initialize_before_run();
            initialize_scheduler();
            /* Init slidingshards. */
            bname = base_filename;
            initialize_sliding_shards_XG();
            omp_set_nested(1);

            /* Install a 'mock'-scheduler to chicontext if scheduler is not used. */
            chicontext.scheduler = scheduler;
            if (scheduler == NULL) {
                chicontext.scheduler = new non_scheduler();
            }

            /* Print configuration */
            print_config();

	    /* Load and construct subgraph. */
	    interval = CO;
            exec_interval = CO;
	    std::cout<<"Init Subgraph: "<<exec_interval<<std::endl;

 	    /* Determine interval limits */
            vid_t interval_st = get_interval_start(exec_interval);
            vid_t interval_en = get_interval_end(exec_interval);
            /* Initialize memory shard */
            if (memoryshard != NULL) delete memoryshard;
            memoryshard = create_memshard(interval_st, interval_en);
            memoryshard->only_adjacency = only_adjacency;
            memoryshard->set_disable_async_writes(randomization);

            sub_interval_st = interval_st;
            sub_interval_en = interval_en;

	    degree_handler->load(sub_interval_st, sub_interval_en);

            /* Initialize vertices */
            int nvertices = sub_interval_en - sub_interval_st + 1;
            CE_Graph_edge<EdgeDataType> * edata = NULL;
            std::vector<svertex_t> vertices(nvertices, svertex_t());
            init_vertices(vertices, edata);


            /* Load data */
            load_before_updates(vertices);
	    std::cout<<"Init Subgraph: "<<exec_interval<<" finished."<<std::endl;
	    /* Send a message to xgMaster this init has finished. */
            //char* msg = (char*)"XGACKS";
            if (send(fd_master, msg, strlen(msg), 0) == -1)
                   std::cout<<"send back XGACKS fail!"<<std::endl;

            /* Main loop */
	    //fcntl(fd_master,F_SETFL,O_NONBLOCK);

	//create socket

	/*		
	struct sockaddr_in    servaddr[M];
	int sockfd[M];	
 	for(int i = 0;i < M;++i){
		if( (sockfd[i] = socket(AF_INET, SOCK_STREAM, 0)) < 0){
    			printf("create socket error: %s(errno: %d)\n", strerror(errno),errno);
    			exit(0);
		}
    	} //sockfd[i]  for every worker
	for(int i = 0; i < M;++i){
		memset(&servaddr[i], 0, sizeof(servaddr[i]));
    		servaddr[i].sin_family = AF_INET;
    		servaddr[i].sin_port = htons(6666);//port
    		if( inet_pton(AF_INET, workerIP[i], &servaddr[i].sin_addr) <= 0){
    			printf("inet_pton error for %s\n",argv[1]);
   	 		exit(0);
    		}
	
    		if( connect(sockfd[i], (struct sockaddr*)&servaddr[i], sizeof(servaddr[i])) < 0){
   	 		printf("connect error: %s(errno: %d)\n",strerror(errno),errno);
    			exit(0);
    		}
	}*/
	// connect to M -1 worker
	std::queue<connecter> mque;
	int index = 0;
	int sockfd =0;
	struct sockaddr_in serv_addr;
	for(index  = 0; index < M ;++index){
		if(index ==exec_interval)continue;
	        if (( sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
               std::cout<<"socket error!"<<std::endl;
               exit(-1);
        }
		
        bzero(&serv_addr,sizeof(serv_addr));
        serv_addr.sin_family    = AF_INET;
        serv_addr.sin_port      = htons(PORT1);
        serv_addr.sin_addr.s_addr= inet_addr(workerIP[index]);
        if(connect(sockfd, (struct sockaddr *)&serv_addr,sizeof(struct sockaddr)) == -1) {
              std::cout<<"connect error!"<<errno<<std::endl;
              exit(-1);
        }
		connecter tmp;
		tmp.sockfd = sockfd;
		tmp.exec_interval = index;
		std::cout <<"send  workerIP"<<workerIP[index]<< "worker ID" <<index <<"sockfd" << sockfd<<std::endl;
	
		mque.push(tmp);
	}
	std::cout << "connect to other worker already"<<std::endl;
		CThreadManager * pManager_send = new CThreadManager(m_recv_to_worker,m_send_to_worker,M-1);
		CThreadManager * pManager_recv = new CThreadManager(m_recv_to_worker,m_send_to_worker,M-1);

			
			
			for(int i = 0; i < N; i ++){
	    /* Waiting for message from xgMaster to begin a new iteration. */

            	ret =  read(fd_master, buf, 1024); //Receive 'I' Package

                metrics_entry me4 = m.start_time();
                std::stringstream ss4;
                ss4<<"Iteration"<<i<<" runtime";
		std::cout<<"Starting iteration .......... "<<i<<"/"<<N<<std::endl;

            	modification_lock.lock();
	    	crucial_updates(userprogram, vertices);
	    	modification_lock.unlock();
		std::cout<<"Iteration "<<i<<" finished."<<std::endl;
		m.stop_time(me4,ss4.str(),false);
/*
		int m_test = 0;
		while(m_test < M- 1){
			connecter tmp = mque.front(); 
			mque.pop();
			mque.push(tmp);
			std::cout <<"need to SEND this sockfd" <<tmp.sockfd <<"to worker"<<tmp.exec_interval<<std::endl;
			++m_test;
		}
		m_test =0;
		std::cout << clientfds.size()<<std::endl;
		while(m_test < M -1){
			int tmp = clientfds.front(); 
			clientfds.pop();
			clientfds.push(tmp);
			std::cout <<"need to RECV this sockfd" <<tmp <<std::endl;
			++m_test;

		}
*/
 /* TODO: :Send P-1 data blocks to other P-1 subgraphs. */
		int j = 0;
		int recv_jobs = 0;
		int send_jobs = 0;
		metrics_entry me5 = m.start_time();
                std::stringstream ss5;
                ss5<<"Iteration"<<i<<"data change";
		std::cout<<"Starting change .......... "<<i<<"/"<<N<<std::endl;

		while(send_jobs < M-1){
			if(exec_interval == 1)break;
			connecter tmp = mque.front();
			mque.pop();
			mque.push(tmp);
			//bind send
			m_work info;
			info.sockfd = tmp.sockfd;
			info.p = exec_interval;
			info.type = 2;
			if(info.exec_interval == 1)continue;
			info.buf =obuf[tmp.exec_interval];
			info.olength = olength[tmp.exec_interval];
			pManager_send->PushWorkQue(info);
			++send_jobs;
		}
		pManager_send->start();
		while(recv_jobs < M -1){
			int tmp = clientfds.front();
			clientfds.pop();
			clientfds.push(tmp);
			//bind recv
			m_work info;
			info.type =1 ;
			info.sockfd = tmp;
			pManager_recv->PushWorkQue(info);
			++recv_jobs;
		}
		pManager_recv->start();
		pManager_send->wait();
		pManager_recv->wait();
		//sleep(1);		
		std::cout<<"change data "<<i<<" finished."<<std::endl;
		m.stop_time(me5,ss5.str(),false);

	       	/* Send a message to xgMaster this iteration has finished. */
            	char* msg = (char*)"XGACKS";
            	if (send(fd_master, msg, strlen(msg), 0) == -1)
                   std::cout<<"send back XGACKS fail!"<<std::endl;

	    }
	delete pManager_recv;
	delete pManager_send;
	while(!mque.empty()){
		connecter tmp = mque.front();
		mque.pop();
		close(tmp.sockfd);
	}
	
        }
        
        virtual void iteration_finished() {
            // Do nothing
        }
        
        stripedio * get_iomanager() {
            return iomgr;
        }
        
        virtual void set_modifies_inedges(bool b) {
            modifies_inedges = b;
        }
        
        virtual void set_modifies_outedges(bool b) {
            modifies_outedges = b;
        }
        
        virtual void set_only_adjacency(bool b) {
            only_adjacency = b;
        }

        virtual void set_preload_commit(bool b){
            preload_commit = b;
        }
        
        virtual void set_disable_outedges(bool b) {
            disable_outedges = b;
        }
        
        /**
         * Configure the blocksize used when loading shards.
         * Default is one megabyte.
         * @param blocksize_in_bytes the blocksize in bytes
         */
        void set_blocksize(size_t blocksize_in_bytes) {
            blocksize = blocksize_in_bytes;
        }
        
        /**
         * Set the amount of memory available for loading graph
         * data. Default is 1000 megabytes.
         * @param mbs amount of memory to be used.
         */
        void set_membudget_mb(int mbs) {
            membudget_mb = mbs;
        }
        
        
        void set_load_threads(int lt) {
            load_threads = lt;
        }
        
        void set_exec_threads(int et) {
            exec_threads = et;
        }
        
        /**
         * Sets whether the engine is run in the deterministic
         * mode. Default true.
         */
        void set_enable_deterministic_parallelism(bool b) {
#ifdef DYNAMICEDATA
            if (!b) {
                logstream(LOG_ERROR) << "With dynamic edge data, you cannot disable determinic parallelism." << std::endl;
                logstream(LOG_ERROR) << "Otherwise race conditions would corrupt the structure of the data." << std::endl;
                assert(b);
                return;
            }
#endif
            enable_deterministic_parallelism = b;
        }
      
    public:
        void set_disable_vertexdata_storage() {
            this->disable_vertexdata_storage = true;
        }
        
        void set_enable_vertexdata_storage() {
            this->disable_vertexdata_storage = false;
        }
       
        void set_maxwindow(unsigned int _maxwindow){ 
            maxwindow = _maxwindow;
        }; 
        
        
        /* Outputs */
        size_t add_output(ioutput<VertexDataType, EdgeDataType> * output) {
            outputs.push_back(output);
            return (outputs.size() - 1);
        }
         
        ioutput<VertexDataType, EdgeDataType> * output(size_t idx) {
            if (idx >= outputs.size()) {
                logstream(LOG_FATAL) << "Tried to get output with index " << idx << ", but only " << outputs.size() << " outputs were initialized!" << std::endl;
            }
            assert(idx < outputs.size());
            return outputs[idx];
        }
        
    protected:
              
        virtual void _load_vertex_intervals() {
            load_vertex_intervals(base_filename, nshards, intervals);
        }
        
    protected:
        mutex httplock;
        std::map<std::string, std::string> json_params;
        
    public:
        
        /**
         * Replace all shards with zero values in edges.
         */
        template<typename ET>
        void reinitialize_edge_data(ET zerovalue) {
            
            for(int p=0; p < nshards; p++) {
                std::string edatashardname =  filename_shard_edata<ET>(base_filename, p, nshards);
                std::string dirname = dirname_shard_edata_block(edatashardname, blocksize);
                size_t edatasize = get_shard_edata_filesize<ET>(edatashardname);
                logstream(LOG_INFO) << "Clearing data: " << edatashardname << " bytes: " << edatasize << std::endl;
                int nblocks = (int) ((edatasize / blocksize) + (edatasize % blocksize == 0 ? 0 : 1));
                for(int i=0; i < nblocks; i++) {
                    std::string block_filename = filename_shard_edata_block(edatashardname, i, blocksize);
                    int len = (int) std::min(edatasize - i * blocksize, blocksize);
                    int f = open(block_filename.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
                    ET * buf =  (ET *) malloc(len);
                    for(int i=0; i < (int) (len / sizeof(ET)); i++) {
                        buf[i] = zerovalue;
                    }
                    write_compressed(f, buf, len);
                    close(f);
                    
#ifdef DYNAMICEDATA
                    write_block_uncompressed_size(block_filename, len);
#endif
                    
                }
            }
        }
        
        
        /**
          * If true, the vertex data is initialized before
          * the engineis started. Default false.
          */
        void set_reset_vertexdata(bool reset) {
            reset_vertexdata = reset;
        }
        
        
        /**
         * Whether edges should be saved after in-memory mode
         */
        virtual void set_save_edgesfiles_after_inmemmode(bool b) {
            this->save_edgesfiles_after_inmemmode = b;
        }

        virtual void set_initialize_edges_before_run(bool b) {
            this->initialize_edges_before_run = b;
        }
        
        
        /**
         * HTTP admin management
         */
        
        void set_json(std::string key, std::string value) {
            httplock.lock();
            json_params[key] = value;
            httplock.unlock();
        }
        
        template <typename T>
        void set_json(std::string key, T val) {
            std::stringstream ss;
            ss << val;
            set_json(key, ss.str());
        }
        
        std::string get_info_json() {
            std::stringstream json;
            json << "{";
            json << "\"file\" : \"" << base_filename << "\",\n";
            json << "\"numOfShards\": " << nshards << ",\n";
            json << "\"iteration\": " << chicontext.iteration << ",\n";
            json << "\"numIterations\": " << chicontext.num_iterations << ",\n";
            json << "\"runTime\": " << chicontext.runtime() << ",\n";
            
            json << "\"updates\": " << nupdates << ",\n";
            json << "\"nvertices\": " << chicontext.nvertices << ",\n";
            json << "\"interval\":" << exec_interval << ",\n";
            json << "\"windowStart\":" << sub_interval_st << ",";
            json << "\"windowEnd\": " << sub_interval_en << ",";
            json << "\"shards\": [";
            
            for(int p=0; p < (int)nshards; p++) {
                if (p>0) json << ",";
                
                json << "{";
                json << "\"p\": " << p << ", ";
                json << sliding_shards[p]->get_info_json();
                json << "}";
            }
            
            json << "]";
            json << "}";
            return json.str();
        }
        
    };
    
    
};



#endif


