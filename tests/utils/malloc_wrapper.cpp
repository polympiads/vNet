
#include <cstddef>
#include <vector>

#include "utils/malloc_wrapper.hpp"

static bool g_force_malloc_fail    = false;
static bool g_store_malloc_results = false;

static std::vector<size_t> g_malloc_storage;

extern "C" void* __real_malloc(size_t size);
extern "C" void* __wrap_malloc(size_t size) {
    if (g_store_malloc_results) {
        g_malloc_storage.push_back(size);
    }
    if (g_force_malloc_fail) {
        return NULL;
    }

    return __real_malloc(size); 
}

void  enable_malloc () {
    g_force_malloc_fail = false;
}
void disable_malloc () {
    g_force_malloc_fail = true;
}

void use_malloc (bool enabled) {
    g_force_malloc_fail = !enabled;
}

void enable_malloc_storage () {
    g_store_malloc_results = true;
}
void disable_malloc_storage () {
    g_store_malloc_results = false;
}


void clear_malloc_storage () {
    g_malloc_storage.clear();
}
size_t get_malloc_storage_size () {
    return g_malloc_storage.size();
}
size_t get_malloc_ith_call (int call_id) {
    return g_malloc_storage[call_id];
}
