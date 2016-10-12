#include "half_fit.h"
#include <lpc17xx.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include "uart.h"

const int BYTES_TO_BIT_SHIFT = 3;
const int BUCKET_COUNT = 11; // 32-63, 64-127, 128-255, 256-511; 512-1023, 1024-2047, 2048, 4096, 8192, 16384-32767, 32768
const int HEADER_SIZE = 32; // bits
const int HEADER_SIZE_BYTES = 4;
const int CHUNK_SIZE_POWER = 5; // 2^5 = 32 bytes
const int CHUNK_SIZE = 1 << CHUNK_SIZE_POWER; // 32 bytes
const int CHUNK_SIZE_BITS = (1 << CHUNK_SIZE_POWER) << BYTES_TO_BIT_SHIFT; // 256 bits
const int MAX_SIZE_BITS = CHUNK_SIZE_BITS<<10; // bits
const int MAX_SIZE = CHUNK_SIZE<<10; // 1024*32 bytes
// set aside memory (32 kB)
unsigned char memory_pool[MAX_SIZE] __attribute__ ((section(".ARM.__at_0x10000000"), zero_init));
void * memory_address = &memory_pool;

struct bit_vector_t bit_vector;

void *bucket_heads[BUCKET_COUNT];

__inline void * to_pointer(unsigned int x) {
    //todo remove this debugging check
    if ((x - (U32)(memory_address)) >= 32768) {
        printf("Memory address out of bounds %d", x);
    }

    return (void*)(long)(x);
}

void  half_init(void){
    U32 i;
    U32 short_address = 0;
    block_header_t * header = (block_header_t *)(memory_address);
    mprint0("Starting init\n");
    header->next_block = short_address;
    header->previous_block = short_address;
    header->block_size = shorten_block_size(MAX_SIZE_BITS);
    header->allocated = 0;

    // create bit vector that contains whether buckets are empty or not
    bit_vector.buckets = 0;

    for (i = 0; i < BUCKET_COUNT; i++) {
        bucket_heads[i] = 0;
    }
    // add reserved memory to largest bucket
    add_to_known_bucket(memory_address, BUCKET_COUNT-1);

    mprint0("Ending init\n");
}

/**
 * Allocates a block of memory of 'size' bytes or greater. Size of memory will be a multiple of 32
 * @param size
 * @return Pointer
 */
void *half_alloc(U32 size){
    // effective size of size+4. We'll be using that from now on
    U32 block_size;
    block_header_t *header;
    void * first_block_address;
    U32 effective_size = round_up_to_chunk_size(size+HEADER_SIZE_BYTES); // bytes

    // find bucket
    signed int bucket_index = find_bucket(effective_size);
    mprint2("Starting alloc for size: %d, effective_size: %d\n", size, effective_size);
    if (bucket_index == -1) {
        return NULL;
    }

    // take first block from bucket
    first_block_address = bucket_heads[bucket_index];
    header = (block_header_t *)(first_block_address);

    if (first_block_address) {
        // Remove allocated block from its bucket, by modifying the points of its neighbours
        remove_from_known_bucket(first_block_address, (U32)bucket_index);

        // split block if >= 32 bytes larger than requested size
        // block size should be in bytes
        block_size = expand_block_size(header->block_size);

        if (block_size >= effective_size + CHUNK_SIZE) {
            U32 new_block_size;
            S32 new_bucket_index;
            void * new_block_address;
            U32 new_block_short_address;
            block_header_t *new_header;
            block_header_t * next_block;
            U32 short_address;
            mprint("Block size %d is bigger than requested size, splitting\n", block_size);
            // create new free block, add to bucket
            mprint("math: %d\n", block_size - effective_size);
            new_block_size = block_size - effective_size;
            new_block_address = to_pointer((int)(header) + (effective_size<<BYTES_TO_BIT_SHIFT));
            mprint("new block header at %d\n", new_block_address);
            new_block_short_address = shorten_address(new_block_address); // 10 bit address

            // update the header of the newly created block
            new_header = (block_header_t *)(new_block_address);
            mprint0("Assigning header values");
            new_header->block_size = shorten_block_size(new_block_size);
            mprint0("Assigning next_block");
            new_header->next_block = header->next_block;

            short_address = shorten_address(first_block_address);
            new_header->previous_block = short_address;
            new_header->allocated = 0;

            // update previous block of next block
            mprint0("Updating next block");
            next_block = (block_header_t*)(expand_address(header->next_block, (U32)header));
            if (next_block) {
                next_block->previous_block = new_block_short_address;
            }

            // Add new block to appropriate bucket
            mprint0("Adding to bucket");
            new_bucket_index = get_bucket_index(new_block_size);
            if (new_bucket_index == -1) {
                mprint("Invalid index for new bucket of size %d", new_block_size);
            }
            add_to_known_bucket(new_block_address, (U32)new_bucket_index);


            // Change the size of this header
            header->block_size = shorten_block_size(effective_size);
            // update pointers
            header->next_block = new_block_short_address;
        }

        header->allocated = 1;

        mprint0("Ending alloc\n");
        return to_pointer((U32)first_block_address + HEADER_SIZE);

    } else {
        mprint("No address in bucket %d. Returning null", bucket_index);
        return NULL;
    }
}

