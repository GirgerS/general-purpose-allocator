#ifndef ALLOCATORS_H
#define ALLOCATORS_H

#include "stdint.h"
#include "assert.h"
#include "stdbool.h"
#include "string.h"

// nocommit: make sure we don't compute redundant hashes. Even though I am using VERY fast hash and RBT part is excluded, it still take almost half of the programm time.
// nocommit: bake hash that I'am going to use into this file
#include "meow_hash_x64_aesni.h"

typedef struct MemoryBlock MemoryBlock;
struct MemoryBlock {
    MemoryBlock *next;
};

typedef enum {
    RBT_RED,
    RBT_BLACK,
} RBT_Color;

typedef struct AllocationNode AllocationNode;
struct AllocationNode {
    int64_t size;
    int64_t used_size; // arena currently doesn't create nodes smaller than sizeof(AllocationNode), so they remain as a part of the previous node, affecting its size. And in some cases we want to know what is the size that user asked for

    // note: we don't need these field if the memory is allocated, meaning that it is safe to hand them to the user, decreasing allocation overhead!
    AllocationNode  *parent;
    AllocationNode  *left;
    AllocationNode  *right;
    AllocationNode  *previous; 
    AllocationNode  *next;
    RBT_Color color;
    bool occupied;

    // todo: it is trivial to remove this field: it is used only during HeapArenaFree, and wouldn't be nessesary even there is we don't link nodes that belong to different memory block
    MemoryBlock     *memory_block;
    AllocationNode  *previous_in_order;
    AllocationNode  *next_in_order;

    // todo: I cannot use __m128 in there, because than compiler will assume that alignement of this struct is at least 16 bytes, and will issue erronous movaps instuction. This needs to be fixed
    uint64_t checksum;
};

typedef struct HeapArena HeapArena;
struct HeapArena {
    AllocationNode *root; // root of the red-black tree of allocations
    AllocationNode *first_node; // first node, in order 
    AllocationNode *last_node;
    MemoryBlock *first_block;
    MemoryBlock *last_block;

    int64_t allocated_size;
    int64_t free_size;
};

void *HeapArenaAllocate(HeapArena *arena, int64_t size);
void *HeapArenaRealloc(HeapArena *arena, void *memory, int64_t new_size);
void HeapArenaFree(HeapArena *arena, void *memory);
void HeapArenaRelease(HeapArena *arena);
void HeapArenaDump(HeapArena *arena);

#endif /* ALLOCATORS_H */

#ifdef ALLOCATORS_IMPLEMENTATION

// These function should be defined by the user
void *PlatformGetMemory(int64_t size);
void  PlatformFreeMemory(void *memory);

// also, NORMAL_ALLOCATION_SIZE macro can be defined to set default page size for the allocator, for example:
// #define NORMAL_ALLOCATION_SIZE 1024*1024

// If requested size is larger than NORMAL_ALLOCATION_SIZE, then allocator will try to allocate page that has exactly the desired size (todo: right now it doesn't account for allocation granularity, and may waste so memory)

#ifndef NORMAL_ALLOCATION_SIZE
#define NORMAL_ALLOCATION_SIZE 1024
#endif

#include "stdlib.h"
#include "stdbool.h"
#include "stdint.h"
#include "stdio.h"
#include "assert.h"

// nocommit: find a better place for this function
static inline uint64_t HeapArenaGetNodeChecksum(AllocationNode *node) {
    AllocationNode test_node = *node;
    test_node.checksum = 0;

    test_node.parent   = 0;
    test_node.left     = 0;
    test_node.right    = 0;
    test_node.previous = 0; 
    test_node.next     = 0;
    test_node.color    = (RBT_Color)0;
    test_node.occupied = (bool)0;

    int64_t node_size = (int64_t)sizeof(AllocationNode); 
    meow_u128 checksum   = MeowHash(MeowDefaultSeed, node_size, &test_node); 
    uint64_t  checksum64 = MeowU64From(checksum, 0);
    return checksum64;
}

// current source: https://en.wikipedia.org/wiki/Red%E2%80%93black_tree 
// todo: I am sure that my implementation of Red-Black Tree is total bs, and there is a much better way to create self-balancing search tree, so TODO: check if there is a way to make it faster
typedef enum {
    RBT_LEFT,
    RBT_RIGHT,
} RBT_Direction;

static inline void RBT_ResetNode(AllocationNode *node) {
    node->left = 0;
    node->right = 0;
    node->previous = 0; 
    node->next = 0;
    node->parent = 0;
    node->color = RBT_RED;
}

