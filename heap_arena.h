#ifndef HEAP_ARENA_H
#define HEAP_ARENA_H

#include "stdint.h"
#include "assert.h"
#include "stdbool.h"
#include "string.h"

typedef struct MemoryBlock MemoryBlock;
struct MemoryBlock {
    MemoryBlock *next;
};

typedef enum RBT_Color RBT_Color;
enum RBT_Color {
    RBT_RED,
    RBT_BLACK,
};

typedef struct AllocationNode AllocationNode;
struct AllocationNode {
    int64_t size;
    int64_t used_size; // arena currently doesn't create nodes smaller than sizeof(AllocationNode), so they remain in the node, affecting its size. And sometimes we want to know what size user asked for

    AllocationNode  *parent;
    AllocationNode  *left;
    AllocationNode  *right;
    AllocationNode  *previous; 
    AllocationNode  *next;
    
    // todo: these vaiables can be baked into 'size' member to save some space
    RBT_Color color;
    bool occupied;

    MemoryBlock *memory_block;
    AllocationNode  *previous_in_order;
    AllocationNode  *next_in_order;
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

#endif /* HEAP_ARENA_H */

#ifdef HEAP_ARENA_IMPLEMENTATION

// These function should be defined by the user
void *PlatformGetMemory(int64_t size);
void  PlatformFreeMemory(void *memory);

// also, NORMAL_ALLOCATION_SIZE macro can be defined to set default page size for the allocator, for example:
// #define NORMAL_ALLOCATION_SIZE 1024*1024

// I requested size is larger than NORMAL_ALLOCATION_SIZE, then allocator will try to allocate page that has exactly the desired size (todo: right now it doesn't account for allocation granularity, and may waste so memory)

#ifndef NORMAL_ALLOCATION_SIZE
#define NORMAL_ALLOCATION_SIZE (64 * 1024)
#endif

#include "stdlib.h"
#include "stdbool.h"
#include "stdint.h"
#include "stdio.h"
#include "assert.h"

// current source: https://en.wikipedia.org/wiki/Red%E2%80%93black_tree 
// todo: I am sure that my implementation of Red-Black Tree is total bs, and there is a much better way to create self-balancing search tree, so TODO: check if there is a way to make it faster
typedef enum RBT_Direction RBT_Direction;
enum RBT_Direction {
    RBT_LEFT,
    RBT_RIGHT,
};

inline void RBT_ResetNode(AllocationNode *node) {
    node->left = 0;
    node->right = 0;
    node->previous = 0; 
    node->next = 0;
    node->parent = 0;
    node->color = RBT_RED;
}

inline AllocationNode *RBT_RotateRight(AllocationNode *root, AllocationNode *first) {
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

inline AllocationNode *RBT_RotateLeft(AllocationNode *root, AllocationNode *first) {
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
inline AllocationNode *RBT_SwapNodes(AllocationNode *root, AllocationNode *first, AllocationNode *second) {
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
        AllocationNode *parent         = node->parent;
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
    AllocationNode *previous = node->previous;
    if (previous) {
        assert(previous->next == node);
        
        previous->next = node->next;
        if (previous->next) {
            previous->next->previous = previous;
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

#define PRINT(...) fprintf(stdout, __VA_ARGS__);
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

inline AllocationNode *SkipMemoryBlockHeader(MemoryBlock *header) {
    uint8_t *res = (uint8_t*)header;
    res += sizeof(MemoryBlock); 
    return (AllocationNode*)res;
}

inline void *SkipAllocationNode(AllocationNode *info) {
    uint8_t *res = (uint8_t*)info;
    res += sizeof(AllocationNode);
    return res;
}

inline AllocationNode *GetAllocationNode(void *memory) {
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

    arena->allocated_size += size;
    arena->free_size += info->size;
    return res;
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
        arena->root = RBT_AddNode(arena->root, node);
    }

    node->occupied = true;
    node->used_size = size;
    arena->root = RBT_RemoveSize(arena->root, node);
    RBT_ResetNode(node);
    if (node->size > size + sizeof(AllocationNode)) {
        uint8_t *memory = SkipAllocationNode(node);
        memory += size;
 
        AllocationNode *next = (AllocationNode*)memory;
        memset(next, 0, sizeof(AllocationNode));
        int64_t free_size = node->size - size - sizeof(AllocationNode);
        assert(free_size >= 0);

        node->size = size;

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
        arena->free_size -= sizeof(AllocationNode);
    }

    arena->free_size -= node->size;

    void *res = SkipAllocationNode(node);
    return res;
}

void HeapArenaFree(HeapArena *arena, void *memory) {
    AllocationNode *info = GetAllocationNode(memory);
    info->occupied = false;
    info->used_size = 0;
    arena->free_size += info->size;

    AllocationNode *next = info->next_in_order;
    if (next && !next->occupied && (next->memory_block == info->memory_block)) {
        assert(next->previous_in_order == info);
        arena->free_size += sizeof(AllocationNode);
        info->size = info->size + sizeof(AllocationNode) + next->size;
        info->next_in_order = next->next_in_order;
        if (info->next_in_order) {
            info->next_in_order->previous_in_order = info;
        } else {
            assert(arena->last_node == next);
            arena->last_node = info;
        }
        arena->root = RBT_RemoveSize(arena->root, next);
        RBT_ResetNode(next);
    } 

    AllocationNode *previous = info->previous_in_order;
    if (previous && !previous->occupied && (previous->memory_block == info->memory_block)) {
        assert(previous->next_in_order == info);
        arena->free_size += sizeof(AllocationNode);
        previous->size = previous->size + sizeof(AllocationNode) + info->size;
        previous->next_in_order = info->next_in_order;
        if (previous->next_in_order) {
            previous->next_in_order->previous_in_order = previous;
        } else {
            assert(arena->last_node == info);
            arena->last_node = previous;
        }
        arena->root = RBT_RemoveSize(arena->root, previous);
        RBT_ResetNode(previous);
        info = previous;
    } 

    arena->root = RBT_AddNode(arena->root, info);
}

// todo: make these functions to be first-class citezens, i.e have their own implementation? 
void *HeapArenaExtend(HeapArena *arena, void *memory, int64_t new_size) {
    assert(arena->first_block && "Nothing is allocated yet");

    AllocationNode *node = GetAllocationNode(memory);
    int64_t old_size = node->used_size;
    assert(old_size < new_size && "Node is already larger than desired size or equal to it");

    // volatile: make sure that it won't change contents of the node
    HeapArenaFree(arena, memory);
    void *new_memory = HeapArenaAllocate(arena, new_size);

    if (new_memory == memory) {
        return new_memory; 
    }

    memcpy(new_memory, memory, old_size);
    return new_memory;
}

void *HeapArenaShrink(HeapArena *arena, void *memory, int64_t new_size) {
    assert(arena->first_block && "Nothing is allocated yes");

    AllocationNode *node = GetAllocationNode(memory);
    int64_t old_size = node->used_size; 
    assert(old_size > new_size && "Node is already smaller than desired size or equal to it");

    HeapArenaFree(arena, memory);
    void *new_memory = HeapArenaAllocate(arena, new_size);
    
    if (new_memory == memory) {
        return new_memory;
    }

    memcpy(new_memory, memory, new_size);
    return new_memory;
}

void *HeapArenaRealloc(HeapArena *arena, void *memory, int64_t new_size) {
    AllocationNode *node = GetAllocationNode(memory);
    if (node->used_size == new_size) {
        return memory;
    } else if (node->used_size > new_size) {
        void *res = HeapArenaShrink(arena, memory, new_size);
        return res;
    } else {
        void *res = HeapArenaExtend(arena, memory, new_size);
        return res;
    }
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
        PRINT("\tNode(size=%lld, occupied=%d, ptr=%p)\n", node->size, node->occupied, node); 
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

#undef PRINT_INDENT
#undef PRINT

#endif /* HEAP_ARENA_IMPLEMENTATION */
