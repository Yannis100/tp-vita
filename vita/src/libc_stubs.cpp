/**
 * Vita port: libc/libdl symbols that newlib or the VitaSDK libdl (which needs taiHEN) lack.
 * Dynamic loading and process restart are not supported on Vita.
 */
#include <cerrno>

extern "C" {

int execv(const char*, char* const[]) {
    errno = ENOSYS;
    return -1;
}

void* dlopen(const char*, int) { return nullptr; }
void* dlsym(void*, const char*) { return nullptr; }
int dlclose(void*) { return 0; }
const char* dlerror(void) { return "dynamic loading is not supported on PS Vita"; }

}