static inline AllocationNode *RBT_RotateRight(AllocationNode *root, AllocationNode *first) {
    AllocationNode *grandparent = first->parent;
    AllocationNode *second = first->left;
    assert(second);

    first->left = second->right;
    if (first->left) first->left->parent = first;

    second->right = first;
    first->parent = second;

    second->parent = grandparent;
    if (!grandparent) {
        assert(first == root);
        return second;
    } 

    if (grandparent->left == first) {
        grandparent->left  = second;
    } else if (grandparent->right == first) {
        grandparent->right = second;
    } else {
        assert(0);
    }
    
    return root;
}

static inline AllocationNode *RBT_RotateLeft(AllocationNode *root, AllocationNode *first) {
    AllocationNode *grandparent = first->parent;
    AllocationNode *second = first->right;
    assert(second);

    first->right = second->left;   
    if (first->right) first->right->parent = first;

    second->left = first;
    first->parent = second;

    second->parent = grandparent;
    if (!grandparent) {
        assert(first == root);
        return second;
    } 

    if (grandparent->left == first) {
        grandparent->left  = second;
    } else if (grandparent->right == first) {
        grandparent->right = second;
    } else {
        assert(0);
    }
    
    return root;
}

static inline AllocationNode *RBT_AddNode(AllocationNode *parent, AllocationNode *new_node) {
    assert(new_node);
    assert(new_node->color == RBT_RED);
    assert(!new_node->left);
    assert(!new_node->right);
    assert(!new_node->parent);
    assert(!new_node->next);
    assert(!new_node->previous);

    if (!parent) {
        return new_node;
    }

    AllocationNode *root = parent;
    while(true) {
        if (parent->size == new_node->size) {
            assert(parent != new_node && "One node is inserted multiple times");
            if (parent->next) {
                new_node->next = parent->next;
                new_node->next->previous = new_node;
            }

            parent->next = new_node;
            new_node->previous = parent;
            return root;
        }
        if (parent->size > new_node->size) {
            if (parent->left) {
                parent = parent->left;
            } else {
                parent->left = new_node;
                break;
            }
        } else {
            if (parent->right) {
                parent = parent->right;
            } else {
                parent->right = new_node;
                break;
            }
        }
    }  

    AllocationNode *node = new_node;
    node->parent = parent;
    while (true) {
        AllocationNode *parent = node->parent; 
        if (!parent) {
            return node;
        }

        if (parent->color == RBT_BLACK) {
            return root;
        }

        RBT_Direction insert_dir;
        if (parent->left == node) {
            insert_dir = RBT_LEFT;
        } else {
            insert_dir = RBT_RIGHT;
        }

        AllocationNode *grandparent = parent->parent;
        if (!grandparent) {
            parent->color = RBT_BLACK;
            return root;
        }

        RBT_Direction parent_dir;
        AllocationNode *uncle = 0;
        if (grandparent->left == parent) {
            uncle = grandparent->right; 
            parent_dir = RBT_LEFT;
        } else {
            uncle = grandparent->left;
            parent_dir = RBT_RIGHT;
        }

        if (uncle && uncle->color == RBT_RED) {
            parent->color = RBT_BLACK; 
            uncle->color  = RBT_BLACK;
            grandparent->color = RBT_RED;
        
            node = grandparent;
            continue; 
        }  

        // TODO: I for sure can get rid of nested if-elses
        if (parent_dir != insert_dir) {
            if (insert_dir == RBT_LEFT) {
                root = RBT_RotateRight(root, parent);
            } else {
                root = RBT_RotateLeft(root, parent);
            }
            // note: we don't reference node below, so we don't update it 
#if 0
            parent = node;
#else
            if (parent_dir == RBT_LEFT) {
                parent = grandparent->left;
            } else {
                parent = grandparent->right;
            }
#endif
        }

        if (parent_dir == RBT_LEFT) {
            root = RBT_RotateRight(root, grandparent);
        } else {
            root = RBT_RotateLeft(root, grandparent);
        }

        parent->color = RBT_BLACK;
        grandparent->color = RBT_RED;
        return root;
    }
}