void  half_free(void * address){
    U32 new_block_size;
    S32 next_bucket_index;
    S32 previous_bucket_index;
    S32 new_block_bucket;
    block_header_t * header;
    block_header_t * new_header;
    block_header_t * previous_block;
    block_header_t * next_block;
    block_header_t * new_next_block;
    // free the block at the given address
    // create a new block from the adjacent blocks, if they are unallocated
    void * effective_address = to_pointer((U32)address - HEADER_SIZE);
    mprint("Starting free address %d\n", address);
    header = (block_header_t *)(effective_address);
    // pointer to the location of the new header
    new_header = header;

    new_block_size = expand_block_size(header->block_size);
    previous_block = (block_header_t *)expand_address(header->previous_block, (U32)effective_address);
    next_block = (block_header_t *)expand_address(header->next_block, (U32)effective_address);
    // pointer to the block after the new block
    new_next_block = next_block;

    if (next_block && !next_block->allocated) {
        new_block_size += expand_block_size(next_block->block_size);
        new_next_block = (block_header_t *)expand_address(next_block->next_block, (U32)next_block);

        next_bucket_index = get_bucket_index(next_block->block_size);
        remove_from_known_bucket(next_block, (U32)next_bucket_index);
    }
    if (previous_block && !previous_block->allocated) {
        new_block_size += expand_block_size(previous_block->block_size);
        new_header = previous_block;
        previous_bucket_index = get_bucket_index(previous_block->block_size);
        remove_from_known_bucket(previous_block, (U32)previous_bucket_index);
    }

    new_header->block_size = shorten_block_size(new_block_size);
    new_header->allocated = 0;

    if (new_next_block) {
        new_header->next_block = shorten_address(new_next_block);
    } else {
        new_header->next_block = shorten_address(new_header); // point to null
    }

    // add block to appropriate bucket
    new_block_bucket = get_bucket_index(new_block_size);
    // todo handle -1 (size is invalid)
    add_to_known_bucket(new_header, (U32)new_block_bucket);
    mprint0("Ending free\n");
}

/**
 * Remove the given, currently unused block from the given bucket
 */
void remove_from_known_bucket(void * block_address, U32 bucket_index) {
    // todo previous in bucket pointer
    void * next_in_bucket_pointer;
    unused_block_header_t *next_header;
    unused_block_header_t *header = (unused_block_header_t*)((int)block_address+HEADER_SIZE);

    mprint2("Removing address %d from bucket %d\n", block_address, bucket_index);

    next_in_bucket_pointer = expand_address(header->next_block, (U32)block_address);

    // could be null, or a valid pointer
    bucket_heads[bucket_index] = next_in_bucket_pointer;

    if (next_in_bucket_pointer) {
        mprint("Updating next in bucket at address: %d\n", next_in_bucket_pointer);
        next_header = (unused_block_header_t*)((int)next_in_bucket_pointer+HEADER_SIZE);
        next_header->previous_block = header->next_block; // point to itself to indicate null;
    } else {
        // bucket is empty
        mprint0("Bucket is empty\n");
        bit_vector.buckets = bit_vector.buckets & !(1 << bucket_index);
    }
    mprint0("Ending remove\n");
}

