// js_native_api.h — 最小化的 N-API 函数声明（本地 shim）
//
// 只声明本工程实际用到的入口点。每个原型都与官方 js_native_api.h / node_api.h
// 完全一致；改错原型会导致运行时栈错乱，所以增删条目时务必对照官方头文件。

#ifndef SHADOW_JS_NATIVE_API_H_
#define SHADOW_JS_NATIVE_API_H_

#include "node_api_types.h"

// ---------------------------------------------------------------------------
// 通用
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_double(napi_env env, double value, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_double(napi_env env, napi_value value, double* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int32(napi_env env, napi_value value, int32_t* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int64(napi_env env, napi_value value, int64_t* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bool(napi_env env, napi_value value, bool* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_typeof(napi_env env, napi_value value, napi_valuetype* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_instanceof(napi_env env, napi_value object, napi_value constructor, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env, napi_value* result);

// ---------------------------------------------------------------------------
// 字符串
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf8(napi_env env, const char* str, size_t length, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf16(napi_env env, const char16_t* str, size_t length, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf8(napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env, napi_value value, char16_t* buf, size_t bufsize, size_t* result);

// ---------------------------------------------------------------------------
// 对象 / 属性
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_array_with_length(napi_env env, size_t length, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_has_element(napi_env env, napi_value object, uint32_t index, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_array_length(napi_env env, napi_value value, uint32_t* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_property(napi_env env, napi_value object, napi_value key, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_has_property(napi_env env, napi_value object, napi_value key, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_delete_property(napi_env env, napi_value object, napi_value key, bool* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_set_named_property(napi_env env, napi_value object, const char* utf8name, napi_value value);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(napi_env env, napi_value object, const char* utf8name, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_has_named_property(napi_env env, napi_value object, const char* utf8name, bool* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_define_properties(napi_env env, napi_value object, size_t property_count, const napi_property_descriptor* properties);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_property_names(napi_env env, napi_value object, napi_value* result);

// ---------------------------------------------------------------------------
// 函数 / 调用
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_create_function(napi_env env, const char* utf8name, size_t length, napi_callback cb, void* data, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_call_function(napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value* argv, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t* argc, napi_value* argv, napi_value* this_arg, void** data);
NAPI_EXTERN napi_status NAPI_CDECL napi_new_instance(napi_env env, napi_value constructor, size_t argc, const napi_value* argv, napi_value* result);

// ---------------------------------------------------------------------------
// 错误
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error);
NAPI_EXTERN napi_status NAPI_CDECL napi_throw_error(napi_env env, const char* code, const char* msg);
NAPI_EXTERN napi_status NAPI_CDECL napi_throw_type_error(napi_env env, const char* code, const char* msg);

// ---------------------------------------------------------------------------
// 句柄作用域 / 生命周期
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_open_handle_scope(napi_env env, napi_handle_scope* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_close_handle_scope(napi_env env, napi_handle_scope scope);
NAPI_EXTERN napi_status NAPI_CDECL napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope);
NAPI_EXTERN napi_status NAPI_CDECL napi_escape_handle(napi_env env, napi_escapable_handle_scope scope, napi_value escapee, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_delete_reference(napi_env env, napi_ref ref);
NAPI_EXTERN napi_status NAPI_CDECL napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_reference_value(napi_env env, napi_ref ref, napi_value* result);

// ---------------------------------------------------------------------------
// 外部内存 / ArrayBuffer
// ---------------------------------------------------------------------------
NAPI_EXTERN napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env, size_t byte_length, void** data, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env, void* external_data, size_t byte_length,
    napi_finalize finalize_cb, void* finalize_hint, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void** data, size_t* byte_length);
NAPI_EXTERN napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer);
NAPI_EXTERN napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env, napi_value value, bool* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer(napi_env env, size_t length, void** data, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_buffer(napi_env env, size_t length, void* data, napi_finalize finalize_cb, void* finalize_hint, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_buffer_info(napi_env env, napi_value value, void** data, size_t* length);

NAPI_EXTERN napi_status NAPI_CDECL napi_create_typedarray(napi_env env, napi_typedarray_type type, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env, napi_value typedarray, napi_typedarray_type* type, size_t* length, void** data, napi_value* arraybuffer, size_t* byte_offset);

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external(napi_env env, void* data, napi_finalize finalize_cb, void* finalize_hint, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_external(napi_env env, napi_value value, void** result);

// ---------------------------------------------------------------------------
// node_api.h 里的扩展
// ---------------------------------------------------------------------------
// 注意：node_api_get_module_file_name 在 electron.exe 的导出表里不存在
// （node.exe 有），所以这里不声明、代码里也不要用。

// 模块注册入口（由运行时的 dlsym/GetProcAddress 查找）
NAPI_EXTERN napi_status NAPI_CDECL napi_module_register(void* mod);

#endif // SHADOW_JS_NATIVE_API_H_
