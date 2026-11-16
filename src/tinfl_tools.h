/* The tools overlay's own copy of miniz's inflater (the image overlay has the other one: an
 * object can only be in one overlay), under other names. */
#pragma once
#define tinfl_decompress                 tools_tinfl_decompress
#define tinfl_decompress_mem_to_mem      tools_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback tools_tinfl_decompress_mem_to_callback
#define tinfl_decompress_mem_to_heap     tools_tinfl_decompress_mem_to_heap
#define tinfl_decompressor_alloc         tools_tinfl_decompressor_alloc
#define tinfl_decompressor_free          tools_tinfl_decompressor_free
#include "third_party/miniz/miniz_tinfl.h"
