#define EPOCH_COUNT 0
#define PRINT_STEPS             FALSE
#define ENABLE_INTEGRITY_CHECKS TRUE
#define NORMAL_ALLOCATION_SIZE  1024
#define MAX_AMOUNT_TO_ALLOCATE  1024*100
#define ALLOCATION_COUNT        1024*5
#define CHANCE_TO_REALLOCATE    10
#define CHANCE_TO_DEALLOCATE    10

#include "stdlib.h"
#include "assert.h"
#include "stdio.h"
#include "time.h"

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

uint64_t HighResolutionClockFrequency() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq); 
    return freq.QuadPart;
}

uint64_t GetHighResolutionClock() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter); 
    return counter.QuadPart;
}

double HighResolutionClockDiff(uint64_t start, uint64_t end) {
    double diff = (float)(end - start);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq); 

    double res = diff / (float)freq.QuadPart;
    return res;
}


int64_t random_i64(int64_t min, int64_t max) {
    assert(min <= max);
    int64_t res = min + rand() % (max - min + 1);
    return res;
}

uint8_t random_byte(uint8_t min, uint8_t max) {
    assert(min <= max);
    uint8_t res = min + rand() % (max - min + 1);
    return res;
}

void maybe_random_fill(uint8_t *first_buffer, uint8_t *second_buffer, int64_t size) {
#if ENABLE_INTEGRITY_CHECKS
    for (int64_t byte_index=0;byte_index<size;++byte_index) {
        uint8_t byte = random_byte(0, 255);
        first_buffer[byte_index]  = byte;
        second_buffer[byte_index] = byte;
    }
#endif
}

int64_t GetBlackNodesCountOfTheFirstLeaf(AllocationNode *root) {
    assert(root);

    AllocationNode *node = root;
    int64_t black_nodes_count = 0;
    while(true) {
        if (node->color == RBT_BLACK) {
            black_nodes_count += 1;
        }

        if (!node->left) {
            break;
        }

        node = node->left;
    }
    return black_nodes_count;
}

bool TestNodeIntegrity(AllocationNode *node, int64_t black_nodes_count, int64_t target) {
    if (node->color == RBT_BLACK) {
        black_nodes_count += 1;
    }

    if (!node->left && !node->right) {
        if (target != black_nodes_count) {
            printf("Unexpected black ancestors count: expected %lld, got %lld\n", target, black_nodes_count);
            return false;
        }
        return true;
    }

    if (node->left) {
        AllocationNode *left = node->left;
        if (left->parent != node) {
            if (!left->parent) {
                printf("Invalid node(%lld) parent(null)\n", left->size);
                return false;
            }
            printf("Invalid node(%lld) parent(%lld)\n", left->size, left->parent->size);
            return false;
        }
        if (node->color == RBT_RED && node->left->color == RBT_RED) { 
            printf("Red node has a red child\n");
            return false;
        }
        bool ok = TestNodeIntegrity(node->left, black_nodes_count, target);
        if (!ok) {
            return false;
        }
    }
    
    if (node->right) {
        AllocationNode *right = node->right;
        if (node->right->parent != node) {
            if (!right->parent) {
                printf("Invalid node(%lld) parent(null)\n", right->size);
                return false;
            }
            printf("Invalid node(%lld) parent(%lld)\n", right->size, right->parent->size);
            return false;
        }
        if (node->color == RBT_RED && node->right->color == RBT_RED) {
            printf("Red node has a red child\n");
            return false;
        }
        bool ok = TestNodeIntegrity(node->right, black_nodes_count, target);
        if (!ok) {
            return false;
        }
    }

    return true;
}

void TestRBTIntegrity(AllocationNode *root) {
#if ENABLE_INTEGRITY_CHECKS
    if (!root) {
        return;
    } 

    int64_t target = GetBlackNodesCountOfTheFirstLeaf(root);
    bool res = TestNodeIntegrity(root, 0, target);
    assert(res && "Red-Black tree integrity test failed");
#endif
}

