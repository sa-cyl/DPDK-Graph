#ifndef DEF_CE_Graph_TYPES
#define DEF_CE_Graph_TYPES


#include <stdint.h>

namespace CE_Graph {
    
    typedef uint32_t vid_t;
    
    
    /** 
      * PairContainer encapsulates a pair of values of some type.
      * Useful for bulk-synchronuos computation.
      */
    template <typename ET>
    struct PairContainer {
        ET left;
        ET right;
        
        PairContainer() {
            left = ET();
            right = ET();
        }
        
        ET & oldval(int iter) {
            return (iter % 2 == 0 ? left : right);
        }
        
        void set_newval(int iter, ET x) {
            if (iter % 2 == 0) {
                right = x;
            } else {
                left = x;
            }
        }
    };

}


#endif



