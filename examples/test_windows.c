#define EPOCH_COUNT 0
#define PRINT_STEPS 0
#define MAX_AMOUNT_TO_ALLOCATE 1287
#define ALLOCATION_COUNT 2*1024*1099
#define CHANCE_TO_REALLOCATE     50
#define CHANCE_TO_DEALLOCATE     42
#define COMPARE_AGAINST_MALLOC 0

#include "stdlib.h"
#include "time.h"

#define HEAP_ARENA_IMPLEMENTATION
#include "heap_arena.h"

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
    if (min == max) return min;
    int64_t res = min + rand() % (max - min + 1);
    return res;
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

bool TestIntegrityOfOurNode(AllocationNode *node, int64_t black_nodes_count, int64_t target) {
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
        bool ok = TestIntegrityOfOurNode(node->left, black_nodes_count, target);
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
        bool ok = TestIntegrityOfOurNode(node->right, black_nodes_count, target);
        if (!ok) {
            return false;
        }
    }

    return true;
}

// this function name sounds like a communist slogan
bool TestOurIntegrity(AllocationNode *root) {
    if (!root) {
        return true;
    } 

    int64_t target = GetBlackNodesCountOfTheFirstLeaf(root);
    bool res = TestIntegrityOfOurNode(root, 0, target);
    return res;
}

typedef struct Memory Memory;
struct Memory {
    void *ptr;
    int64_t size;
};

int main() {
    srand(time(0));

    HeapArena arena = {0};
    int64_t epoch = 0;

    Memory *our_memory_list    = malloc(ALLOCATION_COUNT * sizeof(Memory));
    Memory *malloc_memory_list = malloc(ALLOCATION_COUNT * sizeof(Memory));
#if !EPOCH_COUNT
    while(true) {
#endif
    int64_t our_memory_index = 0;
    int64_t malloc_memory_index = 0;

    clock_t our_clocks = 0;
    clock_t malloc_clocks = 0;
    int64_t epoch_allocated_size = 0;
    for (int64_t i=0; i<ALLOCATION_COUNT; ++i) {
        int64_t to_allocate = random_i64(0, MAX_AMOUNT_TO_ALLOCATE);
        epoch_allocated_size += to_allocate;

#if PRINT_STEPS
        printf("Iteration(%lld), allocating %lld\n", i, to_allocate);
#endif

        clock_t start = clock(); 
        void *our_memory = HeapArenaAllocate(&arena, to_allocate);
        our_clocks += (clock() - start);
        memset(our_memory, 42, to_allocate);

#if PRINT_STEPS
        printf("Iteration(%lld), memory %p\n", i, our_memory);
        HeapArenaDump(&arena);
        printf("\n\n");
#endif

        assert(0 <= our_memory_index && our_memory_index < ALLOCATION_COUNT);
        our_memory_list[our_memory_index++] = (Memory){our_memory, to_allocate};

        start = clock();
#if COMPARE_AGAINST_MALLOC
        void *malloc_memory = malloc(to_allocate);
#endif
        malloc_clocks += (clock() - start);

#if COMPARE_AGAINST_MALLOC
        assert(0 <= malloc_memory_index && malloc_memory_index < ALLOCATION_COUNT);
        malloc_memory_list[malloc_memory_index++] = (Memory){malloc_memory, to_allocate};  
#endif

        int64_t roll = random_i64(0, 100);
        if (roll < CHANCE_TO_REALLOCATE) {
            int64_t index_to_extend = random_i64(0, our_memory_index-1);
            Memory memory_to_extend = our_memory_list[index_to_extend]; 

            int64_t new_size = random_i64(0, MAX_AMOUNT_TO_ALLOCATE);

#if PRINT_STEPS
            printf("Iteration(%lld), reallocating(%p) from %lld to %lld bytes\n", i, (uint8_t*)memory_to_extend.ptr, memory_to_extend.size, new_size);
#endif

            void *new_ptr = HeapArenaRealloc(&arena, memory_to_extend.ptr, new_size);
            memset(new_ptr, 42, new_size);
            our_memory_list[index_to_extend].ptr  = new_ptr;
            our_memory_list[index_to_extend].size = new_size;
#if PRINT_STEPS
            HeapArenaDump(&arena);
            printf("\n\n");
#endif
        } 
        else if (roll < CHANCE_TO_DEALLOCATE) { 
            assert(our_memory_index);
            int64_t index_to_free = random_i64(0, our_memory_index-1);
            Memory memory_to_free = our_memory_list[index_to_free];     

#if PRINT_STEPS
            printf("Iteration(%lld), deallocating(%p)\n", i, (uint8_t*)memory_to_free.ptr-sizeof(AllocationNode));
#endif
            HeapArenaFree(&arena, memory_to_free.ptr);
            epoch_allocated_size -= to_allocate;

            our_memory_list[index_to_free] = our_memory_list[our_memory_index-1];
            our_memory_list[our_memory_index-1] = (Memory){0};
            our_memory_index -= 1;
#if PRINT_STEPS
            HeapArenaDump(&arena);
            printf("\n\n");
#endif
        }
    }

    if (!TestOurIntegrity(arena.root)) {
        HeapArenaDump(&arena);
        assert(0 && "Catch me!");
    }
   
    printf("-------Epoch %lld is finished-------\n", epoch);
    printf("Our Time: %lf seconds\n", (float)our_clocks/(float)CLOCKS_PER_SEC);
#if COMPARE_AGAINST_MALLOC
    printf("Malloc Time: %lf seconds\n", (float)malloc_clocks/(float)CLOCKS_PER_SEC);
#endif

    printf("Arena: Allocated size: %lld\n", arena.allocated_size);
    printf("Arena: Free size: %lld\n", arena.free_size);
    printf("Epoch: Allocted size: %lld\n", epoch_allocated_size);

    HeapArenaRelease(&arena);

#if !EPOCH_COUNT
    epoch += 1;
    }
#endif

}
