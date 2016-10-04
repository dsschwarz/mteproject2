#include "half_fit.h"
#include <lpc17xx.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include "uart.h"

const BYTES_TO_BIT_SHIFT = 3;
const short BUCKET_COUNT = 11; // 32-63, 64-127, 128-255, 256-511; 512-1023, 1024-2047, 2048, 4096, 8192, 16384-32767, 32768
const short HEADER_SIZE = 32; // bits
const short CHUNK_SIZE_POWER = 5; // 2^8 = 32*8 = 256
const int CHUNK_SIZE = 1 << CHUNK_SIZE_POWER; // 32 bytes
const int CHUNK_SIZE_BITS = (1 << CHUNK_SIZE_POWER) << BYTES_TO_BIT_SHIFT; // 256 bits
const int MAX_SIZE_BITS = CHUNK_SIZE_BITS<<10; // bits
const int MAX_SIZE = CHUNK_SIZE<<10; // bytes
// set aside memory (32 kB)
unsigned char memory_pool[MAX_SIZE_BITS]__attribute__((section(".ARM.__at_0x10000000"), zero_init));
void * memory_address = &memory_pool;

void *bucket_heads[BUCKET_COUNT];

// indicates whether the buckets are empty or full
bit_vector_t bit_vector;

void  half_init(void){
    int i;

    // create bit vector that contains whether buckets are empty or not
    bit_vector.buckets = 0;
    // add reserved memory to largest bucket
    add_to_known_bucket(memory_address, BUCKET_COUNT-1);
}

/**
 * Allocates a block of memory of 'size' bytes or greater. Size of memory will be a multiple of 32
 * @param size
 * @return Pointer
 */
void *half_alloc(unsigned int size){
    // effective size of size+4. We'll be using that from now on
    unsigned int effective_size = round_up_to_chunk_size(size+4);
    unsigned int block_size;

    // find bucket
    signed int bucket_index = find_bucket(effective_size);
    if (bucket_index == -1) {
        return NULL;
    }

    // take first block from bucket
    void * first_block_address = bucket_heads[bucket_index];
    block_header_t *header = (block_header_t *)(first_block_address);

    if (first_block_address) {
        // Remove allocated block from its bucket, by modifying the points of its neighbours
        remove_from_known_bucket(first_block_address, bucket_index);

        // split block if >= 32 bytes larger than requested size
        // block size should be in bytes
        block_size = expand_block_size(header->block_size);

        // todo find out if the smallest block is 32 bytes (4 bytes header, 28 usable). Will this block ever be used?
        if (block_size >= effective_size + CHUNK_SIZE) {
            // create new free block, add to bucket
            unsigned int new_block_size = block_size - effective_size;
            void * new_block_address = first_block_address + effective_size;
            block_header_t *new_header = (block_header_t *)(new_block_address);
            new_header->block_size = shorten_block_size(new_block_size);
            new_header->next_block = header->next_block;
            new_header->previous_block = shorten_address(first_block_address);
            new_header->allocated = 0;

            // Add new block to appropriate bucket
            unsigned int new_bucket_index = get_bucket_index(new_block_size);
            add_to_known_bucket(new_block_address, new_bucket_index);


            // Change the size of this header
            header->block_size = shorten_block_size(effective_size);
            // update pointers
            header->next_block = shorten_address(new_block_address);
        }

        header->allocated = 1;
        return first_block_address + HEADER_SIZE;

    } else {
        return NULL;
    }

}

void  half_free(void * address){
    // Merge with either/both of neighbouring blocks if they are free
    // add block to appropriate bucket
}

/**
 * Remove the given, currently unused block from the given bucket
 */
void remove_from_known_bucket(void * block_address, void * bucket_index) {
    unused_block_header_t *header = (unused_block_header_t*)(block_address+HEADER_SIZE);

    void * next_in_bucket_pointer = expand_address(header->next_block, block_address);

    bucket_heads[bucket_index] = next_in_bucket_pointer;

    if (next_in_bucket_pointer) {
        unused_block_header_t *next_header = (unused_block_header_t*)(next_in_bucket_pointer+HEADER_SIZE);
        next_header->previous_block = header->next_block; // point to itself to indicate null;
    } else {
        bit_vector.buckets = bit_vector.buckets & !(1 << bucket_index);
    }
}