void TestAllocatorIntegrity(HeapArena *arena, int64_t allocated_size) {
#if ENABLE_INTEGRITY_CHECKS
    int64_t remaining_size = allocated_size;

    // memory blocks
    AllocationNode *previos_node = 0;
    AllocationNode *node         = 0;
    MemoryBlock *block = arena->first_block;
    AllocationNode *ll_node = arena->first_node;
    bool have_seen_first_node = false;

    // note: this loop may segfault, but then we know that allocator is definetely corrupted
    while(block) {
        node = SkipMemoryBlockHeader(block); 
        if (!have_seen_first_node) {
            have_seen_first_node = true;
            assert(node == arena->first_node && "Invalid first node");
        }

        while(true) {
            assert(ll_node == node && "Linked list node and in-memory node differ");
            if (node->occupied) {
                remaining_size -= node->used_size;
            }
            if (!node->next_in_order || node->next_in_order->memory_block != block) {
                if (ll_node->next_in_order) {
                    ll_node = ll_node->next_in_order;
                }
                break;
            }
            
            AllocationNode *next = node->next_in_order; 
            assert(next->previous_in_order == node && "Invalid next_in_order");

            uint8_t *next_ptr = (uint8_t*)SkipAllocationNode(node) + node->size;
            node = (AllocationNode*)next_ptr;
            ll_node = ll_node->next_in_order;
        } 

        if (!block->next) {
            assert(block == arena->last_block && "Invalid last block");
            assert(node  == arena->last_node  && "Invalid last node");
        }

        block = block->next;
    }

    if (!have_seen_first_node) {
        assert(arena->first_block == 0);
        assert(arena->first_node  == 0);
    }

    assert(remaining_size == 0 && "Invalid allocated size");
#endif
}

typedef struct Memory Memory;
struct Memory {
    void *ptr;
    int64_t size;
};

void maybe_printf(char *format, ...) {
#if PRINT_STEPS
    va_list args;
    va_start(args, format); 
    vprintf(format, args);
    va_end(args);
#endif
}

void CheckMemory(Memory *memory_array, Memory *reference_array, int64_t count) {
#if ENABLE_INTEGRITY_CHECKS
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
#endif
}

