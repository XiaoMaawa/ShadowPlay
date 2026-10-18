// node_api_types.h — 最小化的 N-API 类型定义（本地 shim）
//
// 本机没有任何 Node/Electron 开发头文件，也没有 node-gyp 可以下载它们，
// 所以这里按官方 node_api_types.h 的定义手写一份“只包含本工程用到的部分”的等价声明。
//
// N-API 是 ABI 稳定的：只要结构体布局和枚举取值与官方一致，编译出来的 .node
// 就能被任何支持该 N-API 版本的运行时（node.exe / electron.exe）加载。
// 这里所有类型都是官方的精确镜像，改动它们会破坏 ABI，请不要随意修改。

#ifndef SHADOW_NODE_API_TYPES_H_
#define SHADOW_NODE_API_TYPES_H_

#include <stddef.h>
#include <stdint.h>

// 让 <windows.h> 的 min/max 宏不要破坏 std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

typedef struct napi_env__* napi_env;
typedef struct napi_value__* napi_value;
typedef struct napi_ref__* napi_ref;
typedef struct napi_handle_scope__* napi_handle_scope;
typedef struct napi_escapable_handle_scope__* napi_escapable_handle_scope;
typedef struct napi_callback_info__* napi_callback_info;
typedef struct napi_deferred__* napi_deferred;

typedef enum {
  napi_default = 0,
  napi_writable = 1 << 0,
  napi_enumerable = 1 << 1,
  napi_configurable = 1 << 2,
  napi_static = 1 << 10,
  napi_default_method = napi_writable | napi_configurable,
  napi_default_jsproperty = napi_writable | napi_enumerable | napi_configurable
} napi_property_attributes;

typedef enum {
  napi_undefined,
  napi_null,
  napi_boolean,
  napi_number,
  napi_string,
  napi_symbol,
  napi_object,
  napi_function,
  napi_external,
  napi_bigint
} napi_valuetype;

typedef enum {
  napi_int8_array,
  napi_uint8_array,
  napi_uint8_clamped_array,
  napi_int16_array,
  napi_uint16_array,
  napi_int32_array,
  napi_uint32_array,
  napi_float32_array,
  napi_float64_array,
  napi_bigint64_array,
  napi_biguint64_array
} napi_typedarray_type;

typedef enum {
  napi_ok,
  napi_invalid_arg,
  napi_object_expected,
  napi_string_expected,
  napi_name_expected,
  napi_function_expected,
  napi_number_expected,
  napi_boolean_expected,
  napi_array_expected,
  napi_generic_failure,
  napi_pending_exception,
  napi_cancelled,
  napi_escape_called_twice,
  napi_handle_scope_mismatch,
  napi_callback_scope_mismatch,
  napi_queue_full,
  napi_closing,
  napi_bigint_expected,
  napi_date_expected,
  napi_arraybuffer_expected,
  napi_detachable_arraybuffer_expected,
  napi_would_deadlock,
  napi_no_external_buffers_allowed,
  napi_cannot_run_js
} napi_status;

typedef napi_value (*napi_callback)(napi_env env, napi_callback_info info);
typedef void (*napi_finalize)(napi_env env, void* finalize_data, void* finalize_hint);

typedef struct {
  const char* utf8name;
  napi_value name;
  napi_callback method;
  napi_callback getter;
  napi_callback setter;
  napi_value value;
  napi_property_attributes attributes;
  void* data;
} napi_property_descriptor;

typedef struct {
  int32_t (*version)(void);
  const char* (*get_module_file_name)(napi_env env);
} napi_node_version;

// ---------------------------------------------------------------------------
// 链接方式
// ---------------------------------------------------------------------------
// 官方头文件在这里铺开一堆内联函数（napi_get_undefined 等）来换取编译期检查。
// 我们不需要那层包装：N-API 的入口点就是运行时导出的普通 C 符号，
// 直接按官方头文件里的“同一原型”把函数指针声明出来即可。
//
// NAPI_EXTERN(napi_get_undefined) 展开成：
//     extern "C" napi_status napi_get_undefined(napi_env env, napi_value* result);
// 在 C++ 下指向的就是名字一模一样的导出符号。
//
// 官方 js_native_api.h 会 assert NAPI_VERSION 落在支持区间内。
// 这里只需要一个数值：编译期用不到，运行时也读不到它（模块结构体里的
// nm_version 才是真正被检查的字段）。统一在 js_native_api.h 里定义，避免重定义警告。

#ifdef __cplusplus
#define NAPI_EXTERN extern "C"
#else
#define NAPI_EXTERN extern
#endif

#if defined(_WIN32)
#define NAPI_CDECL __cdecl
#else
#define NAPI_CDECL
#endif

#endif // SHADOW_NODE_API_TYPES_H_
