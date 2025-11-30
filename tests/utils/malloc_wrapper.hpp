
void  enable_malloc ();
void disable_malloc ();

void use_malloc (bool enabled);

void  enable_malloc_storage ();
void disable_malloc_storage ();

void   clear_malloc_storage ();
size_t get_malloc_storage_size ();
size_t get_malloc_ith_call (int call_id);
