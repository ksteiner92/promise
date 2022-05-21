#ifndef UTIL_BUFFER_H_
#define UTIL_BUFFER_H_

#include "slice.h"
#include <memory>

namespace ks {

template <class T>
class Buffer {
 public:
  inline T* data() { return data_.get(); }

  inline const T* data() const { return data_.get(); }

  inline size_t size() const { return size_; }

  inline bool empty() const { return size_ == 0; }

  inline size_t capacity() const { return capacity_; }

  inline T& operator[](size_t i) { return data_[i]; }

  inline const T& operator[](size_t i) const { return data_[i]; }

  inline operator Slice() { return Slice(data(), size()); }

  void reserve(size_t bytes) {
    if (bytes > capacity_) {
      size_t new_capacity = std::max<size_t>(capacity_ * 1.5, bytes);
      std::unique_ptr<T[]> new_data = std::make_unique<T[]>(new_capacity);
      memcpy(new_data.get(), data_.get(), sizeof(T) * size_);
      data_ = std::move(new_data);
      capacity_ = new_capacity;
    }
  }

  inline void set_size(size_t size) { size_ = size; }

  inline void set_end(const T* p) { size_ = p - data(); }

  inline void clear() { size_ = 0; }

  inline T* begin() { return data(); }

  inline T* end() { return data() + size(); }

  std::unique_ptr<T[]> detach() {
    size_ = capacity_ = 0;
    return std::move(data_);
  }

 private:
  std::unique_ptr<T[]> data_;
  size_t capacity_ = 0;
  size_t size_ = 0;
};

}  // namespace ks

#endif  // UTIL_BUFFER_H_