void add_to_known_bucket(void * address, unsigned int bucket_index) {
    unsigned int short_address = shorten_address(address);
    unused_block_header_t *this_header = (unused_block_header_t*)(address+HEADER_SIZE);

    // updates pointers in header
    void * next_address = bucket_heads[bucket_index];
    if (next_address) {
        // bucket has children
        unused_block_header_t *next_header = (unused_block_header_t*)(next_address+HEADER_SIZE);
        next_header->previous_block = short_address;
        this_header->next_block = shorten_address(next_address);
        // this_header->previous_block = short_address; // todo does it matter if previous block is set to null?

        bucket_heads[bucket_index] = address;
    } else {
        bucket_heads[bucket_index] = address;
        this_header->next_block = short_address; // set to null by setting to itself
    }

    // update bit vector. Bucket is non empty
    // Put here for extra safety - it could also be put in the else branch
    bit_vector.buckets = bit_vector.buckets | (1 << bucket_index);
}

/**
 * Gives the index of the smallest bucket that has a memory block larger than the given size
 * @param size
 * @return
 */
signed int find_bucket(unsigned int size) {
    signed int guaranteed_index = get_guaranteed_bucket(size);

    if (guaranteed_index == -1) {
        return guaranteed_index;
    } else {
        
    }
}

/**
 * Get the index for the bucket that contains the given size
 * @param size Size in bytes
 * @return index of the corresponding bucket. -1 if no bucket exists
 */
signed int get_bucket_index(unsigned int size) {
    if (size > MAX_SIZE) {
        return -1;
    }

    int bucket_index = 0;
    // value is initially the number of 32 byte chunks that fit inside size
    int value = size >> CHUNK_SIZE_POWER; // shift it right 5

    while (value > 1) {
        value = value >> 1;
        bucket_index++;
    }
    return bucket_index;
}
/**
 * Get the index for the bucket that will definitely fit the given size
 * @param size Size in bytes
 * @return index of the corresponding bucket. -1 if no bucket exists
 */
signed int get_guaranteed_bucket(unsigned int size) {
    if (size > MAX_SIZE) {
        return -1;
    }
    // examples
    // 16 bytes -> bucket 0 (32-63 byte bucket)
    // 32 bytes -> bucket 0
    // 33 -> b1 (64-127 byte bucket)
    // 63 -> b1
    // 64 -> b1
    // 65 -> b2 (127-255 byte bucket)
    // 255 -> b2
    // 256 -> b2
    // 257 -> b3 (256-511)

    int bucket_index = 0;
    // value is initially the number of 32 byte chunks that fit inside size
    int value = size >> CHUNK_SIZE_POWER; // shift it right 5
    int has_remainder = 0; // true if size is not a power of 2

    if ((size & 31) && value==1) {
    has_remainder=1; // value is in 33-63 range
    }

    while (value > 1) {
    if ((size & 1) != 0) {
    has_remainder = 1;
    }
    value = value >> 1;
    bucket_index++;
    }
    if (has_remainder) {
    bucket_index += 1;
    }
    return bucket_index;
}

/**
 * Given a 10 bit 'pointer', convert it to an actual pointer.
 * Requires the 'null pointer' value be provided. The null_pointer_value is the memory location of the header
 * the pointer is being read from
 */
void * expand_address(unsigned int short_address, unsigned int null_pointer_value) {
    void * address = (short_address << CHUNK_SIZE_BITS) + memory_address;
    if (address == null_pointer_value) {
        return NULL;
    } else {
        return address;
    }
}

unsigned int shorten_address(void * address) {
    if (address < memory_address) {
        printf("ERROR: address is out of bounds");
    }
    return (address - memory_address) >> (CHUNK_SIZE_BITS);
}

unsigned int round_up_to_chunk_size(unsigned int value) {
    unsigned int remainder = value & CHUNK_SIZE;
    unsigned int scaled = value >> CHUNK_SIZE_POWER;
    if (remainder > 0) {
        scaled += 1;
    }
    return scaled << CHUNK_SIZE_POWER;
}

/**
 * Gets the block size in bytes
 * @param short_size The value stored in the block_size field in the block header
 * @return
 */
unsigned int expand_block_size(unsigned int short_size) {
    return (short_size + 1) << CHUNK_SIZE_POWER; // multiply by 32
}

/**
 * Given a block size in bytes, shortens it. Block size must be a multiple of 32!
 * @return
 */
unsigned int shorten_block_size(unsigned int size) {
    if ((size & 31) != 0) {
        printf("ERROR size is not a multiple of 32: %d", size);
    }
    if (size == 0) {
        printf("ERROR size is 0");
    }
    return (size >> CHUNK_SIZE_POWER) - 1; // multiply by 32
}