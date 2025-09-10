#define FOREVER      TRUE
#define PRINT_STEPS  FALSE
#define ALLOCATIONS_COUNT 4096
#define STATIC_ARENA_PAGE_TOTAL_SIZE 1024
#define MAX_AMOUNT_TO_ALLOCATE STATIC_ARENA_PAGE_TOTAL_SIZE - sizeof(uint8_t*)
#define CHANCE_TO_REALLOCATE 0.2f
#define CHANCE_TO_RESET      0.000f

#include "stdlib.h"
#include "assert.h"
#include "stdio.h"
#include "time.h"
#include "stdarg.h"

#define ALLOCATORS_IMPLEMENTATION
#include "allocators.h"


#include "windows.h"
void *PlatformGetMemory(int64_t size) {
    void *memory = VirtualAlloc(0, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    assert(memory);
    return memory; 
}

void PlatformFreeMemory(void *memory) {
    VirtualFree(memory, 0, MEM_RELEASE);
}

int64_t random_i64(int64_t min, int64_t max) {
    assert(min <= max);
    assert(max <= RAND_MAX);
    int64_t res = min + rand() % (max - min + 1);
    return res;
}

uint8_t random_byte(uint8_t min, uint8_t max) {
    assert(min <= max);
    uint8_t res = min + rand() % (max - min + 1);
    return res;
}

float random_float_01() {
    float value = (float)rand();
    value /= (float)RAND_MAX;
    return value;
}

void maybe_printf(char *format, ...) {
#if PRINT_STEPS
    va_list args;
    va_start(args, format); 
    vprintf(format, args);
    va_end(args);
#endif
}

typedef struct Memory Memory;
struct Memory {
    void *ptr;
    int64_t size;
};

void CheckMemory(Memory *memory_array, Memory *reference_array, int64_t count) {
    for (int64_t i=0;i<count;++i) {
        Memory memory   = memory_array[i]; 
        Memory reference = reference_array[i];
    
        assert(memory.size == reference.size && "Internal memory size mismatch"); 
        for (int64_t j=0;j<memory.size;++j) {
            uint8_t memory_byte    = ((uint8_t*)memory.ptr)[j];
            uint8_t reference_byte = ((uint8_t*)reference.ptr)[j];

            if (memory_byte != reference_byte) {
                assert(0 && "memory is corrupted");
            }
        }
    }
}

int main() {
    srand(time(0));

    StaticArena arena = {0};
    int64_t epoch = 0;

    Memory *our_memory_list    = malloc(ALLOCATIONS_COUNT * sizeof(Memory));
    Memory *malloc_memory_list = malloc(ALLOCATIONS_COUNT * sizeof(Memory));
#if !EPOCH_COUNT
    while(true) {
#else 
    for (;epoch<EPOCH_COUNT;) {
#endif
        for (int64_t j=0;j<ALLOCATIONS_COUNT;++j) {
            int64_t to_allocate = random_i64(0, MAX_AMOUNT_TO_ALLOCATE);
            maybe_printf("Allocation: %lld. Allocating %lld\n", j, to_allocate);

            uint8_t *res = StaticArenaAlloc(&arena, to_allocate);
            uint8_t *check = malloc(to_allocate);

            for (int64_t byte_index=0;byte_index<to_allocate;++byte_index) {
                uint8_t byte = random_byte(0, 255);
                res[byte_index] = byte;
                check[byte_index] = byte;
            }
            our_memory_list[j]    = (Memory){res,   to_allocate};
            malloc_memory_list[j] = (Memory){check, to_allocate};

            CheckMemory(our_memory_list, malloc_memory_list, j); 

            float roll = random_float_01();
            if (roll < CHANCE_TO_REALLOCATE) { 
                int64_t new_size = random_i64(0, MAX_AMOUNT_TO_ALLOCATE);
                maybe_printf("Reallocating. New size: %lld\n", new_size);

                uint8_t *realloc_res = StaticArenaReallocLast(&arena, res, new_size);
                uint8_t *realloc_check = realloc(check, new_size);
                
                for (int64_t byte_index=0;byte_index<new_size;++byte_index) {
                    uint8_t byte = random_byte(0, 255);
                    realloc_res[byte_index] = byte;
                    realloc_check[byte_index] = byte;
                }
                our_memory_list[j]    = (Memory){realloc_res,   new_size};
                malloc_memory_list[j] = (Memory){realloc_check, new_size};

                CheckMemory(our_memory_list, malloc_memory_list, j); 
            }

            if (roll < CHANCE_TO_RESET) {
                assert(0 && "Broken(no reset logic for memory arrays)");
                maybe_printf("Resetting arena\n");
                StaticArenaReset(&arena);

                
                CheckMemory(our_memory_list, malloc_memory_list, j); 
            } 
            
        }
        printf("-------Epoch %lld is finished-------\n", epoch);
        epoch += 1;
        StaticArenaReset(&arena);

        for (int64_t index=0;index<ALLOCATIONS_COUNT;++index) {
            Memory mem = malloc_memory_list[index];
            free(mem.ptr);
        }
    }


}