// note: second node should be deeper than first node or at least on the same level with it
// there it no way to check it, thought, so this function might silently fail...
static inline AllocationNode *RBT_SwapNodes(AllocationNode *root, AllocationNode *first, AllocationNode *second) {
    RBT_Direction second_dir;
    if (second->parent->left == second) {
        second_dir = RBT_LEFT;
    } else {
        second_dir = RBT_RIGHT;
    }

    RBT_Direction first_dir = {0};
    if (first->parent) {
        if (first->parent->left == first) {
            first_dir = RBT_LEFT;
        } else {
            first_dir = RBT_RIGHT;
        }
    }

    AllocationNode *first_left  = first->left;
    AllocationNode *first_right = first->right;
    RBT_Color first_color = first->color;

    first->left = second->left;
    if (first->left) {
        first->left->parent = first;
    }
    first->right = second->right;
    if (first->right) {
        first->right->parent = first;
    }
    first->color = second->color;

    second->left  = first_left;
    if (second->left) {
        second->left->parent = second;
    }
    second->right = first_right; 
    if (second->right) {
        second->right->parent = second;
    }
    second->color = first_color;

    AllocationNode *new_first_parent  = second->parent;
    AllocationNode *new_second_parent = first->parent;
    assert(new_first_parent);
    if (new_first_parent == first) {
        new_first_parent = second;
    } 

    first->parent = new_first_parent;
    if (second_dir == RBT_LEFT) {
        new_first_parent->left  = first;
    } else {
        new_first_parent->right = first;
    }

    second->parent = new_second_parent;
    if (!new_second_parent) {
        return second;
    }

    if (first_dir == RBT_LEFT) {
        new_second_parent->left  = second;
    } else {
        new_second_parent->right = second;
    }

    return root;
}

static inline AllocationNode *RBT_RemoveNode(AllocationNode *root, AllocationNode *node) {
    // simple cases
    if (node->left && node->right) {
        AllocationNode *leftmost_child = node->right;
        while(true) {
            if (!leftmost_child->left) {
                break; 
            }
            leftmost_child = leftmost_child->left;
        }
       
        root = RBT_SwapNodes(root, node, leftmost_child);
        root = RBT_RemoveNode(root, node);

        return root;
    }

    if (node->left) {
        AllocationNode *parent = node->parent;
        AllocationNode *new_node = node->left;

        new_node->color = RBT_BLACK;
        new_node->parent = parent;
        if (!parent) {
            return new_node;
        }
        if (parent->left == node) {
            parent->left  = new_node;
        } else if (parent->right == node) {
            parent->right = new_node;
        } else {
            assert(0);
        }

        new_node->left  = 0;
        return root;
    }
    
    if (node->right) {
        AllocationNode *parent = node->parent;
        AllocationNode *new_node = node->right;

        new_node->color = RBT_BLACK;
        new_node->parent = parent;
        if (!parent) {
            return new_node;
        }
        if (parent->left == node) {
            parent->left  = new_node;
        } else if (parent->right == node) {
            parent->right = new_node;
        } else {
            assert(0);
        }

        new_node->right  = 0;
        return root;
    }

    if (node->parent == 0) {
        // we are the root
        return 0;
    }

    if (node->color == RBT_RED) {
        AllocationNode *parent = node->parent; 
        assert(parent && "last node should always be black");

        if (parent->left == node) {
            parent->left  = 0;     
        } else {
            parent->right = 0;
        }

        return root;
    }
    
    RBT_Direction dir;
    if (node->parent->left == node) {
        dir = RBT_LEFT;
        node->parent->left = 0;
    } else if (node->parent->right == node) {
        dir = RBT_RIGHT;
        node->parent->right = 0;
    } else {
        assert(0);
    }

    // comlex cases
    while (true) {
        AllocationNode *parent = node->parent;
        AllocationNode *sibling = 0;
        AllocationNode *close_newphew = 0;
        AllocationNode *distant_newphew = 0;
        if (dir == RBT_LEFT) {
            sibling = parent->right;
            if (sibling) {
                close_newphew   = sibling->left;
                distant_newphew = sibling->right;
            }
        } else {
            sibling = parent->left;
            if (sibling) {
                close_newphew   = sibling->right;
                distant_newphew = sibling->left;
            }
        }

        bool parent_is_black =
            parent->color == RBT_BLACK;
        bool sibling_is_black = 
            !sibling || sibling->color == RBT_BLACK;
        bool close_newphew_is_black = 
            !close_newphew || close_newphew->color == RBT_BLACK;
        bool distant_newphew_is_black = 
            !distant_newphew || distant_newphew->color == RBT_BLACK;

        if (parent_is_black && sibling_is_black && close_newphew_is_black && distant_newphew_is_black) {
            // Case #2
            sibling->color = RBT_RED; 
            node = parent;

            // Case #1 
            if (!node->parent) {
                return root;
            }

            if (node->parent->left == node) {
                dir = RBT_LEFT;
            } else if (node->parent->right == node) {
                dir = RBT_RIGHT;
            } else {
                assert(0);
            }
            continue;
        }

        // todo: we should assert that sibling exists...
        if (!sibling_is_black) {
            // Case #3
            if (dir == RBT_LEFT) {
                root = RBT_RotateLeft(root, parent); 
                parent->color = RBT_RED;
                sibling->color = RBT_BLACK;

                sibling = close_newphew;
                close_newphew  = sibling->left;
                distant_newphew = sibling->right; 
            } else {
                root = RBT_RotateRight(root, parent);
                parent->color = RBT_RED;
                sibling->color = RBT_BLACK;

                sibling = close_newphew;
                close_newphew  = sibling->right;
                distant_newphew = sibling->left; 
            }
        }

        sibling_is_black = 
            !sibling || sibling->color == RBT_BLACK;
        close_newphew_is_black = 
            !close_newphew || close_newphew->color == RBT_BLACK;
        distant_newphew_is_black = 
            !distant_newphew || distant_newphew->color == RBT_BLACK;

        assert(sibling_is_black);

        if (sibling_is_black && close_newphew_is_black && distant_newphew_is_black) {
            assert(parent->color == RBT_RED);
            parent->color = RBT_BLACK;
            sibling->color = RBT_RED;
            return root;
        }

        // if our distant newphew is already red, we can skip unnessesary rotataion (although if we don't skip it, resulting tree seems to be perfecly fine)
        if (distant_newphew_is_black && !close_newphew_is_black) {
            if (dir == RBT_LEFT) {
                root = RBT_RotateRight(root, sibling);

                sibling = close_newphew;
                close_newphew = sibling->left;
                distant_newphew = sibling->right;

                sibling->color = RBT_BLACK;
                distant_newphew->color = RBT_RED;
            } else {
                root = RBT_RotateLeft(root, sibling);

                sibling = close_newphew;
                close_newphew = sibling->right;
                distant_newphew = sibling->left;

                sibling->color = RBT_BLACK;
                distant_newphew->color = RBT_RED;
            }

            // note: these check exist only for debugging purposes
            sibling_is_black = 
                !sibling || sibling->color == RBT_BLACK;
            close_newphew_is_black = 
                !close_newphew || close_newphew->color == RBT_BLACK;
            distant_newphew_is_black = 
                !distant_newphew || distant_newphew->color == RBT_BLACK;
        }

        assert(sibling_is_black);
        assert(!distant_newphew_is_black); 
        if (dir == RBT_LEFT) {
            root = RBT_RotateLeft(root, parent);
            sibling->color = parent->color;
        } else {
            root = RBT_RotateRight(root, parent);
            sibling->color = parent->color;
        }

        parent->color = RBT_BLACK;
        distant_newphew->color = RBT_BLACK;

        return root;
    }
}

