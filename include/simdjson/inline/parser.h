#ifndef SIMDJSON_INLINE_PARSER_H
#define SIMDJSON_INLINE_PARSER_H

#include "simdjson/dom/document_stream.h"
#include "simdjson/dom/parser.h"
#include "simdjson/implementation.h"
#include "simdjson/internal/jsonformatutils.h"
#include "simdjson/portability.h"
#include <cstdio>
#include <climits>

namespace simdjson {
namespace dom {

//
// parser inline implementation
//
really_inline parser::parser(size_t max_capacity) noexcept
  : _max_capacity{max_capacity},
    loaded_bytes(nullptr, &aligned_free_char)
    {}
inline bool parser::is_valid() const noexcept { return valid; }
inline int parser::get_error_code() const noexcept { return error; }
inline std::string parser::get_error_message() const noexcept { return error_message(error); }
inline bool parser::print_json(std::ostream &os) const noexcept {
  if (!valid) { return false; }
  os << doc.root();
  return true;
}

really_inline parser::~parser() noexcept {
  UNUSED auto result = allocate(0, 0);
}

inline bool parser::dump_raw_tape(std::ostream &os) const noexcept {
  return valid ? doc.dump_raw_tape(os) : false;
}

inline simdjson_result<size_t> parser::read_file(const std::string &path) noexcept {
  // Open the file
  SIMDJSON_PUSH_DISABLE_WARNINGS
  SIMDJSON_DISABLE_DEPRECATED_WARNING // Disable CRT_SECURE warning on MSVC: manually verified this is safe
  std::FILE *fp = std::fopen(path.c_str(), "rb");
  SIMDJSON_POP_DISABLE_WARNINGS

  if (fp == nullptr) {
    return IO_ERROR;
  }

  // Get the file size
  if(std::fseek(fp, 0, SEEK_END) < 0) {
    std::fclose(fp);
    return IO_ERROR;
  }
  long len = std::ftell(fp);
  if((len < 0) || (len == LONG_MAX)) {
    std::fclose(fp);
    return IO_ERROR;
  }

  // Make sure we have enough capacity to load the file
  if (_loaded_bytes_capacity < size_t(len)) {
    loaded_bytes.reset( internal::allocate_padded_buffer(len) );
    if (!loaded_bytes) {
      std::fclose(fp);
      return MEMALLOC;
    }
    _loaded_bytes_capacity = len;
  }

  // Read the string
  std::rewind(fp);
  size_t bytes_read = std::fread(loaded_bytes.get(), 1, len, fp);
  if (std::fclose(fp) != 0 || bytes_read != size_t(len)) {
    return IO_ERROR;
  }

  return bytes_read;
}

inline simdjson_result<element> parser::load(const std::string &path) & noexcept {
  size_t len;
  error_code code;
  read_file(path).tie(len, code);
  if (code) { return code; }

  return parse(loaded_bytes.get(), len, false);
}

inline document_stream parser::load_many(const std::string &path, size_t batch_size) noexcept {
  size_t len;
  error_code code;
  read_file(path).tie(len, code);
  return document_stream(*this, (const uint8_t*)loaded_bytes.get(), len, batch_size, code);
}

inline simdjson_result<element> parser::parse(const uint8_t *buf, size_t len, bool realloc_if_needed) & noexcept {
  error_code code = ensure_capacity(len);
  if (code) { return code; }

  if (realloc_if_needed) {
    const uint8_t *tmp_buf = buf;
    buf = (uint8_t *)internal::allocate_padded_buffer(len);
    if (buf == nullptr)
      return MEMALLOC;
    memcpy((void *)buf, tmp_buf, len);
  }

  code = simdjson::active_implementation->parse(buf, len, *this);
  if (realloc_if_needed) {
    aligned_free((void *)buf); // must free before we exit
  }
  if (code) { return code; }

  // We're indicating validity via the simdjson_result<element>, so set the parse state back to invalid
  valid = false;
  error = UNINITIALIZED;
  return doc.root();
}
really_inline simdjson_result<element> parser::parse(const char *buf, size_t len, bool realloc_if_needed) & noexcept {
  return parse((const uint8_t *)buf, len, realloc_if_needed);
}
really_inline simdjson_result<element> parser::parse(const std::string &s) & noexcept {
  return parse(s.data(), s.length(), s.capacity() - s.length() < SIMDJSON_PADDING);
}
really_inline simdjson_result<element> parser::parse(const padded_string &s) & noexcept {
  return parse(s.data(), s.length(), false);
}

inline document_stream parser::parse_many(const uint8_t *buf, size_t len, size_t batch_size) noexcept {
  return document_stream(*this, buf, len, batch_size);
}
inline document_stream parser::parse_many(const char *buf, size_t len, size_t batch_size) noexcept {
  return parse_many((const uint8_t *)buf, len, batch_size);
}
inline document_stream parser::parse_many(const std::string &s, size_t batch_size) noexcept {
  return parse_many(s.data(), s.length(), batch_size);
}
inline document_stream parser::parse_many(const padded_string &s, size_t batch_size) noexcept {
  return parse_many(s.data(), s.length(), batch_size);
}

really_inline size_t parser::capacity() const noexcept {
  return _capacity;
}
really_inline size_t parser::max_capacity() const noexcept {
  return _max_capacity;
}
really_inline size_t parser::max_depth() const noexcept {
  return _max_depth;
}

WARN_UNUSED
inline error_code parser::allocate(size_t capacity, size_t max_depth) noexcept {
  //
  // If capacity has changed, reallocate capacity-based buffers
  //
  if (_capacity != capacity || !doc.tape) {
    error_code err = doc.allocate(capacity);
    if (err) { return err; }
  }
  if (_capacity != capacity || _max_depth != max_depth) {
    error_code err = active_implementation->allocate(*this, capacity, max_depth);
    if (err) {
      _capacity = 0;
      _max_depth = 0;
      return err;
    }

    _capacity = capacity;
    _max_depth = max_depth;
  }

  return SUCCESS;
}

WARN_UNUSED
inline bool parser::allocate_capacity(size_t capacity, size_t max_depth) noexcept {
  return !allocate(capacity, max_depth);
}

really_inline void parser::set_max_capacity(size_t max_capacity) noexcept {
  _max_capacity = max_capacity;
}

inline error_code parser::ensure_capacity(size_t desired_capacity) noexcept {
  // If we don't have enough capacity, (try to) automatically bump it.
  // If the document was taken, reallocate that too.
  // Both in one if statement to minimize unlikely branching.
  if (unlikely(desired_capacity > capacity() || !doc.tape)) {
    if (desired_capacity > max_capacity()) {
      return error = CAPACITY;
    }
    return allocate(desired_capacity, _max_depth > 0 ? _max_depth : DEFAULT_MAX_DEPTH);
  }

  return SUCCESS;
}

template<typename T>
inline T& parser::implementation_state() noexcept {
  static_assert(sizeof(T) <= sizeof(_implementation_state), "Implementation state doesn't fit in the parser!");
  return *reinterpret_cast<T*>(&_implementation_state);
}

} // namespace dom
} // namespace simdjson

#endif // SIMDJSON_INLINE_PARSER_H
