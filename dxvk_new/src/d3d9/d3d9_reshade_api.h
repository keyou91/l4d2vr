/*
 * Minimal ABI declarations derived from ReShade's public add-on headers.
 * ReShade copyright (C) Patrick Mours and contributors.
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER)
#define L4D2VR_RESHADE_NOVTABLE __declspec(novtable)
#else
#define L4D2VR_RESHADE_NOVTABLE
#endif

// Minimal ABI mirror of the public ReShade add-on API 18 interfaces used by
// L4D2VR. The declaration order and called method signatures are taken from
// ReShade's public include/reshade_api*.hpp headers. Keeping this local subset
// avoids a link-time dependency and still lets the compiler perform normal
// virtual dispatch instead of calling hard-coded vtable slots by hand.
namespace dxvk::reshade_api {

  constexpr uint32_t ApiVersion = 18;

  struct resource {
    uint64_t handle = 0;
  };

  struct resource_view {
    uint64_t handle = 0;
  };

  struct effect_uniform_variable {
    uint64_t handle = 0;
  };

  struct effect_texture_variable {
    uint64_t handle = 0;
  };

  enum class format : uint32_t;

  enum class device_api : uint32_t {
    vulkan = 0x20000,
  };

  enum class addon_event : uint32_t {
    destroy_effect_runtime = 10,
    reshade_begin_effects = 76,
    reshade_reloaded_effects = 78,
  };

  struct command_list;
  struct command_queue;
  struct device;
  struct effect_runtime;

  struct L4D2VR_RESHADE_NOVTABLE api_object {
    virtual uint64_t get_native() const = 0;
    virtual void get_private_data(const uint8_t guid[16], uint64_t* data) const = 0;
    virtual void set_private_data(const uint8_t guid[16], uint64_t data) = 0;
  };

  struct L4D2VR_RESHADE_NOVTABLE device : api_object {
    virtual device_api get_api() const = 0;
  };

  struct L4D2VR_RESHADE_NOVTABLE device_object : api_object {
    virtual device* get_device() = 0;
  };

  struct L4D2VR_RESHADE_NOVTABLE effect_runtime : device_object {
    virtual void* get_hwnd() const = 0;
    virtual resource get_back_buffer(uint32_t index) = 0;
    virtual uint32_t get_back_buffer_count() const = 0;
    virtual uint32_t get_current_back_buffer_index() const = 0;
    virtual command_queue* get_command_queue() = 0;
    virtual void render_effects(command_list* cmd_list, resource_view rtv, resource_view rtv_srgb) = 0;
    virtual bool capture_screenshot(void* pixels) = 0;
    virtual void get_screenshot_width_and_height(uint32_t* out_width, uint32_t* out_height) const = 0;
    virtual bool is_key_down(uint32_t keycode) const = 0;
    virtual bool is_key_pressed(uint32_t keycode) const = 0;
    virtual bool is_key_released(uint32_t keycode) const = 0;
    virtual bool is_mouse_button_down(uint32_t button) const = 0;
    virtual bool is_mouse_button_pressed(uint32_t button) const = 0;
    virtual bool is_mouse_button_released(uint32_t button) const = 0;
    virtual void get_mouse_cursor_position(uint32_t* out_x, uint32_t* out_y, int16_t* out_wheel_delta = nullptr) const = 0;

    virtual void enumerate_uniform_variables(
      const char* effect_name,
      void (*callback)(effect_runtime* runtime, effect_uniform_variable variable, void* user_data),
      void* user_data) = 0;
    virtual effect_uniform_variable find_uniform_variable(const char* effect_name, const char* variable_name) const = 0;
    virtual void get_uniform_variable_type(
      effect_uniform_variable variable,
      format* out_base_type,
      uint32_t* out_rows = nullptr,
      uint32_t* out_columns = nullptr,
      uint32_t* out_array_length = nullptr) const = 0;
    virtual void get_uniform_variable_name(effect_uniform_variable variable, char* name, size_t* name_size) const = 0;
    virtual bool get_annotation_bool_from_uniform_variable(effect_uniform_variable variable, const char* name, bool* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_float_from_uniform_variable(effect_uniform_variable variable, const char* name, float* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_int_from_uniform_variable(effect_uniform_variable variable, const char* name, int32_t* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_uint_from_uniform_variable(effect_uniform_variable variable, const char* name, uint32_t* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_string_from_uniform_variable(effect_uniform_variable variable, const char* name, char* value, size_t* value_size) const = 0;
    virtual void get_uniform_value_bool(effect_uniform_variable variable, bool* values, size_t count, size_t array_index = 0) const = 0;
    virtual void get_uniform_value_float(effect_uniform_variable variable, float* values, size_t count, size_t array_index = 0) const = 0;
    virtual void get_uniform_value_int(effect_uniform_variable variable, int32_t* values, size_t count, size_t array_index = 0) const = 0;
    virtual void get_uniform_value_uint(effect_uniform_variable variable, uint32_t* values, size_t count, size_t array_index = 0) const = 0;
    virtual void set_uniform_value_bool(effect_uniform_variable variable, const bool* values, size_t count, size_t array_index = 0) = 0;
    virtual void set_uniform_value_float(effect_uniform_variable variable, const float* values, size_t count, size_t array_index = 0) = 0;
    virtual void set_uniform_value_int(effect_uniform_variable variable, const int32_t* values, size_t count, size_t array_index = 0) = 0;
    virtual void set_uniform_value_uint(effect_uniform_variable variable, const uint32_t* values, size_t count, size_t array_index = 0) = 0;

    virtual void enumerate_texture_variables(
      const char* effect_name,
      void (*callback)(effect_runtime* runtime, effect_texture_variable variable, void* user_data),
      void* user_data) = 0;
    virtual effect_texture_variable find_texture_variable(const char* effect_name, const char* variable_name) const = 0;
    virtual void get_texture_variable_name(effect_texture_variable variable, char* name, size_t* name_size) const = 0;
    virtual bool get_annotation_bool_from_texture_variable(effect_texture_variable variable, const char* name, bool* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_float_from_texture_variable(effect_texture_variable variable, const char* name, float* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_int_from_texture_variable(effect_texture_variable variable, const char* name, int32_t* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_uint_from_texture_variable(effect_texture_variable variable, const char* name, uint32_t* values, size_t count, size_t array_index = 0) const = 0;
    virtual bool get_annotation_string_from_texture_variable(effect_texture_variable variable, const char* name, char* value, size_t* value_size) const = 0;
    virtual void update_texture(effect_texture_variable variable, uint32_t width, uint32_t height, const void* pixels) = 0;
    virtual void get_texture_binding(effect_texture_variable variable, resource_view* out_srv, resource_view* out_srv_srgb) const = 0;
    virtual void update_texture_bindings(const char* semantic, resource_view srv, resource_view srv_srgb) = 0;
  };

  static_assert(sizeof(resource) == sizeof(uint64_t));
  static_assert(sizeof(resource_view) == sizeof(uint64_t));
  static_assert(sizeof(effect_uniform_variable) == sizeof(uint64_t));
  static_assert(sizeof(effect_texture_variable) == sizeof(uint64_t));

}

#undef L4D2VR_RESHADE_NOVTABLE