// todo: rename remove functions
AllocationNode *RBT_RemoveSize(AllocationNode *root, AllocationNode *node) {
    uint64_t checksum = HeapArenaGetNodeChecksum(node);
    assert(node->checksum == checksum && "corrupted memory");

    AllocationNode *previous = node->previous;
    if (previous) {
        checksum = HeapArenaGetNodeChecksum(previous);
        assert(previous->checksum == checksum);

        assert(previous->next == node); 
        
        previous->next = node->next;
        if (previous->next) {
            checksum = HeapArenaGetNodeChecksum(previous->next);
            assert(previous->next->checksum == checksum);
            previous->next->previous = previous;

            checksum = HeapArenaGetNodeChecksum(previous->next);
            previous->next->checksum = checksum;
        } 
       
        return root;
    }

    AllocationNode *next = node->next;
    if (next) {
        next->color  = node->color;
        next->left   = node->left;
        if (next->left) {
            next->left->parent = next;
        }
        next->right  = node->right;
        if (next->right) {
            next->right->parent = next;
        }
        next->previous = 0;

        next->parent = node->parent;
        if (!next->parent) {
            assert(node == root);
            return next;
        }

        if (next->parent->left == node) {
            next->parent->left = next;
        } else if (next->parent->right == node) {
            next->parent->right = next;
        } else {
            assert(0);
        }

        return root;
    }

    return RBT_RemoveNode(root, node);
}

AllocationNode *RBT_FindClosest(AllocationNode *root, int64_t size) {
    if (!root) {
        return 0;  
    }
    AllocationNode *node    = root;
    AllocationNode *closest = 0;
    while(true) {
        assert(!node->occupied && "shouldn't see occupied node inside tree");
        assert(!node->used_size && "shouldn't see occupied node inside tree");
        if (node->size == size) {
            return node;
        } 
        if (node->size < size) {
            if (!node->right) {
                return closest;
            }
            node = node->right;
        }
        else {
            closest = node;
            if (!node->left) {
                return closest;
            } 
            node = node->left;
        }
    } 
}

