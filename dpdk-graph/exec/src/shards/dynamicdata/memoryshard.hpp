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

#ifndef DEF_CE_Graph_MEMSHARD
#define DEF_CE_Graph_MEMSHARD


#include <iostream>
#include <cstdio>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string>

#include "api/graph_objects.hpp"
#include "metrics/metrics.hpp"
#include "io/stripedio.hpp"
#include "CE_Graph_types.hpp"
#include "shards/dynamicdata/dynamicblock.hpp"

namespace CE_Graph {
    
       
    template <typename VT, typename ET, typename svertex_t = CE_Graph_vertex<VT, ET> >
    class memory_shard {
        
        stripedio * iomgr;
        
        std::string filename_edata;
        std::string filename_adj;
        
        vid_t range_st;
        vid_t range_end;
        size_t adjfilesize;
        size_t edatafilesize;
        
        size_t edgeptr;
        vid_t streaming_offset_vid;
        size_t streaming_offset; // The offset where streaming should continue
        size_t range_start_offset; // First byte for this range's vertices (used for writing only outedges)
        size_t range_start_edge_ptr;
        size_t streaming_offset_edge_ptr;
        uint8_t * adjdata;
        char ** edgedata;
        std::vector<size_t> blocksizes;
        uint64_t chunkid;
        
        std::vector<int> block_edatasessions;
        int adj_session;
        
        bool is_loaded;
        size_t blocksize;
        metrics &m;

        bool disable_async_writes;

        
    public:
        std::vector< dynamicdata_block<ET> * > dynamicblocks;
        bool only_adjacency;
        
        /* Dynamic edata */ 
        memory_shard(stripedio * iomgr,
                     std::string _filename_edata,
                     std::string _filename_adj,
                     vid_t _range_start,
                     vid_t _range_end,
                     size_t _blocksize,
                     metrics &_m) : iomgr(iomgr), filename_edata(_filename_edata),
        filename_adj(_filename_adj),
        range_st(_range_start), range_end(_range_end), blocksize(_blocksize),  m(_m) {
            adjdata = NULL;
            only_adjacency = false;
            is_loaded = false;
            disable_async_writes= false;
            adj_session = -1;
            edgedata = NULL;
        }
        
        /* Dynamic edata */ 
        ~memory_shard() {
            int nblocks = (int) block_edatasessions.size();
            for(int i=0; i < P; i++) {
                /*
                if (edgedata[i] != NULL) {
                    iomgr->managed_release(block_edatasessions[i], &edgedata[i]);
                    iomgr->close_session(block_edatasessions[i]);
                    
                }
                */
                if (dynamicblocks[i] != NULL){
                    delete dynamicblocks[i];
                }
                dynamicblocks[i] = NULL;
            }
            dynamicblocks.clear();
            if (adj_session >= 0) {
                if (adjdata != NULL) iomgr->managed_release(adj_session, &adjdata);
                iomgr->close_session(adj_session);
            }
            if (edgedata != NULL)
                free(edgedata);
            edgedata = NULL;
        }
        
        
        void set_disable_async_writes(bool b) {
            disable_async_writes = b;
        }
        
        /* Dynamic edata */ 
        void write_and_release_block(int i) {
            std::string block_filename = filename_shard_edata_block(filename_edata, i, blocksize);

            dynamicdata_block<ET> * dynblock = dynamicblocks[i];
            if (dynblock != NULL) {
                uint8_t * outdata;
                int outsize;
                dynblock->write(&outdata, outsize);
                write_block_uncompressed_size(block_filename, outsize);
                iomgr->managed_pwritea_now(block_edatasessions[i], &outdata, outsize, 0);
                iomgr->managed_release(block_edatasessions[i], &edgedata[i]);
                iomgr->close_session(block_edatasessions[i]);
                free(outdata);
                delete dynblock;
            }
            dynamicblocks[i] = NULL;
        }
        
