/* Copyright 2022-2023 John "topjohnwu" Wu
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// Public API for Zygisk modules. DO NOT MODIFY ANY CODE IN THIS HEADER.

// WARNING: this file may contain changes that are not finalized.
// Always use the following published header for development:
// https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp

#pragma once

#include <jni.h>

#define ZYGISK_API_VERSION 5

/*

Modules are only loaded after the zygote fork, so ALL OF YOUR CODE RUNS IN
THE APP/SYSTEM_SERVER PROCESS, NOT THE ZYGOTE DAEMON!

Inherit zygisk::ModuleBase; REGISTER_ZYGISK_MODULE(clazz) registers it.

class ExampleModule : public zygisk::ModuleBase {
    void onLoad(zygisk::Api *api, JNIEnv *env) override { this->api = api; }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        JNINativeMethod m[] = {
            { "logger_entry_max_payload_native", "()I", (void*) my_func },
        };
        api->hookJniNativeMethods(env, "android/util/Log", m, 1);
        *(void **) &orig_entry_max = m[0].fnPtr;
    }
};
REGISTER_ZYGISK_MODULE(ExampleModule)

Your class never runs in a true superuser environment (zygote's privilege in
pre[XXX]Specialize, target sandbox in post[XXX]Specialize); register a companion
handler for superuser access -- it runs in a root daemon reached over a socket.

*/

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

class ModuleBase {
public:

    virtual void onLoad([[maybe_unused]] Api *api, [[maybe_unused]] JNIEnv *env) {}

    // Called before the app process is specialized: just forked from zygote,
    // no sandbox restrictions yet, still zygote's privilege. Args are
    // readable/writable; Api::connectCompanion() reaches superuser.
    virtual void preAppSpecialize([[maybe_unused]] AppSpecializeArgs *args) {}

    // Called after the app process is specialized: all sandbox restrictions
    // are in place, so this runs with the app's own privilege.
    virtual void postAppSpecialize([[maybe_unused]] const AppSpecializeArgs *args) {}

    // Called before the system server is specialized; see preAppSpecialize().
    virtual void preServerSpecialize([[maybe_unused]] ServerSpecializeArgs *args) {}

    // Called after system_server is specialized; runs with its privilege.
    virtual void postServerSpecialize([[maybe_unused]] const ServerSpecializeArgs *args) {}
};

struct AppSpecializeArgs {
    // Required arguments: guaranteed to exist on all Android versions.
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jobjectArray &rlimits;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    // Optional arguments: check the pointer for null before de-referencing.
    jintArray *const fds_to_ignore;
    jboolean *const is_child_zygote;
    jboolean *const is_top_app;
    jobjectArray *const pkg_data_info_list;
    jobjectArray *const whitelisted_data_info_list;
    jboolean *const mount_data_dirs;
    jboolean *const mount_storage_dirs;
    jboolean *const mount_sysprop_overrides;

    AppSpecializeArgs() = delete;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

    ServerSpecializeArgs() = delete;
};

namespace internal {
struct api_table;
template <class T> void entry_impl(api_table *, JNIEnv *);
}

enum Option : int {
    // Force Magisk's denylist unmount routines: unmounts all Magisk and
    // modules' files regardless of denylist; only in preAppSpecialize.
    FORCE_DENYLIST_UNMOUNT = 0,

    // Your module's library is dlclose-ed after post[XXX]Specialize: all of
    // your code will be unmapped from memory. YOU MUST NOT ENABLE THIS OPTION
    // AFTER HOOKING ANY FUNCTIONS IN THE PROCESS.
    DLCLOSE_MODULE_LIBRARY = 1,
};

// Bit masks of the return value of Api::getFlags()
enum StateFlag : uint32_t {
    PROCESS_GRANTED_ROOT = (1u << 0),

    PROCESS_ON_DENYLIST = (1u << 1),
};

// All API methods stop working after post[XXX]Specialize: Zygisk is unloaded
struct Api {

    // Returns a socket fd connected to the socket given to the companion
    // handler, or -1 on failure. Only works in pre[XXX]Specialize, where code
    // still runs with zygote's privilege. ABI aware: 32-bit callers get a
    // 32-bit companion, and vice versa.
    int connectCompanion();

    // Returns the fd of this module's root folder, or -1 on error. Usable only
    // in pre[XXX]Specialize, or in the root companion with the fd passed over
    // the socket; zygote must be able to read the module dir (system_file
    // context) or socket messages fail (SELinux and UID).
    int getModuleDir();

    // Accepts one single option at a time; see zygisk::Option.
    void setOption(Option opt);