#define PRINT(...) fprintf(stdout, __VA_ARGS__)
#define PRINT_INDENT(size) fprintf(stdout, "%*s", (int)(size), "")
void RBT_DumpNode(AllocationNode *node, int64_t indent) {
    char *color_string = 0;
    if (node->color == RBT_RED) {
        color_string = "Red";       
    } else {
        color_string = "Black";
    }

    fprintf(stdout, "%*s", (int)(indent), "");
    //PRINT_INDENT(indent);

    int64_t count = 1;
    AllocationNode *next = node->next;
    if (node == next) {
        count = -1;
    } else {
        while(true) {
            if (!next) {
                break;
            }
            if (next == next->next) {
                count = -1;
                break;
            }
            count += 1;
            next = next->next;
        }
    }
    
    PRINT("%s(%lld, count=%lld, ptr=%p)\n", color_string, node->size, count, node);

    if (node->left) {
        if (node == node->left) {
            PRINT_INDENT(indent + 2);
            PRINT("<<recursive leaf>>\n");
        } else {
            RBT_DumpNode(node->left, indent + 2);
        }
    } else {
        PRINT_INDENT(indent + 2);
        PRINT("None\n");
    }

    if (node->right) {
        if (node == node->right) {
            PRINT_INDENT(indent + 2);
            PRINT("<<recursive leaf>>\n");
        }
        RBT_DumpNode(node->right, indent + 2);
    } else {
        PRINT_INDENT(indent + 2);
        PRINT("None\n");
    }
}

void RBT_Dump(AllocationNode *node) {
    if (!node) {
        PRINT("Tree is empty");
        return;
    }

    RBT_DumpNode(node, 0);
}

static inline AllocationNode *SkipMemoryBlockHeader(MemoryBlock *header) {
    uint8_t *res = (uint8_t*)header;
    res += sizeof(MemoryBlock); 
    return (AllocationNode*)res;
}

static inline void *SkipAllocationNode(AllocationNode *info) {
    uint8_t *res = (uint8_t*)info;
    res += sizeof(AllocationNode);
    return res;
}

static inline AllocationNode *GetAllocationNode(void *memory) {
    return (AllocationNode*)((uint8_t*)memory - sizeof(AllocationNode));
}

MemoryBlock *AllocateNewBlock(HeapArena *arena, int64_t size) { 
    size = size + sizeof(MemoryBlock) + sizeof(AllocationNode);
    if (NORMAL_ALLOCATION_SIZE > size) {
        size = NORMAL_ALLOCATION_SIZE;
    }
   
    MemoryBlock *res = PlatformGetMemory(size);
    AllocationNode *info = SkipMemoryBlockHeader(res);
    info->size = size - sizeof(MemoryBlock) - sizeof(AllocationNode);
    info->memory_block = res;

    uint64_t checksum = HeapArenaGetNodeChecksum(info);
    info->checksum = checksum;

    arena->allocated_size += size;
    arena->free_size += info->size;
    return res;
}

// Note: the only reason these functions exist is because HeapArenaAllocate and HeapArenaReallocate share almost the same code, except that in cast of reallocation it should copy memory from the previous allocation, right in between these functions. If it weren't for this difference, these function would be merged with HeapArenaAllocate
static inline AllocationNode *HeapArenaGetNode(HeapArena *arena, int64_t size) {
    AllocationNode *node = RBT_FindClosest(arena->root, size);
    if (!node) {
        MemoryBlock *block = AllocateNewBlock(arena, size);
        assert(!arena->last_block->next);
        arena->last_block->next = block;
        arena->last_block = block;

        node = SkipMemoryBlockHeader(block);

        node->previous_in_order = arena->last_node;
        if (node->previous_in_order) {
            node->previous_in_order->next_in_order = node;
        }
        arena->last_node = node;
         
        node->memory_block = block;
    } else {
        arena->root = RBT_RemoveSize(arena->root, node);
    }

    node->occupied = true;
    node->used_size = size;
    RBT_ResetNode(node);

    if (node->previous_in_order) {
        uint64_t checksum = HeapArenaGetNodeChecksum(node->previous_in_order);
        node->previous_in_order->checksum = checksum;
    }

    uint64_t checksum = HeapArenaGetNodeChecksum(node);
    node->checksum = checksum;

    arena->free_size -= node->size;
    return node;
}