        /* Dynamic edata */ 
        void commit(bool commit_inedges, bool commit_outedges) {
            if (block_edatasessions.size() == 0 || only_adjacency) return;
            assert(is_loaded);
            metrics_entry cm = m.start_time();
            
            /**
             * This is an optimization that is relevant only if memory shard
             * has been used in a case where only out-edges are considered.
             * Out-edges are in a continuous "window", while in-edges are
             * scattered all over the shard
             */
            int nblocks = (int) block_edatasessions.size();

            if (commit_inedges) {
                for(int i=0; i < nblocks; i++) {
                    /* NOTE: WRITE ALL BLOCKS SYNCHRONOUSLY */
                    write_and_release_block(i);
                    edgedata[i] = NULL;
                }
            } else if (commit_outedges) {
                size_t last = streaming_offset_edge_ptr;
                if (last == 0){
                    // rollback
                    last = edatafilesize;
                }
                //char * bufp = ((char*)edgedata + range_start_edge_ptr);
                int startblock = (int) (range_start_edge_ptr / blocksize);
                int endblock = (int) (last / blocksize);
                for(int i=0; i < nblocks; i++) {
                    if (i >= startblock && i <= endblock) {
                        write_and_release_block(i);
                    } else {
                        iomgr->managed_release(block_edatasessions[i], &edgedata[i]);
                    }
                    edgedata[i] = NULL;
                    iomgr->close_session(block_edatasessions[i]);
                }
            }
            m.stop_time(cm, "memshard_commit");
            
            iomgr->managed_release(adj_session, &adjdata);
            // FIXME: this is duplicated code from destructor
            for(int i=0; i < nblocks; i++) {
                if (edgedata[i] != NULL) {
                    iomgr->managed_release(block_edatasessions[i], &edgedata[i]);
                }
            }
            block_edatasessions.clear();
            is_loaded = false;
        }

        void commit_XG(bool commit_inedges, bool commit_outedges) {
            metrics_entry cm = m.start_time();
            int volatile cc = 0;
            if(CO == 2){ 
                #pragma omp parallel for
                for(int i=0; i < P; i++) {
                     assert(inbuf[i]!=NULL);
                     iomgr->managed_pwritea_now(block_edatasessions[i], &inbuf[i], inlength[i], 0); 
                     iomgr->managed_release(block_edatasessions[i], &inbuf[i]);
                     iomgr->close_session(block_edatasessions[i]);
                     inbuf[i] = NULL;
                     inlength[i] = -1; 
                     __sync_add_and_fetch(&cc, 1); 
                 }   
                  while(cc < P); 
                                
            }else{
                for(int i=0; i<P; i++)iomgr->close_session(block_edatasessions[i]); 
            }   
                  
            assert(adjdata!=NULL);
            iomgr->managed_release(adj_session, &adjdata);
            iomgr->close_session(adj_session);
                                                                                                                                                                                                                  
            block_edatasessions.clear();
            is_loaded = false;
            m.stop_time(cm, "memshard_commit");
        } 
        
        bool loaded() {
            return is_loaded;
        }
        
    private:
        
