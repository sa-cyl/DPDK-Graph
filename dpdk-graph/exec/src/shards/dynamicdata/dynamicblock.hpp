
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
 * Dynamic data version: manages a block.
 */

#ifndef CE_Graph_xcode_dynamicblock_hpp
#define CE_Graph_xcode_dynamicblock_hpp

#include <stdint.h>

namespace CE_Graph {
    
    int get_block_uncompressed_size(std::string blockfilename, int defaultsize);
        int get_block_uncompressed_size(std::string blockfilename, int defaultsize) {
                   size_t fsize;
                   std::string fname = blockfilename + ".size";
                   std::ifstream ifs(fname.c_str());
                   if (!ifs.good()) {
                                   logstream(LOG_FATAL) << "Could not load " << fname << ". Preprocessing forgotten?" << std::endl;
                                   assert(ifs.good());
                                                                   
                              
                   }
                    ifs >> fsize;
                    ifs.close();
                    return fsize;
            
        }
        /*
    int get_block_uncompressed_size(std::string blockfilename, int defaultsize) {
        std::string szfilename = blockfilename + ".size";
        FILE * f = fopen(szfilename.c_str(), "r");
        if (f != NULL) {
            int sz;
            fread(&sz, 1, sizeof(int), f);
            fclose(f);
            return sz;
        } else {
            std::cout<<"open file fail!"<<szfilename<<std::endl;
            assert(false);
        }
    }
    */
    
    void write_block_uncompressed_size(std::string blockfilename, int size);
    void write_block_uncompressed_size(std::string blockfilename, int size) {
        std::string szfilename = blockfilename + ".size";
        FILE * f = fopen(szfilename.c_str(), "w");
        fwrite(&size, 1, sizeof(int), f);
        fclose(f);
        
        if (size > 20000000) {
            logstream(LOG_DEBUG) << "Block " << blockfilename << " size:" << size << std::endl;
        }
    }
    
    void delete_block_uncompressed_sizefile(std::string blockfilename);
    void delete_block_uncompressed_sizefile(std::string blockfilename) {
        std::string szfilename = blockfilename + ".bsize";
        int err = remove(szfilename.c_str());
        if (err != 0) {
            // File did not exist - ok
            
        }
    }
    
    
    template <typename ET>
    struct dynamicdata_block {
        int nitems;
        uint8_t * data;
        ET * chivecs;
        
        dynamicdata_block() : data(NULL), chivecs(NULL) {}
        
        dynamicdata_block(int nitems, uint8_t * data, int datasize) : nitems(nitems){
            chivecs = new ET[nitems];
            uint8_t * ptr = data;
            for(int i=0; i < nitems; i++) {
                assert(ptr - data <= datasize);
                typename ET::sizeword_t * sz = ((typename ET::sizeword_t *) ptr);
                ptr += sizeof(typename ET::sizeword_t);
                chivecs[i] = ET(((uint16_t *)sz)[0], ((uint16_t *)sz)[1], (typename ET::element_type_t *) ptr);
                //if(chivecs[i].nsize!=0||chivecs[i].ncapacity!=0) std::cout<<" 0: "<<chivecs[i].nsize<<"  1: "<<chivecs[i].ncapacity<<std::endl;
                //if((int) ((uint16_t *)sz)[1]>0) std::cout<<"sz[1]: "<<(int) ((uint16_t *)sz)[1]<<"ptr-data"<<(int)(ptr-data)<<"  filesize :"<<datasize<<std::endl;
                ptr += (int) ((uint16_t *)sz)[1] * sizeof(typename ET::element_type_t);
            }
        }
        
        ET * edgevec(int i) {
            if(i>=nitems) std::cout<<"i:  "<<i<<"  nitems: "<<nitems<<std::endl;
            assert(i < nitems);
            assert(chivecs != NULL);
            return &chivecs[i];
        }
        
        void write(uint8_t ** outdata, int & size) {
            // First compute size
            size = 0;
            for(int i=0; i < nitems; i++) {
                size += chivecs[i].capacity() * sizeof(typename ET::element_type_t) + sizeof(typename ET::sizeword_t);
            }
            
            *outdata = (uint8_t *) malloc(size);
            uint8_t * ptr = *outdata;
            for(int i=0; i < nitems; i++) {
                ET & vec = chivecs[i];
                ((uint16_t *) ptr)[0] = vec.size();
                ((uint16_t *) ptr)[1] = vec.capacity();

                ptr += sizeof(typename ET::sizeword_t);
                vec.write((typename ET::element_type_t *)  ptr);
                ptr += vec.capacity() * sizeof(typename ET::element_type_t);
            }
        }
        
        ~dynamicdata_block() {
            if (chivecs != NULL) {
                delete [] chivecs;
            }
        }
        
    };

    
};


#endif