    // Returns bitwise-or'd zygisk::StateFlag values about the current process.
    uint32_t getFlags();

    // Exempt the provided file descriptor from being automatically closed. Only
    // makes sense in preAppSpecialize; elsewhere a no-op (returns true) or an
    // error (returns false). On false, zygote eventually closes the fd.
    bool exemptFd(int fd);

    // Hook JNI native methods for a class: replaces every registered native
    // method with yours, saving the original pointer in each JNINativeMethod's
    // fnPtr; if class, method name or signature is not found, that fnPtr is
    // set to nullptr.
    void hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods);

    // Hook functions in the PLT of ELFs loaded in memory. The `dev`/`inode`
    // pair uniquely identifies a mapped file, to be found in /proc/[PID]/maps.
    // For matching ELFs, replace `symbol` with `newFunc`; if `oldFunc` is not
    // nullptr, the original pointer is saved to `oldFunc`.
    // https://man7.org/linux/man-pages/man5/proc.5.html
    void pltHookRegister(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc);

    // Commit all the hooks that were previously registered; false on error.
    bool pltHookCommit();

private:
    internal::api_table *tbl;
    template <class T> friend void internal::entry_impl(internal::api_table *, JNIEnv *);
};

#define REGISTER_ZYGISK_MODULE(clazz) \
void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
    zygisk::internal::entry_impl<clazz>(table, env);                        \
}

// Register a root companion request handler: it runs in a superuser daemon
// process and accepts an integer, a Unix domain socket connected to the target
// process (see Api::connectCompanion()). NOTE: it can run concurrently on
// multiple threads; be aware of race conditions on globally shared resources.

#define REGISTER_ZYGISK_COMPANION(func) \
void zygisk_companion_entry(int client) { func(client); }

// The following is internal ABI implementation detail.

namespace internal {

struct module_abi {
    long api_version;
    ModuleBase *impl;

    void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
    void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
    void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
    void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);

    module_abi(ModuleBase *module) : api_version(ZYGISK_API_VERSION), impl(module) {
        preAppSpecialize = [](auto m, auto args) { m->preAppSpecialize(args); };
        postAppSpecialize = [](auto m, auto args) { m->postAppSpecialize(args); };
        preServerSpecialize = [](auto m, auto args) { m->preServerSpecialize(args); };
        postServerSpecialize = [](auto m, auto args) { m->postServerSpecialize(args); };
    }
};

struct api_table {
    void *impl;
    bool (*registerModule)(api_table *, module_abi *);

    void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int);
    void (*pltHookRegister)(dev_t, ino_t, const char *, void *, void **);
    bool (*exemptFd)(int);
    bool (*pltHookCommit)();
    int  (*connectCompanion)(void * /* impl */);
    void (*setOption)(void * /* impl */, Option);
    int  (*getModuleDir)(void * /* impl */);
    uint32_t (*getFlags)(void * /* impl */);
};

template <class T>
void entry_impl(api_table *table, JNIEnv *env) {
    static Api api;
    api.tbl = table;
    static T module;
    ModuleBase *m = &module;
    static module_abi abi(m);
    if (!table->registerModule(table, &abi)) return;
    m->onLoad(&api, env);
}

} // namespace internal

inline int Api::connectCompanion() {
    return tbl->connectCompanion ? tbl->connectCompanion(tbl->impl) : -1;
}
inline int Api::getModuleDir() {
    return tbl->getModuleDir ? tbl->getModuleDir(tbl->impl) : -1;
}
inline void Api::setOption(Option opt) {
    if (tbl->setOption) tbl->setOption(tbl->impl, opt);
}
inline uint32_t Api::getFlags() {
    return tbl->getFlags ? tbl->getFlags(tbl->impl) : 0;
}
inline bool Api::exemptFd(int fd) {
    return tbl->exemptFd != nullptr && tbl->exemptFd(fd);
}
inline void Api::hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods) {
    if (tbl->hookJniNativeMethods) tbl->hookJniNativeMethods(env, className, methods, numMethods);
}
inline void Api::pltHookRegister(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc) {
    if (tbl->pltHookRegister) tbl->pltHookRegister(dev, inode, symbol, newFunc, oldFunc);
}
inline bool Api::pltHookCommit() {
    return tbl->pltHookCommit != nullptr && tbl->pltHookCommit();
}

} // namespace zygisk

extern "C" {

[[gnu::visibility("default"), maybe_unused]]
void zygisk_module_entry(zygisk::internal::api_table *, JNIEnv *);

[[gnu::visibility("default"), maybe_unused]]
void zygisk_companion_entry(int);

} // extern "C"