        /* Dynamic edata */ 
        void load_edata() {
            bool async_inedgedata_loading = false; // Not supported with dynamic edgedata
            assert(blocksize % sizeof(int) == 0);
            int nblocks = (int) (edatafilesize / blocksize + (edatafilesize % blocksize != 0));
            edgedata = (char **) calloc(nblocks, sizeof(char*));
            size_t compressedsize = 0;
            int blockid = 0;
           
            
            while(true) {
                std::string block_filename = filename_shard_edata_block(filename_edata, blockid, blocksize);
                if (file_exists(block_filename)) {
                    size_t fsize = get_block_uncompressed_size(block_filename, std::min(edatafilesize - blocksize * blockid, blocksize)); //std::min(edatafilesize - blocksize * blockid, blocksize);
                    compressedsize += get_filesize(block_filename);
                    int blocksession = iomgr->open_session(block_filename, false, true); // compressed
                    block_edatasessions.push_back(blocksession);
                    blocksizes.push_back(fsize);
                    edgedata[blockid] = NULL;
                    iomgr->managed_malloc(blocksession, &edgedata[blockid], fsize, 0);
                    if (async_inedgedata_loading) {
                        assert(false);
                    } else {
                        iomgr->managed_preada_now(blocksession, &edgedata[blockid], fsize, 0);
                    }
                    dynamicblocks.push_back(NULL);

                    blockid++;

                } else {
                    if (blockid == 0) {
                        logstream(LOG_ERROR) << "Shard block file did not exists:" << block_filename << std::endl;
                    }
                    break;
                }
            }
            assert(blockid == nblocks);
            logstream(LOG_DEBUG) << "Compressed/full size: " << compressedsize * 1.0 / edatafilesize <<
                            " number of blocks: " << nblocks << std::endl;
        }
        void load_edata_XG() {
             int nblocks;
             int dirt;
             if(CO == 0){
                 std::cout<<"dynamicdata only for CO==1,2,3"<<std::endl;
                 assert(false);
            }else if(CO == 1 || CO == 3) { 
                 if(interval<M) dirt = interval;
                 else dirt = mini(M, P-1);
                 nblocks = P - dirt;
            }else if(CO == 2){
                 nblocks = P;
                     
            } 
             size_t compressedsize = 0;
             int blockid = 0;

             int *doneptr = (int *) malloc(nblocks * sizeof(int));
             for(int i=0; i < nblocks; i++) doneptr[i] = 1;

             int pos;
             for(int i = 0; i < P; i++){
                    dynamicblocks.push_back(NULL);
                    std::string block_filename = filename_block_edata<int>(bname, exec_interval, i, P, 0); 
                    if (file_exists(block_filename)) {
                           size_t fsize = get_block_uncompressed_size(block_filename, -1);  //fsize = nedges * sizeof(int);                   
                           size_t fs = get_filesize(block_filename); //fs = the bytes of the file;
                           bool isload=false;
                           pos = exec_interval -1;
                           for(int y = 0; y < nblocks; y++){
                                 pos++;
                                 if(pos>=P) pos = 0;
                                 if(pos == i) {
                                      isload=true;
                                      break;
                                 }
                           }
                           compressedsize += fsize;
                           int blocksession = iomgr->open_session(block_filename, false, true);                       
                           block_edatasessions.push_back(blocksession);
                           blocksizes.push_back(fsize);                    
                           if(isload){
                                iomgr->managed_malloc(blocksession, &(inbuf[i]), fs+1, 0);
                                iomgr->managed_preada_now(block_edatasessions[i], &inbuf[i], fs, 0);
                                inlength[i] = fs;
                           } else{
                                //pthread_mutex_lock(&tlock);
                                while(inlength[i]==-1){ //waitint for crucial block...
                                }
                                //pthread_mutex_unlock(&tlock);
                           }
                     } else {
                            if (blockid == 0) {
                                  logstream(LOG_ERROR) << "Shard block file did not exists:" << block_filename << std::endl;
                            }
                            if (blockid < nblocks) {
                                  logstream(LOG_ERROR) << "Did not find block " << block_filename << std::endl;
                                  logstream(LOG_ERROR) << strerror(errno)<<std::endl;
                                  logstream(LOG_ERROR) << "Going to exit..." <<std::endl;
                             }
                             assert(false);
                      }
                                      
                 } 
        }
        /* Initialize a dynamic block if required */
        void check_block_initialized(int blockid) {
            if (dynamicblocks[blockid] == NULL) {
                int nedges = blocksizes[blockid] / sizeof(int);
                //std::cout<<"bid:   "<<blockid<<"blocksizes: "<<blocksizes[blockid]<<"filezie: "<<inlength[blockid]<<"exec_interval: "<< exec_interval<<std::endl;
                dynamicblocks[blockid] = new dynamicdata_block<ET>(nedges, (uint8_t*) inbuf[blockid], inlength[blockid]);
            }
        }
        
        
    public:
        
        /* Dynamic edata */ 
        void load() {
            is_loaded = true;
            adjfilesize = get_filesize(filename_adj);
            edatafilesize = get_shard_edata_filesize<ET>(filename_edata);            
            
#ifdef SUPPORT_DELETIONS
            async_inedgedata_loading = false;  // Currently we encode the deleted status of an edge into the edge value (should be changed!),
            // so we need the edge data while loading
#endif
            
            //preada(adjf, adjdata, adjfilesize, 0);
            
            adj_session = iomgr->open_session(filename_adj, true);
            iomgr->managed_malloc(adj_session, &adjdata, adjfilesize, 0);
            
            size_t bufsize = 16 * 1204 * 1024;
            int n = (int) (adjfilesize / bufsize + 1);

#pragma omp parallel for
            for(int i=0; i < n; i++) {
                size_t toread = std::min(adjfilesize - i * bufsize, (size_t)bufsize);
                iomgr->preada_now(adj_session, adjdata + i * bufsize, toread, i * bufsize, true);
            }
            
            
            /* Initialize edge data asynchonous reading */
            if (!only_adjacency) {
                load_edata();
            }
        }
        
        void load_XG() {
            is_loaded = true;
            filename_adj = filename_block_adj(bname, exec_interval, exec_interval, P);
            adjfilesize = get_filesize(filename_adj);
            //edatafilesize = get_shard_edata_filesize<ET>(filename_edata);//todo:cyl            
            
#ifdef SUPPORT_DELETIONS
            async_inedgedata_loading = false;  // Currently we encode the deleted status of an edge into the edge value (should be changed!),
            // so we need the edge data while loading
#endif
            
            //preada(adjf, adjdata, adjfilesize, 0);
            
            adj_session = iomgr->open_session(filename_adj, true);
            iomgr->managed_malloc(adj_session, &adjdata, adjfilesize, 0);
            
            size_t bufsize = 16 * 1204 * 1024;
            int n = (int) (adjfilesize / bufsize + 1);

#pragma omp parallel for
            for(int i=0; i < n; i++) {
                size_t toread = std::min(adjfilesize - i * bufsize, (size_t)bufsize);
                iomgr->preada_now(adj_session, adjdata + i * bufsize, toread, i * bufsize, true);
            }
            
            
            /* Initialize edge data asynchonous reading */
            if (!only_adjacency) {
                load_edata_XG();
            }
        }
        
        
        