int main() {
    srand(time(0));

    HeapArena arena = {0};
    int64_t epoch = 0;

    Memory *our_memory_list    = malloc(ALLOCATION_COUNT * sizeof(Memory));
    Memory *malloc_memory_list = malloc(ALLOCATION_COUNT * sizeof(Memory));
#if !EPOCH_COUNT
    while(true) {
#else
    for (int64_t j=0;j<EPOCH_COUNT;++j) {
#endif
    int64_t memory_index = 0;

    uint64_t epoch_start = GetHighResolutionClock();
    double our_alloc_time = 0;
    double our_realloc_time = 0;
    double our_free_time = 0;
    double std_alloc_time = 0;
    double std_realloc_time = 0;
    double std_free_time = 0;
    int64_t epoch_allocated_size = 0;
    for (int64_t i=0; i<ALLOCATION_COUNT; ++i) {
        int64_t to_allocate = random_i64(0, MAX_AMOUNT_TO_ALLOCATE);
        epoch_allocated_size += to_allocate;

        maybe_printf("Iteration(%lld), allocating %lld\n", i, to_allocate);


        uint64_t start = GetHighResolutionClock(); 
        void *malloc_memory = malloc(to_allocate);
        uint64_t end = GetHighResolutionClock();
        double time_diff = HighResolutionClockDiff(start, end);
        std_alloc_time += time_diff;

        start = GetHighResolutionClock(); 
        void *our_memory = HeapArenaAllocate(&arena, to_allocate);
        end = GetHighResolutionClock();
        time_diff = HighResolutionClockDiff(start, end);
        our_alloc_time += time_diff;


        maybe_random_fill(our_memory, malloc_memory, to_allocate);
#if PRINT_STEPS
        maybe_printf("Iteration(%lld), memory %p\n", i, our_memory);
        HeapArenaDump(&arena);
        maybe_printf("\n\n");
#endif

        assert(0 <= memory_index && memory_index < ALLOCATION_COUNT);
        our_memory_list[memory_index] = (Memory){our_memory, to_allocate};

        malloc_memory_list[memory_index] = (Memory){malloc_memory, to_allocate};  
        memory_index += 1;

        CheckMemory(our_memory_list, malloc_memory_list, memory_index);
        TestAllocatorIntegrity(&arena, epoch_allocated_size); 
        TestRBTIntegrity(arena.root);

        int64_t roll = random_i64(0, 100);
        if (roll < CHANCE_TO_REALLOCATE) {
            int64_t index_to_extend = random_i64(0, memory_index-1);
            Memory our_memory_to_extend = our_memory_list[index_to_extend]; 
            Memory malloc_memory_to_extend = malloc_memory_list[index_to_extend];

            int64_t new_size = random_i64(0, MAX_AMOUNT_TO_ALLOCATE);
            int64_t old_size = our_memory_to_extend.size;
            int64_t size_diff = new_size - old_size;
            epoch_allocated_size += size_diff;
            maybe_printf("Iteration(%lld), reallocating(%p) from %lld to %lld bytes\n", i, GetAllocationNode(our_memory_to_extend.ptr), old_size, new_size);
 

            start = GetHighResolutionClock();
            void *new_malloc_memory = realloc(malloc_memory_to_extend.ptr, new_size);
            end = GetHighResolutionClock();
            time_diff = HighResolutionClockDiff(start, end);
            std_realloc_time += time_diff;

            start = GetHighResolutionClock();
            void *new_ptr = HeapArenaRealloc(&arena, our_memory_to_extend.ptr, new_size);
            end = GetHighResolutionClock();
            time_diff = HighResolutionClockDiff(start, end);
            our_realloc_time += time_diff;


            maybe_printf("\tOld address(%p), New address(%p)\n", GetAllocationNode(our_memory_to_extend.ptr), GetAllocationNode(new_ptr));

            // note: first we check that old contents are preserved, and ony then refill memory
            int64_t preserved_size = new_size <= old_size ? new_size : old_size;
            our_memory_list[index_to_extend]    = (Memory){new_ptr, preserved_size};
            malloc_memory_list[index_to_extend] = (Memory){new_malloc_memory, preserved_size};
            CheckMemory(our_memory_list, malloc_memory_list, memory_index);

            our_memory_list[index_to_extend] = (Memory){new_ptr, new_size};
            malloc_memory_list[index_to_extend] = (Memory){new_malloc_memory, new_size};
            maybe_random_fill(new_ptr, new_malloc_memory, new_size);
            CheckMemory(our_memory_list, malloc_memory_list, memory_index);
            TestAllocatorIntegrity(&arena, epoch_allocated_size); 
            TestRBTIntegrity(arena.root);
#if PRINT_STEPS
            HeapArenaDump(&arena);
            maybe_printf("\n\n");
#endif
        } 
        if (roll < CHANCE_TO_DEALLOCATE) { 
            assert(memory_index);
            int64_t index_to_free = random_i64(0, memory_index-1);
            Memory memory_to_free = our_memory_list[index_to_free];     
            Memory malloc_to_free = malloc_memory_list[index_to_free];
            epoch_allocated_size -= memory_to_free.size;

            maybe_printf("Iteration(%lld), deallocating(%p)\n", i, GetAllocationNode(memory_to_free.ptr));

            start = GetHighResolutionClock();
            free(malloc_to_free.ptr);
            end = GetHighResolutionClock();
            time_diff = HighResolutionClockDiff(start, end);
            std_free_time += time_diff;

            start = GetHighResolutionClock();
            HeapArenaFree(&arena, memory_to_free.ptr);
            end = GetHighResolutionClock();
            time_diff = HighResolutionClockDiff(start, end);
            our_free_time += time_diff;

            our_memory_list[index_to_free] = our_memory_list[memory_index-1];
            our_memory_list[memory_index-1] = (Memory){0};
            malloc_memory_list[index_to_free] = malloc_memory_list[memory_index-1];
            malloc_memory_list[memory_index - 1] = (Memory){0};

            memory_index -= 1;
            CheckMemory(our_memory_list, malloc_memory_list, memory_index);
            TestAllocatorIntegrity(&arena, epoch_allocated_size);
            TestRBTIntegrity(arena.root);
#if PRINT_STEPS
            HeapArenaDump(&arena);
            maybe_printf("\n\n");
#endif
        }
    }
  
    uint64_t cleanup_start = GetHighResolutionClock();

    HeapArenaRelease(&arena);
    for (int64_t index=0;index<memory_index;++index) {
        Memory mem = malloc_memory_list[index];
        free(mem.ptr);
    }

    uint64_t cleanup_end = GetHighResolutionClock();
    float cleanup_time = HighResolutionClockDiff(cleanup_start, cleanup_end);

    uint64_t freq      = HighResolutionClockFrequency();
    uint64_t epoch_end = GetHighResolutionClock();
    float epoch_time = HighResolutionClockDiff(epoch_start, epoch_end);

    printf("-------Epoch %lld is finished-------\n", epoch);
    printf("Total   time: %lf second\n", epoch_time);
    printf("Cleanup time: %lf second\n", cleanup_time);
    printf("\n");
    printf("Our Time: %lf seconds\n", our_alloc_time + our_realloc_time + our_free_time);
    printf("    Alloc   Time: %lf seconds\n", our_alloc_time);
    printf("    Realloc Time: %lf seconds\n", our_realloc_time);
    printf("    Free    Time: %lf seconds\n", our_free_time);
    printf("\n");
    printf("Std Time: %lf seconds\n", std_alloc_time + std_realloc_time + std_free_time);
    printf("    Alloc   Time: %lf seconds\n", std_alloc_time);
    printf("    Realloc Time: %lf seconds\n", std_realloc_time);
    printf("    Free    Time: %lf seconds\n", std_free_time);
    printf("\n");
    printf("Arena: Allocated size: %lld\n", arena.allocated_size);
    printf("Arena: Free size: %lld\n", arena.free_size);
    printf("Epoch: Allocted size: %lld\n", epoch_allocated_size);

    epoch += 1;

    }
}
