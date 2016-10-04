#ifndef HALF_FIT_H_
#define HALF_FIT_H_

/*
 * Author names:
 *   1.  uWaterloo User ID:  dschwarz@uwaterloo.ca
 *   2.  uWaterloo User ID:  xxxxxxxx@uwaterloo.ca
 */

#include "type.h"

#define smlst_blk                       5
#define smlst_blk_sz  ( 1 << smlst_blk )   // 32
#define lrgst_blk                       15 
#define lrgst_blk_sz    ( 1 << lrgst_blk ) // 32768

#define NULL ((char *)0)

struct {
    unsigned int buckets : 11;
} bit_vector;

/**
 * The header in all blocks. Points to the address of the adjacent blocks,
 * stores block size, and an allocated flag
 */
typedef struct {
    // These pointers are considered null if they point to this block of memory
    // to use the pointer, (pointer*32*8)+base_memory_address
    unsigned int previous_block : 10;
    unsigned int next_block : 10;
    // The size of this block, including the header
    // (block_size+1) * 32 bytes = actual block size
    unsigned int block_size: 10;
    // 1 if allocated, 0 if not
    unsigned int allocated : 1;
} block_header_t;

/**
 * points to the previous and next blocks in the bucket
 */
typedef struct {
    unsigned int previous_block : 10;
    unsigned int next_block : 10;
} unused_block_header_t;

void  half_init( void );
void *half_alloc( unsigned int );
void  half_free( void * );

signed int find_bucket(unsigned int size);
signed int get_bucket_index(unsigned int size);
signed int get_guaranteed_bucket(unsigned int size);

void remove_from_known_bucket(void * block_address, unsigned int bucket_index);
void add_to_known_bucket(void * address, unsigned int bucket_index);

unsigned int shorten_address(void * address);
void * expand_address(unsigned int short_address, unsigned int null_pointer_value);

U32 expand_block_size(U32 short_size);
U32 shorten_block_size(U32 size);

U32 round_up_to_chunk_size(U32 value);

#endif