        /* Dynamic edata */ 
        void load_vertices_XG(vid_t window_st, vid_t window_en, std::vector<svertex_t> & prealloc, bool inedges=true, bool outedges=true){
             m.start_time("memoryshard_create_edges");
             assert(adjdata != NULL);
             uint8_t * ptr = adjdata;
             uint8_t * end = ptr + adjfilesize;
             vid_t vid = 0;
             edgeptr = 0;
             streaming_offset = 0;
             streaming_offset_vid = 0;
             streaming_offset_edge_ptr = 0;
             range_start_offset = adjfilesize;
             range_start_edge_ptr = edatafilesize;
             bool setoffset = false;
             bool setrangeoffset = false;
             size_t bid = 0;
             size_t brange = blocksizes[0];
             size_t edatasize = 0;
             for(int i = 0; i < P; i ++){
                 edatasize += blocksizes[i];
             }
             //std::cout<<"step...........memory..........1"<<std::endl;
             while (ptr < end) {
                  if (!setoffset && vid > range_end) {
                       streaming_offset = ptr-adjdata;
                       streaming_offset_vid = vid;
                       streaming_offset_edge_ptr = edgeptr;
                       setoffset = true;
                  }
                  if (!setrangeoffset && vid>=range_st) {
                      range_start_offset = ptr-adjdata;
                      range_start_edge_ptr = edgeptr;
                      setrangeoffset = true;
                  }
                  uint8_t ns = *ptr;
                  int n;
                  ptr += sizeof(uint8_t);

                  if (ns == 0x00) {                                                                                                                                                     
                      uint8_t nz = *ptr;
                      ptr += sizeof(uint8_t);
                      vid++;
                      vid += nz;
                      continue;
                   }
                  if (ns == 0xff) {
                      n = *((uint32_t*)ptr);
                      ptr += sizeof(uint32_t);
                  } else {
                      n = ns;
                  }
                  svertex_t* vertex = NULL;
                                    
                  if (vid>=window_st && vid <=window_en) {
                          vertex = &prealloc[vid-window_st];
                          if (!vertex->scheduled) vertex = NULL;
                  }
                  bool any_edges = false;
                  while(--n>=0) {
                      //int blockid = (int) (edgeptr / blocksize);
                      /*
                      if (!only_adjacency) {
                            while(doneptr[edgeptr / blocksize] != 0) { usleep(10); 
                            }
                      }
                      */
                      vid_t target = *((vid_t*) ptr);
                      ptr += sizeof(vid_t);
                      if (vertex != NULL && outedges)
                      {
                           check_block_initialized(bid);
                           vertex->add_outedge(target, (only_adjacency ? NULL : dynamicblocks[bid]->edgevec((edgeptr-(brange-blocksizes[bid]))/sizeof(int))), false);
                      }
                                                        
                      if (target >= window_st)  {
                            if (target <= window_en) {
                                 if (inedges) {
                                      svertex_t & dstvertex = prealloc[target - window_st];
                                      if((vid<window_st || vid>window_en)&&(CO==1 || CO==3)) dstvertex.crucial_flag = true;
                                      if (dstvertex.scheduled) {
                                            any_edges = true;
                                            check_block_initialized(bid);
                                            ET * eptr = (only_adjacency ? NULL  : dynamicblocks[bid]->edgevec((edgeptr-(brange-blocksizes[bid]))/sizeof(int)));
                                            dstvertex.add_inedge(vid,  (only_adjacency ? NULL : (ET*) eptr), false);
                                            dstvertex.parallel_safe = dstvertex.parallel_safe && (vertex == NULL);
                                    }
                                 }
                            } else {
                                 if (vertex == NULL) {
                                      ptr += sizeof(vid_t) * n;
                                      edgeptr += (n + 1) * sizeof(int);
                                      while(edgeptr>=brange) {
                                          bid++;
                                          brange += blocksizes[bid];
                                      }
                                      break;
                                 }
                             }
                    }
                     
                        edgeptr += sizeof(int);
                        if(edgeptr>=brange) {
                            bid++;
                            brange += blocksizes[bid];
                        } 
                    }
                                    
                    if (any_edges && vertex != NULL) {
                           vertex->parallel_safe = false;
                    }
                    vid++;
                                
            }
                    //std::cout<<"step...........memory..........2"<<std::endl;
                    m.stop_time("memoryshard_create_edges", false);
                
        }