void add_to_known_bucket(void * address, U32 bucket_index) {
    U32 short_address = shorten_address(address);
    unused_block_header_t *this_header = (unused_block_header_t*)((int)address+HEADER_SIZE);

    // updates pointers in header
    void * next_address = bucket_heads[bucket_index];
    mprint2("Adding address %d to bucket %d\n", address, bucket_index);
    if (next_address) {
        // bucket has children
        unused_block_header_t *next_header = (unused_block_header_t*)((int)next_address+HEADER_SIZE);
        mprint("Next address is %d", next_address);
        next_header->previous_block = short_address;
        this_header->next_block = shorten_address(next_address);
        // this_header->previous_block = short_address; // todo does it matter if previous block is set to null?

        bucket_heads[bucket_index] = address;
    } else {
        mprint("Next block is null, using short_address: %d\n", short_address);
        bucket_heads[bucket_index] = address;
        this_header->next_block = short_address; // set to null by setting to itself
    }

    // update bit vector. Bucket is non empty
    // Put here for extra safety - it could also be put in the else branch
    bit_vector.buckets = bit_vector.buckets | (1 << bucket_index);
    mprint0("Done adding\n");
}

/**
 * Gives the index of the smallest bucket that has a memory block larger than the given size
 * @param size
 * @return
 */
signed int find_bucket(U32 size) {
    signed int guaranteed_index = get_guaranteed_bucket(size);

    if (guaranteed_index == -1) {
        return guaranteed_index;
    } else {
        while((bit_vector.buckets & (1 << guaranteed_index)) == 0) {
            guaranteed_index++;
            if (guaranteed_index >= BUCKET_COUNT) {
                return -1; // early return here, to stop an out of bounds exception later
            }
        }
        return guaranteed_index;
    }
}

/**
 * Get the index for the bucket that contains the given size
 * @param size Size in bytes
 * @return index of the corresponding bucket. -1 if no bucket exists
 */
signed int get_bucket_index(U32 size) {
    int bucket_index;
    int value;
    if (size > MAX_SIZE) {
        mprint("Size is greater than max size: %d\n", size);
        return -1;
    }

    bucket_index = 0;
    // value is initially the number of 32 byte chunks that fit inside size
    value = size >> CHUNK_SIZE_POWER; // shift it right 5

    // keep dividing by two until the value is 1. If the value is 0 or 1, never enter the while loop
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
signed int get_guaranteed_bucket(U32 size) {
    int bucket_index;
    int value;
    int has_remainder;
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

    bucket_index = 0;
    // value is initially the number of 32 byte chunks that fit inside size
    value = size >> CHUNK_SIZE_POWER; // shift it right 5, i.e. divide it by 32
    has_remainder = 0; // true if size is not a power of 2

    if ((size & 31) && value==1) {
        has_remainder=1; // value is in 33-63 range
    }

    // keep dividing the value by two until it equals 1
    while (value > 1) {
        if ((size & 1) != 0) {
            has_remainder = 1;
        }
        value = value >> 1;
        bucket_index++;
    }
    if (has_remainder) {
        // if there was a remainder, then we need the next bucket up to get a guaranteed fit
        bucket_index += 1;
    }
    return bucket_index;
}

/**
 * Given a 10 bit 'pointer', convert it to an actual pointer.
 * Requires the 'null pointer' value be provided. The null_pointer_value is the memory location of the header
 * the pointer is being read from
 */
void * expand_address(U32 short_address, U32 null_pointer_value) {
    void * address = to_pointer((short_address << (CHUNK_SIZE_POWER + BYTES_TO_BIT_SHIFT)) + (int)memory_address);
    if ((int)address == null_pointer_value) {
        return NULL;
    } else {
        return address;
    }
}


U32 shorten_address(void *address) {
    if (address < memory_address) {
        mprint0("ERROR: address is out of bounds\n");
    }
    return (((unsigned int)address) - ((unsigned int)memory_address)) >> (CHUNK_SIZE_POWER + BYTES_TO_BIT_SHIFT);
}

U32 round_up_to_chunk_size(U32 value) {
    U32 remainder = value & (CHUNK_SIZE-1);
    U32 scaled = value >> CHUNK_SIZE_POWER;
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
U32 expand_block_size(U32 short_size) {
    return (short_size + 1) << CHUNK_SIZE_POWER; // multiply by 32
}

/**
 * Given a block size in bytes, shortens it. Block size must be a multiple of 32!
 * @return
 */
U32 shorten_block_size(U32 size) {
    mprint("Shortening block size: %d\n", size);
    if ((size & 31) != 0) {
        mprint("ERROR size is not a multiple of 32: %d", size);
    }
    if (size < 32) {
        mprint0("ERROR size is less than chunk size");
    }
    return (size >> CHUNK_SIZE_POWER) - 1;
}