static inline AllocationNode *HeapArenaSeparateExtraMemory(HeapArena *arena, AllocationNode *node) {
    uint64_t target_checksum = HeapArenaGetNodeChecksum(node);
    uint64_t test_checksum   = HeapArenaGetNodeChecksum(node);
    assert(node->checksum == target_checksum && "Corrupted memory");

    int64_t free_size = node->size - node->used_size - (int64_t)sizeof(AllocationNode);
    if (free_size <= 0) {
        return 0;
    }

    uint8_t *memory = SkipAllocationNode(node);
    memory += node->used_size;
 
    AllocationNode *next = (AllocationNode*)memory;
    memset(next, 0, sizeof(AllocationNode)); 

    node->size = node->used_size;

    next->memory_block = node->memory_block;
    next->size = free_size;

    next->previous_in_order = node;
    next->next_in_order     = node->next_in_order;
    next->previous_in_order->next_in_order = next;
    if (next->next_in_order) {
        next->next_in_order->previous_in_order = next;
    }
    if (arena->last_node == node) {
        arena->last_node = next;
    }

    arena->root = RBT_AddNode(arena->root, next);
    arena->free_size += free_size;

    uint64_t node_checksum = HeapArenaGetNodeChecksum(node);
    node->checksum = node_checksum;
    uint64_t next_checksum = HeapArenaGetNodeChecksum(next);
    next->checksum = next_checksum;

    if (next->next_in_order) {
        uint64_t next_next_checksum = HeapArenaGetNodeChecksum(next->next_in_order);
        next->next_in_order->checksum = next_next_checksum;
    }

    return next;
}

void *HeapArenaAllocate(HeapArena *arena, int64_t size) {
    if (!arena->first_block) {
        assert(!arena->last_block);    

        arena->first_block = AllocateNewBlock(arena, size);
        arena->last_block  = arena->first_block;

        arena->root = SkipMemoryBlockHeader(arena->first_block);
        arena->first_node = arena->root;
        arena->last_node  = arena->root;
    }     

    AllocationNode *node  = HeapArenaGetNode(arena, size);
    HeapArenaSeparateExtraMemory(arena, node); 

    void *res = SkipAllocationNode(node);
    return res;
}

void HeapArenaFree(HeapArena *arena, void *memory) {
    AllocationNode *info = GetAllocationNode(memory);
    uint64_t target_info_checksum = HeapArenaGetNodeChecksum(info);
    assert(info->checksum == target_info_checksum && "corrupted memory");

    info->occupied = false;
    info->used_size = 0;
    arena->free_size += info->size;

    AllocationNode *next = info->next_in_order;
    if (next && !next->occupied && (next->memory_block == info->memory_block)) {
        uint64_t target_next_checksum = HeapArenaGetNodeChecksum(next);
        assert(next->checksum == target_next_checksum && "corrupted memory");
        assert(next->previous_in_order == info);

        arena->root = RBT_RemoveSize(arena->root, next);
        RBT_ResetNode(next); 

        arena->free_size += sizeof(AllocationNode);
        info->size = info->size + sizeof(AllocationNode) + next->size;
        info->next_in_order = next->next_in_order;
        if (info->next_in_order) {
            info->next_in_order->previous_in_order = info;
        } else {
            assert(arena->last_node == next);
            arena->last_node = info;
        }
    } 

    /* 
    It is stupid how many time I've this mistake...

    I can't just add size of the 'previous' node to 'current' node, because of how they are ordered in memory:  

    |      | |      | |      |
    ^        ^        ^
    previous node     next
    
    (free space between nodes in the diagram is decorative, these nodes are actually contiguous in memory)
    
    Lets say that we change node.size to be previous.size + node.size, now we have a buffer overrun:
            
             node + previous
             v
             |         xxxxxx| < overlapping memory

    |      | |      | |      |
    ^        ^        ^
    previous node     next

    It happens because memory that we are claiming to have is actually located before us, in the previous node. Obviously, to fix this we just need to go back one node, so we start at the previous one:

    previous + node
    v
    |                         |
     
    |      |           |      |  |      | 
    ^                  ^         ^ 
    node(old previous) old node  next
    */

    AllocationNode *previous = info->previous_in_order;
    if (previous && !previous->occupied && (previous->memory_block == info->memory_block)) {
        uint64_t target_previous_checksum = HeapArenaGetNodeChecksum(previous);
        assert(previous->checksum == target_previous_checksum && "corrupted memory");
        assert(previous->next_in_order == info);

        arena->root = RBT_RemoveSize(arena->root, previous);
        RBT_ResetNode(previous);
       
        next = info;
        info = previous;

        arena->free_size += sizeof(AllocationNode);
        info->size = info->size + sizeof(AllocationNode) + next->size;
        info->next_in_order = next->next_in_order;
        if (info->next_in_order) {
            info->next_in_order->previous_in_order = info;
        } else {
            assert(arena->last_node == next);
            arena->last_node = info;
        }
    } 

    arena->root = RBT_AddNode(arena->root, info);

    if (info->next_in_order) {
        uint64_t new_next_checksum = HeapArenaGetNodeChecksum(info->next_in_order);
        info->next_in_order->checksum = new_next_checksum;
    }

    if (info->previous_in_order) {
        uint64_t new_previous_checksum = HeapArenaGetNodeChecksum(info->previous_in_order);
        info->previous_in_order->checksum = new_previous_checksum;
    }

    uint64_t new_info_checksum = HeapArenaGetNodeChecksum(info);
    info->checksum = new_info_checksum;
}

