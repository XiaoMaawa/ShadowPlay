// node_api.h — 模块注册相关的 shim
//
// 只保留 NAPI_MODULE / NODE_API_MODULE 的展开逻辑，让 .node 文件被 dlopen 时
// 通过 napi_module_register 自动注册。

#ifndef SHADOW_NODE_API_H_
#define SHADOW_NODE_API_H_

#include "js_native_api.h"

// 官方 napi_module 的布局（node_api.h 中的 _napi_module_register）
typedef struct {
  int nm_version;
  unsigned int nm_flags;
  const char* nm_filename;
  napi_value (*nm_register_func)(napi_env env, napi_value exports);
  void* nm_modname;
  void* nm_priv;
  void* reserved[4];
} napi_module;

// NAPI_MODULE_X 的 MSVC 版本：静态构造期调用 napi_module_register 完成注册。
#define NAPI_MODULE_X(modname, regfunc, priv, flags)          \
  static napi_module _module_##modname = {                    \
      NAPI_VERSION,                                           \
      (flags),                                                \
      __FILE__,                                               \
      (regfunc),                                              \
      (void*)(#modname),                                      \
      (priv),                                                 \
      {0, 0, 0, 0}};                                          \
  static void __cdecl _register_##modname(void) {             \
    napi_module_register(&_module_##modname);                 \
  }                                                           \
  namespace {                                                 \
  struct _napi_register_##modname {                           \
    _napi_register_##modname() { _register_##modname(); }     \
  } _napi_reg_instance_##modname;                             \
  }

#define NAPI_MODULE(modname, regfunc) \
  NAPI_MODULE_X(modname, regfunc, nullptr, 0)

#define NAPI_MODULE_INIT()                                            \
  napi_value _napi_module_init(napi_env env, napi_value exports)

#define NAPI_MODULE_INITIALIZER_X(modname, regfunc) \
  NAPI_MODULE_X(modname, regfunc, nullptr, 0)

// N-API 模式下的入口宏
#define NODE_API_MODULE(modname, regfunc) \
  NAPI_MODULE_X(modname, regfunc, nullptr, 0)

#endif // SHADOW_NODE_API_H_
