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

int main() {
    char *hello_world = "Hello, world!";
    int64_t string_size = strlen(hello_world) + 1;

    HeapArena arena = {0};
    char *memory = HeapArenaAllocate(&arena, string_size);
    memcpy(memory, hello_world, string_size);
    printf("%s", memory);

    HeapArenaFree(&arena, memory);
    HeapArenaRelease(&arena);
}