        void load_vertices(vid_t window_st, vid_t window_en, std::vector<svertex_t> & prealloc, bool inedges=true, bool outedges=true) {
            /* Find file size */            
            m.start_time("memoryshard_create_edges");
            
            assert(adjdata != NULL);
            
            // Now start creating vertices
            uint8_t * ptr = adjdata;
            uint8_t * end = ptr + adjfilesize;
            vid_t vid = 0;
            edgeptr = 0;
            
            streaming_offset = 0;
            streaming_offset_vid = 0;
            streaming_offset_edge_ptr = 0;
            range_start_offset = adjfilesize;
            range_start_edge_ptr = edatafilesize;
            
            bool setoffset = false;
            bool setrangeoffset = false;
            while (ptr < end) {
                if (!setoffset && vid > range_end) {
                    // This is where streaming should continue. Notice that because of the
                    // non-zero counters, this might be a bit off.
                    streaming_offset = ptr-adjdata;
                    streaming_offset_vid = vid;
                    streaming_offset_edge_ptr = edgeptr;
                    setoffset = true;
                }
                if (!setrangeoffset && vid>=range_st) {
                    range_start_offset = ptr-adjdata;
                    range_start_edge_ptr = edgeptr;
                    setrangeoffset = true;
                }
                
                uint8_t ns = *ptr;
                int n;
                
                ptr += sizeof(uint8_t);
                
                if (ns == 0x00) {
                    // next value tells the number of vertices with zeros
                    uint8_t nz = *ptr;
                    ptr += sizeof(uint8_t);
                    vid++;
                    vid += nz;
                    continue;
                }
                
                if (ns == 0xff) {  // If 255 is not enough, then stores a 32-bit integer after.
                    n = *((uint32_t*)ptr);
                    ptr += sizeof(uint32_t);
                } else {
                    n = ns;
                }
                svertex_t* vertex = NULL;
                
                if (vid>=window_st && vid <=window_en) { // TODO: Make more efficient
                    vertex = &prealloc[vid-window_st];
                    if (!vertex->scheduled) vertex = NULL;
                }
                bool any_edges = false;
                while(--n>=0) {
                    int blockid = (int) (edgeptr / blocksize);
                                        
                    vid_t target = *((vid_t*) ptr);
                    ptr += sizeof(vid_t);
                    if (vertex != NULL && outedges)
                    {
                        check_block_initialized(blockid);
                        vertex->add_outedge(target, (only_adjacency ? NULL : dynamicblocks[blockid]->edgevec((edgeptr % blocksize)/sizeof(int))), false);
                    }
                    
                    if (target >= window_st)  {
                        if (target <= window_en) {                        /* In edge */
                            if (inedges) {
                                svertex_t & dstvertex = prealloc[target - window_st];
                                if (dstvertex.scheduled) {
                                    any_edges = true;
                                    //  assert(only_adjacency ||  edgeptr < edatafilesize);
                                    check_block_initialized(blockid);
                                    ET * eptr = (only_adjacency ? NULL  : dynamicblocks[blockid]->edgevec((edgeptr % blocksize)/sizeof(int)));

                                    dstvertex.add_inedge(vid,  (only_adjacency ? NULL : eptr), false);
                                    dstvertex.parallel_safe = dstvertex.parallel_safe && (vertex == NULL); // Avoid if
                                }
                            }
                        } else { // Note, we cannot skip if there can be "special edges". FIXME so dirty.
                            // This vertex has no edges any more for this window, bail out
                            if (vertex == NULL) {
                                ptr += sizeof(vid_t) * n;
                                edgeptr += (n + 1) * sizeof(int);
                                break;
                            }
                        }
                    }
                    edgeptr += sizeof(int);
                    
                }
                
                if (any_edges && vertex != NULL) {
                    vertex->parallel_safe = false;
                }
                vid++;
            }
            m.stop_time("memoryshard_create_edges", false);
        }
        
        size_t offset_for_stream_cont() {
            return streaming_offset;
        }
        vid_t offset_vid_for_stream_cont() {
            return streaming_offset_vid;
        }
        size_t edata_ptr_for_stream_cont() {
            return streaming_offset_edge_ptr;
        }
        
        
        
        
    };
};

#endif

