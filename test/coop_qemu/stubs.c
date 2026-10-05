struct __lock { int x; };
struct __lock __lock___malloc_recursive_mutex, __lock___sinit_recursive_mutex, __lock___sfp_recursive_mutex, __lock___atexit_recursive_mutex, __lock___env_recursive_mutex, __lock___tz_mutex, __lock___dd_hash_mutex, __lock___arc4random_mutex, __lock___at_quick_exit_mutex;
void __retarget_lock_acquire_recursive(void* l) { (void)l; }
void __retarget_lock_release_recursive(void* l) { (void)l; }
void __retarget_lock_acquire(void* l) { (void)l; }
void __retarget_lock_release(void* l) { (void)l; }
void __retarget_lock_init_recursive(void** l) { (void)l; }
void __retarget_lock_close_recursive(void* l) { (void)l; }
void _init(void) {}
void _fini(void) {}
int _getpid(void) { return 1; }
int _kill(int a, int b) { (void)a; (void)b; return -1; }