// Note: from what i've seen, this function is not vectorized by the compiler
void HeapArenaCopyMemory(void *dest, void *source, int64_t size) {
#if 0
    int64_t start = 0;
    int64_t end  =  size;
    int64_t step = 1;
    if (dest > source) {
        start = size - 1;
        end =  -1;
        step = -1; 
    }

    uint8_t *to   = (uint8_t*)dest;
    uint8_t *from = (uint8_t*)source;
    for (int64_t i=start; i != end; i+=step) {
        to[i] = from[i]; 
    }
#else
    memmove(dest, source, size);
    return;
#endif
}

/* Performance: make seperate path in the case when next node is free and just use its memory, without juggling other functions */
void *HeapArenaRealloc(HeapArena *arena, void *memory, int64_t new_size) {
    assert(arena->first_block && "Nothing is allocated yet");

    AllocationNode *node = GetAllocationNode(memory); 
    uint64_t target_checksum = HeapArenaGetNodeChecksum(node);
    assert(target_checksum == node->checksum && "corrupted memory"); 

    int64_t old_size = node->used_size;

    if (old_size == new_size) {
        return memory;
    }
    // volatile: make sure that it won't change contents of the node
    HeapArenaFree(arena, memory);
     
    AllocationNode *new_node = HeapArenaGetNode(arena, new_size);
    void *new_memory = SkipAllocationNode(new_node);
    if (new_memory != memory) {
        int64_t saved_size = old_size;
        if (old_size > new_size) {
            saved_size = new_size;
        }
        HeapArenaCopyMemory(new_memory, memory, saved_size);
    }
    HeapArenaSeparateExtraMemory(arena, new_node); 
    return new_memory;
}

void HeapArenaDump(HeapArena *arena) {
    PRINT("------------Temporary Arena Dump---------------\n");
    int64_t block_count = 0;
    MemoryBlock *block = arena->first_block;
    while(block) {
        block_count += 1;
        block = block->next;
    }
    PRINT("Block count: %lld\n", block_count);

    int64_t node_count = 0;
    AllocationNode *node = arena->first_node;
    while(node) {
        node_count += 1;
        node = node->next_in_order;
    }
    PRINT("Nodes(%lld):\n", node_count);
    node = arena->first_node;
    while(node) {
        PRINT("\tNode(size=%lld, occupied=%d, checksum=%llu, ptr=%p)\n", node->size, node->occupied, node->checksum, node); 
        node = node->next_in_order;
    }

    PRINT("Tree:\n");
    RBT_Dump(arena->root);
}

void HeapArenaRelease(HeapArena *arena) {
    MemoryBlock *block = arena->first_block;
    while(block) {
        MemoryBlock *next = block->next;
        assert(block != next);
        PlatformFreeMemory(block);
        block = next;
    }

    memset(arena, 0, sizeof(HeapArena));
}

#ifndef STATIC_ARENA_PAGE_TOTAL_SIZE
#define STATIC_ARENA_PAGE_TOTAL_SIZE 1024 * 1024
#endif
#define STATIC_ARENA_PAGE_AVAILABLE_SIZE (STATIC_ARENA_PAGE_TOTAL_SIZE - sizeof(uint8_t*))
static_assert(
    STATIC_ARENA_PAGE_AVAILABLE_SIZE > 0, 
    "Specified STATIC_ARENA_PAGE_TOTAL_SIZE is too small, expected at least size of a pointer"
);

// TODO: move these things to the header
typedef struct StaticArena StaticArena;
struct StaticArena {
    uint8_t *first;
    uint8_t *last;
    int64_t current_page_cursor;

    void     *last_allocated_block;
    int64_t  last_allocation_size;
};

uint8_t *StaticArenaGetPageBase(uint8_t *page) {
    return page - sizeof(uint8_t*);
} 

uint8_t *StaticArenaGetNextPage(uint8_t *page) {
    uint8_t *base = StaticArenaGetPageBase(page);
    return (uint8_t*)base; 
}

uint8_t *StaticArenaNewPage() {
    uint8_t *mem = malloc(STATIC_ARENA_PAGE_TOTAL_SIZE);   
    *((uint8_t**)mem) = 0;
    mem += sizeof(uint8_t*);
    return mem;
}

void StaticArenaNextPage(StaticArena *arena) {
    assert(arena->first && arena->last);

    uint8_t *last_page_base = StaticArenaGetPageBase(arena->last);
    uint8_t *new_page = StaticArenaNewPage();

    *((uint8_t**)last_page_base) = new_page;

    arena->last = new_page;
}

bool StaticArenaIsZeroed(StaticArena *arena) {
    StaticArena zeroed = {0};
    int32_t res = memcmp(&zeroed, arena, sizeof(StaticArena));
    if (res != 0) {
        return false;
    }
    return true;
}

void *StaticArenaAlloc(StaticArena *arena, int64_t size) {
    bool zeroed = StaticArenaIsZeroed(arena);
    if (zeroed) {  
        arena->first = StaticArenaNewPage();
        arena->last = arena->first;
        arena->current_page_cursor = 0;
    }

    assert(arena->first);
    assert(arena->last);
    assert(arena->current_page_cursor <= STATIC_ARENA_PAGE_AVAILABLE_SIZE);
    assert(size <= STATIC_ARENA_PAGE_AVAILABLE_SIZE);

    int64_t available = STATIC_ARENA_PAGE_AVAILABLE_SIZE - arena->current_page_cursor;
    if (size > available) {
        uint8_t *base = StaticArenaGetPageBase(arena->last);
        uint8_t *next = *((uint8_t**)base);
        if (next) { 
            arena->last = next;
        } else {
            StaticArenaNextPage(arena);
        } 
        arena->current_page_cursor = 0;
    }
    
    void *elem = arena->last + arena->current_page_cursor;

    arena->last_allocated_block = elem;
    arena->last_allocation_size = size;
    arena->current_page_cursor += size;

    assert(arena->current_page_cursor <= STATIC_ARENA_PAGE_AVAILABLE_SIZE);
    return elem;
}

void StaticArenaReset(StaticArena *arena) {
    arena->last = arena->first;
    arena->current_page_cursor  = 0;
    arena->last_allocated_block = 0;
    arena->last_allocation_size  = 0;
}

void StaticArenaDestroy(StaticArena *arena) {
    uint8_t *next = arena->first;

    while (next) { 
        uint8_t *current = next; 
        uint8_t *base = StaticArenaGetPageBase(current);
        next = *((uint8_t**)base); 

        free(base);
    }
}

void *StaticArenaReallocLast(StaticArena *arena, void *block, int64_t new_size) {
    assert(new_size >= 0 && "Requested size is less than zero");
    assert(new_size <= STATIC_ARENA_PAGE_AVAILABLE_SIZE);
    assert(block == arena->last_allocated_block && "Given pointer doesn't point to the last allocation. Static arena can reallocate only the last block"); 
    int64_t old_size  = arena->last_allocation_size;
    int64_t size_diff = new_size - old_size; 
    if (size_diff <= 0) {
        arena->current_page_cursor += size_diff;
        return arena->last_allocated_block;
    }

    int64_t available = STATIC_ARENA_PAGE_AVAILABLE_SIZE - arena->current_page_cursor;
    if (size_diff <= available) {
        arena->current_page_cursor += size_diff;
        return arena->last_allocated_block;
    }

    uint8_t *base = StaticArenaGetPageBase(arena->last);
    uint8_t *next = *((uint8_t**)base);
    if (next) { 
        arena->last = next;
    } else {
        StaticArenaNextPage(arena);
    } 
    arena->current_page_cursor = 0;
    memcpy(arena->last, block, old_size);
    arena->current_page_cursor += new_size;
    arena->last_allocated_block = arena->last;
    arena->last_allocation_size  = new_size;
    return arena->last_allocated_block;
}


#undef PRINT_INDENT
#undef PRINT

#endif /* ALLOCATORS_IMPLEMENTATION */
